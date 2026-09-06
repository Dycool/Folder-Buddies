use std::{
    collections::HashMap,
    fs::{self, File, OpenOptions},
    io::{Read, Seek, SeekFrom, Write},
    path::{Component, Path, PathBuf},
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};

use serde_json::{Value, json};

pub const WEB_BINARY_MAGIC: u32 = 0x4642_494e;
pub const WEB_CHUNK: usize = 64 * 1024;

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum WebOutbound {
    Text(String),
    Binary(Vec<u8>),
}

#[derive(Debug)]
pub struct WebProtocolHost {
    root: PathBuf,
    allow_writes: bool,
    uploads: Mutex<HashMap<u32, File>>,
}

impl WebProtocolHost {
    pub fn new(root: impl AsRef<Path>, allow_writes: bool) -> Result<Self, String> {
        let root = root.as_ref();
        if is_boundary_link(root).map_err(|error| error.to_string())? {
            return Err("Cannot host a symlink, junction, or projected filesystem root".to_owned());
        }
        let root = fs::canonicalize(root).map_err(|error| error.to_string())?;
        if !root.is_dir() {
            return Err("Not a directory".to_owned());
        }
        Ok(Self {
            root,
            allow_writes,
            uploads: Mutex::new(HashMap::new()),
        })
    }

    #[must_use]
    pub const fn allow_writes(&self) -> bool {
        self.allow_writes
    }

    pub fn handle_text(&self, text: &str) -> Vec<WebOutbound> {
        let message = match serde_json::from_str::<Value>(text) {
            Ok(Value::Object(object)) => Value::Object(object),
            _ => return Vec::new(),
        };
        let id = json_id(&message);
        match self.handle_json(&message) {
            Ok(messages) => messages,
            Err(error) => vec![WebOutbound::Text(compact(&json!({
                "t": "error",
                "id": id,
                "message": error,
            })))],
        }
    }

    pub fn handle_binary(&self, bytes: &[u8]) -> u64 {
        let Some((id, payload)) = decode_binary_frame(bytes) else {
            return 0;
        };
        let Ok(mut uploads) = self.uploads.lock() else {
            return 0;
        };
        let Some(file) = uploads.get_mut(&id) else {
            return 0;
        };
        if file.write_all(payload).is_ok() {
            u64::try_from(payload.len()).unwrap_or(u64::MAX)
        } else {
            0
        }
    }

    fn handle_json(&self, message: &Value) -> Result<Vec<WebOutbound>, String> {
        let kind = message.get("t").and_then(Value::as_str).unwrap_or_default();
        let id = json_id(message);
        match kind {
            "list" => self.list(id, message),
            "download" => self.download(id, message),
            "uploadStart" => self.upload_start(id, message),
            "mkdir" => self.mkdir(id, message),
            "uploadEnd" => self.upload_end(id),
            "delete" => self.delete(id, message),
            _ => Err("Unknown request".to_owned()),
        }
    }

    fn list(&self, id: i64, message: &Value) -> Result<Vec<WebOutbound>, String> {
        let path = message.get("path").and_then(Value::as_str).unwrap_or("/");
        let absolute = self
            .resolve(path)
            .ok_or_else(|| "Not a directory".to_owned())?;
        if !absolute.is_dir() {
            return Err("Not a directory".to_owned());
        }
        let mut entries = Vec::new();
        let mtime = unix_millis() as f64;
        let directory = fs::read_dir(&absolute).map_err(|_| "Not a directory".to_owned())?;
        for item in directory {
            let item = match item {
                Ok(item) => item,
                Err(_) => continue,
            };
            let item_path = item.path();
            if is_boundary_link(&item_path).unwrap_or(true) {
                continue;
            }
            let file_type = match item.file_type() {
                Ok(file_type) => file_type,
                Err(_) => continue,
            };
            let name = item.file_name().to_string_lossy().into_owned();
            let child = if path == "/" {
                format!("/{name}")
            } else {
                format!("{path}/{name}")
            };
            let mut entry = json!({
                "name": name,
                "path": child,
                "kind": if file_type.is_dir() { "directory" } else { "file" },
                "mtime": mtime,
            });
            if file_type.is_file()
                && let Ok(metadata) = item.metadata()
            {
                entry["size"] = Value::from(metadata.len() as f64);
            }
            entries.push(entry);
        }
        Ok(vec![WebOutbound::Text(compact(&json!({
            "t": "listResult",
            "id": id,
            "path": path,
            "entries": entries,
            "write": self.allow_writes,
            "ranges": true,
        })))])
    }

    fn download(&self, id: i64, message: &Value) -> Result<Vec<WebOutbound>, String> {
        let path = message.get("path").and_then(Value::as_str).unwrap_or_default();
        let absolute = self.resolve(path).ok_or_else(|| "Not a file".to_owned())?;
        if !absolute.is_file() {
            return Err("Not a file".to_owned());
        }
        let size = fs::metadata(&absolute)
            .map_err(|_| "Not a file".to_owned())?
            .len();
        let offset = nonnegative_u64(message.get("offset")).min(size);
        let mut remaining = size.saturating_sub(offset);
        if message.get("length").is_some() {
            remaining = remaining.min(nonnegative_u64(message.get("length")));
        }
        let name = absolute
            .file_name()
            .map_or_else(String::new, |name| name.to_string_lossy().into_owned());
        let mut output = vec![WebOutbound::Text(compact(&json!({
            "t": "fileStart",
            "id": id,
            "name": name,
            "size": remaining as f64,
            "offset": offset as f64,
        })))];
        let mut file = File::open(&absolute).map_err(|_| "Not a file".to_owned())?;
        file.seek(SeekFrom::Start(offset))
            .map_err(|_| "Not a file".to_owned())?;
        let mut buffer = vec![0_u8; WEB_CHUNK];
        while remaining > 0 {
            let wanted = usize::try_from(remaining.min(WEB_CHUNK as u64)).unwrap_or(WEB_CHUNK);
            let read = file
                .read(&mut buffer[..wanted])
                .map_err(|_| "Not a file".to_owned())?;
            if read == 0 {
                break;
            }
            output.push(WebOutbound::Binary(encode_binary_frame(
                u32::try_from(id).unwrap_or_default(),
                &buffer[..read],
            )));
            remaining = remaining.saturating_sub(read as u64);
        }
        output.push(WebOutbound::Text(compact(&json!({
            "t": "fileEnd",
            "id": id,
        }))));
        Ok(output)
    }

    fn upload_start(&self, id: i64, message: &Value) -> Result<Vec<WebOutbound>, String> {
        self.require_writes()?;
        let path = message.get("path").and_then(Value::as_str).unwrap_or_default();
        let absolute = self
            .resolve_write(path)
            .ok_or_else(|| "Bad upload path".to_owned())?;
        if let Some(parent) = absolute.parent() {
            fs::create_dir_all(parent).map_err(|_| "Bad upload path".to_owned())?;
        }
        let file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&absolute)
            .map_err(|_| "Bad upload path".to_owned())?;
        self.uploads
            .lock()
            .map_err(|_| "Bad upload path".to_owned())?
            .insert(u32::try_from(id).unwrap_or_default(), file);
        Ok(vec![WebOutbound::Text(compact(&json!({
            "t": "uploadReady",
            "id": id,
        })))])
    }

    fn mkdir(&self, id: i64, message: &Value) -> Result<Vec<WebOutbound>, String> {
        self.require_writes()?;
        let path = message.get("path").and_then(Value::as_str).unwrap_or_default();
        let absolute = self
            .resolve_write(path)
            .ok_or_else(|| "Bad path".to_owned())?;
        fs::create_dir_all(absolute).map_err(|_| "Create folder failed".to_owned())?;
        Ok(vec![ok_message(id)])
    }

    fn upload_end(&self, id: i64) -> Result<Vec<WebOutbound>, String> {
        self.uploads
            .lock()
            .map_err(|_| "Unknown request".to_owned())?
            .remove(&u32::try_from(id).unwrap_or_default());
        Ok(vec![ok_message(id)])
    }

    fn delete(&self, id: i64, message: &Value) -> Result<Vec<WebOutbound>, String> {
        self.require_writes()?;
        let path = message.get("path").and_then(Value::as_str).unwrap_or_default();
        let absolute = self.resolve(path).ok_or_else(|| "Bad path".to_owned())?;
        let result = if !absolute.exists() {
            Ok(())
        } else if absolute.is_dir() {
            fs::remove_dir_all(absolute)
        } else {
            fs::remove_file(absolute)
        };
        result.map_err(|_| "Delete failed".to_owned())?;
        Ok(vec![ok_message(id)])
    }

    fn require_writes(&self) -> Result<(), String> {
        if self.allow_writes {
            Ok(())
        } else {
            Err("The host has writes disabled".to_owned())
        }
    }

    fn resolve(&self, relative: &str) -> Option<PathBuf> {
        self.resolve_common(relative)
    }

    fn resolve_write(&self, relative: &str) -> Option<PathBuf> {
        self.resolve_common(relative)
    }

    fn resolve_common(&self, relative: &str) -> Option<PathBuf> {
        let normalized = normalize_relative(relative)?;
        let mut current = self.root.clone();
        let mut tail = PathBuf::new();
        let mut missing = false;
        for part in normalized.split('/').filter(|part| !part.is_empty()) {
            if missing {
                tail.push(part);
                continue;
            }
            let next = current.join(part);
            match fs::symlink_metadata(&next) {
                Ok(_) => {
                    if is_boundary_link(&next).ok()? {
                        return None;
                    }
                    current = fs::canonicalize(&next).ok()?;
                    if !path_within(&self.root, &current) {
                        return None;
                    }
                }
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                    missing = true;
                    tail.push(part);
                }
                Err(_) => return None,
            }
        }
        let candidate = if tail.as_os_str().is_empty() {
            current
        } else {
            current.join(tail)
        };
        path_within(&self.root, &candidate).then_some(candidate)
    }
}

#[must_use]
pub fn encode_binary_frame(id: u32, payload: &[u8]) -> Vec<u8> {
    let mut output = Vec::with_capacity(8 + payload.len());
    output.extend_from_slice(&WEB_BINARY_MAGIC.to_be_bytes());
    output.extend_from_slice(&id.to_be_bytes());
    output.extend_from_slice(payload);
    output
}

#[must_use]
pub fn decode_binary_frame(bytes: &[u8]) -> Option<(u32, &[u8])> {
    if bytes.len() < 8 {
        return None;
    }
    let magic = u32::from_be_bytes(bytes[..4].try_into().ok()?);
    if magic != WEB_BINARY_MAGIC {
        return None;
    }
    let id = u32::from_be_bytes(bytes[4..8].try_into().ok()?);
    Some((id, &bytes[8..]))
}

fn normalize_relative(input: &str) -> Option<String> {
    let mut path = if input.is_empty() {
        "/".to_owned()
    } else {
        input.to_owned()
    };
    if !path.starts_with('/') {
        path.insert(0, '/');
    }
    let mut components = Vec::new();
    for component in Path::new(&path).components() {
        match component {
            Component::RootDir | Component::CurDir => {}
            Component::Normal(part) => components.push(part.to_string_lossy().into_owned()),
            Component::ParentDir | Component::Prefix(_) => return None,
        }
    }
    Some(if components.is_empty() {
        "/".to_owned()
    } else {
        format!("/{}", components.join("/"))
    })
}

fn path_within(root: &Path, candidate: &Path) -> bool {
    candidate == root || candidate.starts_with(root)
}

#[cfg(windows)]
fn is_boundary_link(path: &Path) -> Result<bool, std::io::Error> {
    use std::os::windows::fs::MetadataExt as _;
    const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
    let metadata = fs::symlink_metadata(path)?;
    Ok(metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0)
}

#[cfg(not(windows))]
fn is_boundary_link(path: &Path) -> Result<bool, std::io::Error> {
    Ok(fs::symlink_metadata(path)?.file_type().is_symlink())
}

fn json_id(message: &Value) -> i64 {
    message.get("id").and_then(Value::as_i64).unwrap_or_default()
}

fn nonnegative_u64(value: Option<&Value>) -> u64 {
    value
        .and_then(Value::as_f64)
        .unwrap_or(0.0)
        .max(0.0)
        .min(u64::MAX as f64) as u64
}

fn compact(value: &Value) -> String {
    serde_json::to_string(value).unwrap_or_else(|_| "{}".to_owned())
}

fn ok_message(id: i64) -> WebOutbound {
    WebOutbound::Text(compact(&json!({"t": "ok", "id": id})))
}

fn unix_millis() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |duration| {
            i64::try_from(duration.as_millis()).unwrap_or(i64::MAX)
        })
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicU64, Ordering};

    use super::*;

    static NEXT_TEMP_ROOT: AtomicU64 = AtomicU64::new(0);

    fn temporary_root() -> PathBuf {
        let unique = format!(
            "folderbuddies-web-protocol-{}-{}-{}",
            std::process::id(),
            unix_millis(),
            NEXT_TEMP_ROOT.fetch_add(1, Ordering::Relaxed)
        );
        let root = std::env::temp_dir().join(unique);
        fs::create_dir_all(&root).expect("temp root");
        root
    }

    #[test]
    fn binary_frame_matches_cpp_contract() {
        let frame = encode_binary_frame(0x0102_0304, b"abc");
        assert_eq!(&frame[..8], b"FBIN\x01\x02\x03\x04");
        let (id, payload) = decode_binary_frame(&frame).expect("frame");
        assert_eq!(id, 0x0102_0304);
        assert_eq!(payload, b"abc");
    }

    #[test]
    fn read_only_host_rejects_mutations_with_cpp_text() {
        let root = temporary_root();
        let host = WebProtocolHost::new(&root, false).expect("host");
        let reply = host.handle_text(r#"{"t":"mkdir","id":7,"path":"/x"}"#);
        let WebOutbound::Text(reply) = &reply[0] else {
            panic!("text reply");
        };
        assert!(reply.contains("The host has writes disabled"));
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn ranged_download_uses_file_start_binary_file_end() {
        let root = temporary_root();
        fs::write(root.join("file.bin"), b"0123456789").expect("write");
        let host = WebProtocolHost::new(&root, false).expect("host");
        let reply = host.handle_text(
            r#"{"t":"download","id":9,"path":"/file.bin","offset":2,"length":4}"#,
        );
        assert!(reply.len() >= 3);
        let WebOutbound::Text(start) = &reply[0] else {
            panic!("start");
        };
        assert!(start.contains("\"size\":4.0"));

        let mut payload = Vec::new();
        for frame in &reply[1..reply.len() - 1] {
            let WebOutbound::Binary(binary) = frame else {
                panic!("binary");
            };
            let (id, chunk) = decode_binary_frame(binary).expect("frame");
            assert_eq!(id, 9);
            payload.extend_from_slice(chunk);
        }
        assert_eq!(payload, b"2345");

        let WebOutbound::Text(end) = &reply[reply.len() - 1] else {
            panic!("end");
        };
        assert!(end.contains("\"t\":\"fileEnd\""));
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn upload_round_trip_matches_fbin_protocol() {
        let root = temporary_root();
        let host = WebProtocolHost::new(&root, true).expect("host");
        let ready = host.handle_text(r#"{"t":"uploadStart","id":5,"path":"/new.bin"}"#);
        assert!(matches!(&ready[0], WebOutbound::Text(text) if text.contains("uploadReady")));
        assert_eq!(host.handle_binary(&encode_binary_frame(5, b"hello")), 5);
        let done = host.handle_text(r#"{"t":"uploadEnd","id":5}"#);
        assert!(matches!(&done[0], WebOutbound::Text(text) if text.contains("\"t\":\"ok\"")));
        assert_eq!(fs::read(root.join("new.bin")).expect("read"), b"hello");
        fs::remove_dir_all(root).expect("cleanup");
    }

    #[test]
    fn parent_escape_fails_closed() {
        let root = temporary_root();
        let host = WebProtocolHost::new(&root, true).expect("host");
        let reply = host.handle_text(r#"{"t":"uploadStart","id":1,"path":"/../escape"}"#);
        assert!(matches!(&reply[0], WebOutbound::Text(text) if text.contains("Bad upload path")));
        fs::remove_dir_all(root).expect("cleanup");
    }
}
