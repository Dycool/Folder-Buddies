#![forbid(unsafe_code)]

fn main() {
    if let Err(error) = folderbuddies::run() {
        eprintln!("Folder Buddies: {error}");
        std::process::exit(1);
    }
}
