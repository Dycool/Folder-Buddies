#include "mainwindow.h"

#include "session.h"
#include "token.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QStatusBar>
#include <QTabBar>
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

static QPlainTextEdit* codeEdit(const QString& placeholder, bool readOnly = false) {
    auto* edit = new QPlainTextEdit;
    edit->setPlaceholderText(placeholder);
    edit->setReadOnly(readOnly);
    edit->setTabChangesFocus(true);
    edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    edit->setFixedHeight(edit->fontMetrics().lineSpacing() * 3 + 22);
    return edit;
}

static QLabel* hintLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setObjectName("hintLabel");
    return label;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Folder Buddies");

    auto* tabs = new QTabWidget;
    tabs->setObjectName("modeTabs");
    tabs->tabBar()->setExpanding(false);
    tabs->addTab(buildShareTab(), "Host");
    tabs->addTab(buildConnectTab(), "Connect");

    auto* central = new QWidget;
    central->setObjectName("windowChrome");
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabs);
    setCentralWidget(central);

    statsLabel_ = new QLabel("Idle");
    statusBar()->addPermanentWidget(statsLabel_);

    statsTimer_ = new QTimer(this);
    connect(statsTimer_, &QTimer::timeout, this, &MainWindow::refreshStats);
    statsTimer_->start(500);

    setFixedSize(560, 460);

    setStyleSheet(R"(
        QMainWindow {
            background: #f5f5f5;
            font-size: 13px;
            color: #1d1d1f;
        }

        QWidget#windowChrome {
            background: #f5f5f5;
        }

        QWidget#tabPage {
            background: #ffffff;
        }

        QTabWidget#modeTabs::pane {
            border: none;
            border-top: 1px solid #d4d4d7;
            background: #ffffff;
        }

        QTabWidget#modeTabs::tab-bar {
            alignment: center;
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

        QLineEdit, QSpinBox, QPlainTextEdit {
            border: 1px solid #c2c2c5;
            border-radius: 5px;
            padding: 6px 8px;
            background: #ffffff;
            color: #1d1d1f;
        }

        QPlainTextEdit {
            font-family: Menlo, Consolas, monospace;
            font-size: 12px;
        }

        QLineEdit:focus, QSpinBox:focus, QPlainTextEdit:focus {
            border-color: #0a64d6;
        }

        QLineEdit[readOnly="true"], QPlainTextEdit[readOnly="true"] {
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
            color: #1d1d1f;
        }

        QLabel {
            color: #6e6e73;
        }

        QLabel#hintLabel {
            color: #6e6e73;
            font-size: 12px;
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
    w->setObjectName("tabPage");
    auto* form = new QFormLayout(w);
    form->setContentsMargins(18, 18, 18, 18);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    auto* folderRow = new QWidget;
    auto* fl = new QHBoxLayout(folderRow);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setSpacing(8);
    folderEdit_ = new QLineEdit;
    folderEdit_->setReadOnly(true);
    folderEdit_->setPlaceholderText("Choose a folder to host");
    auto* browse = new QPushButton("Browse…");
    connect(browse, &QPushButton::clicked, this, &MainWindow::browseFolder);
    fl->addWidget(folderEdit_);
    fl->addWidget(browse);
    form->addRow("Folder:", folderRow);

    lanCheck_ = new QCheckBox("Share on this LAN only");
    form->addRow("", lanCheck_);

    writeCheck_ = new QCheckBox("Allow clients to upload and delete files");
    form->addRow("Access:", writeCheck_);

    shareButton_ = new QPushButton("Host");
    shareButton_->setObjectName("primaryButton");
    connect(shareButton_, &QPushButton::clicked, this, &MainWindow::toggleShare);
    form->addRow("", shareButton_);

    auto* tokenRow = new QWidget;
    auto* tl = new QHBoxLayout(tokenRow);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(8);
    tokenEdit_ = codeEdit("", true);
    copyButton_ = new QPushButton("Copy");
    copyButton_->setEnabled(false);
    connect(copyButton_, &QPushButton::clicked, this, &MainWindow::copyToken);
    tl->addWidget(tokenEdit_);
    tl->addWidget(copyButton_);
    form->addRow("Connect code:", tokenRow);

    shareStatus_ = new QLabel("Not hosting.");
    shareStatus_->setObjectName("statusLabel");
    form->addRow("Status:", shareStatus_);

    return w;
}

QWidget* MainWindow::buildConnectTab() {
    auto* w = new QWidget;
    w->setObjectName("tabPage");
    auto* form = new QFormLayout(w);
    form->setContentsMargins(18, 18, 18, 18);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(12);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    tokenInput_ = codeEdit("");
    form->addRow("Connect code:", tokenInput_);

    connectButton_ = new QPushButton("Connect");
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

    return w;
}

void MainWindow::browseFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, "Choose folder to host");
    if (!dir.isEmpty()) folderEdit_->setText(dir);
}

void MainWindow::setShareRunning(bool running) {
    shareButton_->setText(running ? "Stop hosting" : "Host");
    folderEdit_->setEnabled(!running);
    lanCheck_->setEnabled(!running);
    writeCheck_->setEnabled(!running);
    copyButton_->setEnabled(running);
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
    if (!fb::start_hosting(*server_, upnp_, folder.toStdString(), 0,
                           lanCheck_->isChecked(), writeCheck_->isChecked(), ticket, err)) {
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
        if (!webCompatHost_->start(folder.toStdString(), ticket.roomCode, writeCheck_->isChecked(), werr)) {
        }
    }
    tokenEdit_->setPlainText(QString::fromStdString(ticket.connectCode));
    shareStatus_->setText(QString("Hosting — 0 client(s)") +
                          (writeCheck_->isChecked() ? " — read/write" : " — read-only"));
    setShareRunning(true);
}

void MainWindow::onClientsChanged() {
    if (!server_ || !server_->running()) return;
    QString t = shareStatus_->text();
    int dash = t.indexOf(" — ");
    if (dash >= 0) t = t.left(dash);
    shareStatus_->setText(t + QString(" — %1 client(s)").arg(server_->clientCount()) +
                          (server_->allowWrites ? " — read/write" : " — read-only"));
}

void MainWindow::copyToken() {
    QApplication::clipboard()->setText(tokenEdit_->toPlainText());
}


void MainWindow::setConnected(bool connected) {
    connectButton_->setText(connected ? "Disconnect" : "Connect");
    tokenInput_->setEnabled(!connected);
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

    const std::string entered = tokenInput_->toPlainText().trimmed().toStdString();
    fb::Token tok;
    std::string decodeErr;
    bool mounted = false;
    std::string err, mountpoint;

    if (fb::resolve_share_code(entered, tok, decodeErr)) {
        client_ = std::make_unique<fb::Client>();
        if (fb::start_mounting(*client_, mount_, tok, mountpoint, err)) {
            mounted = true;
            connectStatus_->setText("Connected.");
        } else {
            client_->disconnect();
            client_.reset();
        }
    }

    if (!mounted && fb::web_compat_available() && fb::looks_like_web_compat_code(entered)) {
        webClient_ = std::make_unique<fb::WebRtcRemoteClient>();
        if (webClient_->connect(entered, err)) {
            std::string name = "Web share";
            if (mount_.start(webClient_.get(), "", name, true, err)) {
                mountpoint = mount_.mountpoint();
                mounted = true;
                connectStatus_->setText("Connected.");
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
