#include "cli.h"
#include "fuse_backend.h"
#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QStyleFactory>
#include <QStringList>
#include <QStyle>

#include <csignal>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#  include <QImage>
#  include <QPixmap>

// Build the window/taskbar icon from the multi-resolution .ico embedded into the
// executable by appicon.rc (resource id 1). This is always present and
// transparent, and — unlike QIcon(":/icon.png") — does not depend on the Qt
// resource system, which proved unreliable in the static Windows build.
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
#ifdef _WIN32
    QIcon embedded = embeddedAppIcon();
    if (!embedded.isNull()) return embedded;
#endif
    return QIcon(":/icon.png");
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

static void applyBundledWindowsQtStyle() {
#ifdef FB_STATIC_QT_WINDOWS11_STYLE
    if (QStyle* style = QStyleFactory::create("windows11")) QApplication::setStyle(style);
#else
    const QStringList available = QStyleFactory::keys();

    // qmodernwindowsstyle exposes these keys when the Qt Windows style plugin is
    // available. In a static build, importing/linking the plugin is not enough
    // to guarantee Qt picks it as the default, so choose it explicitly.
    for (const char* preferred : {"windows11", "windowsvista", "Fusion", "Windows"}) {
        const QString wanted = QString::fromLatin1(preferred);
        for (const QString& key : available) {
            if (key.compare(wanted, Qt::CaseInsensitive) != 0) continue;
            if (QStyle* style = QStyleFactory::create(key)) {
                QApplication::setStyle(style);
                return;
            }
        }
    }
#endif
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
#  ifdef FB_STATIC_QT_WINDOWS11_STYLE
    applyBundledWindowsQtStyle();
#  else
    if (!explicitQtStyle) applyBundledWindowsQtStyle();
#  endif
#endif
    QApplication::setApplicationName("Folder Buddies");
    QApplication::setOrganizationName("FolderBuddies");

    QIcon icon = appWindowIcon();
    if (!icon.isNull()) QApplication::setWindowIcon(icon);

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
