use std::{
    collections::HashMap,
    ffi::c_void,
    fs::File,
    io::Read,
    mem::size_of,
    panic::{AssertUnwindSafe, catch_unwind},
    path::{Path, PathBuf},
    ptr,
    slice,
    sync::{Arc, Mutex},
};

use folderbuddies_core::{
    protocol::{MAX_IO, WireAttr},
    remote_fs::{RemoteFs, RemoteFsError},
};
use libloading::Library;
use windows::{
    Win32::Storage::ProjectedFileSystem::{
        PRJ_CALLBACKS, PRJ_CALLBACK_DATA, PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN,
        PRJ_DIR_ENTRY_BUFFER_HANDLE, PRJ_FILE_BASIC_INFO, PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT,
        PRJ_NOTIFICATION, PRJ_NOTIFICATION_MAPPING, PRJ_NOTIFICATION_PARAMETERS,
        PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_DELETED,
        PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_MODIFIED, PRJ_NOTIFICATION_FILE_OPENED,
        PRJ_NOTIFICATION_FILE_OVERWRITTEN, PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL,
        PRJ_NOTIFICATION_FILE_RENAMED, PRJ_NOTIFICATION_HARDLINK_CREATED,
        PRJ_NOTIFICATION_NEW_FILE_CREATED, PRJ_NOTIFICATION_PRE_DELETE,
        PRJ_NOTIFICATION_PRE_RENAME, PRJ_NOTIFICATION_PRE_SET_HARDLINK, PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_DELETED,
        PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_MODIFIED, PRJ_NOTIFY_FILE_OPENED,
        PRJ_NOTIFY_FILE_OVERWRITTEN, PRJ_NOTIFY_FILE_PRE_CONVERT_TO_FULL,
        PRJ_NOTIFY_FILE_RENAMED, PRJ_NOTIFY_HARDLINK_CREATED, PRJ_NOTIFY_NEW_FILE_CREATED,
        PRJ_NOTIFY_PRE_DELETE, PRJ_NOTIFY_PRE_RENAME, PRJ_NOTIFY_PRE_SET_HARDLINK,
        PRJ_NOTIFY_TYPES, PRJ_PLACEHOLDER_INFO, PRJ_PLACEHOLDER_VERSION_INFO,
        PRJ_STARTVIRTUALIZING_OPTIONS,
    },
    core::{GUID, HRESULT, PCWSTR},
};

use crate::drive::wide;

const EACCES: i16 = 13;
const EEXIST: i16 = 17;
const EISDIR: i16 = 21;
const ENOENT: i16 = 2;
const ENOTDIR: i16 = 20;

const ERROR_FILE_NOT_FOUND: u32 = 2;
const ERROR_ACCESS_DENIED: u32 = 5;
const ERROR_INVALID_DATA: u32 = 13;
const ERROR_GEN_FAILURE: u32 = 31;
const ERROR_NOT_SUPPORTED: u32 = 50;
const ERROR_CANNOT_MAKE: u32 = 82;
const ERROR_INVALID_PARAMETER: u32 = 87;
const ERROR_INSUFFICIENT_BUFFER: u32 = 122;
const ERROR_DIRECTORY: u32 = 267;

const FB_O_WRONLY: i32 = 1;
const FB_O_TRUNC: i32 = 0x0400;

const FILE_ATTRIBUTE_READONLY: u32 = 0x0000_0001;
const FILE_ATTRIBUTE_DIRECTORY: u32 = 0x0000_0010;
const FILE_ATTRIBUTE_NORMAL: u32 = 0x0000_0080;

const S_OK: HRESULT = HRESULT(0);

type MarkDirectoryAsPlaceholderFn = unsafe extern "system" fn(
    PCWSTR,
    PCWSTR,
    *const PRJ_PLACEHOLDER_VERSION_INFO,
    *const GUID,
) -> HRESULT;
type StartVirtualizingFn = unsafe extern "system" fn(
    PCWSTR,
    *const PRJ_CALLBACKS,
    *const c_void,
    *const PRJ_STARTVIRTUALIZING_OPTIONS,
    *mut PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT,
) -> HRESULT;
type StopVirtualizingFn = unsafe extern "system" fn(PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT);
type WritePlaceholderInfoFn = unsafe extern "system" fn(
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT,
    PCWSTR,
    *const PRJ_PLACEHOLDER_INFO,
    u32,
) -> HRESULT;
type WriteFileDataFn = unsafe extern "system" fn(
    PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT,
    *const GUID,
    *const c_void,
    u64,
    u32,
) -> HRESULT;
type FillDirEntryBufferFn = unsafe extern "system" fn(
    PCWSTR,
    *mut PRJ_FILE_BASIC_INFO,
    PRJ_DIR_ENTRY_BUFFER_HANDLE,
) -> HRESULT;
type FileNameMatchFn = unsafe extern "system" fn(PCWSTR, PCWSTR) -> bool;

struct ProjfsApi {
    _library: Library,
    mark_directory_as_placeholder: MarkDirectoryAsPlaceholderFn,
    start_virtualizing: StartVirtualizingFn,
    stop_virtualizing: StopVirtualizingFn,
    write_placeholder_info: WritePlaceholderInfoFn,
    write_file_data: WriteFileDataFn,
    fill_dir_entry_buffer: FillDirEntryBufferFn,
    file_name_match: FileNameMatchFn,
}

impl ProjfsApi {
    fn load() -> Result<Arc<Self>, String> {
        // SAFETY: ProjectedFSLib.dll is a Windows system component. The Library is kept alive
        // for at least as long as every function pointer copied from it.
        let library = unsafe { Library::new("ProjectedFSLib.dll") }
            .map_err(|_| "ProjectedFSLib.dll could not be loaded".to_owned())?;
        // SAFETY: each symbol name and function signature is copied from projectedfslib.h.
        let mark_directory_as_placeholder = unsafe {
            *library
                .get::<MarkDirectoryAsPlaceholderFn>(b"PrjMarkDirectoryAsPlaceholder\0")
                .map_err(|_| "ProjectedFSLib.dll could not be loaded".to_owned())?
        };
        // SAFETY: symbol signature matches projectedfslib.h.
        let start_virtualizing = unsafe {
            *library
                .get::<StartVirtualizingFn>(b"PrjStartVirtualizing\0")
                .map_err(|_| "ProjectedFSLib.dll could not be loaded".to_owned())?
        };
        // SAFETY: symbol signature matches projectedfslib.h.
        let stop_virtualizing = unsafe {
            *library
                .get::<StopVirtualizingFn>(b"PrjStopVirtualizing\0")
                .map_err(|_| "ProjectedFSLib.dll could not be loaded".to_owned())?
        };
        // SAFETY: symbol signature matches projectedfslib.h.
        let write_placeholder_info = unsafe {
            *library
                .get::<WritePlaceholderInfoFn>(b"PrjWritePlaceholderInfo\0")
                .map_err(|_| "ProjectedFSLib.dll could not be loaded".to_owned())?
        };
        // SAFETY: symbol signature matches projectedfslib.h.
        let write_file_data = unsafe {
            *library
                .get::<WriteFileDataFn>(b"PrjWriteFileData\0")
                .map_err(|_| "ProjectedFSLib.dll could not be loaded".to_owned())?
        };
        // SAFETY: symbol signature matches projectedfslib.h.
        let fill_dir_entry_buffer = unsafe {
            *library
                .get::<FillDirEntryBufferFn>(b"PrjFillDirEntryBuffer\0")
                .map_err(|_| "ProjectedFSLib.dll could not be loaded".to_owned())?
        };
        // SAFETY: symbol signature matches projectedfslib.h.
        let file_name_match = unsafe {
            *library
                .get::<FileNameMatchFn>(b"PrjFileNameMatch\0")
                .map_err(|_| "ProjectedFSLib.dll could not be loaded".to_owned())?
        };
        Ok(Arc::new(Self {
            _library: library,
            mark_directory_as_placeholder,
            start_virtualizing,
            stop_virtualizing,
            write_placeholder_info,
            write_file_data,
            fill_dir_entry_buffer,
            file_name_match,
        }))
    }
}

struct EnumEntry {
    name: String,
    wide_name: Vec<u16>,
    info: PRJ_FILE_BASIC_INFO,
}

struct EnumSession {
    entries: Vec<EnumEntry>,
    cursor: usize,
    have_expression: bool,
    expression: Vec<u16>,
}

struct ProjfsState {
    client: Arc<dyn RemoteFs>,
    api: Arc<ProjfsApi>,
    allow_writes: bool,
    root: PathBuf,
    enumerations: Mutex<HashMap<u128, EnumSession>>,
}

pub(crate) struct Projection {
    api: Arc<ProjfsApi>,
    context: PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT,
    state: *mut ProjfsState,
}

impl Projection {
    pub(crate) fn start(
        root: &Path,
        client: Arc<dyn RemoteFs>,
        allow_writes: bool,
    ) -> Result<Self, String> {
        let api = ProjfsApi::load()?;
        let root_wide = wide(&root.to_string_lossy());
        let mut random = [0_u8; 16];
        getrandom::fill(&mut random).map_err(|error| format!("UuidCreate failed: {error}"))?;
        let instance_id = GUID::from_u128(u128::from_le_bytes(random));

        // SAFETY: all pointers refer to live, NUL-terminated buffers/values for the duration of the call.
        let marked = unsafe {
            (api.mark_directory_as_placeholder)(
                PCWSTR(root_wide.as_ptr()),
                PCWSTR::null(),
                ptr::null(),
                &instance_id,
            )
        };
        if failed(marked) && marked != hresult_from_win32(183) {
            return Err(format!(
                "MarkDirectoryAsPlaceholder failed ({}) for {}",
                hresult_hex(marked),
                root.display()
            ));
        }

        let state = Box::new(ProjfsState {
            client,
            api: Arc::clone(&api),
            allow_writes,
            root: root.to_path_buf(),
            enumerations: Mutex::new(HashMap::new()),
        });
        let state = Box::into_raw(state);

        let callbacks = PRJ_CALLBACKS {
            StartDirectoryEnumerationCallback: Some(start_enum_cb),
            EndDirectoryEnumerationCallback: Some(end_enum_cb),
            GetDirectoryEnumerationCallback: Some(get_enum_cb),
            GetPlaceholderInfoCallback: Some(placeholder_cb),
            GetFileDataCallback: Some(file_data_cb),
            NotificationCallback: Some(notification_cb),
            ..Default::default()
        };
        let empty = [0_u16];
        let mut mapping = PRJ_NOTIFICATION_MAPPING {
            NotificationRoot: PCWSTR(empty.as_ptr()),
            NotificationBitMask: write_notify_mask(),
        };
        let options = PRJ_STARTVIRTUALIZING_OPTIONS {
            NotificationMappings: &mut mapping,
            NotificationMappingsCount: 1,
            ..Default::default()
        };
        let mut context = PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT::default();
        // SAFETY: `state` is a Box allocation retained until after PrjStopVirtualizing. ProjFS copies
        // callbacks/options during this call; the callback functions have static addresses.
        let started = unsafe {
            (api.start_virtualizing)(
                PCWSTR(root_wide.as_ptr()),
                &callbacks,
                state.cast(),
                &options,
                &mut context,
            )
        };
        if failed(started) {
            // SAFETY: start failed, so ProjFS cannot retain/use the instance context.
            unsafe { drop(Box::from_raw(state)) };
            return Err(format!(
                "StartVirtualizing failed ({}) for {}",
                hresult_hex(started),
                root.display()
            ));
        }
        Ok(Self {
            api,
            context,
            state,
        })
    }
}

impl Drop for Projection {
    fn drop(&mut self) {
        // SAFETY: this context was returned by a successful PrjStartVirtualizing call. The API
        // contract guarantees PrjStopVirtualizing waits for outstanding callbacks to finish.
        unsafe { (self.api.stop_virtualizing)(self.context) };
        if !self.state.is_null() {
            // SAFETY: callbacks have finished after PrjStopVirtualizing and this pointer came from
            // exactly one Box::into_raw in Projection::start.
            unsafe { drop(Box::from_raw(self.state)) };
            self.state = ptr::null_mut();
        }
    }
}

fn write_notify_mask() -> PRJ_NOTIFY_TYPES {
    PRJ_NOTIFY_FILE_OPENED
        | PRJ_NOTIFY_NEW_FILE_CREATED
        | PRJ_NOTIFY_FILE_OVERWRITTEN
        | PRJ_NOTIFY_PRE_DELETE
        | PRJ_NOTIFY_PRE_RENAME
        | PRJ_NOTIFY_PRE_SET_HARDLINK
        | PRJ_NOTIFY_FILE_RENAMED
        | PRJ_NOTIFY_HARDLINK_CREATED
        | PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_MODIFIED
        | PRJ_NOTIFY_FILE_HANDLE_CLOSED_FILE_DELETED
        | PRJ_NOTIFY_FILE_PRE_CONVERT_TO_FULL
}

unsafe extern "system" fn start_enum_cb(
    callback_data: *const PRJ_CALLBACK_DATA,
    enumeration_id: *const GUID,
) -> HRESULT {
    ffi_guard(|| {
        // SAFETY: ProjFS supplies valid callback/enumeration pointers for this callback invocation.
        let (data, state) = unsafe { callback_state(callback_data) }?;
        if enumeration_id.is_null() {
            return Err(hresult_from_win32(ERROR_INVALID_PARAMETER));
        }
        // SAFETY: null was rejected above and the GUID is valid for the callback duration.
        let id = unsafe { (*enumeration_id).to_u128() };
        // SAFETY: FilePathName belongs to the live callback data.
        let path = unsafe { remote_path(data.FilePathName) };
        let entries = query_dir(state, &path)?;
        state
            .enumerations
            .lock()
            .map_err(|_| hresult_from_win32(ERROR_GEN_FAILURE))?
            .insert(
                id,
                EnumSession {
                    entries,
                    cursor: 0,
                    have_expression: false,
                    expression: Vec::new(),
                },
            );
        Ok(())
    })
}

unsafe extern "system" fn end_enum_cb(
    callback_data: *const PRJ_CALLBACK_DATA,
    enumeration_id: *const GUID,
) -> HRESULT {
    ffi_guard(|| {
        // SAFETY: ProjFS supplies valid callback/enumeration pointers for this callback invocation.
        let (_, state) = unsafe { callback_state(callback_data) }?;
        if enumeration_id.is_null() {
            return Err(hresult_from_win32(ERROR_INVALID_PARAMETER));
        }
        // SAFETY: null was rejected above and the GUID is valid for the callback duration.
        let id = unsafe { (*enumeration_id).to_u128() };
        state
            .enumerations
            .lock()
            .map_err(|_| hresult_from_win32(ERROR_GEN_FAILURE))?
            .remove(&id);
        Ok(())
    })
}

unsafe extern "system" fn get_enum_cb(
    callback_data: *const PRJ_CALLBACK_DATA,
    enumeration_id: *const GUID,
    search_expression: PCWSTR,
    dir_entry_buffer: PRJ_DIR_ENTRY_BUFFER_HANDLE,
) -> HRESULT {
    ffi_guard(|| {
        // SAFETY: ProjFS supplies valid callback/enumeration pointers for this callback invocation.
        let (data, state) = unsafe { callback_state(callback_data) }?;
        if enumeration_id.is_null() {
            return Err(hresult_from_win32(ERROR_INVALID_PARAMETER));
        }
        // SAFETY: null was rejected above and the GUID is valid for the callback duration.
        let id = unsafe { (*enumeration_id).to_u128() };
        let mut sessions = state
            .enumerations
            .lock()
            .map_err(|_| hresult_from_win32(ERROR_GEN_FAILURE))?;
        let session = sessions
            .get_mut(&id)
            .ok_or_else(|| hresult_from_win32(ERROR_INVALID_PARAMETER))?;
        let restart = data.Flags.0 & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN.0 != 0;
        if restart || !session.have_expression {
            session.cursor = 0;
            session.have_expression = true;
            // SAFETY: SearchExpression belongs to the live callback and may be null.
            session.expression = unsafe { wide_to_vec(search_expression) };
        }
        let mut wrote_entry = false;
        while session.cursor < session.entries.len() {
            let entry = &session.entries[session.cursor];
            let matches = if session.expression.is_empty() {
                true
            } else {
                // SAFETY: both buffers are NUL-terminated and live for this call.
                unsafe {
                    (state.api.file_name_match)(
                        PCWSTR(entry.wide_name.as_ptr()),
                        PCWSTR(session.expression.as_ptr()),
                    )
                }
            };
            if !matches {
                session.cursor += 1;
                continue;
            }
            let mut info = entry.info;
            // SAFETY: entry name and info live for the call; ProjFS owns the directory buffer handle.
            let result = unsafe {
                (state.api.fill_dir_entry_buffer)(
                    PCWSTR(entry.wide_name.as_ptr()),
                    &mut info,
                    dir_entry_buffer,
                )
            };
            if failed(result) {
                if result == hresult_from_win32(ERROR_INSUFFICIENT_BUFFER) {
                    if wrote_entry {
                        return Ok(());
                    }
                    return Err(result);
                }
                return Err(result);
            }
            wrote_entry = true;
            session.cursor += 1;
        }
        Ok(())
    })
}

unsafe extern "system" fn placeholder_cb(callback_data: *const PRJ_CALLBACK_DATA) -> HRESULT {
    ffi_guard(|| {
        // SAFETY: ProjFS supplies a valid callback pointer for this invocation.
        let (data, state) = unsafe { callback_state(callback_data) }?;
        // SAFETY: FilePathName belongs to the live callback data.
        let path = unsafe { remote_path(data.FilePathName) };
        let attr = state.client.get_attr(&path).map_err(remote_hresult)?;
        let info = PRJ_PLACEHOLDER_INFO {
            FileBasicInfo: apply_readonly(to_basic_info(&attr), state.allow_writes),
            ..Default::default()
        };
        // SAFETY: callback context and path remain live for the call; `info` has the SDK layout.
        let result = unsafe {
            (state.api.write_placeholder_info)(
                data.NamespaceVirtualizationContext,
                data.FilePathName,
                &info,
                u32::try_from(size_of::<PRJ_PLACEHOLDER_INFO>()).unwrap_or(u32::MAX),
            )
        };
        if failed(result) { Err(result) } else { Ok(()) }
    })
}

unsafe extern "system" fn file_data_cb(
    callback_data: *const PRJ_CALLBACK_DATA,
    byte_offset: u64,
    length: u32,
) -> HRESULT {
    ffi_guard(|| {
        // SAFETY: ProjFS supplies a valid callback pointer for this invocation.
        let (data, state) = unsafe { callback_state(callback_data) }?;
        // SAFETY: FilePathName belongs to the live callback data.
        let path = unsafe { remote_path(data.FilePathName) };
        let handle = state.client.open(&path, 0).map_err(remote_hresult)?;
        let mut offset = byte_offset;
        let mut remaining = length;
        let mut outcome = Ok(());
        while remaining > 0 {
            let chunk = remaining.min(MAX_IO);
            match state.client.read(handle, offset, chunk) {
                Ok(bytes) => {
                    if bytes.is_empty() {
                        break;
                    }
                    if bytes.len() > chunk as usize {
                        outcome = Err(hresult_from_win32(ERROR_INVALID_DATA));
                        break;
                    }
                    let write_len = u32::try_from(bytes.len())
                        .map_err(|_| hresult_from_win32(ERROR_INVALID_DATA))?;
                    // SAFETY: bytes remains allocated for the call; DataStreamId/context are owned by ProjFS.
                    let result = unsafe {
                        (state.api.write_file_data)(
                            data.NamespaceVirtualizationContext,
                            &data.DataStreamId,
                            bytes.as_ptr().cast(),
                            offset,
                            write_len,
                        )
                    };
                    if failed(result) {
                        outcome = Err(result);
                        break;
                    }
                    offset = offset.saturating_add(u64::from(write_len));
                    remaining -= write_len;
                    if write_len < chunk {
                        break;
                    }
                }
                Err(error) => {
                    outcome = Err(remote_hresult(error));
                    break;
                }
            }
        }
        let _ = state.client.release(handle);
        outcome
    })
}

unsafe extern "system" fn notification_cb(
    callback_data: *const PRJ_CALLBACK_DATA,
    is_directory: bool,
    notification: PRJ_NOTIFICATION,
    destination_file_name: PCWSTR,
    operation_parameters: *mut PRJ_NOTIFICATION_PARAMETERS,
) -> HRESULT {
    ffi_guard(|| {
        // SAFETY: ProjFS supplies callback data and notification parameters according to the callback contract.
        let (data, state) = unsafe { callback_state(callback_data) }?;
        // SAFETY: the callback path pointers remain live for this invocation.
        let path = unsafe { remote_optional_path(data.FilePathName) };
        // SAFETY: destination may be null; helper handles it.
        let destination = unsafe { remote_optional_path(destination_file_name) };

        if notification == PRJ_NOTIFICATION_FILE_OPENED {
            // SAFETY: for this notification the union begins with PostCreate.NotificationMask.
            unsafe { set_notification_mask(operation_parameters, write_notify_mask()) };
            return Ok(());
        }
        if notification == PRJ_NOTIFICATION_NEW_FILE_CREATED {
            // SAFETY: for this notification the union begins with PostCreate.NotificationMask.
            unsafe { set_notification_mask(operation_parameters, write_notify_mask()) };
            require_writes(state)?;
            if is_directory {
                return remote_mkdir(state, &path);
            }
            return remote_create_empty_file(state, &path);
        }
        if notification == PRJ_NOTIFICATION_FILE_OVERWRITTEN {
            // SAFETY: for this notification the union begins with PostCreate.NotificationMask.
            unsafe { set_notification_mask(operation_parameters, write_notify_mask()) };
            require_writes(state)?;
            if is_directory {
                return Ok(());
            }
            return remote_create_empty_file(state, &path);
        }
        if notification == PRJ_NOTIFICATION_PRE_DELETE {
            require_writes(state)?;
            return remote_delete(state, &path, is_directory);
        }
        if notification == PRJ_NOTIFICATION_PRE_RENAME {
            require_writes(state)?;
            if path == "/" {
                return Err(hresult_from_win32(ERROR_ACCESS_DENIED));
            }
            if destination.is_empty() {
                return remote_delete(state, &path, is_directory);
            }
            if path.is_empty() || path == "/" {
                return Ok(());
            }
            return state
                .client
                .rename(&path, &destination)
                .map_err(remote_hresult);
        }
        if notification == PRJ_NOTIFICATION_FILE_RENAMED {
            // SAFETY: for this notification the union begins with FileRenamed.NotificationMask.
            unsafe { set_notification_mask(operation_parameters, write_notify_mask()) };
            require_writes(state)?;
            if (path.is_empty() || path == "/") && !destination.is_empty() {
                return sync_local_to_remote(state, &destination);
            }
            return Ok(());
        }
        if notification == PRJ_NOTIFICATION_PRE_SET_HARDLINK {
            return Err(hresult_from_win32(ERROR_NOT_SUPPORTED));
        }
        if notification == PRJ_NOTIFICATION_HARDLINK_CREATED {
            return require_writes(state);
        }
        if notification == PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL {
            return require_writes(state);
        }
        if notification == PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_MODIFIED {
            require_writes(state)?;
            if is_directory {
                return Ok(());
            }
            return sync_local_to_remote(state, &path);
        }
        if notification == PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_DELETED {
            return require_writes(state);
        }
        Ok(())
    })
}

fn query_dir(state: &ProjfsState, path: &str) -> Result<Vec<EnumEntry>, HRESULT> {
    let mut entries = state
        .client
        .read_dir(path)
        .map_err(remote_hresult)?
        .into_iter()
        .map(|entry| EnumEntry {
            wide_name: wide(entry.name()),
            name: entry.name().to_owned(),
            info: apply_readonly(to_basic_info(entry.attr()), state.allow_writes),
        })
        .collect::<Vec<_>>();
    entries.sort_by(|left, right| {
        left.name
            .to_lowercase()
            .cmp(&right.name.to_lowercase())
            .then_with(|| left.name.cmp(&right.name))
    });
    Ok(entries)
}

fn to_basic_info(attr: &WireAttr) -> PRJ_FILE_BASIC_INFO {
    let is_directory = attr.mode() & 0o040000 == 0o040000;
    PRJ_FILE_BASIC_INFO {
        IsDirectory: is_directory,
        FileSize: i64::try_from(attr.size()).unwrap_or(i64::MAX),
        CreationTime: unix_to_filetime(attr.ctime()),
        LastAccessTime: unix_to_filetime(attr.atime()),
        LastWriteTime: unix_to_filetime(attr.mtime()),
        ChangeTime: unix_to_filetime(attr.ctime()),
        FileAttributes: if is_directory {
            FILE_ATTRIBUTE_DIRECTORY
        } else {
            FILE_ATTRIBUTE_NORMAL
        },
    }
}

fn apply_readonly(mut info: PRJ_FILE_BASIC_INFO, allow_writes: bool) -> PRJ_FILE_BASIC_INFO {
    if !allow_writes {
        info.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    }
    info
}

fn unix_to_filetime(unix_seconds: i64) -> i64 {
    unix_seconds
        .saturating_add(11_644_473_600)
        .saturating_mul(10_000_000)
}

fn require_writes(state: &ProjfsState) -> Result<(), HRESULT> {
    if state.allow_writes {
        Ok(())
    } else {
        Err(hresult_from_win32(ERROR_ACCESS_DENIED))
    }
}

fn remote_mkdir(state: &ProjfsState, path: &str) -> Result<(), HRESULT> {
    match state.client.mkdir(path, 0o755) {
        Ok(()) => Ok(()),
        Err(error) if error.status() == EEXIST => Ok(()),
        Err(error) => Err(remote_hresult(error)),
    }
}

fn remote_delete(state: &ProjfsState, path: &str, is_directory: bool) -> Result<(), HRESULT> {
    if is_directory {
        state.client.rmdir(path).map_err(remote_hresult)
    } else {
        state.client.unlink(path).map_err(remote_hresult)
    }
}

fn remote_create_empty_file(state: &ProjfsState, path: &str) -> Result<(), HRESULT> {
    let handle = state
        .client
        .create(path, FB_O_WRONLY | FB_O_TRUNC, 0o644)
        .map_err(remote_hresult)?;
    let _ = state.client.release(handle);
    state.client.truncate(path, 0).map_err(remote_hresult)
}

fn sync_local_to_remote(state: &ProjfsState, remote_path: &str) -> Result<(), HRESULT> {
    let local_path = local_path(state, remote_path);
    if local_path.is_dir() {
        return remote_mkdir(state, remote_path);
    }
    if !local_path.exists() {
        return Err(hresult_from_win32(ERROR_FILE_NOT_FOUND));
    }

    let handle = state
        .client
        .create(remote_path, FB_O_WRONLY | FB_O_TRUNC, 0o644)
        .map_err(remote_hresult)?;
    let mut file = match File::open(&local_path) {
        Ok(file) => file,
        Err(_) => {
            let _ = state.client.release(handle);
            return Err(hresult_from_win32(ERROR_GEN_FAILURE));
        }
    };
    let mut buffer = vec![0_u8; MAX_IO as usize];
    let mut offset = 0_u64;
    loop {
        let amount = match file.read(&mut buffer) {
            Ok(amount) => amount,
            Err(_) => {
                let _ = state.client.release(handle);
                return Err(hresult_from_win32(ERROR_GEN_FAILURE));
            }
        };
        if amount == 0 {
            break;
        }
        let written = match state.client.write(handle, offset, &buffer[..amount]) {
            Ok(written) => written,
            Err(error) => {
                let _ = state.client.release(handle);
                return Err(remote_hresult(error));
            }
        };
        if written as usize != amount {
            let _ = state.client.release(handle);
            return Err(hresult_from_win32(ERROR_GEN_FAILURE));
        }
        offset = offset.saturating_add(u64::from(written));
    }
    let _ = state.client.release(handle);
    state
        .client
        .truncate(remote_path, offset)
        .map_err(remote_hresult)
}

fn local_path(state: &ProjfsState, remote_path: &str) -> PathBuf {
    let relative = remote_path.trim_start_matches(['/', '\\']);
    if relative.is_empty() {
        state.root.clone()
    } else {
        state.root.join(relative.replace('/', "\\"))
    }
}

fn remote_hresult(error: RemoteFsError) -> HRESULT {
    status_to_hresult(error.status())
}

fn status_to_hresult(status: i16) -> HRESULT {
    if status == 0 {
        return S_OK;
    }
    match status {
        ENOENT => hresult_from_win32(ERROR_FILE_NOT_FOUND),
        EACCES => hresult_from_win32(ERROR_ACCESS_DENIED),
        ENOTDIR => hresult_from_win32(ERROR_DIRECTORY),
        EISDIR => hresult_from_win32(ERROR_CANNOT_MAKE),
        _ => hresult_from_win32(ERROR_GEN_FAILURE),
    }
}

fn hresult_from_win32(code: u32) -> HRESULT {
    if code == 0 {
        S_OK
    } else {
        HRESULT(((code & 0xffff) | (7 << 16) | 0x8000_0000) as i32)
    }
}

fn hresult_hex(result: HRESULT) -> String {
    format!("0x{:08X}", result.0 as u32)
}

fn failed(result: HRESULT) -> bool {
    result.0 < 0
}

fn ffi_guard(action: impl FnOnce() -> Result<(), HRESULT>) -> HRESULT {
    match catch_unwind(AssertUnwindSafe(action)) {
        Ok(Ok(())) => S_OK,
        Ok(Err(error)) => error,
        Err(_) => hresult_from_win32(ERROR_GEN_FAILURE),
    }
}

/// # Safety
/// `callback_data` must be the live callback pointer supplied by ProjFS. The returned references
/// must not outlive the callback invocation.
unsafe fn callback_state<'a>(
    callback_data: *const PRJ_CALLBACK_DATA,
) -> Result<(&'a PRJ_CALLBACK_DATA, &'a ProjfsState), HRESULT> {
    if callback_data.is_null() {
        return Err(hresult_from_win32(ERROR_INVALID_PARAMETER));
    }
    // SAFETY: the caller guarantees this is the live ProjFS callback pointer.
    let data = unsafe { &*callback_data };
    let state = data.InstanceContext.cast::<ProjfsState>();
    if state.is_null() {
        return Err(hresult_from_win32(ERROR_INVALID_PARAMETER));
    }
    // SAFETY: Projection keeps this Box allocation alive until after PrjStopVirtualizing.
    Ok((data, unsafe { &*state }))
}

/// # Safety
/// `text` must either be null or point to a NUL-terminated UTF-16 string valid for this call.
unsafe fn wide_to_vec(text: PCWSTR) -> Vec<u16> {
    if text.0.is_null() {
        return Vec::new();
    }
    let mut length = 0_usize;
    // SAFETY: the caller guarantees NUL termination and readable memory.
    while unsafe { *text.0.add(length) } != 0 {
        length += 1;
    }
    if length == 0 {
        return Vec::new();
    }
    // SAFETY: `length` was obtained by scanning the same valid string allocation.
    let mut result = unsafe { slice::from_raw_parts(text.0, length) }.to_vec();
    result.push(0);
    result
}

/// # Safety
/// `text` must either be null or point to a NUL-terminated UTF-16 string valid for this call.
unsafe fn wide_to_string(text: PCWSTR) -> String {
    // SAFETY: this function has the same input contract as wide_to_vec and forwards it unchanged.
    let wide = unsafe { wide_to_vec(text) };
    let slice = wide.strip_suffix(&[0]).unwrap_or(&wide);
    String::from_utf16_lossy(slice)
}

/// # Safety
/// `text` follows the ProjFS callback lifetime rules described by `wide_to_string`.
unsafe fn remote_path(text: PCWSTR) -> String {
    // SAFETY: the caller supplies a live ProjFS path pointer satisfying wide_to_string's contract.
    let raw = unsafe { wide_to_string(text) }.replace('\\', "/");
    if raw.is_empty() {
        "/".to_owned()
    } else if raw.starts_with('/') {
        raw
    } else {
        format!("/{raw}")
    }
}

/// # Safety
/// `text` follows the ProjFS callback lifetime rules described by `wide_to_string`.
unsafe fn remote_optional_path(text: PCWSTR) -> String {
    // SAFETY: the caller supplies a live optional ProjFS path pointer satisfying wide_to_string's contract.
    let raw = unsafe { wide_to_string(text) }.replace('\\', "/");
    if raw.is_empty() {
        String::new()
    } else if raw.starts_with('/') {
        raw
    } else {
        format!("/{raw}")
    }
}

/// # Safety
/// For the notifications where this is called, `parameters` is null or points to a live
/// PRJ_NOTIFICATION_PARAMETERS union whose selected PostCreate/FileRenamed member begins with
/// a PRJ_NOTIFY_TYPES NotificationMask, exactly as specified by projectedfslib.h.
unsafe fn set_notification_mask(
    parameters: *mut PRJ_NOTIFICATION_PARAMETERS,
    mask: PRJ_NOTIFY_TYPES,
) {
    if parameters.is_null() {
        return;
    }
    // SAFETY: documented union layout places NotificationMask at offset zero for both members used.
    unsafe { *parameters.cast::<PRJ_NOTIFY_TYPES>() = mask };
}
