#include <algorithm>
#include <string_view>

namespace fb_worker_core {

constexpr std::string_view kBase91 =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?@[]^_`{|}~";

bool is_base91(char c) {
    return kBase91.find(c) != std::string_view::npos;
}

extern "C" bool fb_valid_room_code(const char* ptr, unsigned len) {
    if (!ptr || (len != 4 && len != 8)) return false;
    return std::all_of(ptr, ptr + len, is_base91);
}

} // namespace fb_worker_core
