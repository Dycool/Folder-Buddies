#include "cli.h"
#include "fuse_backend.h"
#include "mainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QIcon>

#include <csignal>

int main(int argc, char** argv) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // A recognised subcommand runs headless; a plain launch opens the GUI.
    if (fb::is_cli_invocation(argc, argv)) return fb::run_cli(argc, argv);

    QApplication app(argc, argv);

    // Force the compiled-in Qt resources (icon.png) to register. In static
    // builds the linker (/OPT:REF) can otherwise drop the auto-generated
    // resource initializer, leaving QIcon(":/icon.png") empty — which is why the
    // static Windows build showed no taskbar/title-bar icon while the dynamic
    // build did. Harmless (idempotent) when the resource is already registered.
    Q_INIT_RESOURCE(resources);

    QApplication::setApplicationName("Folder Buddies");
    QApplication::setOrganizationName("FolderBuddies");
    QApplication::setWindowIcon(QIcon(":/icon.png"));
    
    // Debug: Ensure icon is loaded properly
    QIcon appIcon = QApplication::windowIcon();
    if (appIcon.isNull()) {
        qDebug() << "Warning: Application icon not loaded";
    }

    // On macOS, proactively check if the FUSE backend is installed.
    // If missing, the app will attempt to install it via Homebrew.
    // This is non-fatal: the user can still host shares without mounting.
#ifdef __APPLE__
    std::string fuseErr;
    if (!fb::ensure_fuse_backend(fuseErr) && !fuseErr.empty())
        qWarning("FUSE backend: %s", fuseErr.c_str());
#endif

    MainWindow w;
    w.show();
    return app.exec();
}
