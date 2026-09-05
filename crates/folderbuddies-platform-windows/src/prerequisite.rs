use std::mem::size_of;

use libloading::Library;
use windows::{
    Win32::{
        Foundation::CloseHandle,
        System::Threading::{GetExitCodeProcess, INFINITE, WaitForSingleObject},
        UI::{
            Shell::{SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW, ShellExecuteExW},
            WindowsAndMessaging::{
                IDYES, MB_DEFBUTTON1, MB_ICONINFORMATION, MB_ICONQUESTION, MB_ICONWARNING,
                MB_OK, MB_YESNO, MessageBoxW, SW_HIDE,
            },
        },
    },
    core::PCWSTR,
};

use crate::drive::wide;

const MISSING_PROJFS: &str = "Projected File System (ProjFS) is not enabled.\n\nFolder Buddies uses the Windows Projected File System to create virtual drives in Explorer. Open Windows Settings → Optional features → Projected File System, or let Folder Buddies enable it for you\n(requires administrator privileges and a reboot).";

#[must_use]
pub fn projfs_available() -> bool {
    // SAFETY: loading the Windows system ProjFS library is used only as an availability probe;
    // the handle is immediately dropped and no symbols escape this function.
    unsafe { Library::new("ProjectedFSLib.dll") }.is_ok()
}

pub fn ensure_projfs() -> Result<(), String> {
    if projfs_available() {
        Ok(())
    } else {
        Err(MISSING_PROJFS.to_owned())
    }
}

pub fn enable_projfs() -> Result<(), String> {
    let verb = wide("runas");
    let executable = wide("dism.exe");
    let parameters = wide("/online /enable-feature /featurename:Client-ProjFS /all /norestart");
    let mut execute = SHELLEXECUTEINFOW {
        cbSize: u32::try_from(size_of::<SHELLEXECUTEINFOW>()).unwrap_or(u32::MAX),
        fMask: SEE_MASK_NOCLOSEPROCESS,
        lpVerb: PCWSTR(verb.as_ptr()),
        lpFile: PCWSTR(executable.as_ptr()),
        lpParameters: PCWSTR(parameters.as_ptr()),
        nShow: SW_HIDE.0,
        ..Default::default()
    };
    // SAFETY: execute points to a fully initialized SHELLEXECUTEINFOW whose UTF-16 inputs remain
    // alive until ShellExecuteExW returns. SEE_MASK_NOCLOSEPROCESS requests an owned process handle.
    unsafe { ShellExecuteExW(&mut execute) }
        .map_err(|_| "UAC elevation was declined or failed".to_owned())?;
    if execute.hProcess.is_invalid() {
        return Err("UAC elevation was declined or failed".to_owned());
    }

    // SAFETY: hProcess is the live process handle returned by ShellExecuteExW.
    unsafe { WaitForSingleObject(execute.hProcess, INFINITE) };
    let mut exit_code = 1_u32;
    // SAFETY: hProcess remains valid and exit_code points to writable storage.
    let result = unsafe { GetExitCodeProcess(execute.hProcess, &mut exit_code) };
    // SAFETY: this call owns the hProcess handle and closes it exactly once after waiting.
    let _ = unsafe { CloseHandle(execute.hProcess) };
    result.map_err(|error| format!("GetExitCodeProcess failed: {error}"))?;
    if exit_code != 0 {
        return Err(format!(
            "DISM failed while enabling Client-ProjFS (exit code {exit_code})"
        ));
    }
    Ok(())
}

pub fn prompt_enable_projfs_if_missing() {
    if projfs_available() {
        return;
    }

    let title = wide("Enable Projected File System?");
    let question = wide(
        "Folder Buddies needs the Windows Projected File System (ProjFS) to create virtual drives, but it is not enabled on your system.\n\nWould you like to enable it now?\n\nThis requires administrator privileges and a system reboot.",
    );
    // SAFETY: all UTF-16 strings are NUL-terminated and remain live for the synchronous dialog.
    let response = unsafe {
        MessageBoxW(
            None,
            PCWSTR(question.as_ptr()),
            PCWSTR(title.as_ptr()),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1,
        )
    };
    if response != IDYES {
        return;
    }

    match enable_projfs() {
        Ok(()) => {
            let title = wide("Restart Required");
            let text = wide(
                "ProjFS has been enabled. You must restart your computer for the change to take effect.\n\nAfter rebooting, Folder Buddies will be able to mount remote shares.",
            );
            // SAFETY: all UTF-16 strings are NUL-terminated and remain live for the synchronous dialog.
            unsafe {
                MessageBoxW(
                    None,
                    PCWSTR(text.as_ptr()),
                    PCWSTR(title.as_ptr()),
                    MB_OK | MB_ICONINFORMATION,
                );
            }
        }
        Err(error) => {
            let title = wide("Could Not Enable ProjFS");
            let text = wide(&error);
            // SAFETY: all UTF-16 strings are NUL-terminated and remain live for the synchronous dialog.
            unsafe {
                MessageBoxW(
                    None,
                    PCWSTR(text.as_ptr()),
                    PCWSTR(title.as_ptr()),
                    MB_OK | MB_ICONWARNING,
                );
            }
        }
    }
}