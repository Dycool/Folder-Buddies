#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fb {

inline constexpr char kBase91Alphabet[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?@[]^_`{|}~";
inline constexpr std::size_t kBase91Base = 91;

std::string base91_encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> base91_decode(const std::string& text, bool* ok = nullptr);
bool base91_is_clean(const std::string& text);

} // namespace fb
