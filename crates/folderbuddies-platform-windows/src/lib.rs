#![deny(unsafe_op_in_unsafe_fn)]
#![cfg(windows)]

mod drive;
mod prerequisite;
mod projfs;

use std::{
    fs,
    path::{Path, PathBuf},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread::JoinHandle,
};

use folderbuddies_core::remote_fs::RemoteFs;

use drive::{DriveMapping, clone_drive_name, wait_for_mapping_loss};
use projfs::Projection;

pub use prerequisite::{
    enable_projfs, ensure_projfs, projfs_available, prompt_enable_projfs_if_missing,
};

pub struct WindowsMount {
    projection: Option<Projection>,
    drive: Option<DriveMapping>,
    backing_root: PathBuf,
    mount_path: PathBuf,
    stopping: Arc<AtomicBool>,
    ejected: Arc<AtomicBool>,
    watcher: Option<JoinHandle<()>>,
}

impl WindowsMount {
    pub fn start(
        client: Arc<dyn RemoteFs>,
        share_name: &str,
        allow_writes: bool,
    ) -> Result<Self, String> {
        ensure_projfs()?;
        let base = default_backing_base();
        fs::create_dir_all(&base)
            .map_err(|error| format!("failed to create ProjFS mount base: {error}"))?;
        let label = sanitize(share_name);
        let backing_root = dedupe_path(&base, &label);
        fs::create_dir_all(&backing_root).map_err(|error| {
            format!(
                "failed to create ProjFS root: {}: {error}",
                backing_root.display()
            )
        })?;

        let projection = match Projection::start(&backing_root, client, allow_writes) {
            Ok(projection) => projection,
            Err(error) => {
                let _ = fs::remove_dir_all(&backing_root);
                return Err(error);
            }
        };
        let drive = match DriveMapping::allocate(&backing_root, &label) {
            Ok(drive) => drive,
            Err(error) => {
                drop(projection);
                let _ = fs::remove_dir_all(&backing_root);
                return Err(error);
            }
        };
        let mount_path = drive.root().to_path_buf();
        let stopping = Arc::new(AtomicBool::new(false));
        let ejected = Arc::new(AtomicBool::new(false));
        let watcher = wait_for_mapping_loss(
            clone_drive_name(&drive),
            Arc::clone(&stopping),
            Arc::clone(&ejected),
        );

        Ok(Self {
            projection: Some(projection),
            drive: Some(drive),
            backing_root,
            mount_path,
            stopping,
            ejected,
            watcher: Some(watcher),
        })
    }

    #[must_use]
    pub fn mount_path(&self) -> &Path {
        &self.mount_path
    }

    #[must_use]
    pub fn active(&self) -> bool {
        self.projection.is_some()
            && self.drive.as_ref().is_some_and(DriveMapping::exists)
            && !self.ejected()
    }

    #[must_use]
    pub fn ejected(&self) -> bool {
        self.ejected.load(Ordering::Acquire)
    }

    pub fn stop(&mut self) {
        self.stopping.store(true, Ordering::Release);
        if let Some(mut drive) = self.drive.take() {
            drive.remove();
        }
        if let Some(watcher) = self.watcher.take() {
            let _ = watcher.join();
        }
        self.projection.take();
        let _ = fs::remove_dir_all(&self.backing_root);
    }
}

impl Drop for WindowsMount {
    fn drop(&mut self) {
        self.stop();
    }
}

fn default_backing_base() -> PathBuf {
    if let Some(local) = std::env::var_os("LOCALAPPDATA").filter(|value| !value.is_empty()) {
        return PathBuf::from(local).join("FolderBuddies").join("mounts");
    }
    let profile = std::env::var_os("USERPROFILE")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\"));
    profile
        .join("AppData")
        .join("Local")
        .join("FolderBuddies")
        .join("mounts")
}

fn sanitize(name: &str) -> String {
    let mut result = String::with_capacity(name.len());
    for character in name.chars() {
        if matches!(
            character,
            '/' | '\\' | ':' | '*' | '?' | '"' | '<' | '>' | '|'
        ) {
            result.push('_');
        } else {
            result.push(character);
        }
    }
    if result.is_empty() {
        "share".to_owned()
    } else {
        result
    }
}

fn dedupe_path(base: &Path, name: &str) -> PathBuf {
    for number in 1..1000 {
        let candidate = if number == 1 {
            name.to_owned()
        } else {
            format!("{name}-{number}")
        };
        let path = base.join(candidate);
        if !path.exists() {
            return path;
        }
    }
    base.join(name)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sanitizes_like_cpp_projfs_backend() {
        assert_eq!(sanitize("a:b/c\\d*e?f\"g<h>i|j"), "a_b_c_d_e_f_g_h_i_j");
        assert_eq!(sanitize(""), "share");
    }
}
