use std::{
    path::{Path, PathBuf},
    process::Command,
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

use eframe::egui;

use crate::runtime::{ConnectedSession, HostingSession, PendingConnection};

const REPAINT_INTERVAL: Duration = Duration::from_millis(500);

// Keep ownership of in-flight sessions until the window consumes the result.
// Closing the window joins the worker and drops its result, cleaning up any
// share or mount that completed after the event loop stopped.
struct Background<T>(Option<JoinHandle<Result<T, String>>>);

impl<T: Send + 'static> Background<T> {
    fn start(work: impl FnOnce() -> Result<T, String> + Send + 'static) -> Self {
        Self(Some(thread::spawn(work)))
    }

    fn take_ready(&mut self) -> Option<Result<T, String>> {
        if !self.0.as_ref().is_some_and(JoinHandle::is_finished) {
            return None;
        }
        self.0.take().map(|worker| {
            worker
                .join()
                .unwrap_or_else(|_| Err("Background operation failed.".to_owned()))
        })
    }
}

impl<T> Drop for Background<T> {
    fn drop(&mut self) {
        if let Some(worker) = self.0.take() {
            let _ = worker.join();
        }
    }
}

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
    host_worker: Option<Background<Option<HostingSession>>>,
    stopping_host: bool,
    connect_worker: Option<Background<PendingConnection>>,
    browser_warning: Option<String>,
    host_status: String,
    connect_status: String,
    error_dialog: Option<String>,
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
            host_worker: None,
            stopping_host: false,
            connect_worker: None,
            browser_warning: None,
            host_status: "Not hosting.".to_owned(),
            connect_status: "Not connected.".to_owned(),
            error_dialog: None,
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
        if self.host_worker.is_some() {
            return;
        }
        let folder = self.folder_path.trim();
        if folder.is_empty() {
            self.set_error("Choose a folder to host.");
            return;
        }
        let folder = folder.to_owned();
        let lan_only = self.lan_only;
        let allow_writes = self.allow_writes;
        self.stopping_host = false;
        self.host_worker = Some(Background::start(move || {
            HostingSession::start(folder, lan_only, allow_writes).map(Some)
        }));
        self.host_status = "Starting…".to_owned();
    }

    fn finish_background_work(&mut self) {
        if let Some(result) = self.host_worker.as_mut().and_then(Background::take_ready) {
            self.host_worker = None;
            match result {
                Ok(Some(mut session)) => {
                    self.browser_warning = session.take_browser_warning();
                    self.hosting = Some(session);
                    self.reset_rates();
                    self.host_status = "Hosting started.".to_owned();
                }
                Ok(None) => self.host_status = "Not hosting.".to_owned(),
                Err(error) => {
                    self.host_status = "Not hosting.".to_owned();
                    self.set_error(error);
                }
            }
        }
        if let Some(result) = self
            .connect_worker
            .as_mut()
            .and_then(Background::take_ready)
        {
            self.connect_worker = None;
            match result.and_then(PendingConnection::mount) {
                Ok(session) => {
                    let mount = session.mount_path().display().to_string();
                    let displayed_mount = mount
                        .strip_suffix('/')
                        .or_else(|| mount.strip_suffix('\\'))
                        .unwrap_or(&mount);
                    self.connect_status = format!("Connected - Mounted in {displayed_mount}");
                    self.connected = Some(session);
                    self.reset_rates();
                }
                Err(error) => {
                    self.connect_status = "Not connected.".to_owned();
                    self.set_error(error);
                }
            }
        }
    }

    fn stop_hosting(&mut self) {
        if self.host_worker.is_some() {
            return;
        }
        if let Some(mut session) = self.hosting.take() {
            self.stopping_host = true;
            self.host_worker = Some(Background::start(move || {
                session.stop();
                Ok(None)
            }));
        }
        self.browser_warning = None;
        self.reset_rates();
        self.host_status = "Stopping…".to_owned();
    }

    fn connect(&mut self) {
        if self.connect_worker.is_some() {
            return;
        }
        let code = self.connect_code.trim();
        if code.is_empty() {
            self.set_error("Paste a connect code first.");
            return;
        }
        let code = code.to_owned();
        self.connect_worker = Some(Background::start(move || PendingConnection::start(&code)));
        self.connect_status = "Connecting…".to_owned();
    }

    fn disconnect(&mut self) {
        if let Some(mut session) = self.connected.take() {
            session.disconnect();
        }
        self.reset_rates();
        self.connect_status = "Not connected.".to_owned();
    }

    fn refresh_runtime_state(&mut self) {
        if self
            .hosting
            .as_ref()
            .is_some_and(|session| !session.running())
        {
            self.hosting.take();
            self.host_status = "Not hosting.".to_owned();
            self.set_error("Hosting stopped unexpectedly.");
        }
        if self
            .connected
            .as_ref()
            .is_some_and(|session| !session.connected())
        {
            let ejected = self
                .connected
                .as_ref()
                .is_some_and(ConnectedSession::ejected);
            self.connected.take();
            self.reset_rates();
            if ejected {
                self.connect_status = "Disconnected (ejected).".to_owned();
            } else {
                self.connect_status = "Not connected.".to_owned();
                self.set_error("The remote share disconnected.");
            }
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
        self.host_send_rate = per_second(host_sent, &mut self.last_host_sent);
        self.host_receive_rate = per_second(host_received, &mut self.last_host_received);
        self.read_rate = per_second(read, &mut self.last_read);
        self.write_rate = per_second(written, &mut self.last_written);
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

    fn set_error(&mut self, message: impl Into<String>) {
        self.error_dialog = Some(message.into());
    }

    fn host_ui(&mut self, ui: &mut egui::Ui) {
        let running = self.hosting.as_ref().is_some_and(HostingSession::running);
        let busy = self.host_worker.is_some();
        form_label(ui, 62.0, "Folder:");
        let mut folder_text = self.folder_path.as_str();
        edit_control(
            ui,
            [108.0, 62.0, 348.0, 32.0],
            !running && !busy,
            true,
            egui::TextEdit::singleline(&mut folder_text)
                .hint_text("Choose a folder to host")
                .background_color(egui::Color32::from_gray(240)),
        );
        if control(
            ui,
            [464.0, 63.0, 78.0, 30.0],
            true,
            secondary_button("Browse…"),
        )
        .clicked()
        {
            match choose_folder() {
                Ok(Some(path)) => self.folder_path = path.display().to_string(),
                Ok(None) => {}
                Err(error) => self.set_error(error),
            }
        }
        left_control(
            ui,
            [108.0, 105.0, 434.0, 20.0],
            !running && !busy,
            egui::Checkbox::new(&mut self.lan_only, "Share on this LAN only"),
        );
        form_label(ui, 136.0, "Access:");
        left_control(
            ui,
            [108.0, 136.0, 434.0, 20.0],
            !running && !busy,
            egui::Checkbox::new(
                &mut self.allow_writes,
                "Allow clients to upload and delete files",
            ),
        );
        let button = if busy {
            if self.stopping_host {
                "Stopping…"
            } else {
                "Starting…"
            }
        } else if running {
            "Stop hosting"
        } else {
            "Host"
        };
        let width = if button == "Host" { 64.0 } else { 110.0 };
        if control(
            ui,
            [108.0, 170.0, width, 30.0],
            !busy,
            primary_button(button),
        )
        .clicked()
        {
            if running {
                self.stop_hosting();
            } else {
                self.start_hosting();
            }
        }
        form_label(ui, 212.0, "Connect code:");
        let code = self
            .hosting
            .as_ref()
            .map_or(String::new(), |s| s.connect_code().to_owned());
        let mut code_text = code.as_str();
        edit_control(
            ui,
            [108.0, 212.0, 368.0, 70.0],
            true,
            true,
            egui::TextEdit::multiline(&mut code_text)
                .background_color(egui::Color32::from_gray(240))
                .font(egui::TextStyle::Monospace),
        );
        if control(
            ui,
            [484.0, 232.0, 58.0, 30.0],
            running,
            secondary_button("Copy"),
        )
        .clicked()
        {
            ui.ctx().copy_text(code);
        }
        form_label(ui, 292.0, "Status:");
        let status = self.hosting.as_ref().map_or_else(
            || self.host_status.clone(),
            |host| {
                let (native, browser) = host.client_counts();
                let mut text = format!("Hosting — {} client(s)", native.saturating_add(browser));
                if browser > 0 {
                    text.push_str(&format!(" ({native} native, {browser} browser)"));
                }
                text.push_str(if host.allow_writes() {
                    " — read/write"
                } else {
                    " — read-only"
                });
                text
            },
        );
        left_control(
            ui,
            [108.0, 292.0, 434.0, 22.0],
            true,
            egui::Label::new(status).halign(egui::Align::LEFT),
        );
    }

    fn connect_ui(&mut self, ui: &mut egui::Ui) {
        let busy = self.connect_worker.is_some();
        let connected = self
            .connected
            .as_ref()
            .is_some_and(ConnectedSession::connected);
        form_label(ui, 62.0, "Connect code:");
        edit_control(
            ui,
            [108.0, 62.0, 434.0, 70.0],
            !connected && !busy,
            false,
            egui::TextEdit::multiline(&mut self.connect_code).font(egui::TextStyle::Monospace),
        );
        let button = if busy {
            "Connecting…"
        } else if connected {
            "Disconnect"
        } else {
            "Connect"
        };
        if control(
            ui,
            [108.0, 144.0, 100.0, 30.0],
            !busy,
            primary_button(button),
        )
        .clicked()
        {
            if connected {
                self.disconnect();
            } else {
                self.connect();
            }
        }
        if control(
            ui,
            [108.0, 186.0, 164.0, 30.0],
            connected,
            secondary_button("Open mounted folder"),
        )
        .clicked()
            && let Some(session) = self.connected.as_ref()
        {
            let _ = open_path(session.mount_path());
        }
        form_label(ui, 228.0, "Status:");
        left_control(
            ui,
            [108.0, 228.0, 434.0, 22.0],
            true,
            egui::Label::new(&self.connect_status).halign(egui::Align::LEFT),
        );
    }

    fn stats(&self) -> String {
        let mut parts = Vec::new();
        if self.hosting.as_ref().is_some_and(HostingSession::running) {
            parts.push(format!(
                "Serve ↑{} ↓{}",
                human_rate(self.host_send_rate),
                human_rate(self.host_receive_rate)
            ));
        }
        if self.connected.is_some() {
            parts.push(format!(
                "Mount ↓{} ↑{}",
                human_rate(self.read_rate),
                human_rate(self.write_rate)
            ));
        }
        if parts.is_empty() {
            "Idle".to_owned()
        } else {
            parts.join("   |   ")
        }
    }
}

fn control(
    ui: &mut egui::Ui,
    bounds: [f32; 4],
    enabled: bool,
    widget: impl egui::Widget,
) -> egui::Response {
    let [x, y, width, height] = bounds;
    let rect = egui::Rect::from_min_size(
        ui.max_rect().min + egui::vec2(x, y),
        egui::vec2(width, height),
    );
    ui.add_enabled_ui(enabled, |ui| ui.put(rect, widget)).inner
}

fn form_label(ui: &mut egui::Ui, y: f32, text: &str) {
    let rect = egui::Rect::from_min_size(
        ui.max_rect().min + egui::vec2(18.0, y),
        egui::vec2(78.0, 22.0),
    );
    ui.scope_builder(
        egui::UiBuilder::new()
            .max_rect(rect)
            .layout(egui::Layout::right_to_left(egui::Align::Center)),
        |ui| {
            ui.label(egui::RichText::new(text).color(egui::Color32::from_rgb(110, 110, 115)));
        },
    );
}

fn left_control(
    ui: &mut egui::Ui,
    bounds: [f32; 4],
    enabled: bool,
    widget: impl egui::Widget,
) -> egui::Response {
    let [x, y, width, height] = bounds;
    let rect = egui::Rect::from_min_size(
        ui.max_rect().min + egui::vec2(x, y),
        egui::vec2(width, height),
    );
    ui.scope_builder(
        egui::UiBuilder::new()
            .max_rect(rect)
            .layout(egui::Layout::left_to_right(egui::Align::Center)),
        |ui| ui.add_enabled(enabled, widget),
    )
    .inner
}

fn primary_button(text: &str) -> egui::Button<'_> {
    egui::Button::new(egui::RichText::new(text).color(egui::Color32::WHITE))
        .corner_radius(5)
        .fill(egui::Color32::from_rgb(10, 100, 214))
        .stroke(egui::Stroke::new(1.0, egui::Color32::from_rgb(10, 88, 189)))
}

fn secondary_button(text: &str) -> egui::Button<'_> {
    egui::Button::new(text)
        .corner_radius(5)
        .fill(egui::Color32::from_gray(245))
        .stroke(egui::Stroke::new(
            1.0,
            egui::Color32::from_rgb(194, 194, 197),
        ))
}

fn edit_control(
    ui: &mut egui::Ui,
    bounds: [f32; 4],
    enabled: bool,
    read_only: bool,
    edit: egui::TextEdit<'_>,
) -> egui::Response {
    let [x, y, width, height] = bounds;
    let rect = egui::Rect::from_min_size(
        ui.max_rect().min + egui::vec2(x, y),
        egui::vec2(width, height),
    );
    ui.painter().rect(
        rect,
        5,
        if read_only {
            egui::Color32::from_gray(240)
        } else {
            egui::Color32::WHITE
        },
        egui::Stroke::new(1.0, egui::Color32::from_rgb(194, 194, 197)),
        egui::StrokeKind::Inside,
    );
    control(
        ui,
        [x + 8.0, y + 6.0, width - 16.0, height - 12.0],
        enabled,
        edit.frame(egui::Frame::NONE),
    )
}

impl eframe::App for FolderBuddiesApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.finish_background_work();
        self.refresh_runtime_state();
        self.sample_rates();
        ui.ctx().request_repaint_after(REPAINT_INTERVAL);

        let origin = ui.max_rect().min;
        let width = ui.max_rect().width();
        let height = ui.max_rect().height();
        let painter = ui.painter();
        painter.rect_filled(ui.max_rect(), 0.0, egui::Color32::WHITE);
        painter.rect_filled(
            egui::Rect::from_min_size(origin, egui::vec2(width, 44.0)),
            0.0,
            egui::Color32::from_gray(245),
        );
        painter.hline(
            origin.x..=origin.x + width,
            origin.y + 44.0,
            egui::Stroke::new(1.0, egui::Color32::from_rgb(212, 212, 215)),
        );
        painter.rect_filled(
            egui::Rect::from_min_size(
                origin + egui::vec2(0.0, height - 22.0),
                egui::vec2(width, 22.0),
            ),
            0.0,
            egui::Color32::from_gray(238),
        );
        for (tab, label, x) in [(Tab::Host, "Host", 162.0), (Tab::Connect, "Connect", 280.0)] {
            let fill = if self.tab == tab {
                egui::Color32::WHITE
            } else {
                egui::Color32::from_gray(231)
            };
            if control(
                ui,
                [x, 12.0, 118.0, 32.0],
                true,
                egui::Button::new(label)
                    .fill(fill)
                    .corner_radius(egui::CornerRadius {
                        nw: 7,
                        ne: 7,
                        sw: 0,
                        se: 0,
                    })
                    .stroke(egui::Stroke::new(
                        1.0,
                        egui::Color32::from_rgb(212, 212, 215),
                    )),
            )
            .clicked()
            {
                self.tab = tab;
            }
        }
        match self.tab {
            Tab::Host => self.host_ui(ui),
            Tab::Connect => self.connect_ui(ui),
        }
        let stats_rect = egui::Rect::from_min_size(
            origin + egui::vec2(6.0, height - 22.0),
            egui::vec2(width - 12.0, 22.0),
        );
        ui.scope_builder(
            egui::UiBuilder::new()
                .max_rect(stats_rect)
                .layout(egui::Layout::right_to_left(egui::Align::Center)),
            |ui| {
                ui.label(
                    egui::RichText::new(self.stats()).color(egui::Color32::from_rgb(110, 110, 115)),
                );
            },
        );
        if let Some(message) = self.error_dialog.clone() {
            let modal = egui::Modal::new(egui::Id::new("operation_error")).show(ui.ctx(), |ui| {
                ui.heading("Folder Buddies");
                ui.label(message);
                ui.button("OK").clicked()
            });
            if modal.inner || modal.should_close() {
                self.error_dialog = None;
            }
        }

        if let Some(warning) = self.browser_warning.clone() {
            let modal = egui::Modal::new(egui::Id::new("browser_compatibility_warning")).show(
                ui.ctx(),
                |ui| {
                    ui.heading("Browser Compatibility Unavailable");
                    ui.label("Native sharing is running, but browser clients cannot connect:");
                    ui.add_space(8.0);
                    ui.label(warning);
                    ui.add_space(8.0);
                    ui.button("OK").clicked()
                },
            );
            if modal.inner || modal.should_close() {
                self.browser_warning = None;
            }
        }
    }
}

pub(crate) fn run_gui() -> Result<(), String> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([560.0, 354.0])
            .with_maximize_button(false)
            .with_icon(
                eframe::icon_data::from_png_bytes(include_bytes!("../../../icon.png"))
                    .map_err(|error| error.to_string())?,
            )
            .with_resizable(false),
        centered: true,
        ..Default::default()
    };
    eframe::run_native(
        "Folder Buddies",
        options,
        Box::new(|creation_context| {
            let ctx = &creation_context.egui_ctx;
            ctx.set_visuals(egui::Visuals::light());
            let mut style = (*ctx.style_of(egui::Theme::Light)).clone();
            style.override_font_id = Some(egui::FontId::proportional(12.0));
            style.spacing.button_padding = egui::vec2(10.0, 6.0);
            style.spacing.icon_width = 14.0;
            style.spacing.icon_spacing = 7.0;
            style.visuals.override_text_color = Some(egui::Color32::from_rgb(29, 29, 31));
            style.visuals.widgets.inactive.bg_fill = egui::Color32::WHITE;
            style.visuals.widgets.inactive.bg_stroke =
                egui::Stroke::new(1.0, egui::Color32::from_rgb(156, 163, 175));
            style.visuals.widgets.inactive.corner_radius = egui::CornerRadius::same(3);
            ctx.set_style_of(egui::Theme::Light, style);
            ctx.set_theme(egui::Theme::Light);
            #[cfg(windows)]
            if let Ok(bytes) = std::fs::read("C:/Windows/Fonts/segoeui.ttf") {
                let mut fonts = egui::FontDefinitions::default();
                fonts.font_data.insert(
                    "system".to_owned(),
                    egui::FontData::from_owned(bytes).into(),
                );
                fonts
                    .families
                    .entry(egui::FontFamily::Proportional)
                    .or_default()
                    .insert(0, "system".to_owned());
                ctx.set_fonts(fonts);
            }
            Ok(Box::new(FolderBuddiesApp::default()))
        }),
    )
    .map_err(|error| format!("failed to launch GUI: {error}"))
}

fn per_second(current: u64, last: &mut u64) -> f64 {
    let delta = current.saturating_sub(*last);
    *last = current;
    delta as f64 * 2.0
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
        let script = "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false); $shell = New-Object -ComObject Shell.Application; $folder = $shell.BrowseForFolder(0, 'Choose folder to host', 0, 0); if ($null -ne $folder) { $folder.Self.Path }";
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
            .args([
                "--file-selection",
                "--directory",
                "--title=Choose folder to host",
            ])
            .output()
        {
            if output.status.success() {
                return path_from_stdout(&output.stdout);
            }
            return Ok(None);
        }
        if let Ok(output) = Command::new("kdialog")
            .args([
                "--getexistingdirectory",
                ".",
                "--title",
                "Choose folder to host",
            ])
            .output()
        {
            if output.status.success() {
                return path_from_stdout(&output.stdout);
            }
            return Ok(None);
        }
        Err(
            "No native folder chooser was found. Install zenity or kdialog to choose a folder."
                .to_owned(),
        )
    }

    #[cfg(not(any(target_os = "windows", target_os = "macos", unix)))]
    {
        Err("Folder chooser is unavailable on this platform.".to_owned())
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

#[cfg(test)]
mod tests {
    use super::{Background, FolderBuddiesApp};
    use std::sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc,
    };

    #[test]
    fn empty_connect_error_does_not_replace_host_status() {
        let mut app = FolderBuddiesApp {
            host_status: "Starting…".to_owned(),
            ..Default::default()
        };
        app.connect();
        assert_eq!(app.host_status, "Starting…");
        assert_eq!(app.connect_status, "Not connected.");
        assert_eq!(
            app.error_dialog.as_deref(),
            Some("Paste a connect code first.")
        );
        assert!(app.connect_worker.is_none());
    }

    #[test]
    fn empty_host_error_does_not_replace_connection_status() {
        let mut app = FolderBuddiesApp {
            connect_status: "Connecting…".to_owned(),
            ..Default::default()
        };
        app.start_hosting();
        assert_eq!(app.connect_status, "Connecting…");
        assert_eq!(app.host_status, "Not hosting.");
        assert_eq!(
            app.error_dialog.as_deref(),
            Some("Choose a folder to host.")
        );
        assert!(app.host_worker.is_none());
    }

    #[test]
    fn pending_network_work_does_not_block_ui_polling() {
        let (release, wait) = mpsc::channel();
        let mut work = Background::start(move || {
            wait.recv().expect("release worker");
            Ok(42)
        });
        assert!(work.take_ready().is_none());
        release.send(()).expect("worker is alive");
        while !work.0.as_ref().expect("worker").is_finished() {
            std::thread::yield_now();
        }
        assert_eq!(work.take_ready(), Some(Ok(42)));
        assert!(work.take_ready().is_none());
    }

    #[test]
    fn closing_window_cleans_up_unconsumed_session() {
        struct Session(Arc<AtomicBool>);
        impl Drop for Session {
            fn drop(&mut self) {
                self.0.store(true, Ordering::SeqCst);
            }
        }
        let cleaned = Arc::new(AtomicBool::new(false));
        let flag = Arc::clone(&cleaned);
        let work = Background::start(move || Ok(Session(flag)));
        drop(work);
        assert!(cleaned.load(Ordering::SeqCst));
    }

    #[test]
    fn worker_failure_is_reported_without_panicking_ui() {
        let mut work = Background::<()>::start(|| panic!("simulated failure"));
        while !work.0.as_ref().expect("worker").is_finished() {
            std::thread::yield_now();
        }
        assert!(work.take_ready().expect("finished").is_err());
    }
}
