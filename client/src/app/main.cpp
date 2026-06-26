#include "cli.h"
#include "fuse_backend.h"
#include "mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv) {
    // A recognised subcommand runs headless; a plain launch opens the GUI.
    if (fb::is_cli_invocation(argc, argv)) return fb::run_cli(argc, argv);

    QApplication app(argc, argv);
    QApplication::setApplicationName("Folder Buddies");
    QApplication::setOrganizationName("FolderBuddies");
    QApplication::setWindowIcon(QIcon(":/icon.png"));

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
