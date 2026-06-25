#include "cli.h"
#include "mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv) {
    // A recognised subcommand runs headless (no display needed); otherwise we
    // launch the GUI. This is what lets `folderbuddies host …` / `connect …`
    // work from a terminal while a plain launch opens the window.
    if (fb::is_cli_invocation(argc, argv)) return fb::run_cli(argc, argv);

    QApplication app(argc, argv);
    QApplication::setApplicationName("Folder Buddies");
    QApplication::setOrganizationName("FolderBuddies");
    QApplication::setWindowIcon(QIcon(":/icon.png"));
    MainWindow w;
    w.show();
    return app.exec();
}
