#include "mainwindow.h"

#include "session.h"
#include "token.h"
#include "fuse_backend.h"
#include "native_quic.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <thread>
#include <utility>

namespace {

// Carried back from the connect worker thread to the GUI thread.
struct ConnectResult {
    std::unique_ptr<fb::Client> client;
    std::unique_ptr<fb::NativeQuicRemoteClient> quicClient;
    std::unique_ptr<fb::WebRtcRemoteClient> webClient;
    fb::Token tok;
    bool ok = false;
    std::string err;
    std::string decodeErr;
};

// Carried back from the host worker thread to the GUI thread.
struct HostResult {
    fb::HostedShareTicket ticket;
    std::unique_ptr<fb::WebRtcCompatHost> webCompatHost;
    std::string webCompatWarning;
    bool ok = false;
    std::string err;
};

} // namespace

static QString humanRate(double bytesPerSec) {
    const char* u[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int i = 0;
    while (bytesPerSec >= 1024.0 && i < 3) { bytesPerSec /= 1024.0; ++i; }
    return QString::number(bytesPerSec, 'f', 1) + " " + u[i];
}

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

class CleanCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;

    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        const int box = 14;
        const int gap = 7;
        return QSize(box + gap + fm.horizontalAdvance(text()),
                     std::max(20, fm.height() + 4));
    }

protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::Enter || e->type() == QEvent::Leave)
            update();
        return QCheckBox::event(e);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const int boxSize = 14;
        const QRect box(0, (height() - boxSize) / 2, boxSize, boxSize);
        const bool on = checkState() != Qt::Unchecked;
        const bool mixed = checkState() == Qt::PartiallyChecked;
        const bool active = isEnabled();

        QColor border = on ? QColor("#0a64d6") : QColor("#9ca3af");
        QColor fill = on ? QColor("#0a64d6") : QColor("#ffffff");
        QColor textColor = active ? QColor("#1d1d1f") : QColor("#9a9a9f");

        if (!active) {
            border = QColor("#c9c9ce");
            fill = on ? QColor("#aac8f2") : QColor("#f4f4f5");
        } else if (isDown()) {
            fill = on ? QColor("#084fa8") : QColor("#f3f4f6");
            border = on ? QColor("#084fa8") : QColor("#6b7280");
        } else if (underMouse()) {
            border = on ? QColor("#0a58bd") : QColor("#6b7280");
        }

        painter.setPen(QPen(border, 1.0));
        painter.setBrush(fill);
        painter.drawRoundedRect(box.adjusted(0, 0, -1, -1), 3, 3);

        if (on && !mixed) {
            QPen tickPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(tickPen);
            QPainterPath tick;
            tick.moveTo(box.left() + 3.2, box.top() + 7.2);
            tick.lineTo(box.left() + 5.9, box.top() + 9.8);
            tick.lineTo(box.left() + 10.9, box.top() + 4.4);
            painter.drawPath(tick);
        } else if (mixed) {
            painter.setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(box.left() + 3, box.center().y(), box.right() - 3, box.center().y());
        }

        if (hasFocus()) {
            painter.setPen(QPen(QColor("#0a64d6"), 1.0, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box.adjusted(-2, -2, 1, 1), 5, 5);
        }

        painter.setPen(textColor);
        const QRect textRect(box.right() + 7, 0, width() - box.right() - 7, height());
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, text());
    }
};

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
    statsLabel_->setContentsMargins(0, 0, 6, 0);
    statusBar()->addPermanentWidget(statsLabel_);
    statusBar()->setSizeGripEnabled(false);

    statsTimer_ = new QTimer(this);
    connect(statsTimer_, &QTimer::timeout, this, &MainWindow::refreshStats);
    statsTimer_->start(500);

    mount_.setEjectedCallback([this] {
        QMetaObject::invokeMethod(this, "onMountEjected", Qt::QueuedConnection);
    });

    setFixedWidth(560);
    adjustSize();
    setFixedSize(560, height());

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

#ifdef _WIN32
    QTimer::singleShot(0, this, &MainWindow::checkProjFS);
#endif
}

MainWindow::~MainWindow() {
    // Wait for any in-flight host/connect work so its worker stops touching our
    // members before we tear them down. The workers only post results back via
    // a queued invocation (which won't run once the event loop is gone), so
    // joining here cannot deadlock.
    if (connectWorker_.joinable()) connectWorker_.join();
    if (shareWorker_.joinable()) shareWorker_.join();

    mount_.setEjectedCallback({});
    mount_.stop();
    if (client_) client_->disconnect();
    if (webClient_) webClient_->disconnect();
    if (quicClient_) quicClient_->disconnect();
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

    lanCheck_ = new CleanCheckBox("Share on this LAN only");
    form->addRow("", lanCheck_);

    writeCheck_ = new CleanCheckBox("Allow clients to upload and delete files");
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

#ifdef _WIN32
void MainWindow::checkProjFS() {
    if (fb::projfs_available()) return;

    auto reply = QMessageBox::question(this, "Enable Projected File System?",
        "Folder Buddies needs the Windows Projected File System (ProjFS) to "
        "create virtual drives, but it is not enabled on your system.\n\n"
        "Would you like to enable it now?\n\n"
        "This requires administrator privileges and a system reboot.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (reply == QMessageBox::Yes) {
        std::string err;
        if (fb::enable_fuse_backend(err)) {
            QMessageBox::information(this, "Restart Required",
                "ProjFS has been enabled. You must restart your computer for the "
                "change to take effect.\n\n"
                "After rebooting, Folder Buddies will be able to mount remote shares.");
        } else {
            QMessageBox::warning(this, "Could Not Enable ProjFS",
                QString::fromStdString(err));
        }
    }
}
#endif

void MainWindow::toggleShare() {
    if (busyShare_) return;

    if (server_ && server_->running()) {
        busyShare_ = true;
        shareButton_->setEnabled(false);
        shareButton_->setText("Stopping…");
        shareStatus_->setText("Stopping…");
        tokenEdit_->clear();

        auto srv = std::move(server_);
        auto web = std::move(webCompatHost_);
        auto ticket = std::make_shared<fb::HostedShareTicket>(activeTicket_);
        activeTicket_ = fb::HostedShareTicket{};

        if (shareWorker_.joinable()) shareWorker_.join();
        shareWorker_ = std::thread(
            [this, srv = std::move(srv), web = std::move(web), ticket]() mutable {
                if (web) web->stop();
                upnp_.unmap();
                if (srv) srv->stop();
                if (ticket->cloudPublished) {
                    std::string derr;
                    fb::remove_published_room(*ticket, derr);
                }
                // srv/web are destroyed here, off the GUI thread.
                srv.reset();
                web.reset();
                QMetaObject::invokeMethod(this, [this] {
                    busyShare_ = false;
                    shareButton_->setEnabled(true);
                    shareStatus_->setText("Not hosting.");
                    setShareRunning(false);
                }, Qt::QueuedConnection);
            });
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

    busyShare_ = true;
    shareButton_->setEnabled(false);
    shareButton_->setText("Starting…");
    folderEdit_->setEnabled(false);
    lanCheck_->setEnabled(false);
    writeCheck_->setEnabled(false);
    shareStatus_->setText("Starting…");

    const std::string folderStd = folder.toStdString();
    const bool lanOnly = lanCheck_->isChecked();
    const bool allowWrites = writeCheck_->isChecked();

    if (shareWorker_.joinable()) shareWorker_.join();
    shareWorker_ = std::thread([this, folderStd, lanOnly, allowWrites] {
        auto res = std::make_shared<HostResult>();
        res->ok = fb::start_hosting(*server_, upnp_, folderStd, 0, lanOnly, allowWrites,
                                    res->ticket, res->err);
        if (res->ok && res->ticket.cloudPublished && fb::web_compat_available()) {
            auto wch = std::make_unique<fb::WebRtcCompatHost>();
            wch->onClientsChanged = [this] {
                QMetaObject::invokeMethod(this, "onClientsChanged", Qt::QueuedConnection);
            };
            std::string werr;
            if (wch->start(folderStd, res->ticket.roomCode, allowWrites, server_.get(), werr))
                res->webCompatHost = std::move(wch);
            else
                res->webCompatWarning = werr;
        }
        if (!res->ok) {
            // Undo any partial setup (UPnP map / bound socket) off the GUI thread.
            upnp_.unmap();
            if (server_) server_->stop();
        }

        QMetaObject::invokeMethod(this, [this, res] {
            busyShare_ = false;
            shareButton_->setEnabled(true);
            if (!res->ok) {
                server_.reset();
                setShareRunning(false);
                shareStatus_->setText("Not hosting.");
                QMessageBox::critical(this, "Folder Buddies", QString::fromStdString(res->err));
                return;
            }
            activeTicket_ = res->ticket;
            if (res->webCompatHost) {
                webCompatHost_ = std::move(res->webCompatHost);
            } else if (!res->webCompatWarning.empty()) {
                QMessageBox::warning(
                    this, "Browser Compatibility Unavailable",
                    QString("Native sharing is running, but browser clients cannot connect:\n\n") +
                        QString::fromStdString(res->webCompatWarning));
            }
            tokenEdit_->setPlainText(QString::fromStdString(res->ticket.connectCode));
            refreshShareStatus();
            setShareRunning(true);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::refreshShareStatus() {
    if (!server_ || !server_->running()) {
        shareStatus_->setText("Not hosting.");
        return;
    }

    const int nativeClients = server_->clientCount();
    const int browserClients = webCompatHost_ ? webCompatHost_->clientCount() : 0;
    const int totalClients = nativeClients + browserClients;

    QString text = QString("Hosting — %1 client(s)").arg(totalClients);
    if (browserClients > 0) {
        text += QString(" (%1 native, %2 browser)").arg(nativeClients).arg(browserClients);
    }
    text += server_->allowWrites ? " — read/write" : " — read-only";
    shareStatus_->setText(text);
}

void MainWindow::onClientsChanged() {
    refreshShareStatus();
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
    if (busyConnect_) return;

    if ((client_ && client_->connected()) || (quicClient_ && quicClient_->connected()) ||
        (webClient_ && webClient_->connected())) {
        mount_.stop();
        if (client_) client_->disconnect();
        if (webClient_) webClient_->disconnect();
        if (quicClient_) quicClient_->disconnect();
        client_.reset();
        webClient_.reset();
        quicClient_.reset();
        connectStatus_->setText("Not connected.");
        setConnected(false);
        return;
    }

    const std::string entered = tokenInput_->toPlainText().trimmed().toStdString();
    if (entered.empty()) {
        QMessageBox::warning(this, "Folder Buddies", "Paste a connect code first.");
        return;
    }

    busyConnect_ = true;
    connectButton_->setEnabled(false);
    connectButton_->setText("Connecting…");
    tokenInput_->setEnabled(false);
    connectStatus_->setText("Connecting…");

    const bool tryWeb = fb::web_compat_available();
    if (connectWorker_.joinable()) connectWorker_.join();
    connectWorker_ = std::thread([this, entered, tryWeb] {
        auto res = std::make_shared<ConnectResult>();
        fb::Token tok;
        std::string decodeErr;
        if (fb::resolve_share_code(entered, tok, decodeErr)) {
            std::string quicErr;
            if (fb::looks_like_room_code(entered) && fb::native_quic_available()) {
                auto quic = std::make_unique<fb::NativeQuicRemoteClient>();
                if (quic->connect(entered, tok, quicErr)) {
                    res->quicClient = std::move(quic);
                    res->tok = tok;
                    res->ok = true;
                }
            }

            // Direct TCP is the only fallback. It covers public IPv6 and the
            // existing UPnP mapping; native peers never fall back to WebRTC.
            auto client = std::make_unique<fb::Client>();
            std::string err;
            if (!res->ok && client->connect(tok, fb::kDefaultConns, err)) {
                res->client = std::move(client);
                res->tok = tok;
                res->ok = true;
            } else if (!res->ok) {
                res->err = "Direct QUIC failed: " +
                    (quicErr.empty() ? std::string("unavailable") : quicErr) +
                    "; direct TCP failed: " + err;
            }
        } else {
            res->decodeErr = decodeErr;
        }

        // Browser-only codes retain the browser WebRTC compatibility path.
        if (!res->ok && tryWeb && !res->decodeErr.empty() &&
            fb::looks_like_web_compat_code(entered)) {
            auto webClient = std::make_unique<fb::WebRtcRemoteClient>();
            std::string webErr;
            if (webClient->connect(entered, webErr)) {
                res->webClient = std::move(webClient);
                res->ok = true;
            } else if (res->err.empty()) res->err = webErr;
        }

        QMetaObject::invokeMethod(this, [this, res] {
            busyConnect_ = false;
            connectButton_->setEnabled(true);

            if (!res->ok) {
                setConnected(false);
                connectStatus_->setText("Not connected.");
                QMessageBox::critical(
                    this, "Folder Buddies",
                    QString::fromStdString(res->err.empty() ? res->decodeErr : res->err));
                return;
            }

            std::string err, mountpoint;
            bool mounted = false;
            if (res->client) {
                client_ = std::move(res->client);
                if (mount_.start(client_.get(), "", res->tok.folder, res->tok.allowWrites, err)) {
                    mountpoint = mount_.mountpoint();
                    mounted = true;
                } else {
                    client_->disconnect();
                    client_.reset();
                }
            } else if (res->quicClient) {
                quicClient_ = std::move(res->quicClient);
                if (mount_.start(quicClient_.get(), "", res->tok.folder,
                                 res->tok.allowWrites, err)) {
                    mountpoint = mount_.mountpoint();
                    mounted = true;
                } else {
                    quicClient_->disconnect();
                    quicClient_.reset();
                }
            } else if (res->webClient) {
                webClient_ = std::move(res->webClient);
                std::string name = "Web share";
                if (mount_.start(webClient_.get(), "", name, webClient_->canWrite(), err)) {
                    mountpoint = mount_.mountpoint();
                    mounted = true;
                } else {
                    webClient_->disconnect();
                    webClient_.reset();
                }
            }

            if (!mounted) {
                setConnected(false);
                connectStatus_->setText("Not connected.");
                QMessageBox::critical(this, "Folder Buddies", QString::fromStdString(err));
                return;
            }

            QString displayedMount = QString::fromStdString(mountpoint);
            if (displayedMount.size() > 1 &&
                (displayedMount.endsWith('/') || displayedMount.endsWith('\\'))) {
                displayedMount.chop(1);
            }
            connectStatus_->setText(QString("Connected - Mounted in %1").arg(displayedMount));
            setConnected(true);
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onMountEjected() {
    mount_.stop();
    if (client_) client_->disconnect();
    if (webClient_) webClient_->disconnect();
    if (quicClient_) quicClient_->disconnect();
    client_.reset();
    webClient_.reset();
    quicClient_.reset();
    connectStatus_->setText("Disconnected (ejected).");
    setConnected(false);
    refreshStats();
}

void MainWindow::openMount() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(mount_.mountpoint())));
}

void MainWindow::refreshStats() {
    QStringList parts;
    if (server_ && server_->running()) {
        const uint64_t servedOut = server_->bytesOut.load() +
                                   (webCompatHost_ ? webCompatHost_->bytesOut.load() : 0);
        const uint64_t servedIn = server_->bytesIn.load() +
                                  (webCompatHost_ ? webCompatHost_->bytesIn.load() : 0);
        parts << QString("Serve ↑%1 ↓%2")
                     .arg(humanRate(perSec(servedOut, lastOut_)))
                     .arg(humanRate(perSec(servedIn, lastIn_)));
    }
    fb::RemoteFs* activeRemote = nullptr;
    if (mount_.active() && mount_.remote()) activeRemote = mount_.remote();
    else if (client_ && client_->connected()) activeRemote = client_.get();
    else if (webClient_ && webClient_->connected()) activeRemote = webClient_.get();
    else if (quicClient_ && quicClient_->connected()) activeRemote = quicClient_.get();
    if (activeRemote) {
        parts << QString("Mount ↓%1 ↑%2")
                     .arg(humanRate(perSec(activeRemote->bytesRead.load(), lastRead_)))
                     .arg(humanRate(perSec(activeRemote->bytesWritten.load(), lastWritten_)));
    }
    statsLabel_->setText(parts.isEmpty() ? "Idle" : parts.join("   |   "));
}
