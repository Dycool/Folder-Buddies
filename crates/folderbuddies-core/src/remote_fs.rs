use crate::{
    client::{Client, DirEntry, RemoteError},
    protocol::WireAttr,
    protocol::WireStatFs,
};

/// Transport-independent filesystem client contract.
///
/// The C++ implementation mounts any `RemoteFs` (native TCP, native QUIC, or
/// WebRTC compatibility). Keeping the same boundary in Rust prevents the mount
/// layer from accidentally making transport-specific assumptions.
pub trait RemoteFs: Send + Sync {
    fn connected(&self) -> bool;
    fn disconnect(&self);
    fn bytes_read(&self) -> u64;
    fn bytes_written(&self) -> u64;
    fn take_invalidations(&self) -> Vec<String>;

    fn get_attr(&self, path: &str) -> Result<WireAttr, RemoteError>;
    fn read_dir(&self, path: &str) -> Result<Vec<DirEntry>, RemoteError>;
    fn open(&self, path: &str, flags: i32) -> Result<u64, RemoteError>;
    fn create(&self, path: &str, flags: i32, mode: u32) -> Result<u64, RemoteError>;
    fn read(&self, handle: u64, offset: u64, amount: u32) -> Result<Vec<u8>, RemoteError>;
    fn write(&self, handle: u64, offset: u64, data: &[u8]) -> Result<u32, RemoteError>;
    fn release(&self, handle: u64) -> Result<(), RemoteError>;
    fn fsync(&self, handle: u64) -> Result<(), RemoteError>;
    fn flush(&self, handle: u64) -> Result<(), RemoteError>;
    fn mkdir(&self, path: &str, mode: u32) -> Result<(), RemoteError>;
    fn unlink(&self, path: &str) -> Result<(), RemoteError>;
    fn rmdir(&self, path: &str) -> Result<(), RemoteError>;
    fn rename(&self, from: &str, to: &str) -> Result<(), RemoteError>;
    fn truncate(&self, path: &str, size: u64) -> Result<(), RemoteError>;
    fn stat_fs(&self, path: &str) -> Result<WireStatFs, RemoteError>;
    fn utimens(&self, path: &str, atime: i64, mtime: i64) -> Result<(), RemoteError>;
    fn chmod(&self, path: &str, mode: u32) -> Result<(), RemoteError>;
    fn access(&self, path: &str, mode: u32) -> Result<(), RemoteError>;
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

    fn get_attr(&self, path: &str) -> Result<WireAttr, RemoteError> {
        Client::get_attr(self, path)
    }

    fn read_dir(&self, path: &str) -> Result<Vec<DirEntry>, RemoteError> {
        Client::read_dir(self, path)
    }

    fn open(&self, path: &str, flags: i32) -> Result<u64, RemoteError> {
        Client::open(self, path, flags)
    }

    fn create(&self, path: &str, flags: i32, mode: u32) -> Result<u64, RemoteError> {
        Client::create(self, path, flags, mode)
    }

    fn read(&self, handle: u64, offset: u64, amount: u32) -> Result<Vec<u8>, RemoteError> {
        Client::read(self, handle, offset, amount)
    }

    fn write(&self, handle: u64, offset: u64, data: &[u8]) -> Result<u32, RemoteError> {
        Client::write(self, handle, offset, data)
    }

    fn release(&self, handle: u64) -> Result<(), RemoteError> {
        Client::release(self, handle)
    }

    fn fsync(&self, handle: u64) -> Result<(), RemoteError> {
        Client::fsync(self, handle)
    }

    fn flush(&self, handle: u64) -> Result<(), RemoteError> {
        Client::flush(self, handle)
    }

    fn mkdir(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        Client::mkdir(self, path, mode)
    }

    fn unlink(&self, path: &str) -> Result<(), RemoteError> {
        Client::unlink(self, path)
    }

    fn rmdir(&self, path: &str) -> Result<(), RemoteError> {
        Client::rmdir(self, path)
    }

    fn rename(&self, from: &str, to: &str) -> Result<(), RemoteError> {
        Client::rename(self, from, to)
    }

    fn truncate(&self, path: &str, size: u64) -> Result<(), RemoteError> {
        Client::truncate(self, path, size)
    }

    fn stat_fs(&self, path: &str) -> Result<WireStatFs, RemoteError> {
        Client::stat_fs(self, path)
    }

    fn utimens(&self, path: &str, atime: i64, mtime: i64) -> Result<(), RemoteError> {
        Client::utimens(self, path, atime, mtime)
    }

    fn chmod(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        Client::chmod(self, path, mode)
    }

    fn access(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        Client::access(self, path, mode)
    }
}
