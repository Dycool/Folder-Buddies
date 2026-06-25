// Folder Buddies — Qt6 UI. Two modes in one window: host a folder (server)
// and connect to a folder (client). A machine can do both at once.
#pragma once

#include "client.h"
#include "fuse_fs.h"
#include "server.h"
#include "upnp.h"
#include "signaling.h"

#include <QMainWindow>
#include <memory>

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void toggleShare();
    void toggleConnect();
    void browseFolder();
    void browseMountBase();
    void copyToken();
    void openMount();
    void refreshStats();
    void onClientsChanged();

private:
    QWidget* buildShareTab();
    QWidget* buildConnectTab();
    void setShareRunning(bool running);
    void setConnected(bool connected);

    // Share (server) side
    QLineEdit* folderEdit_;
    QSpinBox* maxClientsSpin_;
    QSpinBox* portSpin_;
    QCheckBox* lanCheck_;
    QCheckBox* writeCheck_;
    QPushButton* shareButton_;
    QLineEdit* tokenEdit_;
    QLineEdit* offlineEdit_;
    QPushButton* copyButton_;
    QLabel* shareStatus_;
    fb::HostedShareTicket activeTicket_;

    // Connect (client) side
    QLineEdit* tokenInput_;
    QLineEdit* mountBaseEdit_;
    QSpinBox* connsSpin_;
    QPushButton* connectButton_;
    QPushButton* openButton_;
    QLabel* connectStatus_;

    QLabel* statsLabel_;
    QTimer* statsTimer_;

    std::unique_ptr<fb::Server> server_;
    fb::Upnp upnp_;
    std::unique_ptr<fb::Client> client_;
    fb::Mount mount_;

    uint64_t lastOut_ = 0, lastIn_ = 0, lastRead_ = 0, lastWritten_ = 0;
};
