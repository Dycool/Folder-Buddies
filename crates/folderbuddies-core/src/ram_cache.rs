use std::{
    collections::{HashMap, HashSet, VecDeque},
    sync::{Arc, Condvar, Mutex},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

use crossbeam_channel::{Sender, TrySendError, bounded};
use sysinfo::System;

use crate::{
    client::{Client, DirEntry, RemoteError},
    protocol::{MAX_IO, WireAttr, WireStatFs},
};

const META_TTL: Duration = Duration::from_secs(2);
const DIRECTORY_TTL: Duration = Duration::from_secs(5);
const NEGATIVE_TTL: Duration = Duration::from_secs(1);
const BUDGET_REFRESH: Duration = Duration::from_secs(2);
const MAX_METADATA_ENTRIES: usize = 8192;
const MAX_DIRECTORY_ENTRIES: usize = 1024;
const MAX_READ_AHEAD_BLOCKS: u32 = 32;
const PREFETCH_QUEUE_MAX: usize = 512;
const MIB: u64 = 1024 * 1024;
const GIB: u64 = 1024 * MIB;

type PrefetchJob = Box<dyn FnOnce() + Send + 'static>;

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
    last_end: u64,
    sequential_run: u32,
    read_ahead_window: u32,
    active_reads: usize,
    closing: bool,
}

impl HandleEntry {
    fn new(path: &str) -> Self {
        Self {
            path: path.to_owned(),
            last_end: 0,
            sequential_run: 0,
            read_ahead_window: 0,
            active_reads: 0,
            closing: false,
        }
    }

    fn observe_read(&mut self, offset: u64, new_end: u64) -> u32 {
        let sequential = offset == self.last_end || (self.last_end == 0 && offset == 0);
        if sequential {
            self.sequential_run = self.sequential_run.saturating_add(1);
            self.read_ahead_window = if self.read_ahead_window == 0 {
                1
            } else {
                self.read_ahead_window
                    .saturating_mul(2)
                    .min(MAX_READ_AHEAD_BLOCKS)
            };
        } else {
            self.sequential_run = 0;
            self.read_ahead_window = 0;
        }
        self.last_end = new_end;
        if self.sequential_run >= 2 {
            self.read_ahead_window
        } else {
            0
        }
    }
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
        self.blocks
            .insert(key.clone(), CachedBlock { version, data });
        self.touch(&key);
        self.evict_to_budget(budget);
    }

    fn evict_to_budget(&mut self, budget: usize) {
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

#[derive(Debug, Default)]
struct FetchState {
    cache: BlockCache,
    inflight: HashSet<BlockKey>,
}

#[derive(Debug)]
struct BudgetState {
    total_memory: u64,
    effective: usize,
    checked_at: Instant,
    fixed: bool,
}

impl BudgetState {
    fn detect() -> Self {
        if let Ok(value) = std::env::var("FOLDERBUDDIES_CACHE_BYTES")
            && let Ok(bytes) = value.parse::<u64>()
        {
            return Self {
                total_memory: 0,
                effective: usize::try_from(bytes).unwrap_or(usize::MAX),
                checked_at: Instant::now(),
                fixed: true,
            };
        }

        let (total, available) = detect_memory();
        Self {
            total_memory: total,
            effective: usize::try_from(budget_from_memory(total, available)).unwrap_or(usize::MAX),
            checked_at: Instant::now(),
            fixed: false,
        }
    }

    fn refresh(&mut self) -> usize {
        if !self.fixed && self.checked_at.elapsed() > BUDGET_REFRESH {
            let (_, available) = detect_memory();
            self.effective = usize::try_from(budget_from_memory(self.total_memory, available))
                .unwrap_or(usize::MAX);
            self.checked_at = Instant::now();
        }
        self.effective
    }
}

struct Shared {
    client: Arc<Client>,
    metadata: Mutex<HashMap<String, MetadataEntry>>,
    directories: Mutex<HashMap<String, DirectoryEntryCache>>,
    handles: Mutex<HashMap<u64, HandleEntry>>,
    handle_changed: Condvar,
    fetch: Mutex<FetchState>,
    fetch_changed: Condvar,
    budget: Mutex<BudgetState>,
}

impl Shared {
    fn effective_budget(&self) -> usize {
        self.budget.lock().map_or(0, |mut budget| budget.refresh())
    }

    fn cached_or_fetch_block(
        &self,
        handle: u64,
        key: &BlockKey,
        version: FileVersion,
        block_start: u64,
    ) -> Result<Arc<[u8]>, RemoteError> {
        let mut fetch = match self.fetch.lock() {
            Ok(fetch) => fetch,
            Err(_) => {
                let data = self.client.read(handle, block_start, MAX_IO)?;
                return Ok(data.into());
            }
        };
        loop {
            if let Some(data) = fetch.cache.get(key, version) {
                return Ok(data);
            }
            if !fetch.inflight.contains(key) {
                fetch.inflight.insert(key.clone());
                break;
            }
            fetch = match self.fetch_changed.wait(fetch) {
                Ok(fetch) => fetch,
                Err(_) => {
                    let data = self.client.read(handle, block_start, MAX_IO)?;
                    return Ok(data.into());
                }
            };
        }
        drop(fetch);

        let result = self.client.read(handle, block_start, MAX_IO);
        let budget = self.effective_budget();
        if let Ok(mut fetch) = self.fetch.lock() {
            fetch.inflight.remove(key);
            if let Ok(data) = &result {
                fetch.cache.insert(key.clone(), version, data.clone(), budget);
            }
            fetch.cache.evict_to_budget(budget);
            self.fetch_changed.notify_all();
        } else {
            self.fetch_changed.notify_all();
        }
        result.map(Into::into)
    }

    fn try_begin_prefetch(&self, handle: u64) -> bool {
        let Ok(mut handles) = self.handles.lock() else {
            return false;
        };
        let Some(entry) = handles.get_mut(&handle) else {
            return false;
        };
        if entry.closing {
            return false;
        }
        entry.active_reads = entry.active_reads.saturating_add(1);
        true
    }

    fn end_read(&self, handle: u64) {
        if let Ok(mut handles) = self.handles.lock()
            && let Some(entry) = handles.get_mut(&handle)
        {
            entry.active_reads = entry.active_reads.saturating_sub(1);
            if entry.active_reads == 0 {
                self.handle_changed.notify_all();
            }
        }
    }

    fn prefetch_block(
        self: Arc<Self>,
        handle: u64,
        key: BlockKey,
        version: FileVersion,
        block_start: u64,
    ) {
        if !self.try_begin_prefetch(handle) {
            return;
        }
        let _ = self.cached_or_fetch_block(handle, &key, version, block_start);
        self.end_read(handle);
    }
}

struct PrefetchPool {
    sender: Option<Sender<PrefetchJob>>,
    workers: Vec<JoinHandle<()>>,
}

impl PrefetchPool {
    fn new() -> Self {
        let (sender, receiver) = bounded::<PrefetchJob>(PREFETCH_QUEUE_MAX);
        let worker_count = thread::available_parallelism()
            .map_or(4, |count| count.get())
            .clamp(2, 8);
        let mut workers = Vec::with_capacity(worker_count);
        for _ in 0..worker_count {
            let receiver = receiver.clone();
            workers.push(thread::spawn(move || {
                while let Ok(job) = receiver.recv() {
                    job();
                }
            }));
        }
        Self {
            sender: Some(sender),
            workers,
        }
    }

    fn submit(&self, job: PrefetchJob) {
        let Some(sender) = &self.sender else {
            return;
        };
        match sender.try_send(job) {
            Ok(()) | Err(TrySendError::Full(_)) | Err(TrySendError::Disconnected(_)) => {}
        }
    }
}

impl Drop for PrefetchPool {
    fn drop(&mut self) {
        self.sender.take();
        for worker in self.workers.drain(..) {
            let _ = worker.join();
        }
    }
}

pub struct RamCache {
    prefetch: PrefetchPool,
    shared: Arc<Shared>,
}

impl std::fmt::Debug for RamCache {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("RamCache")
            .field("connected", &self.connected())
            .field("block_budget", &self.block_budget())
            .finish_non_exhaustive()
    }
}

impl RamCache {
    #[must_use]
    pub fn new(client: Arc<Client>) -> Self {
        Self {
            prefetch: PrefetchPool::new(),
            shared: Arc::new(Shared {
                client,
                metadata: Mutex::new(HashMap::new()),
                directories: Mutex::new(HashMap::new()),
                handles: Mutex::new(HashMap::new()),
                handle_changed: Condvar::new(),
                fetch: Mutex::new(FetchState::default()),
                fetch_changed: Condvar::new(),
                budget: Mutex::new(BudgetState::detect()),
            }),
        }
    }

    #[must_use]
    pub fn connected(&self) -> bool {
        self.shared.client.connected()
    }

    #[must_use]
    pub fn block_budget(&self) -> usize {
        self.shared.effective_budget()
    }

    pub fn get_attr(&self, path: &str) -> Result<WireAttr, RemoteError> {
        self.drain_invalidations();
        let now = Instant::now();
        if let Ok(metadata) = self.shared.metadata.lock()
            && let Some(entry) = metadata.get(path)
        {
            let ttl = if entry
                .result
                .as_ref()
                .is_err_and(|error| error.status() == 2)
            {
                NEGATIVE_TTL
            } else {
                META_TTL
            };
            if now.duration_since(entry.inserted) <= ttl {
                return entry.result.clone();
            }
        }

        let result = self.shared.client.get_attr(path);
        if result.is_ok() || result.as_ref().is_err_and(|error| error.status() == 2) {
            self.cache_metadata(path.to_owned(), result.clone(), now);
        }
        result
    }

    pub fn read_dir(&self, path: &str) -> Result<Vec<DirEntry>, RemoteError> {
        self.drain_invalidations();
        let now = Instant::now();
        if let Ok(directories) = self.shared.directories.lock()
            && let Some(entry) = directories.get(path)
            && now.duration_since(entry.inserted) <= DIRECTORY_TTL
        {
            return Ok(entry.entries.clone());
        }

        let entries = self.shared.client.read_dir(path)?;
        for entry in &entries {
            let child = child_path(path, entry.name());
            self.cache_metadata(child, Ok(*entry.attr()), now);
        }
        if let Ok(mut directories) = self.shared.directories.lock() {
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
        let handle = self.shared.client.open(path, flags)?;
        self.track_handle(handle, path);
        Ok(handle)
    }

    pub fn create(&self, path: &str, flags: i32, mode: u32) -> Result<u64, RemoteError> {
        self.drain_invalidations();
        let handle = self.shared.client.create(path, flags, mode)?;
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
        let Some(path) = self.begin_foreground_read(handle) else {
            return self.shared.client.read(handle, offset, amount);
        };
        let result = self.read_tracked(handle, &path, offset, amount);
        if let Ok(data) = &result {
            let new_end = offset.saturating_add(u64::try_from(data.len()).unwrap_or(u64::MAX));
            self.schedule_read_ahead(handle, &path, offset, new_end);
        }
        self.shared.end_read(handle);
        result
    }

    fn read_tracked(
        &self,
        handle: u64,
        path: &str,
        offset: u64,
        amount: u32,
    ) -> Result<Vec<u8>, RemoteError> {
        let budget = self.shared.effective_budget();
        if budget == 0 {
            return self.shared.client.read(handle, offset, amount);
        }
        let attr = self.get_attr(path)?;
        let version = FileVersion::from(&attr);
        if offset >= version.size {
            return Ok(Vec::new());
        }

        let requested_end = offset.saturating_add(u64::from(amount)).min(version.size);
        let mut cursor = offset;
        let capacity = usize::try_from(requested_end.saturating_sub(offset)).unwrap_or(usize::MAX);
        let mut output = Vec::with_capacity(capacity);
        while cursor < requested_end {
            let block_index = cursor / u64::from(MAX_IO);
            let block_start = block_index.saturating_mul(u64::from(MAX_IO));
            let key = BlockKey {
                path: path.to_owned(),
                index: block_index,
            };
            let block = self
                .shared
                .cached_or_fetch_block(handle, &key, version, block_start)?;
            let within = usize::try_from(cursor.saturating_sub(block_start)).unwrap_or(usize::MAX);
            if within >= block.len() {
                break;
            }
            let remaining =
                usize::try_from(requested_end.saturating_sub(cursor)).unwrap_or(usize::MAX);
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
        let result = self.shared.client.write(handle, offset, data);
        if result.is_ok()
            && let Some(path) = self.handle_path(handle)
        {
            self.invalidate_file(&path);
        }
        result
    }

    pub fn release(&self, handle: u64) -> Result<(), RemoteError> {
        self.drain_invalidations();
        if let Ok(mut handles) = self.shared.handles.lock() {
            if let Some(entry) = handles.get_mut(&handle) {
                entry.closing = true;
            }
            loop {
                let active = handles.get(&handle).map_or(0, |entry| entry.active_reads);
                if active == 0 {
                    break;
                }
                handles = match self.shared.handle_changed.wait(handles) {
                    Ok(handles) => handles,
                    Err(poisoned) => poisoned.into_inner(),
                };
            }
        }
        let result = self.shared.client.release(handle);
        if let Ok(mut handles) = self.shared.handles.lock() {
            handles.remove(&handle);
            self.shared.handle_changed.notify_all();
        }
        result
    }

    pub fn fsync(&self, handle: u64) -> Result<(), RemoteError> {
        self.shared.client.fsync(handle)
    }

    pub fn flush(&self, handle: u64) -> Result<(), RemoteError> {
        self.shared.client.flush(handle)
    }

    pub fn mkdir(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.shared.client.mkdir(path, mode);
        if result.is_ok() {
            self.invalidate_file(path);
            self.invalidate_directory(&parent_path(path));
        }
        result
    }

    pub fn unlink(&self, path: &str) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.shared.client.unlink(path);
        if result.is_ok() {
            self.invalidate_file(path);
            self.invalidate_directory(&parent_path(path));
        }
        result
    }

    pub fn rmdir(&self, path: &str) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.shared.client.rmdir(path);
        if result.is_ok() {
            self.invalidate_file(path);
            self.invalidate_directory(path);
            self.invalidate_directory(&parent_path(path));
        }
        result
    }

    pub fn rename(&self, from: &str, to: &str) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.shared.client.rename(from, to);
        if result.is_ok() {
            self.invalidate_file(from);
            self.invalidate_file(to);
            self.invalidate_directory(from);
            self.invalidate_directory(to);
            self.invalidate_directory(&parent_path(from));
            self.invalidate_directory(&parent_path(to));
            if let Ok(mut handles) = self.shared.handles.lock() {
                for handle in handles.values_mut() {
                    if handle.path == from {
                        handle.path = to.to_owned();
                        handle.last_end = 0;
                        handle.sequential_run = 0;
                        handle.read_ahead_window = 0;
                    }
                }
            }
        }
        result
    }

    pub fn truncate(&self, path: &str, size: u64) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.shared.client.truncate(path, size);
        if result.is_ok() {
            self.invalidate_file(path);
        }
        result
    }

    pub fn stat_fs(&self, path: &str) -> Result<WireStatFs, RemoteError> {
        self.shared.client.stat_fs(path)
    }

    pub fn utimens(&self, path: &str, atime: i64, mtime: i64) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.shared.client.utimens(path, atime, mtime);
        if result.is_ok() {
            self.invalidate_metadata(path);
        }
        result
    }

    pub fn chmod(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        self.drain_invalidations();
        let result = self.shared.client.chmod(path, mode);
        if result.is_ok() {
            self.invalidate_metadata(path);
        }
        result
    }

    pub fn access(&self, path: &str, mode: u32) -> Result<(), RemoteError> {
        self.shared.client.access(path, mode)
    }

    fn track_handle(&self, handle: u64, path: &str) {
        if let Ok(mut handles) = self.shared.handles.lock() {
            handles.insert(handle, HandleEntry::new(path));
        }
    }

    fn begin_foreground_read(&self, handle: u64) -> Option<String> {
        let mut handles = self.shared.handles.lock().ok()?;
        loop {
            let entry = handles.get_mut(&handle)?;
            if entry.closing {
                handles = self.shared.handle_changed.wait(handles).ok()?;
                continue;
            }
            entry.active_reads = entry.active_reads.saturating_add(1);
            return Some(entry.path.clone());
        }
    }

    fn handle_path(&self, handle: u64) -> Option<String> {
        self.shared
            .handles
            .lock()
            .ok()
            .and_then(|handles| handles.get(&handle).map(|entry| entry.path.clone()))
    }

    fn schedule_read_ahead(&self, handle: u64, path: &str, offset: u64, new_end: u64) {
        let budget = self.shared.effective_budget();
        if budget == 0 {
            return;
        }
        let window = {
            let Ok(mut handles) = self.shared.handles.lock() else {
                return;
            };
            let Some(entry) = handles.get_mut(&handle) else {
                return;
            };
            if entry.closing || entry.path != path {
                return;
            }
            entry.observe_read(offset, new_end)
        };
        if window == 0 {
            return;
        }

        let Ok(attr) = self.get_attr(path) else {
            return;
        };
        let version = FileVersion::from(&attr);
        let block_size = u64::from(MAX_IO);
        let half_budget_blocks = (budget / MAX_IO as usize / 2).max(1);
        let cap = u32::try_from(half_budget_blocks)
            .unwrap_or(u32::MAX)
            .min(MAX_READ_AHEAD_BLOCKS);
        let window = window.min(cap);
        let first_index = new_end / block_size;

        for delta in 0..window {
            let index = first_index.saturating_add(u64::from(delta));
            let block_start = index.saturating_mul(block_size);
            if block_start >= version.size {
                break;
            }
            let key = BlockKey {
                path: path.to_owned(),
                index,
            };
            let shared = Arc::clone(&self.shared);
            self.prefetch.submit(Box::new(move || {
                shared.prefetch_block(handle, key, version, block_start);
            }));
        }
    }

    fn cache_metadata(
        &self,
        path: String,
        result: Result<WireAttr, RemoteError>,
        inserted: Instant,
    ) {
        if let Ok(mut metadata) = self.shared.metadata.lock() {
            trim_map(&mut metadata, MAX_METADATA_ENTRIES);
            metadata.insert(path, MetadataEntry { result, inserted });
        }
    }

    fn drain_invalidations(&self) {
        for path in self.shared.client.take_invalidations() {
            self.invalidate_file(&path);
            self.invalidate_directory(&path);
            self.invalidate_directory(&parent_path(&path));
        }
    }

    fn invalidate_file(&self, path: &str) {
        self.invalidate_metadata(path);
        if let Ok(mut fetch) = self.shared.fetch.lock() {
            fetch.cache.invalidate_path(path);
        }
        if let Ok(mut handles) = self.shared.handles.lock() {
            for entry in handles.values_mut() {
                if entry.path == path {
                    entry.last_end = 0;
                    entry.sequential_run = 0;
                    entry.read_ahead_window = 0;
                }
            }
        }
    }

    fn invalidate_metadata(&self, path: &str) {
        if let Ok(mut metadata) = self.shared.metadata.lock() {
            metadata.remove(path);
        }
    }

    fn invalidate_directory(&self, path: &str) {
        if let Ok(mut directories) = self.shared.directories.lock() {
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

fn detect_memory() -> (u64, u64) {
    let mut system = System::new();
    system.refresh_memory();
    (system.total_memory(), system.available_memory())
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
        assert_eq!(
            cache.get(&key0, version).as_deref(),
            Some(&[1, 2, 3, 4][..])
        );
        cache.insert(key1.clone(), version, vec![5, 6, 7, 8], 6);
        assert!(cache.get(&key0, version).is_none());
        assert_eq!(
            cache.get(&key1, version).as_deref(),
            Some(&[5, 6, 7, 8][..])
        );
        assert!(
            cache
                .get(&key1, FileVersion { size: 8, mtime: 2 })
                .is_none()
        );
    }

    #[test]
    fn sequential_reads_expand_and_random_reads_reset_prefetch() {
        let mut handle = HandleEntry::new("/big.bin");
        assert_eq!(handle.observe_read(0, 64), 0);
        assert_eq!(handle.observe_read(64, 128), 2);
        assert_eq!(handle.observe_read(128, 192), 4);
        assert_eq!(handle.observe_read(4096, 4160), 0);
        assert_eq!(handle.sequential_run, 0);
        assert_eq!(handle.read_ahead_window, 0);
    }
}
