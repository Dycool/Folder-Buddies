#include "cli.h"
#include "fuse_backend.h"
#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>
#include <QString>
#include <QStyle>

#include <csignal>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#  include <QImage>
#  include <QPixmap>

static QIcon embeddedAppIcon() {
    QIcon icon;
    HMODULE mod = ::GetModuleHandleW(nullptr);
    for (int sz : {16, 20, 24, 32, 40, 48, 64, 96, 128, 256}) {
        HICON hicon = static_cast<HICON>(
            ::LoadImageW(mod, MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR));
        if (!hicon) continue;
        ICONINFO ii{};
        if (::GetIconInfo(hicon, &ii)) {
            BITMAP bm{};
            if (ii.hbmColor && ::GetObject(ii.hbmColor, sizeof(bm), &bm)) {
                const int w = bm.bmWidth, h = bm.bmHeight;
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;  // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                QImage img(w, h, QImage::Format_ARGB32);
                HDC dc = ::GetDC(nullptr);
                if (dc && ::GetDIBits(dc, ii.hbmColor, 0, h, img.bits(), &bi, DIB_RGB_COLORS))
                    icon.addPixmap(QPixmap::fromImage(img));
                if (dc) ::ReleaseDC(nullptr, dc);
            }
            if (ii.hbmColor) ::DeleteObject(ii.hbmColor);
            if (ii.hbmMask) ::DeleteObject(ii.hbmMask);
        }
        ::DestroyIcon(hicon);
    }
    return icon;
}
#endif

static QIcon appWindowIcon() {
    QIcon resourceIcon(":/icon.png");
    if (!resourceIcon.isNull()) return resourceIcon;
#ifdef _WIN32
    QIcon embedded = embeddedAppIcon();
    if (!embedded.isNull()) return embedded;
#endif
    return {};
}

#ifdef _WIN32
static bool userRequestedQtStyle(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (std::strcmp(argv[i], "-style") == 0 ||
            std::strcmp(argv[i], "--style") == 0 ||
            std::strncmp(argv[i], "-style=", 7) == 0 ||
            std::strncmp(argv[i], "--style=", 8) == 0) {
            return true;
        }
    }
    return false;
}

static void applyWindows11StyleIfAvailable() {
    if (QStyle* style = QStyleFactory::create(QStringLiteral("windows11")))
        QApplication::setStyle(style);
}
#endif

int main(int argc, char** argv) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // A recognised subcommand runs headless; a plain launch opens the GUI.
    if (fb::is_cli_invocation(argc, argv)) return fb::run_cli(argc, argv);

#ifdef _WIN32
    const bool explicitQtStyle = userRequestedQtStyle(argc, argv);
#endif

    QApplication app(argc, argv);
#ifdef _WIN32
    if (!explicitQtStyle) applyWindows11StyleIfAvailable();
#endif
    QApplication::setApplicationName("Folder Buddies");
    QApplication::setOrganizationName("FolderBuddies");

    QIcon icon = appWindowIcon();
    if (!icon.isNull()) QApplication::setWindowIcon(icon);

#ifdef __APPLE__
    std::string fuseErr;
    if (!fb::ensure_fuse_backend(fuseErr) && !fuseErr.empty())
        qWarning("FUSE backend: %s", fuseErr.c_str());
#endif

    MainWindow w;
    w.show();
    return app.exec();
}
