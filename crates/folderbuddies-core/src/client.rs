use std::{
    fmt,
    io,
    net::{IpAddr, Shutdown, SocketAddr, TcpStream},
    sync::{
        Mutex,
        atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
    },
    time::Duration,
};

use crate::{
    crypto::{
        SecureReceiver, SecureSender, auth_proof, derive_session_keys, random_array, sha256,
    },
    protocol::{
        DEFAULT_CONNECTIONS, MAX_IO, Op, PROTOCOL_VERSION, Reader, WireAttr, WireStatFs, Writer,
        read_plain_message, write_plain_message,
    },
    signaling::Token,
};

const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const REQUEST_TIMEOUT: Duration = Duration::from_secs(10);
const EIO: i16 = 5;
const MAX_DIRECTORY_ENTRIES: u32 = 1_000_000;
const MAX_QUEUED_INVALIDATIONS: usize = 1024;

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct RemoteError {
    status: i16,
    message: String,
}

impl RemoteError {
    #[must_use]
    pub const fn status(&self) -> i16 {
        self.status
    }

    #[must_use]
    pub fn message(&self) -> &str {
        &self.message
    }

    fn remote(status: i16) -> Self {
        Self {
            status,
            message: format!("remote filesystem error {status}"),
        }
    }

    fn protocol(message: impl Into<String>) -> Self {
        Self {
            status: EIO,
            message: message.into(),
        }
    }

    fn io(error: io::Error) -> Self {
        let status = error
            .raw_os_error()
            .and_then(|value| i16::try_from(value).ok())
            .filter(|value| *value > 0)
            .unwrap_or(EIO);
        Self {
            status,
            message: error.to_string(),
        }
    }
}

impl fmt::Display for RemoteError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{} (status {})", self.message, self.status)
    }
}

impl std::error::Error for RemoteError {}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DirEntry {
    name: String,
    attr: WireAttr,
}

impl DirEntry {
    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    #[must_use]
    pub const fn attr(&self) -> &WireAttr {
        &self.attr
    }
}

#[derive(Debug)]
struct Connection {
    stream: TcpStream,
    sender: SecureSender,
    receiver: SecureReceiver,
    next_request_id: u64,
}

#[derive(Debug)]
pub struct Client {
    connections: Vec<Mutex<Connection>>,
    round_robin: AtomicUsize,
    connected: AtomicBool,
    bytes_read: AtomicU64,
    bytes_written: AtomicU64,
    invalidations: Mutex<Vec<String>>,
}

impl Client {
    pub fn connect(token: &Token, connections: usize) -> Result<Self, String> {
        let count = if connections == 0 {
            DEFAULT_CONNECTIONS
        } else {
            connections
        };
        let client_id: [u8; 16] = random_array()?;
        let mut established = Vec::with_capacity(count);
        for _ in 0..count {
            established.push(Mutex::new(connect_one(token, &client_id)?));
        }
        Ok(Self {
            connections: established,
            round_robin: AtomicUsize::new(0),
            connected: AtomicBool::new(true),
            bytes_read: AtomicU64::new(0),
            bytes_written: AtomicU64::new(0),
            invalidations: Mutex::new(Vec::new()),
        })
    }

    pub fn connect_default(token: &Token) -> Result<Self, String> {
        Self::connect(token, DEFAULT_CONNECTIONS)
    }

    #[must_use]
    pub fn connected(&self) -> bool {
        self.connected.load(Ordering::Acquire)
    }

    #[must_use]
    pub fn bytes_read(&self) -> u64 {
        self.bytes_read.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn bytes_written(&self) -> u64 {
        self.bytes_written.load(Ordering::Relaxed)
    }

    pub fn disconnect(&self) {
        if !self.connected.swap(false, Ordering::AcqRel) {
            return;
        }
        for connection in &self.connections {
            if let Ok(connection) = connection.lock() {
                let _ = connection.stream.shutdown(Shutdown::Both);
            }
        }
    }

    #[must_use]
    pub fn take_invalidations(&self) -> Vec<String> {
        match self.invalidations.lock() {
            Ok(mut queue) => std::mem::take(&mut *queue),
            Err(_) => Vec::new(),
        }
    }

    pub fn request(&self, op: Op, payload: &[u8]) -> Result<Vec<u8>, RemoteError> {
        if !self.connected() || self.connections.is_empty() {
            return Err(RemoteError::protocol("client is disconnected"));
        }

        let index = self.round_robin.fetch_add(1, Ordering::Relaxed) % self.connections.len();
        let mut connection = self.connections[index]
            .lock()
            .map_err(|_| RemoteError::protocol("connection lock poisoned"))?;
        let Connection {
            stream,
            sender,
            receiver,
            next_request_id,
        } = &mut *connection;
        let request_id = *next_request_id;
        *next_request_id = next_request_id
            .checked_add(1)
            .ok_or_else(|| RemoteError::protocol("request id exhausted"))?;

        if let Err(error) = sender.send(stream, op.code(), 0, request_id, payload) {
            self.connected.store(false, Ordering::Release);
            return Err(RemoteError::io(error));
        }
        self.bytes_written.fetch_add(
            u64::try_from(payload.len()).unwrap_or(u64::MAX),
            Ordering::Relaxed,
        );

        loop {
            let (header, response) = match receiver.recv(stream) {
                Ok(message) => message,
                Err(error) => {
                    self.connected.store(false, Ordering::Release);
                    return Err(RemoteError::io(error));
                }
            };
            if header.op() == Op::Invalidate.code() {
                self.record_invalidation(&response);
                continue;
            }
            if header.request_id() != request_id {
                return Err(RemoteError::protocol("response request id mismatch"));
            }
            self.bytes_read.fetch_add(
                u64::try_from(response.len()).unwrap_or(u64::MAX),
                Ordering::Relaxed,
            );
            if header.status() != 0 {
                return Err(RemoteError::remote(header.status()));
            }
            return Ok(response);
        }
    }

    pub fn get_attr(&self, path: &str) -> Result<WireAttr, RemoteError> {
        let payload = path_payload(path)?;
        let response = self.request(Op::GetAttr, &payload)?;
        let mut reader = Reader::new(&response);
        let attr = WireAttr::read_from(&mut reader).map_err(RemoteError::io)?;
        require_empty(&reader)?;
        Ok(attr)
    }

    pub fn read_dir(&self, path: &str) -> Result<Vec<DirEntry>, RemoteError> {
        let payload = path_payload(path)?;
        let response = self.request(Op::ReadDir, &payload)?;
        let mut reader = Reader::new(&response);
        let count = reader.u32().map_err(RemoteError::io)?;
        if count > MAX_DIRECTORY_ENTRIES {
            return Err(RemoteError::protocol("directory response is unreasonably large"));
        }
        let mut entries = Vec::with_capacity(count as usize);
        for _ in 0..count {
            let name = reader.string().map_err(RemoteError::io)?;
            let attr = WireAttr::read_from(&mut reader).map_err(RemoteError::io)?;
            entries.push(DirEntry { name, attr });
        }
        require_empty(&reader)?;
        Ok(entries)
    }

    pub fn open(&self, path: &str, flags: i32) -> Result<u64, RemoteError> {
        self.open_or_create(Op::Open, path, flags, 0)
    }

    pub fn create(&self, path: &str, flags: i32, mode: u32) -> Result<u64, RemoteError> {
        self.open_or_create(Op::Create, path, flags | 0x0100, mode)
    }

    pub fn read(&self, handle: u64, offset: u64, amount: u32) -> Result<Vec<u8>, RemoteError> {
        if amount > MAX_IO {
            return Err(RemoteError::protocol("read request exceeds 1 MiB"));
        }
        let mut writer = Writer::new();
        writer.u64(handle);
        writer.u64(offset);
        writer.u32(amount);
        let response = self.request(Op::Read, &writer.into_inner())?;
        if response.len() > amount as usize {
            return Err(RemoteError::protocol("server returned too many read bytes"));
        }
        Ok(response)
    }

    pub fn write(&self, handle: u64, offset: u64, data: &[u8]) -> Result<u32, RemoteError> {
        if data.len() > MAX_IO as usize {
            return Err(RemoteError::protocol("write request exceeds 1 MiB"));
        }
        let mut writer = Writer::new();
        writer.u64(handle);
        writer.u64(offset);
        writer.raw(data);
        let response = self.request(Op::Write, &writer.into_inner())?;
        let mut reader = Reader::new(&response);
        let written = reader.u32().map_err(RemoteError::io)?;
        require_empty(&reader)?;
        if written as usize > data.len() {
            return Err(RemoteError::protocol("server reported an oversized write"));
        }
        Ok(written)
    }

    pub fn release(&self, handle: u64) -> Result<(), RemoteError> {
        self.handle_only(Op::Release, handle)
    }

    pub fn fsync(&self, handle: u64) -> Result<(), RemoteError> {
        self.handle_only(Op::Fsync, handle)
    }

    pub fn flush(&self, handle: u64) -> Result<(), RemoteError> {
        self.handle_only(Op::Flush, handle)
    }

    pub fn mkdir(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteError::io)?;
        writer.u32(mode);
        self.expect_empty(Op::Mkdir, &writer.into_inner())
    }

    pub fn unlink(&self, path: &str) -> Result<(), RemoteError> {
        self.path_only(Op::Unlink, path)
    }

    pub fn rmdir(&self, path: &str) -> Result<(), RemoteError> {
        self.path_only(Op::Rmdir, path)
    }

    pub fn rename(&self, from: &str, to: &str) -> Result<(), RemoteError> {
        let mut writer = Writer::new();
        writer.string(from).map_err(RemoteError::io)?;
        writer.string(to).map_err(RemoteError::io)?;
        self.expect_empty(Op::Rename, &writer.into_inner())
    }

    pub fn truncate(&self, path: &str, size: u64) -> Result<(), RemoteError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteError::io)?;
        writer.u64(size);
        self.expect_empty(Op::Truncate, &writer.into_inner())
    }

    pub fn stat_fs(&self, path: &str) -> Result<WireStatFs, RemoteError> {
        let response = self.request(Op::StatFs, &path_payload(path)?)?;
        let mut reader = Reader::new(&response);
        let stat = WireStatFs::read_from(&mut reader).map_err(RemoteError::io)?;
        require_empty(&reader)?;
        Ok(stat)
    }

    pub fn utimens(&self, path: &str, atime: i64, mtime: i64) -> Result<(), RemoteError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteError::io)?;
        writer.i64(atime);
        writer.i64(mtime);
        self.expect_empty(Op::Utimens, &writer.into_inner())
    }

    pub fn chmod(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteError::io)?;
        writer.u32(mode);
        self.expect_empty(Op::Chmod, &writer.into_inner())
    }

    pub fn access(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteError::io)?;
        writer.u32(mode);
        self.expect_empty(Op::Access, &writer.into_inner())
    }

    fn open_or_create(
        &self,
        op: Op,
        path: &str,
        flags: i32,
        mode: u32,
    ) -> Result<u64, RemoteError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteError::io)?;
        writer.i32(flags);
        writer.u32(mode);
        let response = self.request(op, &writer.into_inner())?;
        let mut reader = Reader::new(&response);
        let handle = reader.u64().map_err(RemoteError::io)?;
        require_empty(&reader)?;
        Ok(handle)
    }

    fn handle_only(&self, op: Op, handle: u64) -> Result<(), RemoteError> {
        let mut writer = Writer::new();
        writer.u64(handle);
        self.expect_empty(op, &writer.into_inner())
    }

    fn path_only(&self, op: Op, path: &str) -> Result<(), RemoteError> {
        self.expect_empty(op, &path_payload(path)?)
    }

    fn expect_empty(&self, op: Op, payload: &[u8]) -> Result<(), RemoteError> {
        let response = self.request(op, payload)?;
        if response.is_empty() {
            Ok(())
        } else {
            Err(RemoteError::protocol("expected an empty response payload"))
        }
    }

    fn record_invalidation(&self, payload: &[u8]) {
        let mut reader = Reader::new(payload);
        let Ok(path) = reader.string() else {
            return;
        };
        if !reader.is_empty() {
            return;
        }
        let Ok(mut queue) = self.invalidations.lock() else {
            return;
        };
        if queue.len() == MAX_QUEUED_INVALIDATIONS {
            queue.remove(0);
        }
        queue.push(path);
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        self.disconnect();
    }
}

fn connect_one(token: &Token, client_id: &[u8; 16]) -> Result<Connection, String> {
    let ip: IpAddr = token
        .ip()
        .parse()
        .map_err(|_| format!("invalid IP address: {}", token.ip()))?;
    let address = SocketAddr::new(ip, token.port());
    let mut stream = TcpStream::connect_timeout(&address, CONNECT_TIMEOUT)
        .map_err(|error| format!("connect to {address} failed: {error}"))?;
    stream
        .set_nodelay(true)
        .map_err(|error| format!("TCP_NODELAY failed: {error}"))?;
    stream
        .set_read_timeout(Some(REQUEST_TIMEOUT))
        .map_err(|error| format!("read timeout setup failed: {error}"))?;
    stream
        .set_write_timeout(Some(REQUEST_TIMEOUT))
        .map_err(|error| format!("write timeout setup failed: {error}"))?;

    let nonce_client: [u8; 16] = random_array()?;
    let folder_hash = sha256(token.folder().as_bytes());
    let mut hello = Writer::new();
    hello.u32(PROTOCOL_VERSION);
    hello.raw(client_id);
    hello.raw(&folder_hash);
    hello.raw(&nonce_client);
    write_plain_message(&mut stream, Op::Hello.code(), 0, 1, &hello.into_inner())
        .map_err(|error| format!("handshake send failed: {error}"))?;

    let (challenge, challenge_payload) = read_plain_message(&mut stream)
        .map_err(|error| format!("handshake challenge failed: {error}"))?;
    if challenge.op() != Op::Challenge.code() || challenge_payload.len() < 16 {
        return Err("server rejected connection (folder/full?)".to_owned());
    }
    let nonce_server: [u8; 16] = challenge_payload[..16]
        .try_into()
        .map_err(|_| "server challenge nonce is malformed".to_owned())?;
    let auth_key = sha256(token.secret());
    let proof = auth_proof(&auth_key, &nonce_client, &nonce_server)?;
    write_plain_message(&mut stream, Op::Auth.code(), 0, 1, &proof)
        .map_err(|error| format!("authentication send failed: {error}"))?;

    let (auth_reply, _) = read_plain_message(&mut stream)
        .map_err(|error| format!("authentication reply failed: {error}"))?;
    if auth_reply.op() != Op::AuthOk.code() {
        return Err("authentication failed (wrong password or token)".to_owned());
    }
    let keys = derive_session_keys(&auth_key, &nonce_client, &nonce_server, false)?;
    Ok(Connection {
        stream,
        sender: SecureSender::new(*keys.tx()),
        receiver: SecureReceiver::new(*keys.rx()),
        next_request_id: 1,
    })
}

fn path_payload(path: &str) -> Result<Vec<u8>, RemoteError> {
    let mut writer = Writer::new();
    writer.string(path).map_err(RemoteError::io)?;
    Ok(writer.into_inner())
}

fn require_empty(reader: &Reader<'_>) -> Result<(), RemoteError> {
    if reader.is_empty() {
        Ok(())
    } else {
        Err(RemoteError::protocol("response payload has trailing bytes"))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn remote_error_keeps_positive_status() {
        let error = RemoteError::remote(13);
        assert_eq!(error.status(), 13);
        assert!(error.to_string().contains("13"));
    }
}
