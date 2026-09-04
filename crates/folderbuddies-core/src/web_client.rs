use std::{
    collections::HashMap,
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering},
    },
    thread::{self, JoinHandle},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use bytes::Bytes;
use serde_json::{Value, json};
use tokio::runtime::{Builder, Runtime};
use webrtc::{
    api::{
        APIBuilder,
        interceptor_registry::register_default_interceptors,
        media_engine::MediaEngine,
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
    protocol::{Op, Reader, Writer},
    remote_fs::{RemoteFs, RemoteFsError},
    room_signaling::{RoomEvent, RoomRole},
    room_socket::{RoomSender, RoomSocket},
    signaling::room_lookup_id,
    web_compat::extract_web_room,
    web_protocol::{WEB_CHUNK, decode_binary_frame, encode_binary_frame},
};

const CONNECT_TIMEOUT: Duration = Duration::from_secs(20);
const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const FILE_TIMEOUT: Duration = Duration::from_secs(120);
const UPLOAD_END_TIMEOUT: Duration = Duration::from_secs(60);

const EIO: i16 = 5;
const ENOENT: i16 = 2;
const EBADF: i16 = 9;
const EACCES: i16 = 13;
const EINVAL: i16 = 22;
const EROFS: i16 = 30;
const ENOSYS: i16 = 38;

const FB_O_WRONLY: i32 = 1;
const FB_O_RDWR: i32 = 2;
const FB_O_ACCMODE: i32 = 3;
const FB_O_CREAT: i32 = 0x0100;
const FB_O_TRUNC: i32 = 0x0400;
const FB_O_APPEND: i32 = 0x0800;

#[derive(Default)]
struct ReplyState {
    open: bool,
    dead: bool,
    can_write: bool,
    can_range: bool,
    peer_id: String,
    replies: HashMap<u32, Value>,
    downloads: HashMap<u32, Vec<u8>>,
}

struct Shared {
    state: Mutex<ReplyState>,
    changed: Condvar,
    channel: Mutex<Option<Arc<RTCDataChannel>>>,
    sender: RoomSender,
}

#[derive(Default)]
struct FileState {
    next_handle: u64,
    files: HashMap<u64, FileHandle>,
}

struct FileHandle {
    path: String,
    data: Vec<u8>,
    loaded: bool,
    dirty: bool,
}

pub struct WebRtcRemoteClient {
    runtime: Arc<Runtime>,
    peer_connection: Arc<RTCPeerConnection>,
    shared: Arc<Shared>,
    files: Mutex<FileState>,
    next_id: AtomicU32,
    bytes_read: AtomicU64,
    bytes_written: AtomicU64,
    stop: Arc<AtomicBool>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl std::fmt::Debug for WebRtcRemoteClient {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("WebRtcRemoteClient")
            .field("connected", &self.connected())
            .field("can_write", &self.can_write())
            .finish_non_exhaustive()
    }
}

impl WebRtcRemoteClient {
    pub fn connect(web_code_or_room: &str) -> Result<Arc<Self>, String> {
        let room = extract_web_room(web_code_or_room)
            .ok_or_else(|| "not a web-compatible room code".to_owned())?;
        let lookup = room_lookup_id(&room);
        let socket = RoomSocket::connect(&lookup, RoomRole::Client)?;
        let sender = socket.sender();
        let runtime = Arc::new(
            Builder::new_multi_thread()
                .worker_threads(2)
                .enable_all()
                .build()
                .map_err(|error| format!("failed to initialize WebRTC runtime: {error}"))?,
        );
        let peer_connection = Arc::new(runtime.block_on(new_peer_connection())?);
        let shared = Arc::new(Shared {
            state: Mutex::new(ReplyState::default()),
            changed: Condvar::new(),
            channel: Mutex::new(None),
            sender,
        });
        install_data_channel_handler(&peer_connection, Arc::clone(&shared));
        let stop = Arc::new(AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop);
        let worker_runtime = Arc::clone(&runtime);
        let worker_peer = Arc::clone(&peer_connection);
        let worker_shared = Arc::clone(&shared);
        let worker = thread::Builder::new()
            .name("folderbuddies-webrtc-client".to_owned())
            .spawn(move || {
                room_worker(
                    socket,
                    worker_runtime,
                    worker_peer,
                    worker_shared,
                    worker_stop,
                );
            })
            .map_err(|error| format!("failed to start WebRTC signaling worker: {error}"))?;

        let client = Arc::new(Self {
            runtime,
            peer_connection,
            shared,
            files: Mutex::new(FileState {
                next_handle: 1,
                files: HashMap::new(),
            }),
            next_id: AtomicU32::new(100),
            bytes_read: AtomicU64::new(0),
            bytes_written: AtomicU64::new(0),
            stop,
            worker: Mutex::new(Some(worker)),
        });
        if !client.wait_open(CONNECT_TIMEOUT) {
            client.disconnect();
            return Err("Cloudflare WebRTC compatibility connection timed out".to_owned());
        }
        if let Err(error) = client.refresh_capabilities() {
            client.disconnect();
            return Err(error.message().to_owned());
        }
        Ok(client)
    }

    #[must_use]
    pub fn can_write(&self) -> bool {
        self.shared
            .state
            .lock()
            .is_ok_and(|state| state.can_write)
    }

    fn can_range(&self) -> bool {
        self.shared
            .state
            .lock()
            .is_ok_and(|state| state.can_range)
    }

    fn wait_open(&self, timeout: Duration) -> bool {
        let Ok(state) = self.shared.state.lock() else {
            return false;
        };
        let Ok((state, _)) = self
            .shared
            .changed
            .wait_timeout_while(state, timeout, |state| !state.open && !state.dead)
        else {
            return false;
        };
        state.open
    }

    fn channel(&self) -> Result<Arc<RTCDataChannel>, RemoteFsError> {
        self.shared
            .channel
            .lock()
            .ok()
            .and_then(|channel| channel.clone())
            .ok_or_else(|| RemoteFsError::new(EIO, "WebRTC data channel is closed"))
    }

    fn send_json_wait(
        &self,
        mut value: Value,
        timeout: Duration,
    ) -> Result<Value, RemoteFsError> {
        let id = value
            .get("id")
            .and_then(Value::as_u64)
            .and_then(|id| u32::try_from(id).ok())
            .filter(|id| *id != 0)
            .unwrap_or_else(|| self.next_id.fetch_add(1, Ordering::Relaxed));
        value["id"] = Value::from(id);
        let text = serde_json::to_string(&value)
            .map_err(|error| RemoteFsError::new(EIO, error.to_string()))?;
        let channel = self.channel()?;
        self.runtime
            .block_on(channel.send_text(text))
            .map_err(|error| RemoteFsError::new(EIO, error.to_string()))?;

        let state = self
            .shared
            .state
            .lock()
            .map_err(|_| RemoteFsError::new(EIO, "WebRTC reply lock failed"))?;
        let (mut state, _) = self
            .shared
            .changed
            .wait_timeout_while(state, timeout, |state| {
                !state.replies.contains_key(&id) && !state.dead
            })
            .map_err(|_| RemoteFsError::new(EIO, "WebRTC reply wait failed"))?;
        state
            .replies
            .remove(&id)
            .ok_or_else(|| RemoteFsError::new(EIO, "WebRTC request timed out"))
    }

    fn send_download(&self, mut request: Value) -> Result<Vec<u8>, RemoteFsError> {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        request["id"] = Value::from(id);
        let text = serde_json::to_string(&request)
            .map_err(|error| RemoteFsError::new(EIO, error.to_string()))?;
        self.runtime
            .block_on(self.channel()?.send_text(text))
            .map_err(|error| RemoteFsError::new(EIO, error.to_string()))?;
        let state = self
            .shared
            .state
            .lock()
            .map_err(|_| RemoteFsError::new(EIO, "WebRTC download lock failed"))?;
        let (mut state, _) = self
            .shared
            .changed
            .wait_timeout_while(state, FILE_TIMEOUT, |state| {
                !state.replies.contains_key(&id) && !state.dead
            })
            .map_err(|_| RemoteFsError::new(EIO, "WebRTC download wait failed"))?;
        let reply = state
            .replies
            .remove(&id)
            .ok_or_else(|| RemoteFsError::new(EIO, "WebRTC download timed out"))?;
        if reply.get("t").and_then(Value::as_str) == Some("error") {
            state.downloads.remove(&id);
            return Err(RemoteFsError::new(
                EIO,
                reply
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or("WebRTC download failed"),
            ));
        }
        if reply.get("t").and_then(Value::as_str) != Some("fileEnd") {
            state.downloads.remove(&id);
            return Err(RemoteFsError::new(EIO, "WebRTC download failed"));
        }
        Ok(state.downloads.remove(&id).unwrap_or_default())
    }

    fn fetch_file(&self, path: &str) -> Result<Vec<u8>, RemoteFsError> {
        self.send_download(json!({"t": "download", "path": path}))
    }

    fn fetch_range(
        &self,
        path: &str,
        offset: u64,
        length: u32,
    ) -> Result<Vec<u8>, RemoteFsError> {
        self.send_download(json!({
            "t": "download",
            "path": path,
            "offset": offset as f64,
            "length": length as f64,
        }))
    }

    fn upload_file(&self, path: &str, data: &[u8]) -> Result<(), RemoteFsError> {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        let ready = self.send_json_wait(
            json!({
                "t": "uploadStart",
                "id": id,
                "path": path,
                "size": data.len() as f64,
            }),
            REQUEST_TIMEOUT,
        )?;
        require_web_ok(&ready)?;
        let channel = self.channel()?;
        for chunk in data.chunks(WEB_CHUNK) {
            let frame = encode_binary_frame(id, chunk);
            self.runtime
                .block_on(channel.send(&Bytes::from(frame)))
                .map_err(|error| RemoteFsError::new(EIO, error.to_string()))?;
        }
        let done = self.send_json_wait(
            json!({"t": "uploadEnd", "id": id}),
            UPLOAD_END_TIMEOUT,
        )?;
        require_web_ok(&done)
    }

    fn refresh_capabilities(&self) -> Result<(), RemoteFsError> {
        let reply = self.send_json_wait(json!({"t": "list", "path": "/"}), REQUEST_TIMEOUT)?;
        require_web_ok(&reply)?;
        if reply.get("t").and_then(Value::as_str) != Some("listResult") {
            return Err(RemoteFsError::new(
                EIO,
                "WebRTC compatibility connected, but the host did not return the folder listing",
            ));
        }
        Ok(())
    }

    fn request_web(&self, op: Op, payload: &[u8]) -> Result<Vec<u8>, RemoteFsError> {
        if !self.connected() {
            return Err(RemoteFsError::new(EIO, "WebRTC client is disconnected"));
        }
        if !self.can_write()
            && matches!(
                op,
                Op::Write
                    | Op::Create
                    | Op::Mkdir
                    | Op::Unlink
                    | Op::Rmdir
                    | Op::Rename
                    | Op::Truncate
                    | Op::Chmod
                    | Op::Utimens
            )
        {
            return Err(RemoteFsError::new(EROFS, "read-only filesystem"));
        }
        let mut reader = Reader::new(payload);
        match op {
            Op::GetAttr => {
                let path = reader.string().map_err(invalid_request)?;
                let normalized = normalize_rel(&path)
                    .ok_or_else(|| RemoteFsError::new(EACCES, "invalid path"))?;
                let mut writer = Writer::new();
                if normalized == "/" {
                    write_attr(&mut writer, "/", true, 0, unix_seconds());
                    return Ok(writer.into_inner());
                }
                let reply = self.send_json_wait(
                    json!({"t": "list", "path": parent_path(&normalized)}),
                    REQUEST_TIMEOUT,
                )?;
                require_web_ok(&reply)?;
                let wanted = basename(&normalized);
                let entry = reply
                    .get("entries")
                    .and_then(Value::as_array)
                    .and_then(|entries| {
                        entries.iter().find(|entry| {
                            entry.get("name").and_then(Value::as_str) == Some(wanted)
                        })
                    })
                    .ok_or_else(|| RemoteFsError::new(ENOENT, "not found"))?;
                write_entry_attr(&mut writer, entry);
                Ok(writer.into_inner())
            }
            Op::ReadDir => {
                let path = reader.string().map_err(invalid_request)?;
                let reply = self.send_json_wait(
                    json!({"t": "list", "path": path}),
                    REQUEST_TIMEOUT,
                )?;
                require_web_ok(&reply)?;
                let entries = reply
                    .get("entries")
                    .and_then(Value::as_array)
                    .cloned()
                    .unwrap_or_default();
                let mut writer = Writer::new();
                writer.u32(u32::try_from(entries.len()).unwrap_or(u32::MAX));
                for entry in entries {
                    let name = entry.get("name").and_then(Value::as_str).unwrap_or_default();
                    writer.string(name).map_err(invalid_request)?;
                    write_entry_attr(&mut writer, &entry);
                }
                Ok(writer.into_inner())
            }
            Op::Open | Op::Create => {
                let path = reader.string().map_err(invalid_request)?;
                let flags = reader.i32().map_err(invalid_request)?;
                let _mode = reader.u32().map_err(invalid_request)?;
                let access = flags & FB_O_ACCMODE;
                let write_intent = op == Op::Create
                    || access == FB_O_WRONLY
                    || access == FB_O_RDWR
                    || flags & (FB_O_CREAT | FB_O_TRUNC | FB_O_APPEND) != 0;
                if write_intent && !self.can_write() {
                    return Err(RemoteFsError::new(EROFS, "read-only filesystem"));
                }
                let mut start_empty = flags & FB_O_TRUNC != 0;
                if !start_empty && (op == Op::Create || flags & FB_O_CREAT != 0) {
                    let mut probe = Writer::new();
                    probe.string(&path).map_err(invalid_request)?;
                    start_empty = self
                        .request_web(Op::GetAttr, &probe.into_inner())
                        .is_err_and(|error| error.status() == ENOENT);
                }
                let mut files = self
                    .files
                    .lock()
                    .map_err(|_| RemoteFsError::new(EIO, "file handle lock failed"))?;
                let handle = files.next_handle;
                files.next_handle = files.next_handle.saturating_add(1).max(1);
                files.files.insert(
                    handle,
                    FileHandle {
                        path,
                        data: Vec::new(),
                        loaded: start_empty,
                        dirty: start_empty,
                    },
                );
                let mut writer = Writer::new();
                writer.u64(handle);
                Ok(writer.into_inner())
            }
            Op::Read => {
                let handle = reader.u64().map_err(invalid_request)?;
                let offset = reader.u64().map_err(invalid_request)?;
                let amount = reader.u32().map_err(invalid_request)?;
                let (path, loaded) = {
                    let files = self
                        .files
                        .lock()
                        .map_err(|_| RemoteFsError::new(EIO, "file handle lock failed"))?;
                    let file = files
                        .files
                        .get(&handle)
                        .ok_or_else(|| RemoteFsError::new(EBADF, "bad file handle"))?;
                    (file.path.clone(), file.loaded)
                };
                if !loaded && self.can_range() {
                    let mut data = self.fetch_range(&path, offset, amount)?;
                    data.truncate(amount as usize);
                    self.bytes_read
                        .fetch_add(data.len() as u64, Ordering::Relaxed);
                    return Ok(data);
                }
                self.ensure_loaded(handle, &path)?;
                let files = self
                    .files
                    .lock()
                    .map_err(|_| RemoteFsError::new(EIO, "file handle lock failed"))?;
                let file = files
                    .files
                    .get(&handle)
                    .ok_or_else(|| RemoteFsError::new(EBADF, "bad file handle"))?;
                let position = usize::try_from(offset)
                    .map_err(|_| RemoteFsError::new(EIO, "offset overflow"))?;
                if position >= file.data.len() {
                    return Ok(Vec::new());
                }
                let end = position
                    .saturating_add(amount as usize)
                    .min(file.data.len());
                let data = file.data[position..end].to_vec();
                self.bytes_read
                    .fetch_add(data.len() as u64, Ordering::Relaxed);
                Ok(data)
            }
            Op::Write => {
                let handle = reader.u64().map_err(invalid_request)?;
                let offset = reader.u64().map_err(invalid_request)?;
                let data = reader.remaining();
                let path = {
                    let files = self
                        .files
                        .lock()
                        .map_err(|_| RemoteFsError::new(EIO, "file handle lock failed"))?;
                    files
                        .files
                        .get(&handle)
                        .map(|file| file.path.clone())
                        .ok_or_else(|| RemoteFsError::new(EBADF, "bad file handle"))?
                };
                self.ensure_loaded(handle, &path)?;
                let position = usize::try_from(offset)
                    .map_err(|_| RemoteFsError::new(EIO, "offset overflow"))?;
                let end = position
                    .checked_add(data.len())
                    .ok_or_else(|| RemoteFsError::new(EIO, "write overflow"))?;
                let mut files = self
                    .files
                    .lock()
                    .map_err(|_| RemoteFsError::new(EIO, "file handle lock failed"))?;
                let file = files
                    .files
                    .get_mut(&handle)
                    .ok_or_else(|| RemoteFsError::new(EBADF, "bad file handle"))?;
                if file.data.len() < end {
                    file.data.resize(end, 0);
                }
                file.data[position..end].copy_from_slice(data);
                file.loaded = true;
                file.dirty = true;
                self.bytes_written
                    .fetch_add(data.len() as u64, Ordering::Relaxed);
                let mut writer = Writer::new();
                writer.u32(u32::try_from(data.len()).unwrap_or(u32::MAX));
                Ok(writer.into_inner())
            }
            Op::Release => {
                let handle = reader.u64().map_err(invalid_request)?;
                let file = self
                    .files
                    .lock()
                    .map_err(|_| RemoteFsError::new(EIO, "file handle lock failed"))?
                    .files
                    .remove(&handle);
                if let Some(file) = file
                    && file.dirty
                {
                    self.upload_file(&file.path, &file.data)?;
                }
                Ok(Vec::new())
            }
            Op::Unlink | Op::Rmdir => {
                let path = reader.string().map_err(invalid_request)?;
                let reply = self.send_json_wait(
                    json!({"t": "delete", "path": path}),
                    REQUEST_TIMEOUT,
                )?;
                require_web_ok(&reply)?;
                Ok(Vec::new())
            }
            Op::Mkdir => {
                let path = reader.string().map_err(invalid_request)?;
                let _mode = reader.u32().map_err(invalid_request)?;
                let reply = self.send_json_wait(
                    json!({"t": "mkdir", "path": path}),
                    REQUEST_TIMEOUT,
                )?;
                require_web_ok(&reply)?;
                Ok(Vec::new())
            }
            Op::Truncate => {
                let path = reader.string().map_err(invalid_request)?;
                let size = reader.u64().map_err(invalid_request)?;
                let mut data = self.fetch_file(&path)?;
                data.resize(
                    usize::try_from(size)
                        .map_err(|_| RemoteFsError::new(EIO, "truncate size overflow"))?,
                    0,
                );
                self.upload_file(&path, &data)?;
                Ok(Vec::new())
            }
            Op::Access => {
                let path = reader.string().map_err(invalid_request)?;
                let mode = reader.u32().map_err(invalid_request)?;
                if !self.can_write() && mode & 2 != 0 {
                    return Err(RemoteFsError::new(EROFS, "read-only filesystem"));
                }
                let mut writer = Writer::new();
                writer.string(&path).map_err(invalid_request)?;
                let _ = self.request_web(Op::GetAttr, &writer.into_inner())?;
                Ok(Vec::new())
            }
            Op::Flush | Op::Fsync => Ok(Vec::new()),
            Op::StatFs => {
                let mut writer = Writer::new();
                writer.u64(4096);
                writer.u64(4096);
                writer.u64(1024_u64 * 1024 * 1024);
                writer.u64(1024_u64 * 1024 * 1024);
                writer.u64(1024_u64 * 1024 * 1024);
                writer.u64(0);
                writer.u64(0);
                writer.u64(255);
                Ok(writer.into_inner())
            }
            Op::Rename | Op::Chmod | Op::Utimens => {
                Err(RemoteFsError::new(ENOSYS, "operation is not supported"))
            }
            _ => Err(RemoteFsError::new(ENOSYS, "operation is not supported")),
        }
    }

    fn ensure_loaded(&self, handle: u64, path: &str) -> Result<(), RemoteFsError> {
        let already_loaded = self
            .files
            .lock()
            .map_err(|_| RemoteFsError::new(EIO, "file handle lock failed"))?
            .files
            .get(&handle)
            .is_some_and(|file| file.loaded);
        if already_loaded {
            return Ok(());
        }
        let data = self.fetch_file(path)?;
        let mut files = self
            .files
            .lock()
            .map_err(|_| RemoteFsError::new(EIO, "file handle lock failed"))?;
        let file = files
            .files
            .get_mut(&handle)
            .ok_or_else(|| RemoteFsError::new(EBADF, "bad file handle"))?;
        if !file.loaded {
            file.data = data;
            file.loaded = true;
        }
        Ok(())
    }
}

impl RemoteFs for WebRtcRemoteClient {
    fn connected(&self) -> bool {
        self.shared
            .state
            .lock()
            .is_ok_and(|state| state.open && !state.dead)
    }

    fn disconnect(&self) {
        if self.stop.swap(true, Ordering::AcqRel) {
            return;
        }
        if let Ok(mut state) = self.shared.state.lock() {
            state.dead = true;
            state.open = false;
            self.shared.changed.notify_all();
        }
        let _ = self.runtime.block_on(self.peer_connection.close());
        if let Ok(mut worker) = self.worker.lock()
            && let Some(worker) = worker.take()
        {
            let _ = worker.join();
        }
        if let Ok(mut channel) = self.shared.channel.lock() {
            channel.take();
        }
    }

    fn bytes_read(&self) -> u64 {
        self.bytes_read.load(Ordering::Relaxed)
    }

    fn bytes_written(&self) -> u64 {
        self.bytes_written.load(Ordering::Relaxed)
    }

    fn request(&self, op: Op, payload: &[u8]) -> Result<Vec<u8>, RemoteFsError> {
        self.request_web(op, payload)
    }
}

impl Drop for WebRtcRemoteClient {
    fn drop(&mut self) {
        self.disconnect();
    }
}

async fn new_peer_connection() -> Result<RTCPeerConnection, String> {
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
    api.new_peer_connection(RTCConfiguration {
        ice_servers: vec![RTCIceServer {
            urls: vec!["stun:stun.l.google.com:19302".to_owned()],
            ..Default::default()
        }],
        ..Default::default()
    })
    .await
    .map_err(|error| error.to_string())
}

fn install_data_channel_handler(peer: &RTCPeerConnection, shared: Arc<Shared>) {
    peer.on_data_channel(Box::new(move |channel: Arc<RTCDataChannel>| {
        let shared = Arc::clone(&shared);
        Box::pin(async move {
            if let Ok(mut slot) = shared.channel.lock() {
                *slot = Some(Arc::clone(&channel));
            }
            let open_shared = Arc::clone(&shared);
            channel.on_open(Box::new(move || {
                let shared = Arc::clone(&open_shared);
                Box::pin(async move {
                    if let Ok(mut state) = shared.state.lock() {
                        state.open = true;
                        state.dead = false;
                        shared.changed.notify_all();
                    }
                })
            }));
            let close_shared = Arc::clone(&shared);
            channel.on_close(Box::new(move || {
                let shared = Arc::clone(&close_shared);
                Box::pin(async move {
                    if let Ok(mut state) = shared.state.lock() {
                        state.open = false;
                        state.dead = true;
                        shared.changed.notify_all();
                    }
                })
            }));
            let message_shared = Arc::clone(&shared);
            channel.on_message(Box::new(move |message: DataChannelMessage| {
                handle_channel_message(&message_shared, message);
                Box::pin(async {})
            }));
        })
    }));
}

fn handle_channel_message(shared: &Shared, message: DataChannelMessage) {
    if message.is_string {
        let Ok(text) = std::str::from_utf8(&message.data) else {
            return;
        };
        let Ok(value) = serde_json::from_str::<Value>(text) else {
            return;
        };
        let Some(id) = value
            .get("id")
            .and_then(Value::as_u64)
            .and_then(|id| u32::try_from(id).ok())
        else {
            return;
        };
        let Ok(mut state) = shared.state.lock() else {
            return;
        };
        if value.get("t").and_then(Value::as_str) == Some("listResult") {
            if let Some(can_write) = value.get("write").and_then(Value::as_bool) {
                state.can_write = can_write;
            }
            if let Some(can_range) = value.get("ranges").and_then(Value::as_bool) {
                state.can_range = can_range;
            }
        }
        if value.get("t").and_then(Value::as_str) == Some("fileStart") {
            return;
        }
        state.replies.insert(id, value);
        shared.changed.notify_all();
    } else if let Some((id, payload)) = decode_binary_frame(&message.data)
        && let Ok(mut state) = shared.state.lock()
    {
        state
            .downloads
            .entry(id)
            .or_default()
            .extend_from_slice(payload);
    }
}

fn room_worker(
    mut socket: RoomSocket,
    runtime: Arc<Runtime>,
    peer_connection: Arc<RTCPeerConnection>,
    shared: Arc<Shared>,
    stop: Arc<AtomicBool>,
) {
    while !stop.load(Ordering::Acquire) {
        match socket.try_recv() {
            Some(Ok(RoomEvent::Ready { peer_id, .. })) => {
                if let Some(peer_id) = peer_id {
                    if let Ok(mut state) = shared.state.lock() {
                        state.peer_id.clone_from(&peer_id);
                    }
                    let _ = shared
                        .sender
                        .send_signal(&peer_id, &json!({"type": "compat-hello"}));
                }
            }
            Some(Ok(RoomEvent::HostJoined)) => {
                let peer_id = shared
                    .state
                    .lock()
                    .map_or_else(|_| String::new(), |state| state.peer_id.clone());
                if !peer_id.is_empty() {
                    let _ = shared
                        .sender
                        .send_signal(&peer_id, &json!({"type": "compat-hello"}));
                }
            }
            Some(Ok(RoomEvent::HostLeft)) => {
                mark_dead(&shared);
            }
            Some(Ok(RoomEvent::Signal { peer_id, payload })) => {
                match payload.get("type").and_then(Value::as_str) {
                    Some("offer") => {
                        let sdp = payload
                            .get("sdp")
                            .and_then(Value::as_object)
                            .and_then(|sdp| sdp.get("sdp"))
                            .and_then(Value::as_str)
                            .map(str::to_owned);
                        if let Some(sdp) = sdp {
                            let peer = Arc::clone(&peer_connection);
                            let sender = shared.sender.clone();
                            runtime.spawn(async move {
                                if let Ok(offer) = RTCSessionDescription::offer(sdp)
                                    && peer.set_remote_description(offer).await.is_ok()
                                    && let Ok(answer) = peer.create_answer(None).await
                                {
                                    let mut gather = peer.gathering_complete_promise().await;
                                    if peer.set_local_description(answer).await.is_ok() {
                                        let _ = gather.recv().await;
                                        if let Some(local) = peer.local_description().await {
                                            let _ = sender.send_signal(
                                                &peer_id,
                                                &json!({
                                                    "type": "answer",
                                                    "sdp": {"type": "answer", "sdp": local.sdp},
                                                }),
                                            );
                                        }
                                    }
                                }
                            });
                        }
                    }
                    Some("candidate") => {
                        if let Some(candidate) = candidate_from_signal(&payload) {
                            let peer = Arc::clone(&peer_connection);
                            runtime.spawn(async move {
                                let _ = peer.add_ice_candidate(candidate).await;
                            });
                        }
                    }
                    _ => {}
                }
            }
            Some(Ok(RoomEvent::Error { error })) => {
                eprintln!("Folder Buddies: WebRTC signaling error: {error}");
                mark_dead(&shared);
            }
            Some(Err(error)) => {
                eprintln!("Folder Buddies: WebRTC signaling failed: {error}");
                mark_dead(&shared);
                break;
            }
            Some(Ok(_)) | None => thread::sleep(Duration::from_millis(20)),
        }
    }
    socket.close();
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

fn mark_dead(shared: &Shared) {
    if let Ok(mut state) = shared.state.lock() {
        state.dead = true;
        state.open = false;
        shared.changed.notify_all();
    }
}

fn require_web_ok(reply: &Value) -> Result<(), RemoteFsError> {
    if reply.get("t").and_then(Value::as_str) == Some("error") {
        return Err(RemoteFsError::new(
            EIO,
            reply
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("WebRTC compatibility request failed"),
        ));
    }
    Ok(())
}

fn invalid_request(error: std::io::Error) -> RemoteFsError {
    RemoteFsError::new(EINVAL, error.to_string())
}

fn normalize_rel(path: &str) -> Option<String> {
    let replaced = path.replace('\\', "/");
    let mut components = Vec::new();
    for part in replaced.split('/') {
        match part {
            "" | "." => {}
            ".." => return None,
            part => components.push(part),
        }
    }
    Some(if components.is_empty() {
        "/".to_owned()
    } else {
        format!("/{}", components.join("/"))
    })
}

fn parent_path(path: &str) -> String {
    let trimmed = path.trim_end_matches('/');
    match trimmed.rsplit_once('/') {
        Some(("", _)) | None => "/".to_owned(),
        Some((parent, _)) => parent.to_owned(),
    }
}

fn basename(path: &str) -> &str {
    path.trim_end_matches('/')
        .rsplit('/')
        .next()
        .unwrap_or_default()
}

fn write_entry_attr(writer: &mut Writer, entry: &Value) {
    let path = entry
        .get("path")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let directory = entry.get("kind").and_then(Value::as_str) == Some("directory");
    let size = entry
        .get("size")
        .and_then(Value::as_f64)
        .unwrap_or_default()
        .max(0.0) as u64;
    let raw_mtime = entry
        .get("mtime")
        .and_then(Value::as_f64)
        .unwrap_or_default();
    let mtime = if raw_mtime > 100_000_000_000.0 {
        (raw_mtime / 1000.0) as i64
    } else if raw_mtime > 0.0 {
        raw_mtime as i64
    } else {
        unix_seconds()
    };
    write_attr(writer, path, directory, size, mtime);
}

fn write_attr(writer: &mut Writer, path: &str, directory: bool, size: u64, mtime: i64) {
    writer.u64(if path == "/" {
        1
    } else {
        fnv1a(path.as_bytes())
    });
    writer.u64(size);
    writer.u64(size.saturating_add(511) / 512);
    writer.i64(mtime);
    writer.i64(mtime);
    writer.i64(mtime);
    writer.u32(if directory { 0o040755 } else { 0o100644 });
    writer.u32(if directory { 2 } else { 1 });
    writer.u32(0);
    writer.u32(0);
}

fn fnv1a(bytes: &[u8]) -> u64 {
    let mut hash = 1_469_598_103_934_665_603_u64;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(1_099_511_628_211);
    }
    hash.max(1)
}

fn unix_seconds() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| {
            i64::try_from(duration.as_secs()).unwrap_or(i64::MAX)
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn portable_write_flags_match_cpp() {
        assert_eq!(FB_O_ACCMODE, 3);
        assert_eq!(FB_O_CREAT, 0x0100);
        assert_eq!(FB_O_TRUNC, 0x0400);
        assert_eq!(FB_O_APPEND, 0x0800);
    }

    #[test]
    fn web_inode_hash_matches_cpp_fnv1a() {
        assert_eq!(fnv1a(b"/"), 4_953_208_436_630_043_972);
        assert_ne!(fnv1a(b"/file"), 0);
    }

    #[test]
    fn path_normalization_rejects_parent_escape() {
        assert_eq!(normalize_rel("/a/./b"), Some("/a/b".to_owned()));
        assert_eq!(normalize_rel("/a/../b"), None);
    }
}
