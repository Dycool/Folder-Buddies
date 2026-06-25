// Optional C++23 validation core for tests/WASM experiments.
// Cloudflare's request/KV bindings are exposed through the Worker runtime, so
// worker.mjs is the deployable entrypoint. The privacy-sensitive payload crypto
// remains in the native C++23 app; the Worker only persists opaque strings.

#include <algorithm>
#include <string_view>

namespace fb_worker_core {

constexpr std::string_view kBase91 =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?@[]^_`{|}~";

bool is_base91(char c) {
    return kBase91.find(c) != std::string_view::npos;
}

// Validates the public lookup half the Worker receives: 4 chars (read-only tier)
// or 8 chars (read-write tier). Mirrors ROOM_RE in worker.mjs.
extern "C" bool fb_valid_room_code(const char* ptr, unsigned len) {
    if (!ptr || (len != 4 && len != 8)) return false;
    return std::all_of(ptr, ptr + len, is_base91);
}

} // namespace fb_worker_core
