//! Native host-side filesystem server. The implementation is filled by the Rust port.

#[derive(Debug, Default)]
pub struct Server {
    running: bool,
}

impl Server {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    pub fn running(&self) -> bool {
        self.running
    }
}
