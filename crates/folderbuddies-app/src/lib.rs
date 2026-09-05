#![forbid(unsafe_code)]

mod gui;
#[cfg(target_os = "macos")]
mod macos_prerequisite;
mod mount;
mod runtime;
mod session;

#[must_use]
pub fn run() -> i32 {
    session::run()
}
