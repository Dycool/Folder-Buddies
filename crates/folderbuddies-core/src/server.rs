use std::{
    collections::HashMap,
    fs::{self, File, FileTimes, OpenOptions},
    io,
    net::{IpAddr, Ipv4Addr, Ipv6Addr, Shutdown, SocketAddr, TcpListener, TcpStream},
    path::{Component, Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    thread::{self, JoinHandle},
    time::{Duration, SystemTime, UNIX_EPOCH},
};

use crossbeam_channel::{Sender, TrySendError, bounded};
use socket2::{Domain, Protocol, Socket, Type};

use crate::{
    crypto::{
        Key256, SecureReceiver, SecureSender, auth_proof, derive_session_keys, random_array,
        random_bytes, sha256,
    },
    protocol::{
        MAX_IO, Op, PROTOCOL_VERSION, Reader, WireAttr, WireStatFs, Writer, read_plain_message,
        write_plain_message,
    },
    signaling::{SECRET_BYTES, Token},
};

const LISTEN_BACKLOG: i32 = 64;
const ACCEPT_POLL: Duration = Duration::from_millis(20);
const SOCKET_TIMEOUT: Duration = Duration::from_secs(10);
const MAX_QUEUED_MESSAGES: usize = 257;
const MAX_DIRECTORY_ENTRIES: usize = 1_000_000;

const EACCES: i16 = 13;
const EBADF: i16 = 9;
const EEXIST: i16 = 17;
const EINVAL: i16 = 22;
const EIO: i16 = 5;
const ENOENT: i16 = 2;
const ENOSYS: i16 = 38;
const EROFS: i16 = 30;

const FB_O_ACCMODE: i32 = 3;
const FB_O_WRONLY: i32 = 1;
const FB_O_RDWR: i32 = 2;
const FB_O_CREAT: i32 = 0x0100;
const FB_O_EXCL: i32 = 0x0200;
const FB_O_TRUNC: i32 = 0x0400;
const FB_O_APPEND: i32 = 0x0800;

#[derive(Debug)]
struct Outgoing {
    op: u16,
    status: i16,
    request_id: u64,
    payload: Vec<u8>,
}

#[derive(Debug)]
struct OpenHandle {
    file: Arc<File>,
    path: String,
}

#[derive(Debug)]
struct DispatchResult {
    status: i16,
    payload: Vec<u8>,
    invalidations: Vec<String>,
}

impl DispatchResult {
    fn ok(payload: Vec<u8>) -> Self {
        Self {
            status: 0,
            payload,
            invalidations: Vec::new(),
        }
    }

    fn empty() -> Self {
        Self::ok(Vec::new())
    }

    fn error(status: i16) -> Self {
        Self {
            status,
            payload: Vec::new(),
            invalidations: Vec::new(),
        }
    }

    fn invalidate(mut self, path: String) -> Self {
        self.invalidations.push(path);
        self
    }
}

#[derive(Debug)]
struct ServerInner {
    root: PathBuf,
    share_name: String,
    secret: Vec<u8>,
    auth_key: Key256,
    bound_port: u16,
    allow_writes: AtomicBool,
    running: AtomicBool,
    next_connection_id: AtomicU64,
    next_handle_id: AtomicU64,
    handles: Mutex<HashMap<u64, OpenHandle>>,
    sessions: Mutex<HashMap<[u8; 16], usize>>,
    broadcasts: Mutex<HashMap<u64, Sender<Outgoing>>>,
    active_streams: Mutex<HashMap<u64, TcpStream>>,
    connection_threads: Mutex<Vec<JoinHandle<()>>>,
    bytes_out: AtomicU64,
    bytes_in: AtomicU64,
}

#[derive(Debug)]
pub struct Server {
    inner: Arc<ServerInner>,
    accept_thread: Option<JoinHandle<()>>,
}

impl Server {
    pub fn start(folder: impl AsRef<Path>, port: u16, allow_writes: bool) -> Result<Self, String> {
        let folder = folder.as_ref();
        reject_boundary_link(folder).map_err(|error| format!("cannot host folder: {error}"))?;
        let root = fs::canonicalize(folder)
            .map_err(|error| format!("cannot resolve {}: {error}", folder.display()))?;
        if !root.is_dir() {
            return Err(format!("not a directory: {}", folder.display()));
        }
        reject_boundary_link(&root).map_err(|error| format!("cannot host folder: {error}"))?;

        let share_name = root
            .file_name()
            .and_then(|name| name.to_str())
            .filter(|name| !name.is_empty())
            .unwrap_or("share")
            .to_owned();
        let secret = random_bytes(SECRET_BYTES)?;
        let auth_key = sha256(&secret);
        let listener = bind_listener(port).map_err(|error| format!("bind failed: {error}"))?;
        let bound_port = listener
            .local_addr()
            .map_err(|error| format!("getsockname failed: {error}"))?
            .port();
        listener
            .set_nonblocking(true)
            .map_err(|error| format!("failed to set listener nonblocking: {error}"))?;

        let inner = Arc::new(ServerInner {
            root,
            share_name,
            secret,
            auth_key,
            bound_port,
            allow_writes: AtomicBool::new(allow_writes),
            running: AtomicBool::new(true),
            next_connection_id: AtomicU64::new(1),
            next_handle_id: AtomicU64::new(1),
            handles: Mutex::new(HashMap::new()),
            sessions: Mutex::new(HashMap::new()),
            broadcasts: Mutex::new(HashMap::new()),
            active_streams: Mutex::new(HashMap::new()),
            connection_threads: Mutex::new(Vec::new()),
            bytes_out: AtomicU64::new(0),
            bytes_in: AtomicU64::new(0),
        });
        let accept_inner = Arc::clone(&inner);
        let accept_thread = thread::Builder::new()
            .name("folderbuddies-accept".to_owned())
            .spawn(move || accept_loop(accept_inner, listener))
            .map_err(|error| format!("failed to start accept thread: {error}"))?;

        Ok(Self {
            inner,
            accept_thread: Some(accept_thread),
        })
    }

    #[must_use]
    pub fn running(&self) -> bool {
        self.inner.running.load(Ordering::Acquire)
    }

    #[must_use]
    pub fn bound_port(&self) -> u16 {
        self.inner.bound_port
    }

    #[must_use]
    pub fn share_name(&self) -> &str {
        &self.inner.share_name
    }

    #[must_use]
    pub fn secret(&self) -> &[u8] {
        &self.inner.secret
    }

    #[must_use]
    pub fn allow_writes(&self) -> bool {
        self.inner.allow_writes.load(Ordering::Acquire)
    }

    pub fn set_allow_writes(&self, allow_writes: bool) {
        self.inner
            .allow_writes
            .store(allow_writes, Ordering::Release);
    }

    #[must_use]
    pub fn client_count(&self) -> usize {
        self.inner.sessions.lock().map_or(0, |sessions| sessions.len())
    }

    #[must_use]
    pub fn bytes_sent(&self) -> u64 {
        self.inner.bytes_out.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn bytes_received(&self) -> u64 {
        self.inner.bytes_in.load(Ordering::Relaxed)
    }

    pub fn token(&self, advertised_ip: IpAddr) -> Result<Token, String> {
        Token::new(
            advertised_ip.to_string(),
            self.bound_port(),
            self.secret().to_vec(),
            self.share_name().to_owned(),
            self.allow_writes(),
        )
    }

    pub fn stop(&mut self) {
        if !self.inner.running.swap(false, Ordering::AcqRel) {
            return;
        }
        if let Some(handle) = self.accept_thread.take() {
            let _ = handle.join();
        }

        if let Ok(streams) = self.inner.active_streams.lock() {
            for stream in streams.values() {
                let _ = stream.shutdown(Shutdown::Both);
            }
        }
        self.inner
            .broadcasts
            .lock()
            .map(|mut sessions| sessions.clear())
            .ok();

        let handles = self
            .inner
            .connection_threads
            .lock()
            .map(|mut threads| threads.drain(..).collect::<Vec<_>>())
            .unwrap_or_default();
        for handle in handles {
            let _ = handle.join();
        }
        self.inner
            .handles
            .lock()
            .map(|mut handles| handles.clear())
            .ok();
        self.inner
            .sessions
            .lock()
            .map(|mut sessions| sessions.clear())
            .ok();
        self.inner
            .active_streams
            .lock()
            .map(|mut streams| streams.clear())
            .ok();
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        self.stop();
    }
}

fn bind_listener(port: u16) -> io::Result<TcpListener> {
    if port == 0 {
        return bind_one(0);
    }
    let mut last_error = None;
    for offset in 0_u16..64 {
        let Some(candidate) = port.checked_add(offset) else {
            break;
        };
        match bind_one(candidate) {
            Ok(listener) => return Ok(listener),
            Err(error) => last_error = Some(error),
        }
    }
    Err(last_error.unwrap_or_else(|| io::Error::new(io::ErrorKind::AddrNotAvailable, "no port")))
}

fn bind_one(port: u16) -> io::Result<TcpListener> {
    match bind_socket(Domain::IPV6, SocketAddr::new(IpAddr::V6(Ipv6Addr::UNSPECIFIED), port), true)
    {
        Ok(listener) => Ok(listener),
        Err(ipv6_error) => bind_socket(
            Domain::IPV4,
            SocketAddr::new(IpAddr::V4(Ipv4Addr::UNSPECIFIED), port),
            false,
        )
        .map_err(|_| ipv6_error),
    }
}

fn bind_socket(domain: Domain, address: SocketAddr, dual_stack: bool) -> io::Result<TcpListener> {
    let socket = Socket::new(domain, Type::STREAM, Some(Protocol::TCP))?;
    socket.set_reuse_address(true)?;
    if dual_stack {
        socket.set_only_v6(false)?;
    }
    socket.bind(&address.into())?;
    socket.listen(LISTEN_BACKLOG)?;
    Ok(socket.into())
}

fn accept_loop(inner: Arc<ServerInner>, listener: TcpListener) {
    while inner.running.load(Ordering::Acquire) {
        match listener.accept() {
            Ok((stream, _)) => {
                if let Err(error) = configure_stream(&stream) {
                    eprintln!("Folder Buddies: rejected socket configuration: {error}");
                    continue;
                }
                let connection_id = inner.next_connection_id.fetch_add(1, Ordering::Relaxed);
                let active_clone = match stream.try_clone() {
                    Ok(clone) => clone,
                    Err(error) => {
                        eprintln!("Folder Buddies: socket clone failed: {error}");
                        continue;
                    }
                };
                if let Ok(mut active) = inner.active_streams.lock() {
                    active.insert(connection_id, active_clone);
                }
                let connection_inner = Arc::clone(&inner);
                match thread::Builder::new()
                    .name(format!("folderbuddies-client-{connection_id}"))
                    .spawn(move || handle_stream(connection_inner, connection_id, stream))
                {
                    Ok(handle) => {
                        if let Ok(mut threads) = inner.connection_threads.lock() {
                            threads.push(handle);
                        }
                    }
                    Err(error) => {
                        if let Ok(mut active) = inner.active_streams.lock() {
                            active.remove(&connection_id);
                        }
                        eprintln!("Folder Buddies: failed to start connection thread: {error}");
                    }
                }
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => thread::sleep(ACCEPT_POLL),
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => {
                if inner.running.load(Ordering::Acquire) {
                    eprintln!("Folder Buddies: accept failed: {error}");
                    thread::sleep(ACCEPT_POLL);
                }
            }
        }
    }
}

fn configure_stream(stream: &TcpStream) -> io::Result<()> {
    stream.set_nodelay(true)?;
    stream.set_read_timeout(Some(SOCKET_TIMEOUT))?;
    stream.set_write_timeout(Some(SOCKET_TIMEOUT))
}

fn handle_stream(inner: Arc<ServerInner>, connection_id: u64, mut stream: TcpStream) {
    let handshake = server_handshake(&inner, &mut stream);
    let Ok((client_id, sender, mut receiver)) = handshake else {
        let _ = stream.shutdown(Shutdown::Both);
        remove_active(&inner, connection_id);
        return;
    };

    add_session(&inner, client_id);
    let writer_stream = match stream.try_clone() {
        Ok(clone) => clone,
        Err(_) => {
            remove_session(&inner, client_id);
            remove_active(&inner, connection_id);
            return;
        }
    };
    let (tx, rx) = bounded::<Outgoing>(MAX_QUEUED_MESSAGES);
    if let Ok(mut broadcasts) = inner.broadcasts.lock() {
        broadcasts.insert(connection_id, tx.clone());
    }
    let writer_handle = thread::Builder::new()
        .name(format!("folderbuddies-writer-{connection_id}"))
        .spawn(move || writer_loop(writer_stream, sender, rx));
    let Ok(writer_handle) = writer_handle else {
        remove_broadcast(&inner, connection_id);
        remove_session(&inner, client_id);
        remove_active(&inner, connection_id);
        return;
    };

    while inner.running.load(Ordering::Acquire) {
        let (header, payload) = match receiver.recv(&mut stream) {
            Ok(message) => message,
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted
                ) =>
            {
                if error.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                continue;
            }
            Err(_) => break,
        };
        if header.op() < Op::GetAttr.code() {
            break;
        }
        let result = dispatch(&inner, header.op(), &payload);
        let outgoing = Outgoing {
            op: header.op(),
            status: result.status,
            request_id: header.request_id(),
            payload: result.payload,
        };
        if tx.send(outgoing).is_err() {
            break;
        }
        for path in result.invalidations {
            broadcast_invalidation(&inner, &path);
        }
    }

    let _ = stream.shutdown(Shutdown::Both);
    remove_broadcast(&inner, connection_id);
    drop(tx);
    let _ = writer_handle.join();
    remove_session(&inner, client_id);
    remove_active(&inner, connection_id);
}

fn writer_loop(
    mut stream: TcpStream,
    mut sender: SecureSender,
    receiver: crossbeam_channel::Receiver<Outgoing>,
) {
    while let Ok(message) = receiver.recv() {
        if sender
            .send(
                &mut stream,
                message.op,
                message.status,
                message.request_id,
                &message.payload,
            )
            .is_err()
        {
            break;
        }
    }
    let _ = stream.shutdown(Shutdown::Both);
}

fn server_handshake(
    inner: &ServerInner,
    stream: &mut TcpStream,
) -> Result<([u8; 16], SecureSender, SecureReceiver), String> {
    let (hello, payload) = read_plain_message(stream).map_err(|error| error.to_string())?;
    if hello.op() != Op::Hello.code() {
        return Err("expected HELLO".to_owned());
    }
    let mut reader = Reader::new(&payload);
    let version = reader.u32().map_err(|error| error.to_string())?;
    let client_id: [u8; 16] = reader
        .raw(16)
        .map_err(|error| error.to_string())?
        .try_into()
        .map_err(|_| "malformed client id".to_owned())?;
    let folder_hash: [u8; 32] = reader
        .raw(32)
        .map_err(|error| error.to_string())?
        .try_into()
        .map_err(|_| "malformed folder hash".to_owned())?;
    let nonce_client: [u8; 16] = reader
        .raw(16)
        .map_err(|error| error.to_string())?
        .try_into()
        .map_err(|_| "malformed client nonce".to_owned())?;
    if !reader.is_empty()
        || version != PROTOCOL_VERSION
        || !constant_time_equal(&folder_hash, &sha256(inner.share_name.as_bytes()))
    {
        let _ = write_plain_message(stream, Op::AuthFail.code(), 0, hello.request_id(), &[]);
        return Err("protocol version or folder mismatch".to_owned());
    }

    let nonce_server: [u8; 16] = random_array()?;
    write_plain_message(
        stream,
        Op::Challenge.code(),
        0,
        hello.request_id(),
        &nonce_server,
    )
    .map_err(|error| error.to_string())?;
    let (auth, proof) = read_plain_message(stream).map_err(|error| error.to_string())?;
    if auth.op() != Op::Auth.code() {
        return Err("expected AUTH".to_owned());
    }
    let expected = auth_proof(&inner.auth_key, &nonce_client, &nonce_server)?;
    if proof.len() != expected.len() || !constant_time_equal(&proof, &expected) {
        let _ = write_plain_message(stream, Op::AuthFail.code(), 0, auth.request_id(), &[]);
        return Err("authentication failed".to_owned());
    }
    write_plain_message(stream, Op::AuthOk.code(), 0, auth.request_id(), &[])
        .map_err(|error| error.to_string())?;
    let keys = derive_session_keys(&inner.auth_key, &nonce_client, &nonce_server, true)?;
    Ok((
        client_id,
        SecureSender::new(*keys.tx()),
        SecureReceiver::new(*keys.rx()),
    ))
}

fn dispatch(inner: &ServerInner, op: u16, payload: &[u8]) -> DispatchResult {
    match dispatch_inner(inner, op, payload) {
        Ok(result) => result,
        Err(status) => DispatchResult::error(status),
    }
}

fn dispatch_inner(inner: &ServerInner, op: u16, payload: &[u8]) -> Result<DispatchResult, i16> {
    let mut reader = Reader::new(payload);
    match op {
        value if value == Op::GetAttr.code() => {
            let path = read_path(&mut reader)?;
            require_empty(&reader)?;
            let absolute = resolve_path(&inner.root, &path)?;
            let attr = wire_attr(&absolute)?;
            let mut writer = Writer::new();
            attr.write_to(&mut writer);
            Ok(DispatchResult::ok(writer.into_inner()))
        }
        value if value == Op::ReadDir.code() => {
            let path = read_path(&mut reader)?;
            require_empty(&reader)?;
            let absolute = resolve_path(&inner.root, &path)?;
            let mut entries = Vec::new();
            let directory = fs::read_dir(&absolute).map_err(io_status)?;
            for entry in directory {
                let entry = entry.map_err(io_status)?;
                if entries.len() >= MAX_DIRECTORY_ENTRIES {
                    return Err(EIO);
                }
                if let Ok(attr) = wire_attr(&entry.path()) {
                    entries.push((entry.file_name().to_string_lossy().into_owned(), attr));
                }
            }
            let count = u32::try_from(entries.len()).map_err(|_| EIO)?;
            let mut writer = Writer::new();
            writer.u32(count);
            for (name, attr) in entries {
                writer.string(&name).map_err(|_| EIO)?;
                attr.write_to(&mut writer);
            }
            Ok(DispatchResult::ok(writer.into_inner()))
        }
        value if value == Op::Open.code() || value == Op::Create.code() => {
            let path = read_path(&mut reader)?;
            let flags = reader.i32().map_err(|_| EINVAL)?;
            let mode = reader.u32().map_err(|_| EINVAL)?;
            require_empty(&reader)?;
            let opens_for_write = value == Op::Create.code()
                || matches!(flags & FB_O_ACCMODE, FB_O_WRONLY | FB_O_RDWR)
                || flags & (FB_O_CREAT | FB_O_TRUNC | FB_O_APPEND) != 0;
            if opens_for_write && !inner.allow_writes.load(Ordering::Acquire) {
                return Err(EROFS);
            }
            let absolute = resolve_path(&inner.root, &path)?;
            let file = open_portable(&absolute, flags, mode).map_err(io_status)?;
            let handle = inner.next_handle_id.fetch_add(1, Ordering::Relaxed);
            inner
                .handles
                .lock()
                .map_err(|_| EIO)?
                .insert(
                    handle,
                    OpenHandle {
                        file: Arc::new(file),
                        path: path.clone(),
                    },
                );
            let mut writer = Writer::new();
            writer.u64(handle);
            let mut result = DispatchResult::ok(writer.into_inner());
            if value == Op::Create.code() {
                result = result.invalidate(path);
            }
            Ok(result)
        }
        value if value == Op::Read.code() => {
            let handle = reader.u64().map_err(|_| EINVAL)?;
            let offset = reader.u64().map_err(|_| EINVAL)?;
            let requested = reader.u32().map_err(|_| EINVAL)?.min(MAX_IO);
            require_empty(&reader)?;
            let open = get_handle(inner, handle)?;
            let mut data = vec![0_u8; requested as usize];
            let read = read_at(&open.file, &mut data, offset).map_err(io_status)?;
            data.truncate(read);
            inner.bytes_out.fetch_add(read as u64, Ordering::Relaxed);
            Ok(DispatchResult::ok(data))
        }
        value if value == Op::Write.code() => {
            require_writes(inner)?;
            let handle = reader.u64().map_err(|_| EINVAL)?;
            let offset = reader.u64().map_err(|_| EINVAL)?;
            let data = reader.remaining();
            if data.len() > MAX_IO as usize {
                return Err(EINVAL);
            }
            let open = get_handle(inner, handle)?;
            let written = write_at(&open.file, data, offset).map_err(io_status)?;
            inner
                .bytes_in
                .fetch_add(written as u64, Ordering::Relaxed);
            let written = u32::try_from(written).map_err(|_| EIO)?;
            let mut writer = Writer::new();
            writer.u32(written);
            Ok(DispatchResult::ok(writer.into_inner()).invalidate(open.path))
        }
        value if value == Op::Release.code() => {
            let handle = reader.u64().map_err(|_| EINVAL)?;
            require_empty(&reader)?;
            inner.handles.lock().map_err(|_| EIO)?.remove(&handle);
            Ok(DispatchResult::empty())
        }
        value if value == Op::Fsync.code() => {
            let handle = reader.u64().map_err(|_| EINVAL)?;
            require_empty(&reader)?;
            get_handle(inner, handle)?
                .file
                .sync_data()
                .map_err(io_status)?;
            Ok(DispatchResult::empty())
        }
        value if value == Op::Flush.code() => Ok(DispatchResult::empty()),
        value if value == Op::Mkdir.code() => {
            require_writes(inner)?;
            let path = read_path(&mut reader)?;
            let mode = reader.u32().map_err(|_| EINVAL)?;
            require_empty(&reader)?;
            let absolute = resolve_path(&inner.root, &path)?;
            create_directory(&absolute, mode).map_err(io_status)?;
            Ok(DispatchResult::empty().invalidate(path))
        }
        value if value == Op::Unlink.code() || value == Op::Rmdir.code() => {
            require_writes(inner)?;
            let path = read_path(&mut reader)?;
            require_empty(&reader)?;
            let absolute = resolve_path(&inner.root, &path)?;
            if value == Op::Unlink.code() {
                fs::remove_file(absolute).map_err(io_status)?;
            } else {
                fs::remove_dir(absolute).map_err(io_status)?;
            }
            Ok(DispatchResult::empty().invalidate(path))
        }
        value if value == Op::Rename.code() => {
            require_writes(inner)?;
            let from = read_path(&mut reader)?;
            let to = read_path(&mut reader)?;
            require_empty(&reader)?;
            let absolute_from = resolve_path(&inner.root, &from)?;
            let absolute_to = resolve_path(&inner.root, &to)?;
            fs::rename(absolute_from, absolute_to).map_err(io_status)?;
            Ok(DispatchResult::empty()
                .invalidate(from)
                .invalidate(to))
        }
        value if value == Op::Truncate.code() => {
            require_writes(inner)?;
            let path = read_path(&mut reader)?;
            let size = reader.u64().map_err(|_| EINVAL)?;
            require_empty(&reader)?;
            let absolute = resolve_path(&inner.root, &path)?;
            OpenOptions::new()
                .write(true)
                .open(absolute)
                .map_err(io_status)?
                .set_len(size)
                .map_err(io_status)?;
            Ok(DispatchResult::empty().invalidate(path))
        }
        value if value == Op::StatFs.code() => {
            let path = read_path(&mut reader)?;
            require_empty(&reader)?;
            let absolute = resolve_path(&inner.root, &path)?;
            if !absolute.exists() {
                return Err(ENOENT);
            }
            let stat = stat_fs(&absolute)?;
            let mut writer = Writer::new();
            stat.write_to(&mut writer);
            Ok(DispatchResult::ok(writer.into_inner()))
        }
        value if value == Op::Access.code() => {
            let path = read_path(&mut reader)?;
            let mode = reader.u32().map_err(|_| EINVAL)?;
            require_empty(&reader)?;
            if !inner.allow_writes.load(Ordering::Acquire) && mode & 2 != 0 {
                return Err(EROFS);
            }
            let absolute = resolve_path(&inner.root, &path)?;
            access_path(&absolute, mode)?;
            Ok(DispatchResult::empty())
        }
        value if value == Op::Chmod.code() => {
            require_writes(inner)?;
            let path = read_path(&mut reader)?;
            let mode = reader.u32().map_err(|_| EINVAL)?;
            require_empty(&reader)?;
            let absolute = resolve_path(&inner.root, &path)?;
            set_permissions(&absolute, mode).map_err(io_status)?;
            Ok(DispatchResult::empty().invalidate(path))
        }
        value if value == Op::Utimens.code() => {
            require_writes(inner)?;
            let path = read_path(&mut reader)?;
            let atime = reader.i64().map_err(|_| EINVAL)?;
            let mtime = reader.i64().map_err(|_| EINVAL)?;
            require_empty(&reader)?;
            let absolute = resolve_path(&inner.root, &path)?;
            set_times(&absolute, atime, mtime).map_err(io_status)?;
            Ok(DispatchResult::empty().invalidate(path))
        }
        _ => Err(ENOSYS),
    }
}

fn read_path(reader: &mut Reader<'_>) -> Result<String, i16> {
    reader.string().map_err(|_| EINVAL)
}

fn require_empty(reader: &Reader<'_>) -> Result<(), i16> {
    if reader.is_empty() {
        Ok(())
    } else {
        Err(EINVAL)
    }
}

fn require_writes(inner: &ServerInner) -> Result<(), i16> {
    if inner.allow_writes.load(Ordering::Acquire) {
        Ok(())
    } else {
        Err(EROFS)
    }
}

fn get_handle(inner: &ServerInner, handle: u64) -> Result<OpenHandle, i16> {
    let handles = inner.handles.lock().map_err(|_| EIO)?;
    let open = handles.get(&handle).ok_or(EBADF)?;
    Ok(OpenHandle {
        file: Arc::clone(&open.file),
        path: open.path.clone(),
    })
}

fn resolve_path(root: &Path, relative: &str) -> Result<PathBuf, i16> {
    let trimmed = relative.trim_start_matches(['/', '\\']);
    let mut candidate = root.to_path_buf();
    for component in Path::new(trimmed).components() {
        match component {
            Component::CurDir => {}
            Component::Normal(part) => {
                candidate.push(part);
                match fs::symlink_metadata(&candidate) {
                    Ok(metadata) => {
                        if is_link_or_reparse(&metadata) {
                            return Err(EACCES);
                        }
                        candidate = fs::canonicalize(&candidate).map_err(|_| EACCES)?;
                        if !path_within(root, &candidate) {
                            return Err(EACCES);
                        }
                    }
                    Err(error) if error.kind() == io::ErrorKind::NotFound => {}
                    Err(_) => return Err(EACCES),
                }
            }
            Component::ParentDir | Component::RootDir | Component::Prefix(_) => {
                return Err(EACCES);
            }
        }
    }
    if path_within(root, &candidate) {
        Ok(candidate)
    } else {
        Err(EACCES)
    }
}

#[cfg(not(windows))]
fn path_within(root: &Path, candidate: &Path) -> bool {
    candidate.starts_with(root)
}

#[cfg(windows)]
fn path_within(root: &Path, candidate: &Path) -> bool {
    let root = root.to_string_lossy().replace('\\', "/").to_lowercase();
    let candidate = candidate
        .to_string_lossy()
        .replace('\\', "/")
        .to_lowercase();
    candidate == root
        || candidate
            .strip_prefix(&root)
            .is_some_and(|suffix| suffix.starts_with('/'))
}

fn reject_boundary_link(path: &Path) -> io::Result<()> {
    let metadata = fs::symlink_metadata(path)?;
    if is_link_or_reparse(&metadata) {
        Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "symlink, junction, or reparse-point roots are forbidden",
        ))
    } else {
        Ok(())
    }
}

#[cfg(not(windows))]
fn is_link_or_reparse(metadata: &fs::Metadata) -> bool {
    metadata.file_type().is_symlink()
}

#[cfg(windows)]
fn is_link_or_reparse(metadata: &fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt;

    const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x0000_0400;
    metadata.file_type().is_symlink()
        || metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0
}

#[cfg(unix)]
fn wire_attr(path: &Path) -> Result<WireAttr, i16> {
    use std::os::unix::fs::MetadataExt;

    let metadata = fs::symlink_metadata(path).map_err(io_status)?;
    if metadata.file_type().is_symlink() {
        return Err(EACCES);
    }
    Ok(WireAttr {
        ino: metadata.ino(),
        size: metadata.size(),
        blocks: metadata.blocks(),
        atime: metadata.atime(),
        mtime: metadata.mtime(),
        ctime: metadata.ctime(),
        mode: metadata.mode(),
        nlink: u32::try_from(metadata.nlink()).unwrap_or(u32::MAX),
        uid: metadata.uid(),
        gid: metadata.gid(),
    })
}

#[cfg(windows)]
fn wire_attr(path: &Path) -> Result<WireAttr, i16> {
    use std::os::windows::fs::MetadataExt;

    let metadata = fs::symlink_metadata(path).map_err(io_status)?;
    if is_link_or_reparse(&metadata) {
        return Err(EACCES);
    }
    let is_dir = metadata.is_dir();
    let readonly = metadata.permissions().readonly();
    let size = metadata.file_size();
    Ok(WireAttr {
        ino: 0,
        size,
        blocks: size.div_ceil(512),
        atime: windows_time_to_unix(metadata.last_access_time()),
        mtime: windows_time_to_unix(metadata.last_write_time()),
        ctime: windows_time_to_unix(metadata.creation_time()),
        mode: if is_dir {
            0o040000 | 0o755
        } else if readonly {
            0o100000 | 0o444
        } else {
            0o100000 | 0o644
        },
        nlink: 1,
        uid: 0,
        gid: 0,
    })
}

#[cfg(windows)]
fn windows_time_to_unix(ticks: u64) -> i64 {
    let seconds = i64::try_from(ticks / 10_000_000).unwrap_or(i64::MAX);
    seconds.saturating_sub(11_644_473_600)
}

fn open_portable(path: &Path, flags: i32, mode: u32) -> io::Result<File> {
    let access = flags & FB_O_ACCMODE;
    if !matches!(access, 0 | FB_O_WRONLY | FB_O_RDWR) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "invalid portable access mode",
        ));
    }
    let mut options = OpenOptions::new();
    options.read(access == 0 || access == FB_O_RDWR);
    options.write(access == FB_O_WRONLY || access == FB_O_RDWR);
    if flags & FB_O_APPEND != 0 {
        options.append(true);
    }
    if flags & FB_O_CREAT != 0 {
        if flags & FB_O_EXCL != 0 {
            options.create_new(true);
        } else {
            options.create(true);
        }
    }
    options.truncate(flags & FB_O_TRUNC != 0);
    set_create_mode(&mut options, mode);
    options.open(path)
}

#[cfg(unix)]
fn set_create_mode(options: &mut OpenOptions, mode: u32) {
    use std::os::unix::fs::OpenOptionsExt;

    options.mode(mode);
}

#[cfg(windows)]
fn set_create_mode(_options: &mut OpenOptions, _mode: u32) {}

#[cfg(unix)]
fn read_at(file: &File, buffer: &mut [u8], offset: u64) -> io::Result<usize> {
    use std::os::unix::fs::FileExt;

    file.read_at(buffer, offset)
}

#[cfg(windows)]
fn read_at(file: &File, buffer: &mut [u8], offset: u64) -> io::Result<usize> {
    use std::os::windows::fs::FileExt;

    file.seek_read(buffer, offset)
}

#[cfg(unix)]
fn write_at(file: &File, buffer: &[u8], offset: u64) -> io::Result<usize> {
    use std::os::unix::fs::FileExt;

    file.write_at(buffer, offset)
}

#[cfg(windows)]
fn write_at(file: &File, buffer: &[u8], offset: u64) -> io::Result<usize> {
    use std::os::windows::fs::FileExt;

    file.seek_write(buffer, offset)
}

#[cfg(unix)]
fn create_directory(path: &Path, mode: u32) -> io::Result<()> {
    use std::os::unix::fs::PermissionsExt;

    fs::create_dir(path)?;
    fs::set_permissions(path, fs::Permissions::from_mode(mode))
}

#[cfg(windows)]
fn create_directory(path: &Path, _mode: u32) -> io::Result<()> {
    fs::create_dir(path)
}

#[cfg(unix)]
fn set_permissions(path: &Path, mode: u32) -> io::Result<()> {
    use std::os::unix::fs::PermissionsExt;

    fs::set_permissions(path, fs::Permissions::from_mode(mode))
}

#[cfg(windows)]
fn set_permissions(path: &Path, mode: u32) -> io::Result<()> {
    let mut permissions = fs::metadata(path)?.permissions();
    permissions.set_readonly(mode & 0o222 == 0);
    fs::set_permissions(path, permissions)
}

fn access_path(path: &Path, mode: u32) -> Result<(), i16> {
    let metadata = fs::metadata(path).map_err(io_status)?;
    if mode & 2 != 0 && metadata.permissions().readonly() {
        return Err(EACCES);
    }
    Ok(())
}

fn set_times(path: &Path, atime: i64, mtime: i64) -> io::Result<()> {
    let file = OpenOptions::new().read(true).write(true).open(path)?;
    let times = FileTimes::new()
        .set_accessed(unix_time(atime)?)
        .set_modified(unix_time(mtime)?);
    file.set_times(times)
}

fn unix_time(seconds: i64) -> io::Result<SystemTime> {
    if seconds >= 0 {
        UNIX_EPOCH
            .checked_add(Duration::from_secs(seconds.unsigned_abs()))
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "timestamp overflow"))
    } else {
        UNIX_EPOCH
            .checked_sub(Duration::from_secs(seconds.unsigned_abs()))
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "timestamp underflow"))
    }
}

fn stat_fs(_path: &Path) -> Result<WireStatFs, i16> {
    Ok(WireStatFs {
        bsize: 4096,
        frsize: 4096,
        blocks: 0,
        bfree: 0,
        bavail: 0,
        files: 0,
        ffree: 0,
        namemax: 255,
    })
}

fn io_status(error: io::Error) -> i16 {
    error
        .raw_os_error()
        .and_then(|value| i16::try_from(value).ok())
        .filter(|value| *value > 0)
        .unwrap_or_else(|| match error.kind() {
            io::ErrorKind::NotFound => ENOENT,
            io::ErrorKind::PermissionDenied => EACCES,
            io::ErrorKind::AlreadyExists => EEXIST,
            io::ErrorKind::InvalidInput | io::ErrorKind::InvalidData => EINVAL,
            _ => EIO,
        })
}

fn add_session(inner: &ServerInner, client_id: [u8; 16]) {
    if let Ok(mut sessions) = inner.sessions.lock() {
        *sessions.entry(client_id).or_insert(0) += 1;
    }
}

fn remove_session(inner: &ServerInner, client_id: [u8; 16]) {
    if let Ok(mut sessions) = inner.sessions.lock()
        && let Some(count) = sessions.get_mut(&client_id)
    {
        *count = count.saturating_sub(1);
        if *count == 0 {
            sessions.remove(&client_id);
        }
    }
}

fn remove_active(inner: &ServerInner, connection_id: u64) {
    if let Ok(mut active) = inner.active_streams.lock() {
        active.remove(&connection_id);
    }
}

fn remove_broadcast(inner: &ServerInner, connection_id: u64) {
    if let Ok(mut broadcasts) = inner.broadcasts.lock() {
        broadcasts.remove(&connection_id);
    }
}

fn broadcast_invalidation(inner: &ServerInner, path: &str) {
    let mut writer = Writer::new();
    if writer.string(path).is_err() {
        return;
    }
    let payload = writer.into_inner();
    let sessions = inner
        .broadcasts
        .lock()
        .map(|sessions| sessions.values().cloned().collect::<Vec<_>>())
        .unwrap_or_default();
    for session in sessions {
        let message = Outgoing {
            op: Op::Invalidate.code(),
            status: 0,
            request_id: 0,
            payload: payload.clone(),
        };
        match session.try_send(message) {
            Ok(()) | Err(TrySendError::Full(_)) | Err(TrySendError::Disconnected(_)) => {}
        }
    }
}

fn constant_time_equal(left: &[u8], right: &[u8]) -> bool {
    if left.len() != right.len() {
        return false;
    }
    let mut difference = 0_u8;
    for (left, right) in left.iter().zip(right) {
        difference |= left ^ right;
    }
    difference == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_parent_components() {
        let root = std::env::temp_dir();
        assert_eq!(resolve_path(&root, "../escape"), Err(EACCES));
    }

    #[test]
    fn constant_time_compare_matches_equal_bytes() {
        assert!(constant_time_equal(&[1, 2, 3], &[1, 2, 3]));
        assert!(!constant_time_equal(&[1, 2, 3], &[1, 2, 4]));
        assert!(!constant_time_equal(&[1, 2], &[1, 2, 3]));
    }
}
