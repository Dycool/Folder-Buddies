use std::{fmt, io};

use crate::{
    client::{Client, RemoteError},
    protocol::{MAX_IO, Op, Reader, WireAttr, WireStatFs, Writer},
};

const EIO: i16 = 5;
const MAX_DIRECTORY_ENTRIES: u32 = 1_000_000;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RemoteFsError {
    status: i16,
    message: String,
}

impl RemoteFsError {
    #[must_use]
    pub fn new(status: i16, message: impl Into<String>) -> Self {
        Self {
            status: status.max(1),
            message: message.into(),
        }
    }

    #[must_use]
    pub const fn status(&self) -> i16 {
        self.status
    }

    #[must_use]
    pub fn message(&self) -> &str {
        &self.message
    }

    fn protocol(message: impl Into<String>) -> Self {
        Self::new(EIO, message)
    }

    fn io(error: io::Error) -> Self {
        let status = error
            .raw_os_error()
            .and_then(|value| i16::try_from(value).ok())
            .filter(|value| *value > 0)
            .unwrap_or(EIO);
        Self::new(status, error.to_string())
    }
}

impl From<RemoteError> for RemoteFsError {
    fn from(error: RemoteError) -> Self {
        Self::new(error.status(), error.message())
    }
}

impl fmt::Display for RemoteFsError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{} (status {})", self.message, self.status)
    }
}

impl std::error::Error for RemoteFsError {}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RemoteDirEntry {
    name: String,
    attr: WireAttr,
}

impl RemoteDirEntry {
    #[must_use]
    pub fn new(name: String, attr: WireAttr) -> Self {
        Self { name, attr }
    }

    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    #[must_use]
    pub const fn attr(&self) -> &WireAttr {
        &self.attr
    }
}

/// Transport-independent filesystem contract. Transport implementations only
/// provide `request`; the filesystem operation encoding stays identical for
/// native TCP, QUIC and WebRTC compatibility.
pub trait RemoteFs: Send + Sync {
    fn connected(&self) -> bool;
    fn disconnect(&self);
    fn bytes_read(&self) -> u64;
    fn bytes_written(&self) -> u64;

    fn take_invalidations(&self) -> Vec<String> {
        Vec::new()
    }

    fn request(&self, op: Op, payload: &[u8]) -> Result<Vec<u8>, RemoteFsError>;

    fn get_attr(&self, path: &str) -> Result<WireAttr, RemoteFsError> {
        let response = self.request(Op::GetAttr, &path_payload(path)?)?;
        let mut reader = Reader::new(&response);
        let attr = WireAttr::read_from(&mut reader).map_err(RemoteFsError::io)?;
        require_empty(&reader)?;
        Ok(attr)
    }

    fn read_dir(&self, path: &str) -> Result<Vec<RemoteDirEntry>, RemoteFsError> {
        let response = self.request(Op::ReadDir, &path_payload(path)?)?;
        let mut reader = Reader::new(&response);
        let count = reader.u32().map_err(RemoteFsError::io)?;
        if count > MAX_DIRECTORY_ENTRIES {
            return Err(RemoteFsError::protocol(
                "directory response is unreasonably large",
            ));
        }
        let mut entries = Vec::with_capacity(count as usize);
        for _ in 0..count {
            let name = reader.string().map_err(RemoteFsError::io)?;
            let attr = WireAttr::read_from(&mut reader).map_err(RemoteFsError::io)?;
            entries.push(RemoteDirEntry::new(name, attr));
        }
        require_empty(&reader)?;
        Ok(entries)
    }

    fn open(&self, path: &str, flags: i32) -> Result<u64, RemoteFsError> {
        self.open_or_create(Op::Open, path, flags, 0)
    }

    fn create(&self, path: &str, flags: i32, mode: u32) -> Result<u64, RemoteFsError> {
        self.open_or_create(Op::Create, path, flags | 0x0100, mode)
    }

    fn read(&self, handle: u64, offset: u64, amount: u32) -> Result<Vec<u8>, RemoteFsError> {
        if amount > MAX_IO {
            return Err(RemoteFsError::protocol("read request exceeds 1 MiB"));
        }
        let mut writer = Writer::new();
        writer.u64(handle);
        writer.u64(offset);
        writer.u32(amount);
        let response = self.request(Op::Read, &writer.into_inner())?;
        if response.len() > amount as usize {
            return Err(RemoteFsError::protocol("server returned too many read bytes"));
        }
        Ok(response)
    }

    fn write(&self, handle: u64, offset: u64, data: &[u8]) -> Result<u32, RemoteFsError> {
        if data.len() > MAX_IO as usize {
            return Err(RemoteFsError::protocol("write request exceeds 1 MiB"));
        }
        let mut writer = Writer::new();
        writer.u64(handle);
        writer.u64(offset);
        writer.raw(data);
        let response = self.request(Op::Write, &writer.into_inner())?;
        let mut reader = Reader::new(&response);
        let written = reader.u32().map_err(RemoteFsError::io)?;
        require_empty(&reader)?;
        if written as usize > data.len() {
            return Err(RemoteFsError::protocol("server reported an oversized write"));
        }
        Ok(written)
    }

    fn release(&self, handle: u64) -> Result<(), RemoteFsError> {
        self.handle_only(Op::Release, handle)
    }

    fn fsync(&self, handle: u64) -> Result<(), RemoteFsError> {
        self.handle_only(Op::Fsync, handle)
    }

    fn flush(&self, handle: u64) -> Result<(), RemoteFsError> {
        self.handle_only(Op::Flush, handle)
    }

    fn mkdir(&self, path: &str, mode: u32) -> Result<(), RemoteFsError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteFsError::io)?;
        writer.u32(mode);
        self.expect_empty(Op::Mkdir, &writer.into_inner())
    }

    fn unlink(&self, path: &str) -> Result<(), RemoteFsError> {
        self.path_only(Op::Unlink, path)
    }

    fn rmdir(&self, path: &str) -> Result<(), RemoteFsError> {
        self.path_only(Op::Rmdir, path)
    }

    fn rename(&self, from: &str, to: &str) -> Result<(), RemoteFsError> {
        let mut writer = Writer::new();
        writer.string(from).map_err(RemoteFsError::io)?;
        writer.string(to).map_err(RemoteFsError::io)?;
        self.expect_empty(Op::Rename, &writer.into_inner())
    }

    fn truncate(&self, path: &str, size: u64) -> Result<(), RemoteFsError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteFsError::io)?;
        writer.u64(size);
        self.expect_empty(Op::Truncate, &writer.into_inner())
    }

    fn stat_fs(&self, path: &str) -> Result<WireStatFs, RemoteFsError> {
        let response = self.request(Op::StatFs, &path_payload(path)?)?;
        let mut reader = Reader::new(&response);
        let stat = WireStatFs::read_from(&mut reader).map_err(RemoteFsError::io)?;
        require_empty(&reader)?;
        Ok(stat)
    }

    fn utimens(&self, path: &str, atime: i64, mtime: i64) -> Result<(), RemoteFsError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteFsError::io)?;
        writer.i64(atime);
        writer.i64(mtime);
        self.expect_empty(Op::Utimens, &writer.into_inner())
    }

    fn chmod(&self, path: &str, mode: u32) -> Result<(), RemoteFsError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteFsError::io)?;
        writer.u32(mode);
        self.expect_empty(Op::Chmod, &writer.into_inner())
    }

    fn access(&self, path: &str, mode: u32) -> Result<(), RemoteFsError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteFsError::io)?;
        writer.u32(mode);
        self.expect_empty(Op::Access, &writer.into_inner())
    }

    fn open_or_create(
        &self,
        op: Op,
        path: &str,
        flags: i32,
        mode: u32,
    ) -> Result<u64, RemoteFsError> {
        let mut writer = Writer::new();
        writer.string(path).map_err(RemoteFsError::io)?;
        writer.i32(flags);
        writer.u32(mode);
        let response = self.request(op, &writer.into_inner())?;
        let mut reader = Reader::new(&response);
        let handle = reader.u64().map_err(RemoteFsError::io)?;
        require_empty(&reader)?;
        Ok(handle)
    }

    fn handle_only(&self, op: Op, handle: u64) -> Result<(), RemoteFsError> {
        let mut writer = Writer::new();
        writer.u64(handle);
        self.expect_empty(op, &writer.into_inner())
    }

    fn path_only(&self, op: Op, path: &str) -> Result<(), RemoteFsError> {
        self.expect_empty(op, &path_payload(path)?)
    }

    fn expect_empty(&self, op: Op, payload: &[u8]) -> Result<(), RemoteFsError> {
        let response = self.request(op, payload)?;
        if response.is_empty() {
            Ok(())
        } else {
            Err(RemoteFsError::protocol("expected an empty response payload"))
        }
    }
}

impl RemoteFs for Client {
    fn connected(&self) -> bool {
        Client::connected(self)
    }

    fn disconnect(&self) {
        Client::disconnect(self);
    }

    fn bytes_read(&self) -> u64 {
        Client::bytes_read(self)
    }

    fn bytes_written(&self) -> u64 {
        Client::bytes_written(self)
    }

    fn take_invalidations(&self) -> Vec<String> {
        Client::take_invalidations(self)
    }

    fn request(&self, op: Op, payload: &[u8]) -> Result<Vec<u8>, RemoteFsError> {
        Client::request(self, op, payload).map_err(Into::into)
    }
}

fn path_payload(path: &str) -> Result<Vec<u8>, RemoteFsError> {
    let mut writer = Writer::new();
    writer.string(path).map_err(RemoteFsError::io)?;
    Ok(writer.into_inner())
}

fn require_empty(reader: &Reader<'_>) -> Result<(), RemoteFsError> {
    if reader.is_empty() {
        Ok(())
    } else {
        Err(RemoteFsError::protocol(
            "response payload has trailing bytes",
        ))
    }
}
