use std::{
    collections::HashMap,
    net::Ipv4Addr,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc as std_mpsc,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

use serde_json::{Value, json};
use tokio::{
    io,
    net::{TcpListener, TcpStream},
    runtime::Builder,
    sync::mpsc,
};

use crate::{
    client::Client,
    native_quic::{NativeQuicEndpoint, NativeQuicRole},
    room_signaling::{RoomEvent, RoomRole},
    room_socket::{RoomSender, RoomSocket},
    signaling::{SignalingClient, Token, looks_like_room_code, room_lookup_id},
};

const ROOM_POLL: Duration = Duration::from_millis(20);
const HOST_SIGNAL_TIMEOUT: Duration = Duration::from_secs(20);
const CLIENT_SIGNAL_TIMEOUT: Duration = Duration::from_secs(8);
const CLIENT_CONNECT_TIMEOUT: Duration = Duration::from_secs(12);
const CLIENT_START_TIMEOUT: Duration = Duration::from_secs(45);

#[derive(Debug)]
enum PeerCommand {
    Description(String),
    Stop,
}

#[derive(Debug)]
pub struct NativeQuicHost {
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl NativeQuicHost {
    pub fn start(room_code: &str, native_port: u16) -> Result<Self, String> {
        let lookup = room_lookup_id(room_code);
        let socket = RoomSocket::connect(&lookup, RoomRole::Host)?;
        let sender = socket.sender();
        let stop = Arc::new(AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop);
        let worker = thread::Builder::new()
            .name("folderbuddies-quic-host".to_owned())
            .spawn(move || host_signaling_loop(socket, sender, native_port, worker_stop))
            .map_err(|error| format!("failed to start native QUIC host: {error}"))?;
        Ok(Self {
            stop,
            worker: Some(worker),
        })
    }

    pub fn stop(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Drop for NativeQuicHost {
    fn drop(&mut self) {
        self.stop();
    }
}

fn host_signaling_loop(
    mut socket: RoomSocket,
    sender: RoomSender,
    native_port: u16,
    stop: Arc<AtomicBool>,
) {
    let mut peers: HashMap<String, mpsc::UnboundedSender<PeerCommand>> = HashMap::new();
    let mut peer_threads = Vec::new();
    while !stop.load(Ordering::Acquire) {
        match socket.try_recv() {
            Some(Ok(RoomEvent::Signal { peer_id, payload })) => match signal_type(&payload) {
                Some("native-quic-hello") => {
                    if !peers.contains_key(&peer_id) {
                        let (commands, receiver) = mpsc::unbounded_channel();
                        peers.insert(peer_id.clone(), commands);
                        let room_sender = sender.clone();
                        let peer_stop = Arc::clone(&stop);
                        let thread_peer_id = peer_id.clone();
                        match thread::Builder::new()
                            .name("folderbuddies-quic-peer".to_owned())
                            .spawn(move || {
                                run_host_peer(
                                    room_sender,
                                    thread_peer_id,
                                    native_port,
                                    receiver,
                                    peer_stop,
                                );
                            }) {
                            Ok(handle) => peer_threads.push(handle),
                            Err(error) => {
                                eprintln!(
                                    "Folder Buddies: failed to start native QUIC peer: {error}"
                                );
                                peers.remove(&peer_id);
                            }
                        }
                    }
                }
                Some("native-quic-description") => {
                    if let Some(description) = signal_description(&payload)
                        && let Some(commands) = peers.get(&peer_id)
                    {
                        let _ = commands.send(PeerCommand::Description(description.to_owned()));
                    }
                }
                _ => {}
            },
            Some(Ok(RoomEvent::ClientLeft { peer_id })) => {
                if let Some(commands) = peers.remove(&peer_id) {
                    let _ = commands.send(PeerCommand::Stop);
                }
            }
            Some(Ok(RoomEvent::Error { error })) => {
                eprintln!("Folder Buddies: room signaling error: {error}");
            }
            Some(Err(error)) => {
                eprintln!("Folder Buddies: room signaling failed: {error}");
                break;
            }
            Some(Ok(_)) | None => thread::sleep(ROOM_POLL),
        }
    }
    for (_, commands) in peers.drain() {
        let _ = commands.send(PeerCommand::Stop);
    }
    for handle in peer_threads {
        let _ = handle.join();
    }
    socket.close();
}

fn run_host_peer(
    sender: RoomSender,
    peer_id: String,
    native_port: u16,
    mut commands: mpsc::UnboundedReceiver<PeerCommand>,
    stop: Arc<AtomicBool>,
) {
    let runtime = match Builder::new_current_thread().enable_all().build() {
        Ok(runtime) => runtime,
        Err(error) => {
            eprintln!("Folder Buddies: failed to initialize native QUIC runtime: {error}");
            return;
        }
    };
    if let Err(error) = runtime.block_on(async move {
        let mut endpoint = match NativeQuicEndpoint::start(NativeQuicRole::Server).await {
            Ok(endpoint) => endpoint,
            Err(error) => {
                send_quic_error(&sender, &peer_id, &error);
                return Ok::<(), String>(());
            }
        };
        sender.send_signal(
            &peer_id,
            &json!({
                "type": "native-quic-description",
                "description": endpoint.local_description(),
            }),
        )?;

        let description = receive_description(&mut commands, &stop).await?;
        if let Err(error) = endpoint.set_remote_description(&description).await {
            send_quic_error(&sender, &peer_id, &error);
            endpoint.close().await;
            return Ok(());
        }

        loop {
            if stop.load(Ordering::Acquire) {
                break;
            }
            tokio::select! {
                command = commands.recv() => {
                    match command {
                        Some(PeerCommand::Stop) | None => break,
                        Some(PeerCommand::Description(_)) => {}
                    }
                }
                stream = endpoint.accept_stream() => {
                    let (send, recv) = stream?;
                    tokio::spawn(async move {
                        if let Ok(tcp) = TcpStream::connect((Ipv4Addr::LOCALHOST, native_port)).await {
                            let _ = bridge(tcp, send, recv).await;
                        }
                    });
                }
            }
        }
        endpoint.close().await;
        Ok(())
    }) {
        eprintln!("Folder Buddies: native QUIC peer failed: {error}");
    }
}

fn send_quic_error(sender: &RoomSender, peer_id: &str, message: &str) {
    let _ = sender.send_signal(
        peer_id,
        &json!({
            "type": "native-quic-error",
            "message": message,
        }),
    );
}

async fn receive_description(
    commands: &mut mpsc::UnboundedReceiver<PeerCommand>,
    stop: &AtomicBool,
) -> Result<String, String> {
    let deadline = Instant::now() + HOST_SIGNAL_TIMEOUT;
    loop {
        if stop.load(Ordering::Acquire) {
            return Err("native QUIC host stopped".to_owned());
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err("native QUIC remote description timed out".to_owned());
        }
        match tokio::time::timeout(remaining.min(Duration::from_millis(250)), commands.recv()).await {
            Ok(Some(PeerCommand::Description(description))) => return Ok(description),
            Ok(Some(PeerCommand::Stop)) | Ok(None) => {
                return Err("native QUIC peer disconnected".to_owned());
            }
            Err(_) => {}
        }
    }
}

#[derive(Debug)]
pub struct NativeQuicClient {
    client: Arc<Client>,
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl NativeQuicClient {
    pub fn connect(room_code: &str, token: &Token) -> Result<Self, String> {
        if !looks_like_room_code(room_code)
            || !matches!(SignalingClient::from_environment(), Ok(Some(_)))
        {
            return Err(
                "native QUIC needs a published room code and Cloudflare signaling".to_owned(),
            );
        }

        let lookup = room_lookup_id(room_code);
        let stop = Arc::new(AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop);
        let (ready_sender, ready_receiver) = std_mpsc::sync_channel(1);
        let worker = thread::Builder::new()
            .name("folderbuddies-quic-client".to_owned())
            .spawn(move || client_worker(lookup, worker_stop, ready_sender))
            .map_err(|error| format!("failed to start native QUIC client: {error}"))?;

        let proxy_port = match ready_receiver.recv_timeout(CLIENT_START_TIMEOUT) {
            Ok(Ok(port)) => port,
            Ok(Err(error)) => {
                stop.store(true, Ordering::Release);
                let _ = worker.join();
                return Err(error);
            }
            Err(_) => {
                stop.store(true, Ordering::Release);
                let _ = worker.join();
                return Err("direct QUIC/ICE connection timed out".to_owned());
            }
        };
        let proxy_token = Token::new(
            Ipv4Addr::LOCALHOST.to_string(),
            proxy_port,
            token.secret().to_vec(),
            token.folder().to_owned(),
            token.allow_writes(),
        )?;
        let client = match Client::connect_default(&proxy_token) {
            Ok(client) => Arc::new(client),
            Err(error) => {
                stop.store(true, Ordering::Release);
                let _ = worker.join();
                return Err(error);
            }
        };
        Ok(Self {
            client,
            stop,
            worker: Some(worker),
        })
    }

    #[must_use]
    pub fn client(&self) -> Arc<Client> {
        Arc::clone(&self.client)
    }

    #[must_use]
    pub fn connected(&self) -> bool {
        self.client.connected() && !self.stop.load(Ordering::Acquire)
    }

    pub fn disconnect(&mut self) {
        self.client.disconnect();
        self.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Drop for NativeQuicClient {
    fn drop(&mut self) {
        self.disconnect();
    }
}

fn client_worker(
    lookup: String,
    stop: Arc<AtomicBool>,
    ready_sender: std_mpsc::SyncSender<Result<u16, String>>,
) {
    let mut socket = match RoomSocket::connect(&lookup, RoomRole::Client) {
        Ok(socket) => socket,
        Err(error) => {
            let _ = ready_sender.send(Err(error));
            return;
        }
    };
    let peer_id = match wait_for_client_ready(&socket, &stop) {
        Ok(peer_id) => peer_id,
        Err(error) => {
            let _ = ready_sender.send(Err(error));
            socket.close();
            return;
        }
    };
    let runtime = match Builder::new_current_thread().enable_all().build() {
        Ok(runtime) => runtime,
        Err(error) => {
            let _ = ready_sender.send(Err(format!(
                "failed to initialize native QUIC runtime: {error}"
            )));
            socket.close();
            return;
        }
    };
    let result = runtime.block_on(async {
        let mut endpoint = NativeQuicEndpoint::start(NativeQuicRole::Client).await?;
        socket.send_signal(&peer_id, &json!({"type": "native-quic-hello"}))?;
        socket.send_signal(
            &peer_id,
            &json!({
                "type": "native-quic-description",
                "description": endpoint.local_description(),
            }),
        )?;
        let remote = wait_for_remote_description(&socket, &peer_id, &stop).await?;
        match tokio::time::timeout(
            CLIENT_CONNECT_TIMEOUT,
            endpoint.set_remote_description(&remote),
        )
        .await
        {
            Ok(result) => result?,
            Err(_) => return Err("direct QUIC/ICE connection timed out".to_owned()),
        }
        let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0))
            .await
            .map_err(|error| format!("native QUIC proxy bind failed: {error}"))?;
        let port = listener
            .local_addr()
            .map_err(|error| error.to_string())?
            .port();
        ready_sender
            .send(Ok(port))
            .map_err(|_| "native QUIC caller disappeared".to_owned())?;

        while !stop.load(Ordering::Acquire) && endpoint.connected() {
            tokio::select! {
                accepted = listener.accept() => {
                    let (tcp, _) = accepted.map_err(|error| error.to_string())?;
                    let (send, recv) = endpoint.open_stream().await?;
                    tokio::spawn(async move {
                        let _ = bridge(tcp, send, recv).await;
                    });
                }
                () = tokio::time::sleep(ROOM_POLL) => {}
            }
        }
        endpoint.close().await;
        Ok::<(), String>(())
    });
    if let Err(error) = result {
        let _ = ready_sender.send(Err(error));
    }
    socket.close();
}

fn wait_for_client_ready(socket: &RoomSocket, stop: &AtomicBool) -> Result<String, String> {
    let deadline = Instant::now() + CLIENT_SIGNAL_TIMEOUT;
    loop {
        if stop.load(Ordering::Acquire) {
            return Err("native QUIC client stopped".to_owned());
        }
        if Instant::now() >= deadline {
            return Err("native QUIC signaling timed out".to_owned());
        }
        match socket.try_recv() {
            Some(Ok(RoomEvent::Ready {
                role: RoomRole::Client,
                peer_id: Some(peer_id),
                ..
            })) => return Ok(peer_id),
            Some(Ok(RoomEvent::HostLeft)) => {
                return Err("host left during QUIC negotiation".to_owned());
            }
            Some(Ok(RoomEvent::Error { error })) => return Err(error),
            Some(Err(error)) => return Err(error),
            Some(Ok(_)) | None => thread::sleep(ROOM_POLL),
        }
    }
}

async fn wait_for_remote_description(
    socket: &RoomSocket,
    peer_id: &str,
    stop: &AtomicBool,
) -> Result<String, String> {
    let deadline = Instant::now() + CLIENT_CONNECT_TIMEOUT;
    loop {
        if stop.load(Ordering::Acquire) {
            return Err("native QUIC client stopped".to_owned());
        }
        if Instant::now() >= deadline {
            return Err("direct QUIC/ICE connection timed out".to_owned());
        }
        match socket.try_recv() {
            Some(Ok(RoomEvent::Signal {
                peer_id: signal_peer,
                payload,
            })) if signal_peer == peer_id => match signal_type(&payload) {
                Some("native-quic-description") => {
                    return signal_description(&payload)
                        .map(ToOwned::to_owned)
                        .ok_or_else(|| "native QUIC endpoint closed".to_owned());
                }
                Some("native-quic-error") => {
                    return payload
                        .get("message")
                        .and_then(Value::as_str)
                        .map(ToOwned::to_owned)
                        .ok_or_else(|| "native QUIC endpoint closed".to_owned());
                }
                _ => {}
            },
            Some(Ok(RoomEvent::HostLeft)) => {
                return Err("host left during QUIC negotiation".to_owned());
            }
            Some(Ok(RoomEvent::Error { error })) => return Err(error),
            Some(Err(error)) => return Err(error),
            Some(Ok(_)) | None => tokio::time::sleep(ROOM_POLL).await,
        }
    }
}

async fn bridge(
    tcp: TcpStream,
    mut quic_send: quinn::SendStream,
    mut quic_recv: quinn::RecvStream,
) -> io::Result<()> {
    let (mut tcp_read, mut tcp_write) = tcp.into_split();
    let upload = tokio::io::copy(&mut tcp_read, &mut quic_send);
    let download = tokio::io::copy(&mut quic_recv, &mut tcp_write);
    let (upload_result, download_result) = tokio::join!(upload, download);
    upload_result?;
    download_result?;
    let _ = quic_send.finish();
    Ok(())
}

fn signal_type(payload: &Value) -> Option<&str> {
    payload.get("type")?.as_str()
}

fn signal_description(payload: &Value) -> Option<&str> {
    payload.get("description")?.as_str()
}
