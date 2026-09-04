use std::{
    collections::HashMap,
    fs,
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

use folderbuddies_core::{
    client::{Client, RemoteError},
    protocol::{MAX_IO, WireAttr},
    ram_cache::RamCache,
};
use fsk::{
    DirectoryEntry, DirectorySink, Error, FileType, Filesystem, Metadata, MountSession,
    ReadDirectoryResult, ROOT_INODE, SetMetadata, StatFs,
};

const ENOENT: i32 = 2;
const EROFS: i32 = 30;
const READ_ONLY_FLAGS: i32 = 0;
const READ_WRITE_FLAGS: i32 = 2;
const CREATE_FLAGS: i32 = READ_WRITE_FLAGS | 0x0100 | 0x0200;
const NS_PER_SECOND: u64 = 1_000_000_000;

#[derive(Clone, Debug)]
struct InodeRecord {
    path: String,
    parent: u64,
}

#[derive(Debug)]
struct InodeIndex {
    next_inode: u64,
    by_inode: HashMap<u64, InodeRecord>,
    by_path: HashMap<String, u64>,
}

impl InodeIndex {
    fn new() -> Self {
        let mut by_inode = HashMap::new();
        let mut by_path = HashMap::new();
        by_inode.insert(
            ROOT_INODE,
            InodeRecord {
                path: "/".to_owned(),
                parent: ROOT_INODE,
            },
        );
        by_path.insert("/".to_owned(), ROOT_INODE);
        Self {
            next_inode: ROOT_INODE + 1,
            by_inode,
            by_path,
        }
    }

    fn record(&self, inode: u64) -> Result<InodeRecord, Error> {
        self.by_inode.get(&inode).cloned().ok_or(Error(ENOENT))
    }

    fn intern(&mut self, path: &str, parent: u64) -> Result<u64, Error> {
        if let Some(inode) = self.by_path.get(path) {
            return Ok(*inode);
        }
        let inode = self.next_inode;
        self.next_inode = self.next_inode.checked_add(1).ok_or(Error::IO)?;
        self.by_inode.insert(
            inode,
            InodeRecord {
                path: path.to_owned(),
                parent,
            },
        );
        self.by_path.insert(path.to_owned(), inode);
        Ok(inode)
    }

    fn remove_subtree(&mut self, path: &str) {
        let prefix = format!("{path}/");
        let doomed: Vec<(u64, String)> = self
            .by_inode
            .iter()
            .filter(|(inode, record)| {
                **inode != ROOT_INODE && (record.path == path || record.path.starts_with(&prefix))
            })
            .map(|(inode, record)| (*inode, record.path.clone()))
            .collect();
        for (inode, old_path) in doomed {
            self.by_inode.remove(&inode);
            self.by_path.remove(&old_path);
        }
    }

    fn rename_subtree(&mut self, from: &str, to: &str, new_parent: u64) {
        let prefix = format!("{from}/");
        let affected: Vec<(u64, String, String)> = self
            .by_inode
            .iter()
            .filter(|(inode, record)| {
                **inode != ROOT_INODE && (record.path == from || record.path.starts_with(&prefix))
            })
            .map(|(inode, record)| {
                let suffix = record.path.strip_prefix(from).unwrap_or_default();
                (*inode, record.path.clone(), format!("{to}{suffix}"))
            })
            .collect();
        for (inode, old_path, new_path) in affected {
            self.by_path.remove(&old_path);
            self.by_path.insert(new_path.clone(), inode);
            if let Some(record) = self.by_inode.get_mut(&inode) {
                record.path = new_path;
                if old_path == from {
                    record.parent = new_parent;
                }
            }
        }
    }
}

struct RemoteFilesystem {
    client: RamCache,
    allow_writes: bool,
    inodes: Mutex<InodeIndex>,
}

impl RemoteFilesystem {
    fn new(client: Arc<Client>, allow_writes: bool) -> Self {
        Self {
            client: RamCache::new(client),
            allow_writes,
            inodes: Mutex::new(InodeIndex::new()),
        }
    }

    fn record(&self, inode: u64) -> Result<InodeRecord, Error> {
        self.inodes.lock().map_err(|_| Error::IO)?.record(inode)
    }

    fn child_path(&self, parent: u64, name: &[u8]) -> Result<String, Error> {
        let name = std::str::from_utf8(name).map_err(|_| Error::INVALID)?;
        if name.is_empty()
            || name == "."
            || name == ".."
            || name.contains('/')
            || name.contains('\\')
        {
            return Err(Error::INVALID);
        }
        let parent_path = self.record(parent)?.path;
        Ok(if parent_path == "/" {
            format!("/{name}")
        } else {
            format!("{parent_path}/{name}")
        })
    }

    fn intern(&self, path: &str, parent: u64) -> Result<u64, Error> {
        self.inodes
            .lock()
            .map_err(|_| Error::IO)?
            .intern(path, parent)
    }

    fn require_writes(&self) -> Result<(), Error> {
        if self.allow_writes {
            Ok(())
        } else {
            Err(Error(EROFS))
        }
    }

    fn metadata_for(&self, inode: u64, record: &InodeRecord, attr: &WireAttr) -> Metadata {
        let modified_ns = seconds_to_ns(attr.mtime());
        Metadata {
            inode,
            parent: record.parent,
            size: attr.size(),
            allocated_size: attr.blocks().saturating_mul(512),
            generation: modified_ns.wrapping_add(attr.size()).max(1),
            created_ns: seconds_to_ns(attr.ctime()),
            modified_ns,
            accessed_ns: seconds_to_ns(attr.atime()),
            mode: attr.mode() & 0o7777,
            uid: attr.uid(),
            gid: attr.gid(),
            link_count: attr.nlink().max(1),
            kind: file_type(attr),
        }
    }

    fn release_after<T>(&self, handle: u64, result: Result<T, Error>) -> Result<T, Error> {
        let release = self.client.release(handle).map_err(remote_error);
        match (result, release) {
            (Err(error), _) => Err(error),
            (Ok(_), Err(error)) => Err(error),
            (Ok(value), Ok(())) => Ok(value),
        }
    }
}

impl Filesystem for RemoteFilesystem {
    fn statfs(&self) -> fsk::Result<StatFs> {
        let stat = self.client.stat_fs("/").map_err(remote_error)?;
        let block_size = u32::try_from(stat.block_size()).unwrap_or(u32::MAX).max(1);
        let block_size_u64 = u64::from(block_size);
        Ok(StatFs {
            block_size,
            io_size: MAX_IO,
            total_bytes: stat.blocks().saturating_mul(block_size_u64),
            free_bytes: stat.blocks_free().saturating_mul(block_size_u64),
            available_bytes: stat.blocks_available().saturating_mul(block_size_u64),
            files: stat.files(),
            free_files: stat.files_free(),
        })
    }

    fn metadata(&self, inode: u64) -> fsk::Result<Metadata> {
        let record = self.record(inode)?;
        let attr = self.client.get_attr(&record.path).map_err(remote_error)?;
        Ok(self.metadata_for(inode, &record, &attr))
    }

    fn set_metadata(&self, inode: u64, update: SetMetadata) -> fsk::Result<Metadata> {
        self.require_writes()?;
        let record = self.record(inode)?;
        let current = self.metadata(inode)?;
        if update.uid.is_some_and(|uid| uid != current.uid)
            || update.gid.is_some_and(|gid| gid != current.gid)
        {
            return Err(Error::NOT_SUPPORTED);
        }
        if let Some(size) = update.size {
            self.client.truncate(&record.path, size).map_err(remote_error)?;
        }
        if let Some(mode) = update.mode {
            self.client.chmod(&record.path, mode & 0o7777).map_err(remote_error)?;
        }
        if update.accessed_ns.is_some() || update.modified_ns.is_some() {
            let atime = ns_to_seconds(update.accessed_ns.unwrap_or(current.accessed_ns));
            let mtime = ns_to_seconds(update.modified_ns.unwrap_or(current.modified_ns));
            self.client
                .utimens(&record.path, atime, mtime)
                .map_err(remote_error)?;
        }
        self.metadata(inode)
    }

    fn lookup(&self, parent: u64, name: &[u8]) -> fsk::Result<(u64, FileType)> {
        let path = self.child_path(parent, name)?;
        let attr = self.client.get_attr(&path).map_err(remote_error)?;
        let inode = self.intern(&path, parent)?;
        Ok((inode, file_type(&attr)))
    }

    fn read_directory(
        &self,
        inode: u64,
        cookie: u64,
        verifier: u64,
        sink: &mut dyn DirectorySink,
    ) -> fsk::Result<ReadDirectoryResult> {
        let record = self.record(inode)?;
        let attr = self.client.get_attr(&record.path).map_err(remote_error)?;
        if file_type(&attr) != FileType::Directory {
            return Err(Error::INVALID);
        }
        let current_verifier = seconds_to_ns(attr.mtime()).wrapping_add(attr.size()).max(1);
        if cookie != 0 && verifier != 0 && verifier != current_verifier {
            return Err(Error::STALE);
        }
        let entries = self.client.read_dir(&record.path).map_err(remote_error)?;
        let start = usize::try_from(cookie).map_err(|_| Error::INVALID)?;
        if start > entries.len() {
            return Err(Error::INVALID);
        }
        let mut next_cookie = cookie;
        for (index, entry) in entries.iter().enumerate().skip(start) {
            let child_path = if record.path == "/" {
                format!("/{}", entry.name())
            } else {
                format!("{}/{}", record.path, entry.name())
            };
            let child_inode = self.intern(&child_path, inode)?;
            let next = u64::try_from(index + 1).map_err(|_| Error::IO)?;
            if !sink.push(DirectoryEntry {
                name: entry.name().as_bytes(),
                inode: child_inode,
                kind: file_type(entry.attr()),
                next_cookie: next,
            }) {
                return Ok(ReadDirectoryResult {
                    verifier: current_verifier,
                    next_cookie,
                    eof: false,
                });
            }
            next_cookie = next;
        }
        Ok(ReadDirectoryResult {
            verifier: current_verifier,
            next_cookie,
            eof: true,
        })
    }

    fn read(&self, inode: u64, offset: u64, output: &mut [u8]) -> fsk::Result<usize> {
        let record = self.record(inode)?;
        let handle = self.client.open(&record.path, READ_ONLY_FLAGS).map_err(remote_error)?;
        let result = (|| {
            let mut done = 0_usize;
            while done < output.len() {
                let chunk_len = (output.len() - done).min(MAX_IO as usize);
                let amount = u32::try_from(chunk_len).map_err(|_| Error::IO)?;
                let chunk_offset = offset
                    .checked_add(u64::try_from(done).map_err(|_| Error::IO)?)
                    .ok_or(Error::INVALID)?;
                let data = self.client.read(handle, chunk_offset, amount).map_err(remote_error)?;
                if data.is_empty() {
                    break;
                }
                let end = done.checked_add(data.len()).ok_or(Error::IO)?;
                output.get_mut(done..end).ok_or(Error::IO)?.copy_from_slice(&data);
                done = end;
                if data.len() < chunk_len {
                    break;
                }
            }
            Ok(done)
        })();
        self.release_after(handle, result)
    }

    fn write(&self, inode: u64, offset: u64, input: &[u8]) -> fsk::Result<usize> {
        self.require_writes()?;
        let record = self.record(inode)?;
        let handle = self.client.open(&record.path, READ_WRITE_FLAGS).map_err(remote_error)?;
        let result = (|| {
            let mut done = 0_usize;
            while done < input.len() {
                let chunk_len = (input.len() - done).min(MAX_IO as usize);
                let chunk_offset = offset
                    .checked_add(u64::try_from(done).map_err(|_| Error::IO)?)
                    .ok_or(Error::INVALID)?;
                let written = self
                    .client
                    .write(handle, chunk_offset, &input[done..done + chunk_len])
                    .map_err(remote_error)?;
                let written = usize::try_from(written).map_err(|_| Error::IO)?;
                if written == 0 {
                    return Err(Error::IO);
                }
                done = done.checked_add(written).ok_or(Error::IO)?;
                if written < chunk_len {
                    break;
                }
            }
            self.client.fsync(handle).map_err(remote_error)?;
            Ok(done)
        })();
        self.release_after(handle, result)
    }

    fn create(&self, parent: u64, name: &[u8], kind: FileType, mode: u32) -> fsk::Result<u64> {
        self.require_writes()?;
        let path = self.child_path(parent, name)?;
        match kind {
            FileType::Directory => self.client.mkdir(&path, mode).map_err(remote_error)?,
            FileType::File => {
                let handle = self.client.create(&path, CREATE_FLAGS, mode).map_err(remote_error)?;
                self.client.release(handle).map_err(remote_error)?;
            }
            FileType::Symlink => return Err(Error::NOT_SUPPORTED),
        }
        self.intern(&path, parent)
    }

    fn remove(&self, parent: u64, name: &[u8], inode: u64) -> fsk::Result<()> {
        self.require_writes()?;
        let path = self.child_path(parent, name)?;
        let attr = self.client.get_attr(&path).map_err(remote_error)?;
        if file_type(&attr) == FileType::Directory {
            self.client.rmdir(&path).map_err(remote_error)?;
        } else {
            self.client.unlink(&path).map_err(remote_error)?;
        }
        let mut index = self.inodes.lock().map_err(|_| Error::IO)?;
        if index.record(inode).is_ok() {
            index.remove_subtree(&path);
        }
        Ok(())
    }

    fn rename(
        &self,
        source_parent: u64,
        source_name: &[u8],
        destination_parent: u64,
        destination_name: &[u8],
        replaced_inode: Option<u64>,
    ) -> fsk::Result<()> {
        self.require_writes()?;
        let source = self.child_path(source_parent, source_name)?;
        let destination = self.child_path(destination_parent, destination_name)?;
        self.client.rename(&source, &destination).map_err(remote_error)?;
        let mut index = self.inodes.lock().map_err(|_| Error::IO)?;
        if let Some(replaced) = replaced_inode
            && let Ok(record) = index.record(replaced)
        {
            index.remove_subtree(&record.path);
        }
        index.rename_subtree(&source, &destination, destination_parent);
        Ok(())
    }

    fn synchronize(&self) -> fsk::Result<()> {
        Ok(())
    }
}

pub(crate) struct Mount {
    session: Option<MountSession>,
    mount_path: PathBuf,
    owned_mount_dir: bool,
}

impl Mount {
    pub(crate) fn start(
        client: Arc<Client>,
        share_name: &str,
        allow_writes: bool,
    ) -> Result<Self, String> {
        if cfg!(windows) && allow_writes {
            return Err(
                "writable Windows mounts are disabled until a safe provider can mirror ProjFS writes back to the host"
                    .to_owned(),
            );
        }
        let mount_path = create_mount_dir(share_name)?;
        let filesystem = RemoteFilesystem::new(client, allow_writes);
        match MountSession::mount(filesystem, &mount_path) {
            Ok(session) => Ok(Self {
                session: Some(session),
                mount_path,
                owned_mount_dir: true,
            }),
            Err(error) => {
                let _ = fs::remove_dir(&mount_path);
                Err(format!("mount failed at {}: {error}", mount_path.display()))
            }
        }
    }

    pub(crate) fn mount_path(&self) -> &Path {
        &self.mount_path
    }

    pub(crate) fn unmount(mut self) -> Result<(), String> {
        if let Some(session) = self.session.take() {
            unmount_session(session);
        }
        if self.owned_mount_dir {
            let _ = fs::remove_dir(&self.mount_path);
            self.owned_mount_dir = false;
        }
        Ok(())
    }
}

#[cfg(target_os = "macos")]
fn unmount_session(session: MountSession) {
    let _ = session.unmount();
}

#[cfg(not(target_os = "macos"))]
fn unmount_session(session: MountSession) {
    session.unmount();
}

impl Drop for Mount {
    fn drop(&mut self) {
        self.session.take();
        if self.owned_mount_dir {
            let _ = fs::remove_dir(&self.mount_path);
        }
    }
}

fn remote_error(error: RemoteError) -> Error {
    let status = i32::from(error.status());
    if status > 0 {
        Error(status)
    } else {
        Error::IO
    }
}

fn file_type(attr: &WireAttr) -> FileType {
    if attr.mode() & 0o170000 == 0o040000 {
        FileType::Directory
    } else {
        FileType::File
    }
}

fn seconds_to_ns(seconds: i64) -> u64 {
    u64::try_from(seconds)
        .unwrap_or(0)
        .saturating_mul(NS_PER_SECOND)
}

fn ns_to_seconds(nanoseconds: u64) -> i64 {
    i64::try_from(nanoseconds / NS_PER_SECOND).unwrap_or(i64::MAX)
}

fn create_mount_dir(share_name: &str) -> Result<PathBuf, String> {
    let label = sanitized_label(share_name);
    let root = std::env::temp_dir();
    let pid = std::process::id();
    for suffix in 0_u32..64 {
        let name = if suffix == 0 {
            format!("folderbuddies-{label}-{pid}")
        } else {
            format!("folderbuddies-{label}-{pid}-{suffix}")
        };
        let path = root.join(name);
        match fs::create_dir(&path) {
            Ok(()) => return Ok(path),
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => {
                return Err(format!(
                    "failed to create mount directory {}: {error}",
                    path.display()
                ));
            }
        }
    }
    Err("could not allocate a unique mount directory".to_owned())
}

fn sanitized_label(label: &str) -> String {
    let sanitized: String = label
        .chars()
        .take(48)
        .map(|character| {
            if character.is_ascii_alphanumeric() || matches!(character, '-' | '_' | '.') {
                character
            } else {
                '_'
            }
        })
        .collect();
    if sanitized.is_empty() {
        "share".to_owned()
    } else {
        sanitized
    }
}