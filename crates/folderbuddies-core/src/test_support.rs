use crate::protocol::WireStatFs;

impl WireStatFs {
    pub(crate) const fn bsize(&self) -> u64 {
        self.block_size()
    }

    pub(crate) const fn frsize(&self) -> u64 {
        self.fragment_size()
    }

    pub(crate) const fn bfree(&self) -> u64 {
        self.blocks_free()
    }

    pub(crate) const fn bavail(&self) -> u64 {
        self.blocks_available()
    }

    pub(crate) const fn namemax(&self) -> u64 {
        self.name_max()
    }
}
