#include "fuse_backend.h"

// ===========================================================================
// Windows: native ProjFS optional feature check / silent DISM enable.
// ===========================================================================
#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>

#  include <string>

namespace fb {
namespace {

bool projfs_dll_present() {
    HMODULE dll = LoadLibraryW(L"ProjectedFSLib.dll");
    if (!dll) return false;
    FreeLibrary(dll);
    return true;
}

bool run_dism_enable_projfs(std::string& err) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; // UAC elevation prompt
    sei.lpFile = L"dism.exe";
    sei.lpParameters = L"/online /enable-feature /featurename:Client-ProjFS /all /norestart";
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        err = "Projected File System is disabled and the elevation prompt was declined or failed";
        return false;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0) {
        err = "DISM failed while enabling Client-ProjFS (exit code " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

} // namespace

bool ensure_fuse_backend(std::string& err) {
    if (projfs_dll_present()) return true;
    if (!run_dism_enable_projfs(err)) return false;
    if (projfs_dll_present()) return true;
    err = "Client-ProjFS was enabled but ProjectedFSLib.dll is still unavailable; reboot Windows and try again";
    return false;
}

} // namespace fb

// ===========================================================================
// macOS: FUSE backend installer bundled in the .app, run on first mount.
// ===========================================================================
#elif defined(__APPLE__)

#  include <mach-o/dyld.h>

#  include <cstdint>
#  include <filesystem>
#  include <string>
#  include <vector>

namespace fb {
namespace {

namespace fs = std::filesystem;

bool backend_present() {
    const char* markers[] = {
        "/Library/Filesystems/fuse-t.fs",
        "/Library/Frameworks/fuse_t.framework",
        "/Library/Frameworks/fuse-t.framework",
        "/usr/local/lib/libfuse-t.dylib",
        "/opt/homebrew/lib/libfuse-t.dylib",
        // Fallback markers for developer builds when explicitly configured.
        "/Library/Filesystems/macfuse.fs",
        "/usr/local/lib/libfuse.dylib",     "/usr/local/lib/libfuse.2.dylib",
        "/usr/local/lib/libosxfuse.dylib",
        "/opt/homebrew/lib/libfuse.dylib",  "/opt/homebrew/lib/libfuse.2.dylib",
    };
    std::error_code ec;
    for (const char* m : markers)
        if (fs::exists(m, ec)) return true;
    return false;
}

// <app>/Contents/MacOS/folderbuddies  →  <app>/Contents/Resources
std::string bundle_resources_dir() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    fs::path exe = fs::canonical(fs::path(buf.data()), ec);
    if (ec) return {};
    return (exe.parent_path().parent_path() / "Resources").string(); // MacOS/.. -> Contents
}

// Find the bundled installer package (whatever its exact name) in Resources.
std::string bundled_installer() {
    std::string res = bundle_resources_dir();
    if (res.empty()) return {};
    std::error_code ec;
    for (auto& e : fs::directory_iterator(res, ec)) {
        if (ec) break;
        auto ext = e.path().extension().string();
        if (ext == ".pkg" || ext == ".dmg") return e.path().string();
    }
    return {};
}

} // namespace

// Install a .pkg (optionally one that lives inside a mounted .dmg) with admin
// rights via osascript. Returns the shell exit code (0 == success).
int install_pkg_elevated(const std::string& pkg) {
    std::string script = "do shell script \"installer -pkg '" + pkg +
                         "' -target /\" with administrator privileges";
    return std::system(("osascript -e \"" + script + "\" >/dev/null 2>&1").c_str());
}

bool ensure_fuse_backend(std::string& err) {
    if (backend_present()) return true;

    std::string installer = bundled_installer();
    if (installer.empty()) {
        err = "No FUSE-T backend found and no bundled installer present. Install FUSE-T "
              "with `brew install macos-fuse-t/homebrew-cask/fuse-t` and try again.";
        return false;
    }

    int rc = -1;
    if (fs::path(installer).extension() == ".dmg") {
        // Mount the .dmg, run the .pkg inside it, then detach.
        std::string mountPoint = "/Volumes/FolderBuddies-FUSE";
        std::system(("hdiutil detach '" + mountPoint + "' >/dev/null 2>&1").c_str());
        if (std::system(("hdiutil attach -nobrowse -noverify -mountpoint '" + mountPoint +
                         "' '" + installer + "' >/dev/null 2>&1")
                            .c_str()) == 0) {
            std::error_code ec;
            for (auto& e : fs::recursive_directory_iterator(mountPoint, ec)) {
                if (ec) break;
                if (e.path().extension() == ".pkg") { rc = install_pkg_elevated(e.path().string()); break; }
            }
            std::system(("hdiutil detach '" + mountPoint + "' >/dev/null 2>&1").c_str());
        }
    } else {
        rc = install_pkg_elevated(installer);
    }

    if (rc != 0) {
        err = "FUSE backend install was declined or failed. It is bundled at:\n  " + installer +
              "\nInstall it (admin required; the backend may need a reboot) and try again.";
        return false;
    }
    if (!backend_present()) {
        err = "FUSE backend installed but not active yet — a reboot may be required, "
              "then try mounting again.";
        return false;
    }
    return true;
}

} // namespace fb

// ===========================================================================
// Linux: libfuse3/kernel FUSE is the prerequisite; packaging still needs the
// AppImage runtime compatibility FUSE package on CI.
// ===========================================================================
#else

namespace fb {
bool ensure_fuse_backend(std::string&) { return true; }
} // namespace fb

#endif
