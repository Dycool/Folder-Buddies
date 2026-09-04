use std::{
    path::{Path, PathBuf},
    process::Command,
    time::{Duration, Instant},
};

use eframe::egui;

use crate::runtime::{ConnectedSession, HostingSession};

const REPAINT_INTERVAL: Duration = Duration::from_millis(500);

#[derive(Clone, Copy, PartialEq, Eq)]
enum Tab {
    Host,
    Connect,
}

struct FolderBuddiesApp {
    tab: Tab,
    folder_path: String,
    lan_only: bool,
    allow_writes: bool,
    connect_code: String,
    hosting: Option<HostingSession>,
    connected: Option<ConnectedSession>,
    status: String,
    status_is_error: bool,
    last_sample: Instant,
    last_host_sent: u64,
    last_host_received: u64,
    last_read: u64,
    last_written: u64,
    host_send_rate: f64,
    host_receive_rate: f64,
    read_rate: f64,
    write_rate: f64,
}

impl Default for FolderBuddiesApp {
    fn default() -> Self {
        Self {
            tab: Tab::Host,
            folder_path: String::new(),
            lan_only: false,
            allow_writes: false,
            connect_code: String::new(),
            hosting: None,
            connected: None,
            status: "Idle".to_owned(),
            status_is_error: false,
            last_sample: Instant::now(),
            last_host_sent: 0,
            last_host_received: 0,
            last_read: 0,
            last_written: 0,
            host_send_rate: 0.0,
            host_receive_rate: 0.0,
            read_rate: 0.0,
            write_rate: 0.0,
        }
    }
}

impl FolderBuddiesApp {
    fn start_hosting(&mut self) {
        let folder = self.folder_path.trim();
        if folder.is_empty() {
            self.set_error("Choose a folder to host.");
            return;
        }
        match HostingSession::start(folder, self.lan_only, self.allow_writes) {
            Ok(session) => {
                self.hosting = Some(session);
                self.reset_rates();
                self.set_status("Hosting started.");
            }
            Err(error) => self.set_error(format!("Host failed: {error}")),
        }
    }

    fn stop_hosting(&mut self) {
        if let Some(mut session) = self.hosting.take() {
            session.stop();
        }
        self.reset_rates();
        self.set_status("Hosting stopped.");
    }

    fn connect(&mut self) {
        let code = self.connect_code.trim();
        if code.is_empty() {
            self.set_error("Paste a connect code first.");
            return;
        }
        match ConnectedSession::start(code, 0) {
            Ok(session) => {
                let mount = session.mount_path().display().to_string();
                self.connected = Some(session);
                self.reset_rates();
                self.set_status(format!("Connected and mounted at {mount}"));
            }
            Err(error) => self.set_error(format!("Connect failed: {error}")),
        }
    }

    fn disconnect(&mut self) {
        if let Some(mut session) = self.connected.take() {
            session.disconnect();
        }
        self.reset_rates();
        self.set_status("Disconnected.");
    }

    fn refresh_runtime_state(&mut self) {
        if self
            .hosting
            .as_ref()
            .is_some_and(|session| !session.running())
        {
            self.hosting.take();
            self.set_error("Hosting stopped unexpectedly.");
        }
        if self
            .connected
            .as_ref()
            .is_some_and(|session| !session.connected())
        {
            self.connected.take();
            self.set_error("The remote share disconnected.");
        }
    }

    fn sample_rates(&mut self) {
        let now = Instant::now();
        let elapsed = now.duration_since(self.last_sample).as_secs_f64();
        if elapsed < REPAINT_INTERVAL.as_secs_f64() {
            return;
        }
        let host_sent = self.hosting.as_ref().map_or(0, HostingSession::bytes_sent);
        let host_received = self
            .hosting
            .as_ref()
            .map_or(0, HostingSession::bytes_received);
        let read = self
            .connected
            .as_ref()
            .map_or(0, ConnectedSession::bytes_read);
        let written = self
            .connected
            .as_ref()
            .map_or(0, ConnectedSession::bytes_written);
        self.host_send_rate = per_second(host_sent, &mut self.last_host_sent, elapsed);
        self.host_receive_rate = per_second(host_received, &mut self.last_host_received, elapsed);
        self.read_rate = per_second(read, &mut self.last_read, elapsed);
        self.write_rate = per_second(written, &mut self.last_written, elapsed);
        self.last_sample = now;
    }

    fn reset_rates(&mut self) {
        self.last_sample = Instant::now();
        self.last_host_sent = 0;
        self.last_host_received = 0;
        self.last_read = 0;
        self.last_written = 0;
        self.host_send_rate = 0.0;
        self.host_receive_rate = 0.0;
        self.read_rate = 0.0;
        self.write_rate = 0.0;
    }

    fn set_status(&mut self, message: impl Into<String>) {
        self.status = message.into();
        self.status_is_error = false;
    }

    fn set_error(&mut self, message: impl Into<String>) {
        self.status = message.into();
        self.status_is_error = true;
    }

    fn host_ui(&mut self, ui: &mut egui::Ui) {
        let running = self.hosting.as_ref().is_some_and(HostingSession::running);
        ui.heading("Host a folder");
        ui.label("Share a folder as a mounted remote disk.");
        ui.add_space(8.0);

        ui.horizontal(|ui| {
            ui.label("Folder:");
            let editor = egui::TextEdit::singleline(&mut self.folder_path)
                .hint_text("Choose or type a folder path")
                .desired_width(f32::INFINITY);
            ui.add_enabled(!running, editor);
            if ui
                .add_enabled(!running, egui::Button::new("Browse…"))
                .clicked()
            {
                match choose_folder() {
                    Ok(Some(path)) => self.folder_path = path.display().to_string(),
                    Ok(None) => self.set_status("Folder selection cancelled."),
                    Err(error) => self.set_error(error),
                }
            }
        });
        ui.add_enabled_ui(!running, |ui| {
            ui.checkbox(&mut self.lan_only, "Share on this LAN only");
            ui.checkbox(
                &mut self.allow_writes,
                "Allow clients to upload, edit, and delete files",
            );
        });
        ui.add_space(8.0);

        if ui
            .button(if running { "Stop hosting" } else { "Host" })
            .clicked()
        {
            if running {
                self.stop_hosting();
            } else {
                self.start_hosting();
            }
        }

        if let Some(hosting) = self.hosting.as_ref() {
            ui.separator();
            ui.label(format!("Sharing: {}", hosting.share_name()));
            ui.label(format!("Reach: {}", hosting.reach()));
            ui.label(format!("Signaling: {}", hosting.cloud_status()));
            ui.label(format!(
                "Access: {}",
                if hosting.allow_writes() {
                    "read/write"
                } else {
                    "read-only"
                }
            ));
            ui.label(format!("Clients: {}", hosting.client_count()));
            ui.label(format!(
                "Serve ↑{}  ↓{}",
                human_rate(self.host_send_rate),
                human_rate(self.host_receive_rate)
            ));
            ui.add_space(6.0);
            ui.label("Connect code:");
            ui.monospace(hosting.connect_code());
            if ui.button("Copy connect code").clicked() {
                ui.ctx().copy_text(hosting.connect_code().to_owned());
                self.set_status("Connect code copied.");
            }
        }
    }

    fn connect_ui(&mut self, ui: &mut egui::Ui) {
        let connected = self
            .connected
            .as_ref()
            .is_some_and(ConnectedSession::connected);
        ui.heading("Connect to a folder");
        ui.label("Paste a native room code or offline blob to mount the remote folder.");
        ui.add_space(8.0);
        ui.add_enabled(
            !connected,
            egui::TextEdit::multiline(&mut self.connect_code)
                .hint_text("Connect code")
                .desired_rows(3)
                .desired_width(f32::INFINITY),
        );
        ui.add_space(8.0);
        if ui
            .button(if connected { "Disconnect" } else { "Connect" })
            .clicked()
        {
            if connected {
                self.disconnect();
            } else {
                self.connect();
            }
        }

        if let Some(session) = self.connected.as_ref() {
            ui.separator();
            ui.label(format!("Folder: {}", session.folder()));
            ui.label(format!("Mounted at: {}", session.mount_path().display()));
            ui.label(format!(
                "Access: {}",
                if session.allow_writes() {
                    "read/write"
                } else {
                    "read-only"
                }
            ));
            ui.label(format!(
                "Mount ↓{}  ↑{}",
                human_rate(self.read_rate),
                human_rate(self.write_rate)
            ));
            let mount_path = session.mount_path().to_owned();
            if ui.button("Open mounted folder").clicked() {
                match open_path(&mount_path) {
                    Ok(()) => self.set_status("Opened mounted folder."),
                    Err(error) => self.set_error(error),
                }
            }
        }
    }
}

impl eframe::App for FolderBuddiesApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.refresh_runtime_state();
        self.sample_rates();
        ui.ctx().request_repaint_after(REPAINT_INTERVAL);

        ui.add_space(12.0);
        ui.vertical_centered(|ui| {
            ui.heading("Folder Buddies");
        });
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.selectable_value(&mut self.tab, Tab::Host, "Host");
            ui.selectable_value(&mut self.tab, Tab::Connect, "Connect");
        });
        ui.separator();
        ui.add_space(8.0);

        match self.tab {
            Tab::Host => self.host_ui(ui),
            Tab::Connect => self.connect_ui(ui),
        }

        ui.add_space(12.0);
        ui.separator();
        let color = if self.status_is_error {
            egui::Color32::from_rgb(180, 40, 40)
        } else {
            egui::Color32::from_rgb(70, 90, 70)
        };
        ui.colored_label(color, &self.status);
    }
}

pub(crate) fn run_gui() -> Result<(), String> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([600.0, 430.0])
            .with_min_inner_size([520.0, 360.0]),
        centered: true,
        ..Default::default()
    };
    eframe::run_native(
        "Folder Buddies",
        options,
        Box::new(|_creation_context| Ok(Box::new(FolderBuddiesApp::default()))),
    )
    .map_err(|error| format!("failed to launch GUI: {error}"))
}

fn per_second(current: u64, last: &mut u64, elapsed: f64) -> f64 {
    let delta = current.saturating_sub(*last);
    *last = current;
    delta as f64 / elapsed
}

fn human_rate(mut bytes_per_second: f64) -> String {
    let units = ["B/s", "KB/s", "MB/s", "GB/s"];
    let mut unit = 0_usize;
    while bytes_per_second >= 1024.0 && unit + 1 < units.len() {
        bytes_per_second /= 1024.0;
        unit += 1;
    }
    format!("{bytes_per_second:.1} {}", units[unit])
}

fn path_from_stdout(stdout: &[u8]) -> Result<Option<PathBuf>, String> {
    let path = std::str::from_utf8(stdout)
        .map_err(|error| format!("folder chooser returned invalid UTF-8: {error}"))?
        .trim();
    if path.is_empty() {
        Ok(None)
    } else {
        Ok(Some(PathBuf::from(path)))
    }
}

fn choose_folder() -> Result<Option<PathBuf>, String> {
    #[cfg(target_os = "windows")]
    {
        let script = "$shell = New-Object -ComObject Shell.Application; $folder = $shell.BrowseForFolder(0, 'Choose folder to host', 0, 0); if ($null -ne $folder) { $folder.Self.Path }";
        let output = Command::new("powershell.exe")
            .args(["-NoProfile", "-Command", script])
            .output()
            .map_err(|error| format!("failed to open Windows folder chooser: {error}"))?;
        if !output.status.success() {
            return Ok(None);
        }
        path_from_stdout(&output.stdout)
    }

    #[cfg(target_os = "macos")]
    {
        let output = Command::new("osascript")
            .args([
                "-e",
                "POSIX path of (choose folder with prompt \"Choose folder to host\")",
            ])
            .output()
            .map_err(|error| format!("failed to open macOS folder chooser: {error}"))?;
        if !output.status.success() {
            return Ok(None);
        }
        path_from_stdout(&output.stdout)
    }

    #[cfg(all(unix, not(target_os = "macos")))]
    {
        if let Ok(output) = Command::new("zenity")
            .args(["--file-selection", "--directory", "--title=Choose folder to host"])
            .output()
        {
            if output.status.success() {
                return path_from_stdout(&output.stdout);
            }
            return Ok(None);
        }
        if let Ok(output) = Command::new("kdialog")
            .args(["--getexistingdirectory", ".", "--title", "Choose folder to host"])
            .output()
        {
            if output.status.success() {
                return path_from_stdout(&output.stdout);
            }
            return Ok(None);
        }
        Err(
            "No native folder chooser was found. Install zenity/kdialog or type the folder path directly."
                .to_owned(),
        )
    }

    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    {
        Err("Folder chooser is unavailable on this platform; type the folder path directly.".to_owned())
    }
}

fn open_path(path: &Path) -> Result<(), String> {
    #[cfg(target_os = "windows")]
    let mut command = Command::new("explorer.exe");
    #[cfg(target_os = "macos")]
    let mut command = Command::new("open");
    #[cfg(all(unix, not(target_os = "macos")))]
    let mut command = Command::new("xdg-open");

    #[cfg(any(target_os = "windows", target_os = "macos", unix))]
    {
        command
            .arg(path)
            .spawn()
            .map_err(|error| format!("failed to open {}: {error}", path.display()))?;
        Ok(())
    }

    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    {
        let _ = path;
        Err("Opening mounted folders is unavailable on this platform.".to_owned())
    }
}