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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Folder Buddies");
    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildShareTab(), "Host");
    tabs->addTab(buildConnectTab(), "Connect");
    setCentralWidget(tabs);

    statsLabel_ = new QLabel("Idle", this);
    statusBar()->addPermanentWidget(statsLabel_);

    statsTimer_ = new QTimer(this);
    connect(statsTimer_, &QTimer::timeout, this, &MainWindow::refreshStats);
    statsTimer_->start(500);

    resize(560, 360);
}

MainWindow::~MainWindow() {
    if (mount_.active()) mount_.stop();
    if (client_) client_->disconnect();
    if (activeTicket_.cloudPublished) {
        std::string derr;
        fb::SignalingClient().remove(activeTicket_.lookupId, activeTicket_.ownerToken, derr);
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

    auto* pwNote = new QLabel("Share just the 6-character code (or the offline blob) — there is no "
                             "separate password. Cloudflare only stores an opaque encrypted record "
                             "and never sees the IP, port, or secret.");
    pwNote->setWordWrap(true);
    form->addRow("", pwNote);

    shareButton_ = new QPushButton("Host");
    connect(shareButton_, &QPushButton::clicked, this, &MainWindow::toggleShare);
    form->addRow("", shareButton_);

    auto* tokenRow = new QWidget;
    auto* tl = new QHBoxLayout(tokenRow);
    tl->setContentsMargins(0, 0, 0, 0);
    tokenEdit_ = new QLineEdit;
    tokenEdit_->setReadOnly(true);
    tokenEdit_->setPlaceholderText("6-char room code or offline blob appears here");
    copyButton_ = new QPushButton("Copy all");
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
    connect(connectButton_, &QPushButton::clicked, this, &MainWindow::toggleConnect);
    form->addRow("", connectButton_);

    openButton_ = new QPushButton("Open mounted folder");
    openButton_->setEnabled(false);
    connect(openButton_, &QPushButton::clicked, this, &MainWindow::openMount);
    form->addRow("", openButton_);

    connectStatus_ = new QLabel("Not connected.");
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
    copyButton_->setEnabled(running);
    if (!running) {
        offlineEdit_->clear();
    }
}

void MainWindow::toggleShare() {
    if (server_ && server_->running()) {
        upnp_.unmap();
        server_->stop();
        server_.reset();
        if (activeTicket_.cloudPublished) {
            std::string derr;
            fb::SignalingClient().remove(activeTicket_.lookupId, activeTicket_.ownerToken, derr);
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
                           writeCheck_->isChecked(), ticket, err)) {
        QMessageBox::critical(this, "Folder Buddies", QString::fromStdString(err));
        upnp_.unmap();
        server_->stop();
        server_.reset();
        return;
    }

    activeTicket_ = ticket;
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
    if (client_ && client_->connected()) {
        mount_.stop();
        client_->disconnect();
        client_.reset();
        connectStatus_->setText("Not connected.");
        setConnected(false);
        return;
    }

    fb::Token tok;
    std::string decodeErr;
    if (!fb::resolve_share_code(tokenInput_->text().trimmed().toStdString(), tok, decodeErr)) {
        QMessageBox::warning(this, "Folder Buddies", QString::fromStdString(decodeErr));
        return;
    }

    client_ = std::make_unique<fb::Client>();
    std::string err, mountpoint;
    if (!fb::start_mounting(*client_, mount_, tok, mountBaseEdit_->text().toStdString(),
                            connsSpin_->value(), mountpoint, err)) {
        QMessageBox::critical(this, "Folder Buddies", QString::fromStdString(err));
        client_.reset();
        return;
    }

    connectStatus_->setText("Mounted as " + QString::fromStdString(mountpoint));
    setConnected(true);
}

void MainWindow::openMount() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(mount_.mountpoint())));
}

void MainWindow::refreshStats() {
    QStringList parts;
    if (server_ && server_->running()) {
        uint64_t out = server_->bytesOut.load(), in = server_->bytesIn.load();
        parts << QString("Serve ↑%1 ↓%2")
                     .arg(humanRate((out - lastOut_) * 2.0))
                     .arg(humanRate((in - lastIn_) * 2.0));
        lastOut_ = out;
        lastIn_ = in;
    }
    if (client_ && client_->connected()) {
        uint64_t rd = client_->bytesRead.load(), wr = client_->bytesWritten.load();
        parts << QString("Mount ↓%1 ↑%2")
                     .arg(humanRate((rd - lastRead_) * 2.0))
                     .arg(humanRate((wr - lastWritten_) * 2.0));
        lastRead_ = rd;
        lastWritten_ = wr;
    }
    statsLabel_->setText(parts.isEmpty() ? "Idle" : parts.join("   |   "));
}
