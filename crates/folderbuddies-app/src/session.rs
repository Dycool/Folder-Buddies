pub(crate) fn run() -> Result<(), String> {
    let mut args = std::env::args();
    let _program = args.next();
    match args.next().as_deref() {
        None => super::gui::run_gui(),
        Some("help" | "--help" | "-h") => {
            println!("Folder Buddies Rust port");
            Ok(())
        }
        Some(command) => Err(format!("command is not ported yet: {command}")),
    }
}
