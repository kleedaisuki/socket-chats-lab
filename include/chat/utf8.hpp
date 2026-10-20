#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace chat {
bool valid_utf8(std::string_view);
std::size_t display_width(std::string_view);
std::string terminal_safe(std::string_view);
} // namespace chat
