use std::{
    sync::mpsc as std_mpsc,
    thread::{self, JoinHandle},
    time::Duration,
};

use crossbeam_channel::{Receiver, unbounded};
use futures_util::{SinkExt as _, StreamExt as _};
use serde_json::Value;
use tokio::{runtime::Builder, sync::mpsc};
use tokio_tungstenite::{connect_async, tungstenite::Message};

use crate::room_signaling::{
    RoomEvent, RoomRole, configured_room_websocket_url, encode_signal_message, parse_room_event,
};

const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Debug)]
enum Command {
    Text(String),
    Close,
}

pub struct RoomSocket {
    commands: mpsc::UnboundedSender<Command>,
    events: Receiver<Result<RoomEvent, String>>,
    worker: Option<JoinHandle<()>>,
}

impl std::fmt::Debug for RoomSocket {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.debug_struct("RoomSocket").finish_non_exhaustive()
    }
}

impl RoomSocket {
    pub fn connect(lookup_id: &str, role: RoomRole) -> Result<Self, String> {
        let url = configured_room_websocket_url(lookup_id, role)?
            .ok_or_else(|| "Cloudflare room signaling is not configured".to_owned())?;
        Self::connect_url(url)
    }

    fn connect_url(url: String) -> Result<Self, String> {
        let (command_sender, command_receiver) = mpsc::unbounded_channel();
        let (event_sender, event_receiver) = unbounded();
        let (ready_sender, ready_receiver) = std_mpsc::sync_channel(1);
        let worker = thread::spawn(move || {
            let runtime = match Builder::new_current_thread().enable_all().build() {
                Ok(runtime) => runtime,
                Err(error) => {
                    let _ = ready_sender.send(Err(format!(
                        "failed to initialize signaling runtime: {error}"
                    )));
                    return;
                }
            };
            runtime.block_on(async move {
                let websocket = match tokio::time::timeout(CONNECT_TIMEOUT, connect_async(&url)).await
                {
                    Ok(Ok((websocket, _response))) => websocket,
                    Ok(Err(error)) => {
                        let _ = ready_sender
                            .send(Err(format!("room WebSocket connection failed: {error}")));
                        return;
                    }
                    Err(_) => {
                        let _ = ready_sender
                            .send(Err("room WebSocket connection timed out".to_owned()));
                        return;
                    }
                };
                let _ = ready_sender.send(Ok(()));
                let (mut sink, mut stream) = websocket.split();
                let mut commands = command_receiver;

                loop {
                    tokio::select! {
                        command = commands.recv() => {
                            match command {
                                Some(Command::Text(text)) => {
                                    if let Err(error) = sink.send(Message::Text(text.into())).await {
                                        let _ = event_sender.send(Err(format!(
                                            "room WebSocket send failed: {error}"
                                        )));
                                        break;
                                    }
                                }
                                Some(Command::Close) | None => {
                                    let _ = sink.send(Message::Close(None)).await;
                                    break;
                                }
                            }
                        }
                        message = stream.next() => {
                            match message {
                                Some(Ok(Message::Text(text))) => {
                                    let event = parse_room_event(text.as_ref());
                                    let invalid = event.is_err();
                                    let _ = event_sender.send(event);
                                    if invalid {
                                        break;
                                    }
                                }
                                Some(Ok(Message::Ping(payload))) => {
                                    if let Err(error) = sink.send(Message::Pong(payload)).await {
                                        let _ = event_sender.send(Err(format!(
                                            "room WebSocket pong failed: {error}"
                                        )));
                                        break;
                                    }
                                }
                                Some(Ok(Message::Pong(_))) => {}
                                Some(Ok(Message::Close(_))) | None => break,
                                Some(Ok(_)) => {
                                    let _ = event_sender.send(Err(
                                        "room WebSocket received a non-text protocol message"
                                            .to_owned(),
                                    ));
                                    break;
                                }
                                Some(Err(error)) => {
                                    let _ = event_sender.send(Err(format!(
                                        "room WebSocket receive failed: {error}"
                                    )));
                                    break;
                                }
                            }
                        }
                    }
                }
            });
        });

        match ready_receiver.recv_timeout(CONNECT_TIMEOUT + Duration::from_secs(1)) {
            Ok(Ok(())) => Ok(Self {
                commands: command_sender,
                events: event_receiver,
                worker: Some(worker),
            }),
            Ok(Err(error)) => {
                let _ = worker.join();
                Err(error)
            }
            Err(_) => Err("room WebSocket worker did not initialize".to_owned()),
        }
    }

    pub fn send_signal(&self, peer_id: &str, payload: &Value) -> Result<(), String> {
        let message = encode_signal_message(peer_id, payload)?;
        self.commands
            .send(Command::Text(message))
            .map_err(|_| "room WebSocket is closed".to_owned())
    }

    pub fn recv_timeout(&self, timeout: Duration) -> Result<RoomEvent, String> {
        match self.events.recv_timeout(timeout) {
            Ok(event) => event,
            Err(crossbeam_channel::RecvTimeoutError::Timeout) => {
                Err("room WebSocket event timed out".to_owned())
            }
            Err(crossbeam_channel::RecvTimeoutError::Disconnected) => {
                Err("room WebSocket is closed".to_owned())
            }
        }
    }

    #[must_use]
    pub fn try_recv(&self) -> Option<Result<RoomEvent, String>> {
        self.events.try_recv().ok()
    }

    pub fn close(&mut self) {
        let _ = self.commands.send(Command::Close);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Drop for RoomSocket {
    fn drop(&mut self) {
        self.close();
    }
}
