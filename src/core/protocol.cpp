#include "chat/protocol.hpp"
#include "chat/utf8.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace chat {
namespace {
using Bytes = std::vector<std::byte>;

[[noreturn]] void
invalid() {
    throw std::runtime_error("invalid SCL1 frame");
}

unsigned
byte(std::byte b) {
    return std::to_integer<unsigned>(b);
}

void
put(Bytes& b, std::uint32_t v, unsigned n) {
    for (unsigned i = n; i > 0; --i)
        b.push_back(std::byte((v >> ((i - 1) * 8)) & 255));
}

std::uint32_t
get(std::span<const std::byte> b, std::size_t p, unsigned n) {
    if (p > b.size() || n > b.size() - p)
        invalid();
    std::uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v = (v << 8) | byte(b[p + i]);
    return v;
}

bool
secret(Type t) {
    return t == Type::hello || t == Type::welcome;
}

Bytes
pack(std::span<const std::byte> in) {
    std::array<std::uint32_t, 65536> table;
    table.fill(std::numeric_limits<std::uint32_t>::max());
    Bytes out;
    out.reserve(in.size());
    std::size_t pos = 0, start = 0;
    auto literal = [&](std::size_t end) {
        while (start < end) {
            auto n = std::min<std::size_t>(128, end - start);
            put(out, static_cast<std::uint32_t>(n - 1), 1);
            out.insert(out.end(), in.begin() + start, in.begin() + start + n);
            start += n;
        }
    };
    while (pos + 2 < in.size()) {
        auto h = ((byte(in[pos]) * 251 + byte(in[pos + 1])) * 251 + byte(in[pos + 2])) & 65535;
        auto prev = table[h];
        table[h] = static_cast<std::uint32_t>(pos);
        std::size_t n = 0;
        if (prev < pos && pos - prev <= 65535) {
            auto limit = std::min<std::size_t>(130, in.size() - pos);
            while (n < limit && in[prev + n] == in[pos + n])
                ++n;
        }
        if (n < 3) {
            ++pos;
            continue;
        }
        literal(pos);
        put(out, 128 + static_cast<std::uint32_t>(n - 3), 1);
        put(out, static_cast<std::uint32_t>(pos - prev), 2);
        pos += n;
        start = pos;
    }
    literal(in.size());
    return out;
}

Bytes
unpack(std::span<const std::byte> in, std::size_t size) {
    Bytes out;
    out.reserve(size);
    std::size_t p = 0;
    while (p < in.size()) {
        auto t = byte(in[p++]);
        auto n = std::size_t(t < 128 ? t + 1 : (t & 127) + 3);
        if (n > size - out.size())
            invalid();
        if (t < 128) {
            if (n > in.size() - p)
                invalid();
            out.insert(out.end(), in.begin() + p, in.begin() + p + n);
            p += n;
            continue;
        }
        auto distance = get(in, p, 2);
        p += 2;
        if (distance == 0 || distance > out.size())
            invalid();
        for (std::size_t i = 0; i < n; ++i)
            out.push_back(out[out.size() - distance]);
    }
    if (out.size() != size)
        invalid();
    return out;
}

struct Header {
    Type type;
    std::uint32_t id;
    std::size_t wire, raw;
    bool compressed;
};

Header
header(std::span<const std::byte> b) {
    if (b.size() < 20
        || get(b, 0, 4) != 0x53434c31
        || byte(b[4]) != 1
        || byte(b[5]) < 1
        || byte(b[5]) > 16)
        invalid();
    auto flags = get(b, 6, 2);
    auto wire = get(b, 12, 4);
    auto raw = get(b, 16, 4);
    auto type = static_cast<Type>(byte(b[5]));
    if (flags > 1
        || wire > max_payload
        || raw > max_payload
        || raw < 2
        || wire < 2
        || (!flags && wire != raw)
        || (flags && secret(type)))
        invalid();
    return {type, get(b, 8, 4), wire, raw, flags == 1};
}

Frame
decode(std::span<const std::byte> wire, const Header& h) {
    Bytes storage;
    if (h.compressed) {
        storage = unpack(wire, h.raw);
        wire = storage;
    }
    auto count = get(wire, 0, 2);
    if (count > 128)
        invalid();
    Frame f{h.type, h.id, {}};
    f.fields.reserve(count);
    std::size_t p = 2;
    for (unsigned i = 0; i < count; ++i) {
        auto n = get(wire, p, 4);
        p += 4;
        if (n > wire.size() - p)
            invalid();
        std::string s(reinterpret_cast<const char*>(wire.data() + p), n);
        if (!valid_utf8(s))
            invalid();
        f.fields.push_back(std::move(s));
        p += n;
    }
    if (p != wire.size())
        invalid();
    return f;
}
} // namespace

std::vector<std::byte>
encode(const Frame& f, bool compress) {
    auto type = static_cast<unsigned>(f.type);
    if (type < 1 || type > 16 || f.fields.size() > 128)
        invalid();
    Bytes raw;
    put(raw, static_cast<std::uint32_t>(f.fields.size()), 2);
    for (const auto& s : f.fields) {
        if (s.size() > max_payload || raw.size() + 4 + s.size() > max_payload || !valid_utf8(s))
            invalid();
        put(raw, static_cast<std::uint32_t>(s.size()), 4);
        auto p = reinterpret_cast<const std::byte*>(s.data());
        raw.insert(raw.end(), p, p + s.size());
    }
    Bytes payload;
    bool packed = false;
    if (compress && !secret(f.type)) {
        payload = pack(raw);
        packed = payload.size() < raw.size();
    }
    if (!packed)
        payload = raw;
    Bytes out;
    out.reserve(20 + payload.size());
    put(out, 0x53434c31, 4);
    put(out, 1, 1);
    put(out, type, 1);
    put(out, packed ? 1 : 0, 2);
    put(out, f.request_id, 4);
    put(out, static_cast<std::uint32_t>(payload.size()), 4);
    put(out, static_cast<std::uint32_t>(raw.size()), 4);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<Frame>
Decoder::feed(std::span<const std::byte> input) {
    std::vector<Frame> frames;
    while (!input.empty()) {
        auto target = bytes_.size() < 20 ? 20 : 20 + header(bytes_).wire;
        auto n = std::min(target - bytes_.size(), input.size());
        bytes_.insert(bytes_.end(), input.begin(), input.begin() + n);
        input = input.subspan(n);
        if (bytes_.size() < 20)
            continue;
        auto h = header(bytes_);
        if (bytes_.size() != 20 + h.wire)
            continue;
        frames.push_back(decode(std::span<const std::byte>(bytes_).subspan(20), h));
        bytes_.clear();
    }
    return frames;
}
} // namespace chat

namespace chat {
std::size_t
Decoder::bytes_needed() const {
    return bytes_.size() < 20 ? 20 - bytes_.size() : 20 + header(bytes_).wire - bytes_.size();
}
} // namespace chat
