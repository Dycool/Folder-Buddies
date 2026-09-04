use std::fmt;

use crate::{
    client::{Client, RemoteError},
    protocol::{WireAttr, WireStatFs},
    ram_cache::RamCache,
};

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

/// Transport-independent filesystem client contract, mirroring the C++
/// `RemoteFs` boundary used by native TCP, native QUIC and WebRTC clients.
pub trait RemoteFs: Send + Sync {
    fn connected(&self) -> bool;
    fn disconnect(&self);
    fn bytes_read(&self) -> u64;
    fn bytes_written(&self) -> u64;
    fn take_invalidations(&self) -> Vec<String>;

    fn get_attr(&self, path: &str) -> Result<WireAttr, RemoteFsError>;
    fn read_dir(&self, path: &str) -> Result<Vec<RemoteDirEntry>, RemoteFsError>;
    fn open(&self, path: &str, flags: i32) -> Result<u64, RemoteFsError>;
    fn create(&self, path: &str, flags: i32, mode: u32) -> Result<u64, RemoteFsError>;
    fn read(&self, handle: u64, offset: u64, amount: u32) -> Result<Vec<u8>, RemoteFsError>;
    fn write(&self, handle: u64, offset: u64, data: &[u8]) -> Result<u32, RemoteFsError>;
    fn release(&self, handle: u64) -> Result<(), RemoteFsError>;
    fn fsync(&self, handle: u64) -> Result<(), RemoteFsError>;
    fn flush(&self, handle: u64) -> Result<(), RemoteFsError>;
    fn mkdir(&self, path: &str, mode: u32) -> Result<(), RemoteFsError>;
    fn unlink(&self, path: &str) -> Result<(), RemoteFsError>;
    fn rmdir(&self, path: &str) -> Result<(), RemoteFsError>;
    fn rename(&self, from: &str, to: &str) -> Result<(), RemoteFsError>;
    fn truncate(&self, path: &str, size: u64) -> Result<(), RemoteFsError>;
    fn stat_fs(&self, path: &str) -> Result<WireStatFs, RemoteFsError>;
    fn utimens(&self, path: &str, atime: i64, mtime: i64) -> Result<(), RemoteFsError>;
    fn chmod(&self, path: &str, mode: u32) -> Result<(), RemoteFsError>;
    fn access(&self, path: &str, mode: u32) -> Result<(), RemoteFsError>;
}

macro_rules! impl_remote_fs {
    ($type:ty) => {
        impl RemoteFs for $type {
            fn connected(&self) -> bool {
                <$type>::connected(self)
            }

            fn disconnect(&self) {
                <$type>::disconnect(self);
            }

            fn bytes_read(&self) -> u64 {
                <$type>::bytes_read(self)
            }

            fn bytes_written(&self) -> u64 {
                <$type>::bytes_written(self)
            }

            fn take_invalidations(&self) -> Vec<String> {
                <$type>::take_invalidations(self)
            }

            fn get_attr(&self, path: &str) -> Result<WireAttr, RemoteFsError> {
                <$type>::get_attr(self, path).map_err(Into::into)
            }

            fn read_dir(&self, path: &str) -> Result<Vec<RemoteDirEntry>, RemoteFsError> {
                <$type>::read_dir(self, path)
                    .map(|entries| {
                        entries
                            .into_iter()
                            .map(|entry| RemoteDirEntry::new(entry.name().to_owned(), *entry.attr()))
                            .collect()
                    })
                    .map_err(Into::into)
            }

            fn open(&self, path: &str, flags: i32) -> Result<u64, RemoteFsError> {
                <$type>::open(self, path, flags).map_err(Into::into)
            }

            fn create(&self, path: &str, flags: i32, mode: u32) -> Result<u64, RemoteFsError> {
                <$type>::create(self, path, flags, mode).map_err(Into::into)
            }

            fn read(&self, handle: u64, offset: u64, amount: u32) -> Result<Vec<u8>, RemoteFsError> {
                <$type>::read(self, handle, offset, amount).map_err(Into::into)
            }

            fn write(&self, handle: u64, offset: u64, data: &[u8]) -> Result<u32, RemoteFsError> {
                <$type>::write(self, handle, offset, data).map_err(Into::into)
            }

            fn release(&self, handle: u64) -> Result<(), RemoteFsError> {
                <$type>::release(self, handle).map_err(Into::into)
            }

            fn fsync(&self, handle: u64) -> Result<(), RemoteFsError> {
                <$type>::fsync(self, handle).map_err(Into::into)
            }

            fn flush(&self, handle: u64) -> Result<(), RemoteFsError> {
                <$type>::flush(self, handle).map_err(Into::into)
            }

            fn mkdir(&self, path: &str, mode: u32) -> Result<(), RemoteFsError> {
                <$type>::mkdir(self, path, mode).map_err(Into::into)
            }

            fn unlink(&self, path: &str) -> Result<(), RemoteFsError> {
                <$type>::unlink(self, path).map_err(Into::into)
            }

            fn rmdir(&self, path: &str) -> Result<(), RemoteFsError> {
                <$type>::rmdir(self, path).map_err(Into::into)
            }

            fn rename(&self, from: &str, to: &str) -> Result<(), RemoteFsError> {
                <$type>::rename(self, from, to).map_err(Into::into)
            }

            fn truncate(&self, path: &str, size: u64) -> Result<(), RemoteFsError> {
                <$type>::truncate(self, path, size).map_err(Into::into)
            }

            fn stat_fs(&self, path: &str) -> Result<WireStatFs, RemoteFsError> {
                <$type>::stat_fs(self, path).map_err(Into::into)
            }

            fn utimens(&self, path: &str, atime: i64, mtime: i64) -> Result<(), RemoteFsError> {
                <$type>::utimens(self, path, atime, mtime).map_err(Into::into)
            }

            fn chmod(&self, path: &str, mode: u32) -> Result<(), RemoteFsError> {
                <$type>::chmod(self, path, mode).map_err(Into::into)
            }

            fn access(&self, path: &str, mode: u32) -> Result<(), RemoteFsError> {
                <$type>::access(self, path, mode).map_err(Into::into)
            }
        }
    };
}

impl_remote_fs!(Client);
impl_remote_fs!(RamCache);
