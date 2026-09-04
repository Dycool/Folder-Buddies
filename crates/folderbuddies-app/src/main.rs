#![forbid(unsafe_code)]

fn main() {
    let code = folderbuddies::run();
    if code != 0 {
        std::process::exit(code);
    }
}
