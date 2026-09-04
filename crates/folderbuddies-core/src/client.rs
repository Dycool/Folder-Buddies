//! Native remote-filesystem client. The implementation is filled by the Rust port.

#[derive(Debug, Default)]
pub struct Client {
    connected: bool,
}

impl Client {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    #[must_use]
    pub fn connected(&self) -> bool {
        self.connected
    }
}
