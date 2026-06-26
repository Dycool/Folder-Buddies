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
// macOS: FUSE backend installed via Homebrew.
// ===========================================================================
#elif defined(__APPLE__)

#  include <cstdio>
#  include <cstdlib>
#  include <filesystem>
#  include <string>

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

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string applescript_string(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += "\"";
    return out;
}

int run_admin_osascript(const std::string& cmd, const std::string& prompt) {
    std::string script = "do shell script " + applescript_string(cmd) +
                         " with administrator privileges"
                         " with prompt " + applescript_string(prompt);
    return std::system(("osascript -e " + shell_quote(script) + " 2>/dev/null").c_str());
}

std::string console_user() {
    std::string result;
    char buf[128] = {};
    FILE* fp = popen("stat -f '%Su' /dev/console 2>/dev/null", "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) result = buf;
        pclose(fp);
    }
    if (!result.empty() && result.back() == '\n') result.pop_back();
    return result.empty() ? "unknown" : result;
}

bool install_homebrew(std::string& brewPath, std::string& err) {
    // Determine Homebrew prefix based on architecture
#if defined(__aarch64__)
    std::string prefix = "/opt/homebrew";
#else
    std::string prefix = "/usr/local";
#endif

    // Create the prefix directory with proper ownership (needs admin)
    std::string user = console_user();
    std::string setupCmd = "mkdir -p " + prefix + " 2>/dev/null; "
                           "chown " + user + ":staff " + prefix + " 2>/dev/null";
    run_admin_osascript(setupCmd,
        "Folder Buddies needs to create the Homebrew directory");

    // Download and extract Homebrew (no root needed after directory setup)
    std::string dlCmd = "curl -fsSL https://github.com/Homebrew/brew/tarball/master | "
                        "tar xz --strip 1 -C " + prefix + " 2>/dev/null";
    int rc = std::system(dlCmd.c_str());
    if (rc != 0) {
        err = "Failed to download Homebrew. FUSE-T installation cannot proceed.";
        return false;
    }

    // Fix ownership recursively (some files may have been extracted as root)
    std::string fixCmd = "chown -R " + user + ":staff " + prefix + " 2>/dev/null";
    run_admin_osascript(fixCmd,
        "Folder Buddies is setting up Homebrew ownership");

    brewPath = prefix + "/bin/brew";
    std::error_code ec;
    if (!fs::exists(brewPath, ec)) {
        err = "Homebrew was downloaded but 'brew' command not found at " + brewPath;
        return false;
    }
    return true;
}

bool find_brew(std::string& brewPath) {
    for (const char* p : {"/opt/homebrew/bin/brew", "/usr/local/bin/brew"}) {
        std::error_code ec;
        if (fs::exists(p, ec)) { brewPath = p; return true; }
    }
    if (std::system("command -v brew >/dev/null 2>&1") == 0) {
        brewPath = "brew";
        return true;
    }
    return false;
}

} // namespace

bool ensure_fuse_backend(std::string& err) {
    if (backend_present()) return true;

    // Step 1: Find or install Homebrew
    std::string brewPath;
    if (!find_brew(brewPath)) {
        if (!install_homebrew(brewPath, err)) return false;
    }

    // Step 2: Tap the fuse-t cask and download the installer (no admin needed)
    std::system((brewPath + " tap --quiet macos-fuse-t/homebrew-cask 2>/dev/null").c_str());
    if (std::system((brewPath + " fetch --cask macos-fuse-t/homebrew-cask/fuse-t 2>/dev/null").c_str()) != 0) {
        err = "Failed to download FUSE-T installer via Homebrew.\n"
              "Install it manually: brew install macos-fuse-t/homebrew-cask/fuse-t";
        return false;
    }

    // Step 3: Locate the cached .pkg and install it with admin privileges
    std::string cacheCmd = brewPath + " --cache --cask macos-fuse-t/homebrew-cask/fuse-t 2>/dev/null";
    std::string pkgPath;
    char buf[4096] = {};
    FILE* fp = popen(cacheCmd.c_str(), "r");
    if (fp) {
        if (fgets(buf, sizeof(buf), fp)) pkgPath = buf;
        pclose(fp);
    }
    if (!pkgPath.empty() && pkgPath.back() == '\n') pkgPath.pop_back();

    if (pkgPath.empty()) {
        err = "FUSE-T package was downloaded but could not be located.";
        return false;
    }

    std::error_code ec;
    if (!fs::exists(pkgPath, ec)) {
        err = "FUSE-T package was downloaded but the file is missing at:\n  " + pkgPath;
        return false;
    }

    std::string installCmd = "installer -pkg " + shell_quote(pkgPath) + " -target /";
    int rc = run_admin_osascript(installCmd,
        "Folder Buddies needs to install FUSE-T for mounting remote folders");

    if (rc != 0) {
        err = "FUSE-T installation was declined or failed.\n"
              "Install it manually: brew install macos-fuse-t/homebrew-cask/fuse-t";
        return false;
    }

    if (!backend_present()) {
        err = "FUSE-T was installed but is not active yet \u2014 a reboot may be required.";
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
