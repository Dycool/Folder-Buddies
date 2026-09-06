use std::{
    collections::HashMap,
    net::Ipv4Addr,
    path::Path,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

use bytes::Bytes;
use serde_json::{Value, json};
use tokio::{
    io,
    net::TcpStream,
    runtime::{Builder, Runtime},
    sync::mpsc,
};
use webrtc::{
    api::{
        APIBuilder, interceptor_registry::register_default_interceptors, media_engine::MediaEngine,
    },
    data_channel::{RTCDataChannel, data_channel_message::DataChannelMessage},
    ice_transport::{ice_candidate::RTCIceCandidateInit, ice_server::RTCIceServer},
    interceptor::registry::Registry,
    peer_connection::{
        RTCPeerConnection, configuration::RTCConfiguration,
        sdp::session_description::RTCSessionDescription,
    },
};

use crate::{
    native_quic::{NativeQuicEndpoint, NativeQuicRole},
    room_signaling::{RoomEvent, RoomRole},
    room_socket::{RoomSender, RoomSocket},
    signaling::{looks_like_room_code, room_lookup_id},
    web_protocol::{WebOutbound, WebProtocolHost},
};

const ROOM_POLL: Duration = Duration::from_millis(20);
const HOST_READY_TIMEOUT: Duration = Duration::from_secs(12);
const NATIVE_DESCRIPTION_TIMEOUT: Duration = Duration::from_secs(20);
const BROWSER_CLASSIFY_DELAY: Duration = Duration::from_millis(300);

#[derive(Debug)]
enum NativeCommand {
    Description(String),
    Stop,
}

#[derive(Clone)]
struct HostContext {
    sender: RoomSender,
    runtime: Arc<Runtime>,
    stop: Arc<AtomicBool>,
    protocol: Arc<WebProtocolHost>,
    native_port: u16,
    browser_peers: Arc<Mutex<HashMap<String, Arc<RTCPeerConnection>>>>,
    browser_clients: Arc<AtomicUsize>,
    bytes_out: Arc<AtomicU64>,
    bytes_in: Arc<AtomicU64>,
}

pub struct CompatRoomHost {
    runtime: Arc<Runtime>,
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
    browser_clients: Arc<AtomicUsize>,
    bytes_out: Arc<AtomicU64>,
    bytes_in: Arc<AtomicU64>,
}

impl std::fmt::Debug for CompatRoomHost {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("CompatRoomHost")
            .field("browser_clients", &self.browser_client_count())
            .finish_non_exhaustive()
    }
}

impl CompatRoomHost {
    pub fn start(
        root: impl AsRef<Path>,
        room_code: &str,
        allow_writes: bool,
        native_port: u16,
    ) -> Result<Self, String> {
        if !looks_like_room_code(room_code) {
            return Err("invalid room code for WebRTC compatibility".to_owned());
        }
        let protocol = Arc::new(WebProtocolHost::new(root, allow_writes)?);
        let lookup = room_lookup_id(room_code);
        let mut socket = RoomSocket::connect(&lookup, RoomRole::Host)?;
        wait_host_ready(&socket)?;
        let runtime = Arc::new(
            Builder::new_multi_thread()
                .worker_threads(2)
                .enable_all()
                .build()
                .map_err(|error| format!("failed to initialize compatibility runtime: {error}"))?,
        );
        let stop = Arc::new(AtomicBool::new(false));
        let browser_clients = Arc::new(AtomicUsize::new(0));
        let bytes_out = Arc::new(AtomicU64::new(0));
        let bytes_in = Arc::new(AtomicU64::new(0));
        let context = HostContext {
            sender: socket.sender(),
            runtime: Arc::clone(&runtime),
            stop: Arc::clone(&stop),
            protocol,
            native_port,
            browser_peers: Arc::new(Mutex::new(HashMap::new())),
            browser_clients: Arc::clone(&browser_clients),
            bytes_out: Arc::clone(&bytes_out),
            bytes_in: Arc::clone(&bytes_in),
        };

        let worker = thread::Builder::new()
            .name("folderbuddies-compat-host".to_owned())
            .spawn(move || host_loop(&mut socket, context))
            .map_err(|error| format!("failed to start compatibility host: {error}"))?;

        Ok(Self {
            runtime,
            stop,
            worker: Some(worker),
            browser_clients,
            bytes_out,
            bytes_in,
        })
    }

    #[must_use]
    pub fn running(&self) -> bool {
        !self.stop.load(Ordering::Acquire)
    }

    #[must_use]
    pub fn browser_client_count(&self) -> usize {
        self.browser_clients.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn bytes_out(&self) -> u64 {
        self.bytes_out.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn bytes_in(&self) -> u64 {
        self.bytes_in.load(Ordering::Relaxed)
    }

    pub fn stop(&mut self) {
        if self.stop.swap(true, Ordering::AcqRel) {
            return;
        }
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

impl Drop for CompatRoomHost {
    fn drop(&mut self) {
        self.stop();
        let _ = &self.runtime;
    }
}

fn wait_host_ready(socket: &RoomSocket) -> Result<(), String> {
    let deadline = Instant::now() + HOST_READY_TIMEOUT;
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(
                "Cloudflare WebRTC compatibility signaling did not become ready".to_owned(),
            );
        }
        match socket.recv_timeout(remaining.min(Duration::from_millis(250))) {
            Ok(RoomEvent::Ready {
                role: RoomRole::Host,
                ..
            }) => return Ok(()),
            Ok(RoomEvent::Error { error }) => return Err(error),
            Ok(_) => {}
            Err(error) if error == "room WebSocket event timed out" => {}
            Err(error) => return Err(error),
        }
    }
}

fn host_loop(socket: &mut RoomSocket, context: HostContext) {
    let mut pending_browser = HashMap::<String, Instant>::new();
    let mut native_peers = HashMap::<String, mpsc::UnboundedSender<NativeCommand>>::new();
    while !context.stop.load(Ordering::Acquire) {
        match socket.try_recv() {
            Some(Ok(RoomEvent::ClientJoined { peer_id })) => {
                pending_browser.insert(peer_id, Instant::now() + BROWSER_CLASSIFY_DELAY);
            }
            Some(Ok(RoomEvent::ClientLeft { peer_id })) => {
                pending_browser.remove(&peer_id);
                if let Some(commands) = native_peers.remove(&peer_id) {
                    let _ = commands.send(NativeCommand::Stop);
                }
                remove_browser_peer(&context, &peer_id);
            }
            Some(Ok(RoomEvent::Signal { peer_id, payload })) => {
                match payload.get("type").and_then(Value::as_str) {
                    Some("native-quic-hello") => {
                        pending_browser.remove(&peer_id);
                        if !native_peers.contains_key(&peer_id) {
                            let (commands, receiver) = mpsc::unbounded_channel();
                            native_peers.insert(peer_id.clone(), commands);
                            let sender = context.sender.clone();
                            let stop = Arc::clone(&context.stop);
                            let native_port = context.native_port;
                            context.runtime.spawn(run_native_peer(
                                sender,
                                peer_id,
                                native_port,
                                receiver,
                                stop,
                            ));
                        }
                    }
                    Some("native-quic-description") => {
                        if let Some(description) = payload
                            .get("description")
                            .and_then(Value::as_str)
                            .map(str::to_owned)
                            && let Some(commands) = native_peers.get(&peer_id)
                        {
                            let _ = commands.send(NativeCommand::Description(description));
                        }
                    }
                    Some("compat-hello") => {
                        pending_browser.remove(&peer_id);
                        ensure_browser_peer(&context, &peer_id);
                    }
                    Some("answer") => {
                        if let Some(peer) = browser_peer(&context, &peer_id)
                            && let Some(sdp) = payload
                                .get("sdp")
                                .and_then(Value::as_object)
                                .and_then(|sdp| sdp.get("sdp"))
                                .and_then(Value::as_str)
                                .map(str::to_owned)
                        {
                            context.runtime.spawn(async move {
                                if let Ok(answer) = RTCSessionDescription::answer(sdp) {
                                    let _ = peer.set_remote_description(answer).await;
                                }
                            });
                        }
                    }
                    Some("candidate") => {
                        if let Some(peer) = browser_peer(&context, &peer_id)
                            && let Some(candidate) = candidate_from_signal(&payload)
                        {
                            context.runtime.spawn(async move {
                                let _ = peer.add_ice_candidate(candidate).await;
                            });
                        }
                    }
                    _ => {}
                }
            }
            Some(Ok(RoomEvent::Error { error })) => {
                eprintln!("Folder Buddies: compatibility signaling error: {error}");
            }
            Some(Err(error)) => {
                eprintln!("Folder Buddies: compatibility signaling failed: {error}");
                break;
            }
            Some(Ok(_)) | None => {}
        }

        let now = Instant::now();
        let ready: Vec<String> = pending_browser
            .iter()
            .filter(|(_, deadline)| **deadline <= now)
            .map(|(peer, _)| peer.clone())
            .collect();
        for peer_id in ready {
            pending_browser.remove(&peer_id);
            if !native_peers.contains_key(&peer_id) {
                ensure_browser_peer(&context, &peer_id);
            }
        }
        thread::sleep(ROOM_POLL);
    }

    for (_, commands) in native_peers {
        let _ = commands.send(NativeCommand::Stop);
    }
    let peers = context.browser_peers.lock().map_or_else(
        |_| Vec::new(),
        |mut peers| peers.drain().map(|(_, peer)| peer).collect(),
    );
    context.browser_clients.store(0, Ordering::Relaxed);
    for peer in peers {
        context.runtime.spawn(async move {
            let _ = peer.close().await;
        });
    }
    socket.close();
    context.stop.store(true, Ordering::Release);
}

fn browser_peer(context: &HostContext, peer_id: &str) -> Option<Arc<RTCPeerConnection>> {
    context
        .browser_peers
        .lock()
        .ok()
        .and_then(|peers| peers.get(peer_id).cloned())
}

fn remove_browser_peer(context: &HostContext, peer_id: &str) {
    let peer = context
        .browser_peers
        .lock()
        .ok()
        .and_then(|mut peers| peers.remove(peer_id));
    if let Some(peer) = peer {
        context.browser_clients.fetch_sub(1, Ordering::Relaxed);
        context.runtime.spawn(async move {
            let _ = peer.close().await;
        });
    }
}

fn ensure_browser_peer(context: &HostContext, peer_id: &str) {
    if context
        .browser_peers
        .lock()
        .is_ok_and(|peers| peers.contains_key(peer_id))
    {
        return;
    }
    let context = context.clone();
    let peer_id = peer_id.to_owned();
    context.runtime.clone().spawn(async move {
        let peer = match create_browser_peer(&context, &peer_id).await {
            Ok(peer) => peer,
            Err(error) => {
                eprintln!("Folder Buddies: WebRTC peer failed: {error}");
                return;
            }
        };
        let inserted = context.browser_peers.lock().is_ok_and(|mut peers| {
            if peers.contains_key(&peer_id) {
                false
            } else {
                peers.insert(peer_id.clone(), Arc::clone(&peer));
                true
            }
        });
        if !inserted {
            let _ = peer.close().await;
            return;
        }
        context.browser_clients.fetch_add(1, Ordering::Relaxed);
        if let Err(error) = send_offer(&context.sender, &peer_id, &peer).await {
            eprintln!("Folder Buddies: WebRTC offer failed: {error}");
        }
    });
}

async fn create_browser_peer(
    context: &HostContext,
    peer_id: &str,
) -> Result<Arc<RTCPeerConnection>, String> {
    let mut media = MediaEngine::default();
    media
        .register_default_codecs()
        .map_err(|error| error.to_string())?;
    let registry = register_default_interceptors(Registry::new(), &mut media)
        .map_err(|error| error.to_string())?;
    let api = APIBuilder::new()
        .with_media_engine(media)
        .with_interceptor_registry(registry)
        .build();
    let peer = Arc::new(
        api.new_peer_connection(RTCConfiguration {
            ice_servers: vec![RTCIceServer {
                urls: vec!["stun:stun.l.google.com:19302".to_owned()],
                ..Default::default()
            }],
            ..Default::default()
        })
        .await
        .map_err(|error| error.to_string())?,
    );
    let channel = peer
        .create_data_channel("folderbuddies-files", None)
        .await
        .map_err(|error| error.to_string())?;
    let message_protocol = Arc::clone(&context.protocol);
    let message_channel = Arc::clone(&channel);
    let message_out = Arc::clone(&context.bytes_out);
    let message_in = Arc::clone(&context.bytes_in);
    channel.on_message(Box::new(move |message: DataChannelMessage| {
        let protocol = Arc::clone(&message_protocol);
        let channel = Arc::clone(&message_channel);
        let bytes_out = Arc::clone(&message_out);
        let bytes_in = Arc::clone(&message_in);
        Box::pin(async move {
            if message.is_string {
                if let Ok(text) = std::str::from_utf8(&message.data) {
                    let outbound = protocol.handle_text(text);
                    send_web_outbound(&channel, outbound, &bytes_out).await;
                }
            } else {
                let received = protocol.handle_binary(&message.data);
                bytes_in.fetch_add(received, Ordering::Relaxed);
            }
        })
    }));
    let close_peers = Arc::clone(&context.browser_peers);
    let close_clients = Arc::clone(&context.browser_clients);
    let close_peer_id = peer_id.to_owned();
    channel.on_close(Box::new(move || {
        let peers = Arc::clone(&close_peers);
        let clients = Arc::clone(&close_clients);
        let peer_id = close_peer_id.clone();
        Box::pin(async move {
            let removed = peers
                .lock()
                .is_ok_and(|mut peers| peers.remove(&peer_id).is_some());
            if removed {
                clients.fetch_sub(1, Ordering::Relaxed);
            }
        })
    }));
    Ok(peer)
}

async fn send_offer(
    sender: &RoomSender,
    peer_id: &str,
    peer: &RTCPeerConnection,
) -> Result<(), String> {
    let offer = peer
        .create_offer(None)
        .await
        .map_err(|error| error.to_string())?;
    let mut gathering = peer.gathering_complete_promise().await;
    peer.set_local_description(offer)
        .await
        .map_err(|error| error.to_string())?;
    let _ = gathering.recv().await;
    let local = peer
        .local_description()
        .await
        .ok_or_else(|| "WebRTC local offer is unavailable".to_owned())?;
    sender.send_signal(
        peer_id,
        &json!({
            "type": "offer",
            "sdp": {"type": "offer", "sdp": local.sdp},
        }),
    )
}

async fn send_web_outbound(
    channel: &RTCDataChannel,
    outbound: Vec<WebOutbound>,
    bytes_out: &AtomicU64,
) {
    for message in outbound {
        match message {
            WebOutbound::Text(text) => {
                let _ = channel.send_text(text).await;
            }
            WebOutbound::Binary(bytes) => {
                bytes_out.fetch_add(bytes.len().saturating_sub(8) as u64, Ordering::Relaxed);
                let _ = channel.send(&Bytes::from(bytes)).await;
            }
        }
    }
}

fn candidate_from_signal(payload: &Value) -> Option<RTCIceCandidateInit> {
    let candidate = payload.get("candidate")?.as_object()?;
    Some(RTCIceCandidateInit {
        candidate: candidate.get("candidate")?.as_str()?.to_owned(),
        sdp_mid: candidate
            .get("sdpMid")
            .and_then(Value::as_str)
            .map(str::to_owned),
        sdp_mline_index: candidate
            .get("sdpMLineIndex")
            .and_then(Value::as_u64)
            .and_then(|value| u16::try_from(value).ok())
            .or(Some(0)),
        username_fragment: None,
    })
}

async fn run_native_peer(
    sender: RoomSender,
    peer_id: String,
    native_port: u16,
    mut commands: mpsc::UnboundedReceiver<NativeCommand>,
    stop: Arc<AtomicBool>,
) {
    let mut endpoint = match NativeQuicEndpoint::start(NativeQuicRole::Server).await {
        Ok(endpoint) => endpoint,
        Err(error) => {
            send_quic_error(&sender, &peer_id, &error);
            return;
        }
    };
    if sender
        .send_signal(
            &peer_id,
            &json!({
                "type": "native-quic-description",
                "description": endpoint.local_description(),
            }),
        )
        .is_err()
    {
        endpoint.close().await;
        return;
    }
    let description = match receive_native_description(&mut commands, &stop).await {
        Ok(description) => description,
        Err(error) => {
            send_quic_error(&sender, &peer_id, &error);
            endpoint.close().await;
            return;
        }
    };
    if let Err(error) = endpoint.set_remote_description(&description).await {
        send_quic_error(&sender, &peer_id, &error);
        endpoint.close().await;
        return;
    }
    loop {
        if stop.load(Ordering::Acquire) {
            break;
        }
        tokio::select! {
            command = commands.recv() => {
                match command {
                    Some(NativeCommand::Stop) | None => break,
                    Some(NativeCommand::Description(_)) => {}
                }
            }
            stream = endpoint.accept_stream() => {
                let Ok((send, recv)) = stream else {
                    break;
                };
                tokio::spawn(async move {
                    if let Ok(tcp) = TcpStream::connect((Ipv4Addr::LOCALHOST, native_port)).await {
                        let _ = bridge(tcp, send, recv).await;
                    }
                });
            }
        }
    }
    endpoint.close().await;
}

async fn receive_native_description(
    commands: &mut mpsc::UnboundedReceiver<NativeCommand>,
    stop: &AtomicBool,
) -> Result<String, String> {
    let deadline = Instant::now() + NATIVE_DESCRIPTION_TIMEOUT;
    loop {
        if stop.load(Ordering::Acquire) {
            return Err("native QUIC host stopped".to_owned());
        }
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err("native QUIC remote description timed out".to_owned());
        }
        match tokio::time::timeout(remaining.min(Duration::from_millis(250)), commands.recv()).await
        {
            Ok(Some(NativeCommand::Description(description))) => return Ok(description),
            Ok(Some(NativeCommand::Stop)) | Ok(None) => {
                return Err("native QUIC peer disconnected".to_owned());
            }
            Err(_) => {}
        }
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

async fn bridge(
    tcp: TcpStream,
    mut quic_send: quinn::SendStream,
    mut quic_recv: quinn::RecvStream,
) -> io::Result<()> {
    let (mut tcp_read, mut tcp_write) = tcp.into_split();
    let to_quic = async {
        io::copy(&mut tcp_read, &mut quic_send).await?;
        quic_send
            .finish()
            .map_err(|error| io::Error::other(error.to_string()))?;
        Ok::<(), io::Error>(())
    };
    let from_quic = async {
        io::copy(&mut quic_recv, &mut tcp_write).await?;
        Ok::<(), io::Error>(())
    };
    tokio::try_join!(to_quic, from_quic)?;
    Ok(())
}
