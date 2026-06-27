#include "cli.h"
#include "fuse_backend.h"
#include "mainwindow.h"

#include <QApplication>
#include <QByteArray>
#include <QIcon>
#include <QStyleFactory>
#include <QStringList>
#include <QStyle>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLibraryInfo>

#include <cstdio>

#include <csignal>
#include <cstdlib>
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

static void attachWindowsConsoleForDiagnostics() {
    static bool attached = false;
    if (attached) return;
    attached = true;

    if (!::AttachConsole(ATTACH_PARENT_PROCESS)) {
        ::AllocConsole();
    }

    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
}

static void winDiagLog(const QString& line) {
    attachWindowsConsoleForDiagnostics();
    const QByteArray bytes = line.toLocal8Bit();
    std::fprintf(stderr, "%s\n", bytes.constData());
    std::fflush(stderr);
    ::OutputDebugStringA((bytes + "\n").constData());
}
#endif

static QIcon appWindowIcon() {
    // Use the Qt resource icon first so the static and dynamic Windows builds
    // show the exact same title-bar/taskbar image. The embedded .ico is kept as
    // a Windows fallback and for Explorer's exe icon.
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

static bool wantsQtStyleDiagnostics(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (std::strcmp(argv[i], "--diagnose-qt-style") == 0 ||
            std::strcmp(argv[i], "--require-windows11-style") == 0) {
            return true;
        }
    }
    return false;
}

static bool wantsVerboseQtStyleDebug(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (std::strcmp(argv[i], "--debug-qt-style") == 0 ||
            std::strcmp(argv[i], "--verbose-qt-style") == 0) {
            return true;
        }
    }

    const char* env = std::getenv("FB_QT_STYLE_DEBUG");
    if (!env || !*env) return false;

    return std::strcmp(env, "0") != 0 &&
           _stricmp(env, "false") != 0 &&
           _stricmp(env, "off") != 0 &&
           _stricmp(env, "no") != 0;
}

static QString qtStyleDiagnosticsText() {
    QString text;
    auto add = [&](const QString& line) { text += line + QLatin1Char('\n'); };

    add("=== Folder Buddies Qt style diagnostics ===");
    add(QString("Qt runtime version: %1").arg(qVersion()));
    add(QString("Qt build: %1").arg(QLibraryInfo::build()));
    add(QString("Qt plugin path: %1").arg(QLibraryInfo::path(QLibraryInfo::PluginsPath)));
    add(QString("Qt platform: %1").arg(QGuiApplication::platformName()));
    add(QString("Qt library paths: %1").arg(QCoreApplication::libraryPaths().join(" | ")));
    add(QString("QStyleFactory::keys(): %1").arg(QStyleFactory::keys().join(", ")));
    if (QApplication::style()) {
        add(QString("Current QApplication::style(): class=%1 objectName=%2")
                .arg(QString::fromLatin1(QApplication::style()->metaObject()->className()),
                     QApplication::style()->objectName()));
    } else {
        add("Current QApplication::style(): <null>");
    }
#ifdef FB_STATIC_QT_REQUIRE_WINDOWS11_STYLE
    add("Build policy: FB_STATIC_QT_REQUIRE_WINDOWS11_STYLE=ON");
#else
    add("Build policy: FB_STATIC_QT_REQUIRE_WINDOWS11_STYLE=OFF");
#endif
    return text;
}

static QString findWindows11StyleKey() {
    const QStringList available = QStyleFactory::keys();
    for (const QString& key : available) {
        if (key.compare(QStringLiteral("windows11"), Qt::CaseInsensitive) == 0)
            return key;
    }
    return {};
}

static int failMissingWindows11Style(const QString& reason) {
    const QString msg = reason + QLatin1String("\n\n") + qtStyleDiagnosticsText();
    winDiagLog(msg);
    ::MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(msg.utf16()),
                  L"Folder Buddies fatal Qt style error",
                  MB_ICONERROR | MB_OK | MB_TOPMOST);
    return 111;
}

static int diagnoseRequiredWindows11Style() {
    const QString key = findWindows11StyleKey();
    winDiagLog(QStringLiteral("=== Qt style diagnostic before applying windows11 ===\n") +
               qtStyleDiagnosticsText());

    if (key.isEmpty()) {
        winDiagLog("FATAL: required Qt style 'windows11' is missing from QStyleFactory::keys().");
        return 111;
    }

    QStyle* style = QStyleFactory::create(key);
    if (!style) {
        winDiagLog(QString("FATAL: QStyleFactory listed '%1' but create() returned null.").arg(key));
        return 112;
    }

    QApplication::setStyle(style);
    winDiagLog(QString("OK: required Qt style '%1' was created and applied.").arg(key));
    winDiagLog(QStringLiteral("=== Qt style diagnostic after applying windows11 ===\n") +
               qtStyleDiagnosticsText());
    return 0;
}

static bool applyRequiredWindows11Style() {
    const QString key = findWindows11StyleKey();
    if (key.isEmpty()) return false;

    QStyle* style = QStyleFactory::create(key);
    if (!style) return false;

    QApplication::setStyle(style);
    winDiagLog(QString("Qt style selected: %1").arg(key));
    return true;
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
    const bool qtStyleDiagnostics = wantsQtStyleDiagnostics(argc, argv);
    const bool verboseQtStyleDebug = wantsVerboseQtStyleDebug(argc, argv);
#endif

    QApplication app(argc, argv);
#ifdef _WIN32
    if (qtStyleDiagnostics) return diagnoseRequiredWindows11Style();
    if (verboseQtStyleDebug) {
        winDiagLog(QStringLiteral("=== Qt style debug before policy/style selection ===\n") +
                   qtStyleDiagnosticsText());
    }
#  ifdef FB_STATIC_QT_REQUIRE_WINDOWS11_STYLE
    if (explicitQtStyle) {
        return failMissingWindows11Style(
            "FATAL: this static Windows build forbids overriding the Qt style. "
            "It must use the Qt 'windows11' style.");
    }
    if (!applyRequiredWindows11Style()) {
        return failMissingWindows11Style(
            "FATAL: this static Windows build requires Qt style 'windows11', "
            "but the style was not available/creatable at runtime.");
    }
#  else
    // Dynamic/shared Windows builds use the same Windows 11 style selection and
    // diagnostics machinery as the static build, but they do not hard-fail.
    // This lets CI or a local run compare the dynamic/static style state with:
    //   folderbuddies.exe --debug-qt-style
    // or:
    //   $env:FB_QT_STYLE_DEBUG=1; ./folderbuddies.exe
    if (!explicitQtStyle) {
        if (!applyRequiredWindows11Style() && verboseQtStyleDebug) {
            winDiagLog(QStringLiteral(
                "WARNING: dynamic Windows build could not create Qt style 'windows11'. "
                "Continuing with Qt's default style."));
        }
    } else if (verboseQtStyleDebug) {
        winDiagLog(QStringLiteral(
            "Qt style auto-selection skipped because the user supplied -style/--style."));
    }
#  endif
    if (verboseQtStyleDebug) {
        winDiagLog(QStringLiteral("=== Qt style debug after policy/style selection ===\n") +
                   qtStyleDiagnosticsText());
    }
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
