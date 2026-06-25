#include "mainwindow.h"

#include "session.h"
#include "token.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

static QString humanRate(double bytesPerSec) {
    const char* u[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int i = 0;
    while (bytesPerSec >= 1024.0 && i < 3) { bytesPerSec /= 1024.0; ++i; }
    return QString::number(bytesPerSec, 'f', 1) + " " + u[i];
}

// Per-second rate from a monotonic counter sampled every 500 ms. Saturates at 0
// when the counter goes backwards — tearing down a host/mount and restarting it
// makes a fresh object whose counters begin again at 0, which would otherwise
// underflow the unsigned subtraction into a bogus multi-exabyte/s spike.
static double perSec(uint64_t cur, uint64_t& last) {
    uint64_t delta = cur >= last ? cur - last : 0;
    last = cur;
    return static_cast<double>(delta) * 2.0;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Folder Buddies");

    auto* tabs = new QTabWidget;
    tabs->addTab(buildShareTab(), "Host");
    tabs->addTab(buildConnectTab(), "Connect");

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->addWidget(tabs);
    setCentralWidget(central);

    statsLabel_ = new QLabel("Idle");
    statusBar()->addPermanentWidget(statsLabel_);

    statsTimer_ = new QTimer(this);
    connect(statsTimer_, &QTimer::timeout, this, &MainWindow::refreshStats);
    statsTimer_->start(500);

    resize(560, 400);

    setStyleSheet(R"(
        MainWindow {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                         stop:0 #dfe5ec, stop:1 #c7d0db);
        }

        QTabWidget::pane {
            border: 1px solid #d4d4d7;
            border-top: none;
            border-bottom-left-radius: 10px;
            border-bottom-right-radius: 10px;
            background: #ffffff;
        }

        QTabBar::tab {
            background: #e7e7e7;
            border: 1px solid #d4d4d7;
            border-bottom: none;
            padding: 7px 18px;
            border-top-left-radius: 7px;
            border-top-right-radius: 7px;
            color: #3a3a3c;
            font-weight: 500;
            min-width: 80px;
        }

        QTabBar::tab:selected {
            background: #ffffff;
            color: #1d1d1f;
        }

        QTabBar::tab:hover:!selected {
            background: #efefef;
        }

        QLineEdit, QSpinBox {
            border: 1px solid #c2c2c5;
            border-radius: 5px;
            padding: 6px 8px;
            background: #ffffff;
            color: #1d1d1f;
        }

        QLineEdit:focus, QSpinBox:focus {
            border-color: #0a64d6;
        }

        QLineEdit[readOnly="true"] {
            background: #f1f1f2;
            color: #444444;
        }

        QPushButton {
            border: 1px solid #c2c2c5;
            border-radius: 6px;
            padding: 6px 14px;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                         stop:0 #ffffff, stop:1 #efefef);
            color: #1d1d1f;
            font-weight: 500;
        }

        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                         stop:0 #ffffff, stop:1 #e8e8e8);
        }

        QPushButton:pressed {
            background: #e1e1e1;
        }

        QPushButton:disabled {
            color: #999999;
        }

        QPushButton#primaryButton {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                         stop:0 #0a64d6, stop:1 #0a58bd);
            border-color: #0a58bd;
            color: #ffffff;
            font-weight: 600;
            padding: 6px 18px;
        }

        QPushButton#primaryButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                         stop:0 #0f72e6, stop:1 #0a64d6);
        }

        QPushButton#primaryButton:pressed {
            background: #0950a8;
        }

        QStatusBar {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                         stop:0 #f2f2f2, stop:1 #e6e6e6);
            border-top: 1px solid #d4d4d7;
            font-size: 12px;
            color: #6e6e73;
        }

        QCheckBox {
            spacing: 6px;
        }

        QLabel {
            color: #6e6e73;
        }

        QLabel#statusLabel {
            color: #1d1d1f;
            font-weight: 500;
        }
    )");
}

MainWindow::~MainWindow() {
    if (mount_.active()) mount_.stop();
    if (client_) client_->disconnect();
    if (webClient_) webClient_->disconnect();
    if (webCompatHost_) webCompatHost_->stop();
    if (activeTicket_.cloudPublished) {
        std::string derr;
        fb::remove_published_room(activeTicket_, derr);
    }
    upnp_.unmap();
    if (server_) server_->stop();
}

QWidget* MainWindow::buildShareTab() {
    auto* w = new QWidget;
    auto* form = new QFormLayout;

    auto* folderRow = new QWidget;
    auto* fl = new QHBoxLayout(folderRow);
    fl->setContentsMargins(0, 0, 0, 0);
    folderEdit_ = new QLineEdit;
    auto* browse = new QPushButton("Browse…");
    connect(browse, &QPushButton::clicked, this, &MainWindow::browseFolder);
    fl->addWidget(folderEdit_);
    fl->addWidget(browse);
    form->addRow("Folder:", folderRow);

    maxClientsSpin_ = new QSpinBox;
    maxClientsSpin_->setRange(0, 9999);
    maxClientsSpin_->setValue(0);
    maxClientsSpin_->setSpecialValueText("unlimited");
    form->addRow("Max clients:", maxClientsSpin_);

    portSpin_ = new QSpinBox;
    portSpin_->setRange(0, 65535);
    portSpin_->setValue(0);
    portSpin_->setSpecialValueText("auto");
    form->addRow("Port:", portSpin_);

    lanCheck_ = new QCheckBox("Share on this LAN only (don't expose to the internet)");
    form->addRow("", lanCheck_);

    writeCheck_ = new QCheckBox("Allow clients to upload, edit, and delete files");
    form->addRow("Access:", writeCheck_);

    secureHashCheck_ = new QCheckBox("Secure hash mode (share the long code; skip Cloudflare/Firebase)");
    form->addRow("Security:", secureHashCheck_);

    auto* pwNote = new QLabel("Share just the 6-character code (or the offline blob) — there is no "
                             "separate password. Cloudflare only stores an opaque encrypted record "
                             "and never sees the IP, port, or secret.");
    pwNote->setWordWrap(true);
    form->addRow("", pwNote);

    shareButton_ = new QPushButton("Host");
    shareButton_->setObjectName("primaryButton");
    connect(shareButton_, &QPushButton::clicked, this, &MainWindow::toggleShare);
    form->addRow("", shareButton_);

    auto* tokenRow = new QWidget;
    auto* tl = new QHBoxLayout(tokenRow);
    tl->setContentsMargins(0, 0, 0, 0);
    tokenEdit_ = new QLineEdit;
    tokenEdit_->setReadOnly(true);
    tokenEdit_->setPlaceholderText("6-char room code or offline blob appears here");
    copyButton_ = new QPushButton("Copy");
    copyButton_->setEnabled(false);
    connect(copyButton_, &QPushButton::clicked, this, &MainWindow::copyToken);
    tl->addWidget(tokenEdit_);
    tl->addWidget(copyButton_);
    form->addRow("Connect code:", tokenRow);

    offlineEdit_ = new QLineEdit;
    offlineEdit_->setReadOnly(true);
    offlineEdit_->setPlaceholderText("offline fallback blob appears here");
    form->addRow("Offline fallback:", offlineEdit_);

    shareStatus_ = new QLabel("Not hosting.");
    shareStatus_->setObjectName("statusLabel");
    form->addRow("Status:", shareStatus_);

    w->setLayout(form);
    return w;
}

QWidget* MainWindow::buildConnectTab() {
    auto* w = new QWidget;
    auto* form = new QFormLayout;

    tokenInput_ = new QLineEdit;
    tokenInput_->setPlaceholderText("paste the 6-char room code or long offline Base91 blob");
    form->addRow("Connect code:", tokenInput_);

    auto* mbRow = new QWidget;
    auto* ml = new QHBoxLayout(mbRow);
    ml->setContentsMargins(0, 0, 0, 0);
    mountBaseEdit_ = new QLineEdit(QDir::homePath() + "/FolderBuddies");
    auto* browse = new QPushButton("Browse…");
    connect(browse, &QPushButton::clicked, this, &MainWindow::browseMountBase);
    ml->addWidget(mountBaseEdit_);
    ml->addWidget(browse);
    form->addRow("Mount under:", mbRow);

    connsSpin_ = new QSpinBox;
    connsSpin_->setRange(1, 16);
    connsSpin_->setValue(fb::kDefaultConns);
    form->addRow("Connections:", connsSpin_);

    connectButton_ = new QPushButton("Connect && mount");
    connectButton_->setObjectName("primaryButton");
    connect(connectButton_, &QPushButton::clicked, this, &MainWindow::toggleConnect);
    form->addRow("", connectButton_);

    openButton_ = new QPushButton("Open mounted folder");
    openButton_->setEnabled(false);
    connect(openButton_, &QPushButton::clicked, this, &MainWindow::openMount);
    form->addRow("", openButton_);

    connectStatus_ = new QLabel("Not connected.");
    connectStatus_->setObjectName("statusLabel");
    form->addRow("Status:", connectStatus_);

    w->setLayout(form);
    return w;
}

void MainWindow::browseFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, "Choose folder to host");
    if (!dir.isEmpty()) folderEdit_->setText(dir);
}

void MainWindow::browseMountBase() {
    QString dir = QFileDialog::getExistingDirectory(this, "Choose mount base directory");
    if (!dir.isEmpty()) mountBaseEdit_->setText(dir);
}

void MainWindow::setShareRunning(bool running) {
    shareButton_->setText(running ? "Stop hosting" : "Host");
    folderEdit_->setEnabled(!running);
    maxClientsSpin_->setEnabled(!running);
    portSpin_->setEnabled(!running);
    lanCheck_->setEnabled(!running);
    writeCheck_->setEnabled(!running);
    secureHashCheck_->setEnabled(!running);
    copyButton_->setEnabled(running);
    if (!running) {
        offlineEdit_->clear();
    }
}

void MainWindow::toggleShare() {
    if (server_ && server_->running()) {
        if (webCompatHost_) webCompatHost_->stop();
        webCompatHost_.reset();
        upnp_.unmap();
        server_->stop();
        server_.reset();
        if (activeTicket_.cloudPublished) {
            std::string derr;
            fb::remove_published_room(activeTicket_, derr);
        }
        activeTicket_ = fb::HostedShareTicket{};
        tokenEdit_->clear();
        offlineEdit_->clear();
        shareStatus_->setText("Not hosting.");
        setShareRunning(false);
        return;
    }

    QString folder = folderEdit_->text();
    if (folder.isEmpty()) {
        QMessageBox::warning(this, "Folder Buddies", "Choose a folder to host.");
        return;
    }

    server_ = std::make_unique<fb::Server>();
    server_->onClientsChanged = [this] {
        QMetaObject::invokeMethod(this, "onClientsChanged", Qt::QueuedConnection);
    };
    std::string err;
    fb::HostedShareTicket ticket;
    if (!fb::start_hosting(*server_, upnp_, folder.toStdString(), portSpin_->value(),
                           maxClientsSpin_->value(), lanCheck_->isChecked(),
                           writeCheck_->isChecked(), secureHashCheck_->isChecked(), ticket, err)) {
        QMessageBox::critical(this, "Folder Buddies", QString::fromStdString(err));
        upnp_.unmap();
        server_->stop();
        server_.reset();
        return;
    }

    activeTicket_ = ticket;
    if (ticket.cloudPublished && fb::web_compat_available()) {
        webCompatHost_ = std::make_unique<fb::WebRtcCompatHost>();
        std::string werr;
        if (!webCompatHost_->start(folder.toStdString(), ticket.roomCode, writeCheck_->isChecked(),
                                   maxClientsSpin_->value(), werr)) {
            shareStatus_->setText("Native TCP sharing active; WebRTC compatibility failed: " + QString::fromStdString(werr));
        }
    }
    tokenEdit_->setText(QString::fromStdString(ticket.connectCode));
    offlineEdit_->setText(QString::fromStdString(ticket.offlineBlob));
    shareStatus_->setText(QString("Sharing on port %1 — %2 — %3 — 0 client(s)")
                              .arg(server_->boundPort)
                              .arg(QString::fromStdString(ticket.reach))
                              .arg(QString::fromStdString(ticket.cloudStatus)) +
                          (writeCheck_->isChecked() ? " — read/write" : " — read-only"));
    setShareRunning(true);
}

void MainWindow::onClientsChanged() {
    if (!server_ || !server_->running()) return;
    QString t = shareStatus_->text();
    int dash = t.lastIndexOf(" — ");
    if (dash >= 0) t = t.left(dash);
    shareStatus_->setText(t + QString(" — %1 client(s)").arg(server_->clientCount()));
}

void MainWindow::copyToken() {
    QString text = "Connect code:\n" + tokenEdit_->text() +
                   "\n\nOffline fallback:\n" + offlineEdit_->text();
    QApplication::clipboard()->setText(text);
}


void MainWindow::setConnected(bool connected) {
    connectButton_->setText(connected ? "Disconnect" : "Connect && mount");
    tokenInput_->setEnabled(!connected);
    mountBaseEdit_->setEnabled(!connected);
    connsSpin_->setEnabled(!connected);
    openButton_->setEnabled(connected);
}

void MainWindow::toggleConnect() {
    if ((client_ && client_->connected()) || (webClient_ && webClient_->connected())) {
        mount_.stop();
        if (client_) client_->disconnect();
        if (webClient_) webClient_->disconnect();
        client_.reset();
        webClient_.reset();
        connectStatus_->setText("Not connected.");
        setConnected(false);
        return;
    }

    const std::string entered = tokenInput_->text().trimmed().toStdString();
    fb::Token tok;
    std::string decodeErr;
    bool mounted = false;
    std::string err, mountpoint;

    if (fb::resolve_share_code(entered, tok, decodeErr)) {
        client_ = std::make_unique<fb::Client>();
        if (fb::start_mounting(*client_, mount_, tok, mountBaseEdit_->text().toStdString(),
                               connsSpin_->value(), mountpoint, err)) {
            mounted = true;
            connectStatus_->setText("Mounted as " + QString::fromStdString(mountpoint));
        } else {
            client_->disconnect();
            client_.reset();
        }
    }

    if (!mounted && fb::web_compat_available() && fb::looks_like_web_compat_code(entered)) {
        webClient_ = std::make_unique<fb::WebRtcRemoteClient>();
        if (webClient_->connect(entered, err)) {
            std::string name = "Web share";
            if (mount_.start(webClient_.get(), mountBaseEdit_->text().toStdString(), name, true, err)) {
                mountpoint = mount_.mountpoint();
                mounted = true;
                connectStatus_->setText("Mounted via WebRTC compatibility as " + QString::fromStdString(mountpoint));
            }
        }
        if (!mounted) { webClient_->disconnect(); webClient_.reset(); }
    }

    if (!mounted) {
        QMessageBox::critical(this, "Folder Buddies", QString::fromStdString(err.empty() ? decodeErr : err));
        return;
    }

    setConnected(true);
}

void MainWindow::openMount() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(mount_.mountpoint())));
}

void MainWindow::refreshStats() {
    QStringList parts;
    if (server_ && server_->running()) {
        parts << QString("Serve ↑%1 ↓%2")
                     .arg(humanRate(perSec(server_->bytesOut.load(), lastOut_)))
                     .arg(humanRate(perSec(server_->bytesIn.load(), lastIn_)));
    }
    // Prefer the mount's RAM cache: its byte counters reflect what the kernel
    // actually moved (cache hits included). Fall back to the raw transport.
    fb::RemoteFs* activeRemote = nullptr;
    if (mount_.active() && mount_.remote()) activeRemote = mount_.remote();
    else if (client_ && client_->connected()) activeRemote = client_.get();
    else if (webClient_ && webClient_->connected()) activeRemote = webClient_.get();
    if (activeRemote) {
        parts << QString("Mount ↓%1 ↑%2")
                     .arg(humanRate(perSec(activeRemote->bytesRead.load(), lastRead_)))
                     .arg(humanRate(perSec(activeRemote->bytesWritten.load(), lastWritten_)));
    }
    statsLabel_->setText(parts.isEmpty() ? "Idle" : parts.join("   |   "));
}
