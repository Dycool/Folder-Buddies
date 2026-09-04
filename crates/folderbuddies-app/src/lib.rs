#![forbid(unsafe_code)]

mod gui;
mod mount;
mod runtime;
mod session;

#[must_use]
pub fn run() -> i32 {
    session::run()
}
