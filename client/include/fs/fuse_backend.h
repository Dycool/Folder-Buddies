#pragma once

#include <string>

namespace fb {

bool ensure_fuse_backend(std::string& err);

#ifdef _WIN32
// Non-intrusive check — does not attempt to enable ProjFS.
bool projfs_available();
bool enable_fuse_backend(std::string& err);
#endif

} // namespace fb
