#![forbid(unsafe_code)]

mod gui;
mod mount;
mod runtime;
mod session;

pub fn run() -> Result<(), String> {
    session::run()
}
