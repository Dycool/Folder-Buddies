use std::sync::Arc;

use crate::{
    client::Client,
    protocol::{Op, WireAttr, WireStatFs},
    ram_cache::RamCache,
    remote_fs::{RemoteDirEntry, RemoteFs, RemoteFsError},
};

#[derive(Debug)]
pub struct CachedRemoteFs {
    client: Arc<Client>,
    cache: RamCache,
}

impl CachedRemoteFs {
    #[must_use]
    pub fn new(client: Arc<Client>) -> Self {
        Self {
            cache: RamCache::new(Arc::clone(&client)),
            client,
        }
    }
}

impl RemoteFs for CachedRemoteFs {
    fn connected(&self) -> bool {
        self.client.connected()
    }

    fn disconnect(&self) {
        self.client.disconnect();
    }

    fn bytes_read(&self) -> u64 {
        self.client.bytes_read()
    }

    fn bytes_written(&self) -> u64 {
        self.client.bytes_written()
    }

    fn take_invalidations(&self) -> Vec<String> {
        self.client.take_invalidations()
    }

    fn request(&self, op: Op, payload: &[u8]) -> Result<Vec<u8>, RemoteFsError> {
        self.client.request(op, payload).map_err(Into::into)
    }

    fn get_attr(&self, path: &str) -> Result<WireAttr, RemoteFsError> {
        self.cache.get_attr(path).map_err(Into::into)
    }

    fn read_dir(&self, path: &str) -> Result<Vec<RemoteDirEntry>, RemoteFsError> {
        self.cache
            .read_dir(path)
            .map(|entries| {
                entries
                    .into_iter()
                    .map(|entry| RemoteDirEntry::new(entry.name().to_owned(), *entry.attr()))
                    .collect()
            })
            .map_err(Into::into)
    }

    fn open(&self, path: &str, flags: i32) -> Result<u64, RemoteFsError> {
        self.cache.open(path, flags).map_err(Into::into)
    }

    fn create(&self, path: &str, flags: i32, mode: u32) -> Result<u64, RemoteFsError> {
        self.cache.create(path, flags, mode).map_err(Into::into)
    }

    fn read(&self, handle: u64, offset: u64, amount: u32) -> Result<Vec<u8>, RemoteFsError> {
        self.cache.read(handle, offset, amount).map_err(Into::into)
    }

    fn write(&self, handle: u64, offset: u64, data: &[u8]) -> Result<u32, RemoteFsError> {
        self.cache.write(handle, offset, data).map_err(Into::into)
    }

    fn release(&self, handle: u64) -> Result<(), RemoteFsError> {
        self.cache.release(handle).map_err(Into::into)
    }

    fn fsync(&self, handle: u64) -> Result<(), RemoteFsError> {
        self.cache.fsync(handle).map_err(Into::into)
    }

    fn flush(&self, handle: u64) -> Result<(), RemoteFsError> {
        self.cache.flush(handle).map_err(Into::into)
    }

    fn mkdir(&self, path: &str, mode: u32) -> Result<(), RemoteFsError> {
        self.cache.mkdir(path, mode).map_err(Into::into)
    }

    fn unlink(&self, path: &str) -> Result<(), RemoteFsError> {
        self.cache.unlink(path).map_err(Into::into)
    }

    fn rmdir(&self, path: &str) -> Result<(), RemoteFsError> {
        self.cache.rmdir(path).map_err(Into::into)
    }

    fn rename(&self, from: &str, to: &str) -> Result<(), RemoteFsError> {
        self.cache.rename(from, to).map_err(Into::into)
    }

    fn truncate(&self, path: &str, size: u64) -> Result<(), RemoteFsError> {
        self.cache.truncate(path, size).map_err(Into::into)
    }

    fn stat_fs(&self, path: &str) -> Result<WireStatFs, RemoteFsError> {
        self.cache.stat_fs(path).map_err(Into::into)
    }

    fn utimens(&self, path: &str, atime: i64, mtime: i64) -> Result<(), RemoteFsError> {
        self.cache.utimens(path, atime, mtime).map_err(Into::into)
    }

    fn chmod(&self, path: &str, mode: u32) -> Result<(), RemoteFsError> {
        self.cache.chmod(path, mode).map_err(Into::into)
    }

    fn access(&self, path: &str, mode: u32) -> Result<(), RemoteFsError> {
        self.cache.access(path, mode).map_err(Into::into)
    }
}
