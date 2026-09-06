use std::{path::Path, process::Command};

const BACKEND_MARKERS: &[&str] = &[
    "/Library/Frameworks/fuse_t.framework",
    "/Library/Application Support/fuse-t",
    "/Library/Filesystems/fuse-t.fs",
    "/Library/Frameworks/macFUSE.framework",
    "/Library/Filesystems/macfuse.fs",
];

pub(crate) fn ensure_fuse_backend() -> Result<(), String> {
    if backend_present() {
        return Ok(());
    }

    let brew = match find_brew() {
        Some(path) => path,
        None => install_homebrew()?,
    };

    let _ = Command::new(&brew)
        .args(["tap", "--quiet", "macos-fuse-t/homebrew-cask"])
        .status();
    let fetch = Command::new(&brew)
        .args([
            "fetch",
            "--cask",
            "macos-fuse-t/homebrew-cask/fuse-t",
        ])
        .status()
        .map_err(|_| {
            "Failed to download FUSE-T installer via Homebrew.\nInstall it manually: brew install macos-fuse-t/homebrew-cask/fuse-t"
                .to_owned()
        })?;
    if !fetch.success() {
        return Err(
            "Failed to download FUSE-T installer via Homebrew.\nInstall it manually: brew install macos-fuse-t/homebrew-cask/fuse-t"
                .to_owned(),
        );
    }

    let cache = Command::new(&brew)
        .args(["--cache", "--cask", "macos-fuse-t/homebrew-cask/fuse-t"])
        .output()
        .map_err(|_| "FUSE-T package was downloaded but could not be located.".to_owned())?;
    let package = String::from_utf8_lossy(&cache.stdout).trim().to_owned();
    if package.is_empty() {
        return Err("FUSE-T package was downloaded but could not be located.".to_owned());
    }
    if !Path::new(&package).exists() {
        return Err(format!(
            "FUSE-T package was downloaded but the file is missing at:\n  {package}"
        ));
    }

    let install = format!("installer -pkg {} -target /", shell_quote(&package));
    if !run_admin_osascript(
        &install,
        "Folder Buddies needs to install FUSE-T for mounting remote folders",
    ) {
        return Err(
            "FUSE-T installation was declined or failed.\nInstall it manually: brew install macos-fuse-t/homebrew-cask/fuse-t"
                .to_owned(),
        );
    }

    if !backend_present() {
        return Err(
            "FUSE-T was installed but is not active yet — a reboot may be required.".to_owned(),
        );
    }
    Ok(())
}

fn backend_present() -> bool {
    BACKEND_MARKERS.iter().any(|path| Path::new(path).exists())
}

fn find_brew() -> Option<String> {
    for path in ["/opt/homebrew/bin/brew", "/usr/local/bin/brew"] {
        if Path::new(path).exists() {
            return Some(path.to_owned());
        }
    }
    Command::new("sh")
        .args(["-c", "command -v brew >/dev/null 2>&1"])
        .status()
        .ok()
        .filter(std::process::ExitStatus::success)
        .map(|_| "brew".to_owned())
}

fn install_homebrew() -> Result<String, String> {
    #[cfg(target_arch = "aarch64")]
    let prefix = "/opt/homebrew";
    #[cfg(not(target_arch = "aarch64"))]
    let prefix = "/usr/local";

    let user = console_user();
    let setup = format!(
        "mkdir -p {prefix} 2>/dev/null; chown {}:staff {prefix} 2>/dev/null",
        shell_quote(&user)
    );
    let _ = run_admin_osascript(
        &setup,
        "Folder Buddies needs to create the Homebrew directory",
    );

    let download = format!(
        "curl -fsSL https://github.com/Homebrew/brew/tarball/master | tar xz --strip 1 -C {prefix} 2>/dev/null"
    );
    let status = Command::new("sh")
        .args(["-c", &download])
        .status()
        .map_err(|_| {
            "Failed to download Homebrew. FUSE-T installation cannot proceed.".to_owned()
        })?;
    if !status.success() {
        return Err("Failed to download Homebrew. FUSE-T installation cannot proceed.".to_owned());
    }

    let fix = format!("chown -R {}:staff {prefix} 2>/dev/null", shell_quote(&user));
    let _ = run_admin_osascript(&fix, "Folder Buddies is setting up Homebrew ownership");

    let brew = format!("{prefix}/bin/brew");
    if !Path::new(&brew).exists() {
        return Err(format!(
            "Homebrew was downloaded but 'brew' command not found at {brew}"
        ));
    }
    Ok(brew)
}

fn console_user() -> String {
    Command::new("stat")
        .args(["-f", "%Su", "/dev/console"])
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .filter(|user| !user.is_empty())
        .unwrap_or_else(|| "unknown".to_owned())
}

fn run_admin_osascript(command: &str, prompt: &str) -> bool {
    let script = format!(
        "do shell script {} with administrator privileges with prompt {}",
        applescript_string(command),
        applescript_string(prompt)
    );
    Command::new("osascript")
        .args(["-e", &script])
        .status()
        .is_ok_and(|status| status.success())
}

fn shell_quote(text: &str) -> String {
    let mut output = String::from("'");
    for character in text.chars() {
        if character == '\'' {
            output.push_str("'\\''");
        } else {
            output.push(character);
        }
    }
    output.push('\'');
    output
}

fn applescript_string(text: &str) -> String {
    let mut output = String::from("\"");
    for character in text.chars() {
        if matches!(character, '\\' | '"') {
            output.push('\\');
        }
        output.push(character);
    }
    output.push('"');
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shell_quote_matches_cpp_escape_shape() {
        assert_eq!(shell_quote("a'b"), "'a'\\''b'");
    }

    #[test]
    fn applescript_string_escapes_quotes_and_backslashes() {
        assert_eq!(applescript_string("a\\\"b"), "\"a\\\\\\\"b\"");
    }
}
