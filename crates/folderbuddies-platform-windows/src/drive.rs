use std::{path::{Path, PathBuf}, thread, time::Duration};

use windows::{
    Win32::{
        Foundation::{CloseHandle, GetLastError, WAIT_ABANDONED, WAIT_OBJECT_0},
        Storage::FileSystem::{
            DDD_EXACT_MATCH_ON_REMOVE, DDD_RAW_TARGET_PATH, DDD_REMOVE_DEFINITION,
            DefineDosDeviceW, GetLogicalDrives, QueryDosDeviceW,
        },
        System::{
            Registry::{
                HKEY, HKEY_CURRENT_USER, KEY_SET_VALUE, REG_OPTION_NON_VOLATILE, REG_SZ,
                RegCloseKey, RegCreateKeyExW, RegDeleteKeyW, RegSetValueExW,
            },
            Threading::{CreateMutexW, INFINITE, ReleaseMutex, WaitForSingleObject},
        },
        UI::Shell::{
            SHCNE_DRIVEADD, SHCNE_DRIVEREMOVED, SHCNE_UPDATEDIR, SHCNE_UPDATEITEM, SHCNF_PATHW,
            SHChangeNotify,
        },
    },
    core::{PCWSTR, w},
};

pub(crate) struct DriveMapping {
    drive: String,
    drive_name: Vec<u16>,
    target: Vec<u16>,
    root: PathBuf,
    removed: bool,
}

impl DriveMapping {
    pub(crate) fn allocate(backing: &Path, label: &str) -> Result<Self, String> {
        // SAFETY: CreateMutexW receives a static, NUL-terminated Windows string and no security pointer.
        let mutex = unsafe {
            CreateMutexW(
                None,
                false,
                w!("Local\\FolderBuddies.DriveLetterAllocation"),
            )
        }
        .map_err(|error| format!("CreateMutex for drive allocation failed: {error}"))?;

        // SAFETY: `mutex` is a valid HANDLE returned by CreateMutexW and remains live until CloseHandle.
        let wait = unsafe { WaitForSingleObject(mutex, INFINITE) };
        if wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED {
            // SAFETY: `mutex` is still a valid HANDLE and is not used after this close.
            let _ = unsafe { CloseHandle(mutex) };
            return Err(format!(
                "waiting for drive allocation mutex failed (Windows error {})",
                // SAFETY: GetLastError has no preconditions.
                unsafe { GetLastError().0 }
            ));
        }

        let result = Self::allocate_locked(backing, label);
        // SAFETY: this thread owns (or inherited abandoned ownership of) the named mutex.
        let _ = unsafe { ReleaseMutex(mutex) };
        // SAFETY: `mutex` is no longer needed after release.
        let _ = unsafe { CloseHandle(mutex) };
        result
    }

    fn allocate_locked(backing: &Path, label: &str) -> Result<Self, String> {
        // SAFETY: GetLogicalDrives has no pointer arguments or additional preconditions.
        let mask = unsafe { GetLogicalDrives() };
        if mask == 0 {
            return Err(format!(
                "GetLogicalDrives failed (Windows error {})",
                // SAFETY: GetLastError has no preconditions.
                unsafe { GetLastError().0 }
            ));
        }

        let backing = absolute_path(backing)?;
        let target = wide(&format!(r"\??\{}", backing.display()));
        let mut last_error = None;

        for letter in b'D'..=b'Z' {
            if mask & (1_u32 << u32::from(letter - b'A')) != 0 {
                continue;
            }
            let drive = format!("{}:", char::from(letter));
            let drive_name = wide(&drive);
            if mapping_exists_wide(&drive_name) {
                continue;
            }

            set_drive_label(&drive, label);
            // SAFETY: both UTF-16 buffers are NUL-terminated and remain alive for the duration of the call.
            let mapped = unsafe {
                DefineDosDeviceW(
                    DDD_RAW_TARGET_PATH,
                    PCWSTR(drive_name.as_ptr()),
                    PCWSTR(target.as_ptr()),
                )
            };
            match mapped {
                Ok(()) => {
                    let root = PathBuf::from(format!("{drive}\\"));
                    notify_drive_added(&root);
                    return Ok(Self {
                        drive,
                        drive_name,
                        target,
                        root,
                        removed: false,
                    });
                }
                Err(error) => {
                    last_error = Some(error.to_string());
                    clear_drive_label(&drive);
                }
            }
        }

        if let Some(error) = last_error {
            Err(format!("failed to map any available drive letter: {error}"))
        } else {
            Err("no free drive letter is available from D: through Z:".to_owned())
        }
    }

    pub(crate) fn root(&self) -> &Path {
        &self.root
    }

    pub(crate) fn exists(&self) -> bool {
        mapping_exists_wide(&self.drive_name)
    }

    pub(crate) fn remove(&mut self) {
        if self.removed {
            return;
        }
        self.removed = true;
        clear_drive_label(&self.drive);
        // SAFETY: the name/target buffers are NUL-terminated and owned by this mapping for the call duration.
        let _ = unsafe {
            DefineDosDeviceW(
                DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_RAW_TARGET_PATH,
                PCWSTR(self.drive_name.as_ptr()),
                PCWSTR(self.target.as_ptr()),
            )
        };
        notify_drive_removed(&self.root);
    }
}

impl Drop for DriveMapping {
    fn drop(&mut self) {
        self.remove();
    }
}

pub(crate) fn wait_for_mapping_loss(
    drive_name: Vec<u16>,
    stopping: std::sync::Arc<std::sync::atomic::AtomicBool>,
    ejected: std::sync::Arc<std::sync::atomic::AtomicBool>,
) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        while !stopping.load(std::sync::atomic::Ordering::Acquire) {
            thread::sleep(Duration::from_millis(500));
            if !mapping_exists_wide(&drive_name) {
                if !stopping.load(std::sync::atomic::Ordering::Acquire) {
                    ejected.store(true, std::sync::atomic::Ordering::Release);
                }
                break;
            }
        }
    })
}

pub(crate) fn clone_drive_name(mapping: &DriveMapping) -> Vec<u16> {
    mapping.drive_name.clone()
}

fn mapping_exists_wide(drive_name: &[u16]) -> bool {
    let mut target = vec![0_u16; 32_768];
    // SAFETY: `drive_name` is NUL-terminated and the output buffer is a valid mutable UTF-16 slice.
    unsafe { QueryDosDeviceW(PCWSTR(drive_name.as_ptr()), Some(&mut target)) != 0 }
}

fn absolute_path(path: &Path) -> Result<PathBuf, String> {
    if path.is_absolute() {
        Ok(path.to_path_buf())
    } else {
        std::env::current_dir()
            .map(|current| current.join(path))
            .map_err(|error| format!("failed to resolve backing directory: {error}"))
    }
}

fn drive_icon_key(letter: char) -> String {
    format!(
        r"SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\DriveIcons\{letter}"
    )
}

fn set_drive_label(drive: &str, label: &str) {
    let Some(letter) = drive.chars().next() else {
        return;
    };
    let subkey = wide(&format!(r"{}\DefaultLabel", drive_icon_key(letter)));
    let mut key = HKEY::default();
    // SAFETY: all pointer arguments refer to valid NUL-terminated strings; output HKEY is writable.
    let status = unsafe {
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(subkey.as_ptr()),
            None,
            PCWSTR::null(),
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            None,
            &mut key,
            None,
        )
    };
    if status.0 != 0 {
        return;
    }
    let label_wide = wide(label);
    let mut bytes = Vec::with_capacity(label_wide.len() * 2);
    for unit in label_wide {
        bytes.extend_from_slice(&unit.to_le_bytes());
    }
    // SAFETY: `key` is open for KEY_SET_VALUE and the byte slice contains a NUL-terminated UTF-16 REG_SZ.
    let _ = unsafe { RegSetValueExW(key, PCWSTR::null(), None, REG_SZ, Some(&bytes)) };
    // SAFETY: `key` is an open registry key returned by RegCreateKeyExW.
    let _ = unsafe { RegCloseKey(key) };
}

fn clear_drive_label(drive: &str) {
    let Some(letter) = drive.chars().next() else {
        return;
    };
    let base = drive_icon_key(letter);
    let default_label = wide(&format!(r"{base}\DefaultLabel"));
    let base = wide(&base);
    // SAFETY: both subkey paths are valid NUL-terminated strings. Missing keys are intentionally ignored.
    unsafe {
        let _ = RegDeleteKeyW(HKEY_CURRENT_USER, PCWSTR(default_label.as_ptr()));
        let _ = RegDeleteKeyW(HKEY_CURRENT_USER, PCWSTR(base.as_ptr()));
    }
}

fn notify_drive_added(root: &Path) {
    let root = wide(&root.to_string_lossy());
    // SAFETY: SHChangeNotify only borrows the NUL-terminated path pointer for each synchronous call.
    unsafe {
        let pointer = root.as_ptr().cast();
        SHChangeNotify(SHCNE_DRIVEADD, SHCNF_PATHW, Some(pointer), None);
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, Some(pointer), None);
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, Some(pointer), None);
    }
}

fn notify_drive_removed(root: &Path) {
    let root = wide(&root.to_string_lossy());
    // SAFETY: SHChangeNotify only borrows the NUL-terminated path pointer for the synchronous call.
    unsafe {
        SHChangeNotify(
            SHCNE_DRIVEREMOVED,
            SHCNF_PATHW,
            Some(root.as_ptr().cast()),
            None,
        );
    }
}

pub(crate) fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}
