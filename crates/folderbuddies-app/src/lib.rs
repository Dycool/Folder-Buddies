#![forbid(unsafe_code)]

mod gui;
mod mount;
mod session;

pub fn run() -> Result<(), String> {
    session::run()
}
