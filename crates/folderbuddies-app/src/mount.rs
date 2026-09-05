use std::{path::Path, sync::Arc};

use folderbuddies_core::{
    cached_remote::CachedRemoteFs,
    client::Client,
    remote_fs::RemoteFs,
};

#[cfg(not(windows))]
#[path = "mount_fsk.rs"]
mod fsk_backend;

#[cfg(windows)]
use folderbuddies_platform_windows::WindowsMount;

pub(crate) struct Mount {
    #[cfg(windows)]
    inner: WindowsMount,
    #[cfg(not(windows))]
    inner: fsk_backend::PortableMount,
}

impl Mount {
    pub(crate) fn start(
        client: Arc<Client>,
        share_name: &str,
        allow_writes: bool,
    ) -> Result<Self, String> {
        let client: Arc<dyn RemoteFs> = Arc::new(CachedRemoteFs::new(client));
        Self::start_remote(client, share_name, allow_writes)
    }

    pub(crate) fn start_remote(
        client: Arc<dyn RemoteFs>,
        share_name: &str,
        allow_writes: bool,
    ) -> Result<Self, String> {
        #[cfg(windows)]
        {
            WindowsMount::start(client, share_name, allow_writes).map(|inner| Self { inner })
        }
        #[cfg(not(windows))]
        {
            #[cfg(target_os = "macos")]
            crate::macos_prerequisite::ensure_fuse_backend()?;
            fsk_backend::PortableMount::start_remote(client, share_name, allow_writes)
                .map(|inner| Self { inner })
        }
    }

    pub(crate) fn mount_path(&self) -> &Path {
        self.inner.mount_path()
    }

    pub(crate) fn ejected(&self) -> bool {
        #[cfg(windows)]
        {
            self.inner.ejected()
        }
        #[cfg(not(windows))]
        {
            false
        }
    }

    pub(crate) fn unmount(self) -> Result<(), String> {
        #[cfg(windows)]
        {
            let mut inner = self.inner;
            inner.stop();
            Ok(())
        }
        #[cfg(not(windows))]
        {
            self.inner.unmount()
        }
    }
}
