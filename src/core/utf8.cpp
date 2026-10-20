#include "chat/utf8.hpp"
#include <cstdint>

namespace chat {
namespace {
struct Rune {
    std::uint32_t value;
    std::size_t size;
};

Rune
next(std::string_view s) {
    if (s.empty())
        return {0, 0};
    auto c = static_cast<unsigned char>(s[0]);
    if (c < 128)
        return {c, 1};
    std::size_t n = c >= 0xc2 && c <= 0xdf   ? 2
                    : c >= 0xe0 && c <= 0xef ? 3
                    : c >= 0xf0 && c <= 0xf4 ? 4
                                             : 0;
    if (n == 0 || s.size() < n)
        return {0xfffd, 0};
    std::uint32_t cp = c & ((1u << (7 - n)) - 1);
    for (std::size_t i = 1; i < n; ++i) {
        auto b = static_cast<unsigned char>(s[i]);
        if ((b & 0xc0) != 0x80)
            return {0xfffd, 0};
        cp = (cp << 6) | (b & 63);
    }
    if ((n == 2 && cp < 128)
        || (n == 3 && cp < 2048)
        || (n == 4 && cp < 65536)
        || cp > 0x10ffff
        || (cp >= 0xd800 && cp <= 0xdfff))
        return {0xfffd, 0};
    return {cp, n};
}

bool
unsafe(std::uint32_t c) {
    return c < 32
           || (c >= 127 && c <= 159)
           || c == 0x2028
           || c == 0x2029
           || (c >= 0x202a && c <= 0x202e)
           || (c >= 0x2066 && c <= 0x2069);
}

bool
zero(std::uint32_t c) {
    return (c >= 0x300 && c <= 0x36f)
           || (c >= 0x1ab0 && c <= 0x1aff)
           || (c >= 0x1dc0 && c <= 0x1dff)
           || (c >= 0x20d0 && c <= 0x20ff)
           || (c >= 0xfe00 && c <= 0xfe0f)
           || (c >= 0xfe20 && c <= 0xfe2f)
           || c == 0x200d
           || (c >= 0xe0100 && c <= 0xe01ef);
}

bool
wide(std::uint32_t c) {
    return c >= 0x1100
           && (c <= 0x115f
               || c == 0x2329
               || c == 0x232a
               || (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f)
               || (c >= 0xac00 && c <= 0xd7a3)
               || (c >= 0xf900 && c <= 0xfaff)
               || (c >= 0xfe10 && c <= 0xfe19)
               || (c >= 0xfe30 && c <= 0xfe6f)
               || (c >= 0xff00 && c <= 0xff60)
               || (c >= 0xffe0 && c <= 0xffe6)
               || (c >= 0x1f300 && c <= 0x1faff)
               || (c >= 0x20000 && c <= 0x3fffd));
}
} // namespace

bool
valid_utf8(std::string_view s) {
    while (!s.empty()) {
        auto r = next(s);
        if (!r.size)
            return false;
        s.remove_prefix(r.size);
    }
    return true;
}

std::string
terminal_safe(std::string_view s) {
    std::string out;
    while (!s.empty()) {
        auto r = next(s);
        if (!r.size) {
            out += "\xef\xbf\xbd";
            s.remove_prefix(1);
            continue;
        }
        if (unsafe(r.value))
            out += ' ';
        else
            out.append(s.substr(0, r.size));
        s.remove_prefix(r.size);
    }
    return out;
}

std::size_t
display_width(std::string_view s) {
    std::size_t width = 0;
    while (!s.empty()) {
        auto r = next(s);
        width += !r.size || unsafe(r.value) ? 1 : zero(r.value) ? 0 : wide(r.value) ? 2 : 1;
        s.remove_prefix(r.size ? r.size : 1);
    }
    return width;
}
} // namespace chat
