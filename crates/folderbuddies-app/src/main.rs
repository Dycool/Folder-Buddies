#![forbid(unsafe_code)]
#![cfg_attr(windows, windows_subsystem = "windows")]

fn main() {
    let code = folderbuddies::run();
    if code != 0 {
        std::process::exit(code);
    }
}
