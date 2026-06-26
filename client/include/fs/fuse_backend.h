// Folder Buddies — ensure the OS virtual filesystem backend is available.
//
//   • Linux   — kernel FUSE + libfuse3.
//   • Windows — native Microsoft ProjFS. The app checks at startup and prompts
//               the user to enable it via DISM if missing.
//   • macOS   — libfuse3 provider. Release builds can install FUSE-T, but the
//               mount layer only uses the FUSE3 API.
#pragma once

#include <string>

namespace fb {

bool ensure_fuse_backend(std::string& err);

#ifdef _WIN32
// Non-intrusive check — does not attempt to enable ProjFS.
bool projfs_available();
// Enables Client-ProjFS via DISM (requires admin, may need reboot).
// Caller should show user-facing UI before calling this.
bool enable_fuse_backend(std::string& err);
#endif

} // namespace fb
