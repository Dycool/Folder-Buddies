// Folder Buddies — ensure the OS virtual filesystem backend is available.
//
//   • Linux   — kernel FUSE + libfuse3.
//   • Windows — native Microsoft ProjFS. If Client-ProjFS is disabled, the app
//               asks for UAC and runs:
//               dism /online /enable-feature /featurename:Client-ProjFS /all /norestart
//   • macOS   — libfuse3 provider. Release builds can install FUSE-T, but the
//               mount layer only uses the FUSE3 API.
#pragma once

#include <string>

namespace fb {

bool ensure_fuse_backend(std::string& err);

} // namespace fb
