use std::{
    collections::{HashMap, VecDeque},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use sysinfo::System;

use crate::{
    client::{Client, DirEntry, RemoteError},
    protocol::{MAX_IO, WireAttr, WireStatFs},
};

const META_TTL: Duration = Duration::from_secs(2);
const DIRECTORY_TTL: Duration = Duration::from_secs(5);
const NEGATIVE_TTL: Duration = Duration::from_secs(1);
const MAX_METADATA_ENTRIES: usize = 8192;
const MAX_DIRECTORY_ENTRIES: usize = 1024;
const MIB: u64 = 1024 * 1024;
const GIB: u64 = 1024 * MIB;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct FileVersion {
    size: u64,
    mtime: i64,
}

impl From<&WireAttr> for FileVersion {
    fn from(attr: &WireAttr) -> Self {
        Self {
            size: attr.size(),
            mtime: attr.mtime(),
        }
    }
}

#[derive(Clone, Debug)]
struct MetadataEntry {
    result: Result<WireAttr, RemoteError>,
    inserted: Instant,
}

#[derive(Clone, Debug)]
struct DirectoryEntryCache {
    entries: Vec<DirEntry>,
    inserted: Instant,
}

#[derive(Clone, Debug)]
struct HandleEntry {
    path: String,
}

#[derive(Clone, Debug, Eq, Hash, PartialEq)]
struct BlockKey {
    path: String,
    index: u64,
}

#[derive(Clone, Debug)]
struct CachedBlock {
    version: FileVersion,
    data: Arc<[u8]>,
}

#[derive(Debug, Default)]
struct BlockCache {
    blocks: HashMap<BlockKey, CachedBlock>,
    lru: VecDeque<BlockKey>,
    bytes: usize,
}

impl BlockCache {
    fn get(&mut self, key: &BlockKey, version: FileVersion) -> Option<Arc<[u8]>> {
        let data = self
            .blocks
            .get(key)
            .filter(|block| block.version == version)
            .map(|block| Arc::clone(&block.data));
        if data.is_some() {
            self.touch(key);
        } else if self.blocks.contains_key(key) {
            self.remove(key);
        }
        data
    }

    fn insert(&mut self, key: BlockKey, version: FileVersion, data: Vec<u8>, budget: usize) {
        if budget == 0 || data.is_empty() || data.len() > budget {
            return;
        }
        self.remove(&key);
        let data: Arc<[u8]> = data.into();
        self.bytes = self.bytes.saturating_add(data.len());
        self.blocks.insert(
            key.clone(),
            CachedBlock {
                version,
                data,
            },
        );
        self.touch(&key);
        while self.bytes > budget {
            let Some(oldest) = self.lru.pop_back() else {
                break;
            };
            if let Some(block) = self.blocks.remove(&oldest) {
                self.bytes = self.bytes.saturating_sub(block.data.len());
            }
        }
    }

    fn invalidate_path(&mut self, path: &str) {
        let keys: Vec<BlockKey> = self
            .blocks
            .keys()
            .filter(|key| key.path == path)
            .cloned()
            .collect();
        for key in keys {
            self.remove(&key);
        }
    }

    fn remove(&mut self, key: &BlockKey) {
        if let Some(block) = self.blocks.remove(key) {
            self.bytes = self.bytes.saturating_sub(block.data.len());
        }
        self.lru.retain(|candidate| candidate != key);
    }

    fn touch(&mut self, key: &BlockKey) {
        self.lru.retain(|candidate| candidate != key);
        self.lru.push_front(key.clone());
    }
}

#[derive(Debug)]
pub struct RamCache {
    client: Arc<Client>,
    metadata: Mutex<HashMap<String, MetadataEntry>>,
    directories: Mutex<HashMap<String, DirectoryEntryCache>>,
    handles: Mutex<HashMap<u64, HandleEntry>>,
    blocks: Mutex<BlockCache>,
    block_budget: usize,
}

impl RamCache {
    #[must_use]
    pub fn new(client: Arc<Client>) -> Self {
        Self {
            client,
            metadata: Mutex::new(HashMap::new()),
            directories: Mutex::new(HashMap::new()),
            handles: Mutex::new(HashMap::new()),
            blocks: Mutex::new(BlockCache::default()),
            block_budget: ram_block_budget(),
        }
    }

    #[must_use]
    pub fn connected(&self) -> bool {
        self.client.connected()
    }

    #[must_use]
    pub fn block_budget(&self) -> usize {
        self.block_budget
    }

    pub fn get_attr(&self, path: &str) -> Result<WireAttr, RemoteError> {
        self.drain_invalidations();
        let now = Instant::now();
        if let Ok(metadata) = self.metadata.lock()
            && let Some(entry) = metadata.get(path)
        {
            let ttl = if entry.result.as_ref().is_err_and(|error| error.status() == 2) {
                NEGATIVE_TTL
            } else {
                META_TTL
            };
            if now.duration_since(entry.inserted) <= ttl {
                return entry.result.clone();
            }
        }

        let result = self.client.get_attr(path);
        if result.is_ok() || result.as_ref().is_err_and(|error| error.status() == 2) {
            self.cache_metadata(path.to_owned(), result.clone(), now);
        }
        result
    }

    pub fn read_dir(&self, path: &str) -> Result<Vec<DirEntry>, RemoteError> {
        self.drain_invalidations();
        let now = Instant::now();
        if let Ok(directories) = self.directories.lock()
            && let Some(entry) = directories.get(path)
            && now.duration_since(entry.inserted) <= DIRECTORY_TTL
        {
            return Ok(entry.entries.clone());
        }

        let entries = self.client.read_dir(path)?;
        for entry in &entries {
            let child = child_path(path, entry.name());
            self.cache_metadata(child, Ok(*entry.attr()), now);
        }
        if let Ok(mut directories) = self.directories.lock() {
            trim_map(&mut directories, MAX_DIRECTORY_ENTRIES);
            directories.insert(
                path.to_owned(),
                DirectoryEntryCache {
                    entries: entries.clone(),
                    inserted: now,
                },
            );
        }
        Ok(entries)
    }

    pub fn open(&self, path: &str, flags: i32) -> Result<u64, RemoteError> {
        self.drain_invalidations();
        let handle = self.client.open(path, flags)?;
        self.track_handle(handle, path);
        Ok(handle)
    }

    pub fn create(&self, path: &str, flags: i32, mode: u32) -> Result<u64, RemoteError> {
        self.drain_invalidations();
        let handle = self.client.create(path, flags, mode)?;
        self.track_handle(handle, path);
        self.invalidate_file(path);
        self.invalidate_directory(&parent_path(path));
        Ok(handle)
    }

    pub fn read(&self, handle: u64, offset: u64, amount: u32) -> Result<Vec<u8>, RemoteError> {
        self.drain_invalidations();
        if amount == 0 {
            return Ok(Vec::new());
        }
        let Some(path) = self.handle_path(handle) else {
            return self.client.read(handle, offset, amount);
        };
        if self.block_budget == 0 {
            return self.client.read(handle, offset, amount);
        }
        let attr = self.get_attr(&path)?;
        let version = FileVersion::from(&attr);
        if offset >= version.size {
            return Ok(Vec::new());
        }

        let requested_end = offset
            .saturating_add(u64::from(amount))
            .min(version.size);
        let mut cursor = offset;
        let capacity = usize::try_from(requested_end.saturating_sub(offset)).unwrap_or(usize::MAX);
        let mut output = Vec::with_capacity(capacity);
        while cursor < requested_end {
            let block_index = cursor / u64::from(MAX_IO);
            let block_start = block_index.saturating_mul(u64::from(MAX_IO));
            let key = BlockKey {
                path: path.clone(),
                index: block_index,
            };
            let block = self.cached_or_fetch_block(handle, &key, version, block_start)?;
            let within = usize::try_from(cursor.saturating_sub(block_start)).unwrap_or(usize::MAX);
            if within >= block.len() {
                break;
            }
            let remaining = usize::try_from(requested_end.saturating_sub(cursor)).unwrap_or(usize::MAX);
            let take = remaining.min(block.len() - within);
            output.extend_from_slice(&block[within..within + take]);
            cursor = cursor.saturating_add(u64::try_from(take).unwrap_or(u64::MAX));
            if take == 0 || block.len() < MAX_IO as usize {
                break;
            }
        }
        Ok(output)
    }

    pub fn write(&self, handle: u64, offset: u64, data: &[u8]) -> Result<u32, RemoteError> {
        self.drain_invalidations();
        let result = self.client.write(handle, offset, data);
        if result.is_ok()
            && let Some(path) = self.handle_path(handle)
        {
            self.invalidate_file(&path);
        }
        result
    }

    pub fn release(&self, handle: u64) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.client.release(handle);
        if let Ok(mut handles) = self.handles.lock() {
            handles.remove(&handle);
        }
        result
    }

    pub fn fsync(&self, handle: u64) -> Result<(), RemoteError> {
        self.client.fsync(handle)
    }

    pub fn flush(&self, handle: u64) -> Result<(), RemoteError> {
        self.client.flush(handle)
    }

    pub fn mkdir(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.client.mkdir(path, mode);
        if result.is_ok() {
            self.invalidate_file(path);
            self.invalidate_directory(&parent_path(path));
        }
        result
    }

    pub fn unlink(&self, path: &str) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.client.unlink(path);
        if result.is_ok() {
            self.invalidate_file(path);
            self.invalidate_directory(&parent_path(path));
        }
        result
    }

    pub fn rmdir(&self, path: &str) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.client.rmdir(path);
        if result.is_ok() {
            self.invalidate_file(path);
            self.invalidate_directory(path);
            self.invalidate_directory(&parent_path(path));
        }
        result
    }

    pub fn rename(&self, from: &str, to: &str) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.client.rename(from, to);
        if result.is_ok() {
            self.invalidate_file(from);
            self.invalidate_file(to);
            self.invalidate_directory(&parent_path(from));
            self.invalidate_directory(&parent_path(to));
            if let Ok(mut handles) = self.handles.lock() {
                for handle in handles.values_mut() {
                    if handle.path == from {
                        handle.path = to.to_owned();
                    }
                }
            }
        }
        result
    }

    pub fn truncate(&self, path: &str, size: u64) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.client.truncate(path, size);
        if result.is_ok() {
            self.invalidate_file(path);
        }
        result
    }

    pub fn stat_fs(&self, path: &str) -> Result<WireStatFs, RemoteError> {
        self.client.stat_fs(path)
    }

    pub fn utimens(&self, path: &str, atime: i64, mtime: i64) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.client.utimens(path, atime, mtime);
        if result.is_ok() {
            self.invalidate_metadata(path);
        }
        result
    }

    pub fn chmod(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.client.chmod(path, mode);
        if result.is_ok() {
            self.invalidate_metadata(path);
        }
        result
    }

    pub fn access(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        self.client.access(path, mode)
    }

    fn cached_or_fetch_block(
        &self,
        handle: u64,
        key: &BlockKey,
        version: FileVersion,
        block_start: u64,
    ) -> Result<Arc<[u8]>, RemoteError> {
        if let Ok(mut blocks) = self.blocks.lock()
            && let Some(data) = blocks.get(key, version)
        {
            return Ok(data);
        }
        let data = self.client.read(handle, block_start, MAX_IO)?;
        let returned: Arc<[u8]> = data.clone().into();
        if let Ok(mut blocks) = self.blocks.lock() {
            blocks.insert(key.clone(), version, data, self.block_budget);
        }
        Ok(returned)
    }

    fn track_handle(&self, handle: u64, path: &str) {
        if let Ok(mut handles) = self.handles.lock() {
            handles.insert(
                handle,
                HandleEntry {
                    path: path.to_owned(),
                },
            );
        }
    }

    fn handle_path(&self, handle: u64) -> Option<String> {
        self.handles
            .lock()
            .ok()
            .and_then(|handles| handles.get(&handle).map(|entry| entry.path.clone()))
    }

    fn cache_metadata(
        &self,
        path: String,
        result: Result<WireAttr, RemoteError>,
        inserted: Instant,
    ) {
        if let Ok(mut metadata) = self.metadata.lock() {
            trim_map(&mut metadata, MAX_METADATA_ENTRIES);
            metadata.insert(path, MetadataEntry { result, inserted });
        }
    }

    fn drain_invalidations(&self) {
        for path in self.client.take_invalidations() {
            self.invalidate_file(&path);
            self.invalidate_directory(&path);
            self.invalidate_directory(&parent_path(&path));
        }
    }

    fn invalidate_file(&self, path: &str) {
        self.invalidate_metadata(path);
        if let Ok(mut blocks) = self.blocks.lock() {
            blocks.invalidate_path(path);
        }
    }

    fn invalidate_metadata(&self, path: &str) {
        if let Ok(mut metadata) = self.metadata.lock() {
            metadata.remove(path);
        }
    }

    fn invalidate_directory(&self, path: &str) {
        if let Ok(mut directories) = self.directories.lock() {
            directories.remove(path);
        }
    }
}

fn trim_map<K, V>(map: &mut HashMap<K, V>, limit: usize)
where
    K: Clone + Eq + std::hash::Hash,
{
    if map.len() >= limit
        && let Some(key) = map.keys().next().cloned()
    {
        map.remove(&key);
    }
}

fn child_path(parent: &str, name: &str) -> String {
    if parent == "/" {
        format!("/{name}")
    } else {
        format!("{}/{name}", parent.trim_end_matches('/'))
    }
}

fn parent_path(path: &str) -> String {
    let trimmed = path.trim_end_matches('/');
    match trimmed.rsplit_once('/') {
        Some(("", _)) | None => "/".to_owned(),
        Some((parent, _)) => parent.to_owned(),
    }
}

fn ram_block_budget() -> usize {
    if let Ok(value) = std::env::var("FOLDERBUDDIES_CACHE_BYTES")
        && let Ok(bytes) = value.parse::<u64>()
    {
        return usize::try_from(bytes).unwrap_or(usize::MAX);
    }

    let mut system = System::new();
    system.refresh_memory();
    let total = system.total_memory();
    let available = system.available_memory();
    usize::try_from(budget_from_memory(total, available)).unwrap_or(usize::MAX)
}

fn budget_from_memory(total: u64, available: u64) -> u64 {
    if total == 0 || available < 512 * MIB {
        return 0;
    }
    let cap = if total <= 4 * GIB {
        256 * MIB
    } else if total <= 8 * GIB {
        512 * MIB
    } else if total <= 16 * GIB {
        GIB
    } else if total <= 32 * GIB {
        2 * GIB
    } else {
        4 * GIB
    };
    let mut budget = (available / 4).min(total / 10).min(cap);
    if available < GIB {
        budget = budget.min(64 * MIB);
    } else if available < 2 * GIB {
        budget = budget.min(128 * MIB);
    }
    budget
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn memory_budget_matches_original_thresholds() {
        assert_eq!(budget_from_memory(4 * GIB, 500 * MIB), 0);
        assert_eq!(budget_from_memory(4 * GIB, 900 * MIB), 64 * MIB);
        assert_eq!(budget_from_memory(8 * GIB, 1500 * MIB), 128 * MIB);
        assert_eq!(budget_from_memory(16 * GIB, 8 * GIB), GIB);
        assert_eq!(budget_from_memory(64 * GIB, 32 * GIB), 4 * GIB);
    }

    #[test]
    fn parent_and_child_paths_are_canonical() {
        assert_eq!(parent_path("/file"), "/");
        assert_eq!(parent_path("/dir/file"), "/dir");
        assert_eq!(child_path("/", "file"), "/file");
        assert_eq!(child_path("/dir/", "file"), "/dir/file");
    }

    #[test]
    fn block_cache_is_versioned_and_lru_bounded() {
        let mut cache = BlockCache::default();
        let version = FileVersion { size: 8, mtime: 1 };
        let key0 = BlockKey {
            path: "/a".to_owned(),
            index: 0,
        };
        let key1 = BlockKey {
            path: "/b".to_owned(),
            index: 0,
        };
        cache.insert(key0.clone(), version, vec![1, 2, 3, 4], 6);
        assert_eq!(cache.get(&key0, version).as_deref(), Some(&[1, 2, 3, 4][..]));
        cache.insert(key1.clone(), version, vec![5, 6, 7, 8], 6);
        assert!(cache.get(&key0, version).is_none());
        assert_eq!(cache.get(&key1, version).as_deref(), Some(&[5, 6, 7, 8][..]));
        assert!(cache
            .get(&key1, FileVersion { size: 8, mtime: 2 })
            .is_none());
    }
}