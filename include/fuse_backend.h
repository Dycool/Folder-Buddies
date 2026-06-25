// Folder Buddies — ensure the OS virtual filesystem backend is available.
//
//   • Linux   — kernel FUSE + libfuse3.
//   • Windows — native Microsoft ProjFS. If Client-ProjFS is disabled, the app
//               asks for UAC and runs:
//               dism /online /enable-feature /featurename:Client-ProjFS /all /norestart
//   • macOS   — FUSE-T preferred, with a bundled installer inside the .app for
//               release builds.
#pragma once

#include <string>

namespace fb {

bool ensure_fuse_backend(std::string& err);

} // namespace fb
