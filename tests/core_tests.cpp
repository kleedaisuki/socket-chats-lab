#include "chat/net.hpp"
#include "chat/utf8.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
using namespace chat;

namespace {
void
check(bool yes) {
    if (!yes)
        throw std::runtime_error("test assertion failed");
}

template <class F>
void
rejects(F&& f) {
    bool thrown = false;
    try {
        f();
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    check(thrown);
}

void
same(const Frame& a, const Frame& b) {
    check(a.type == b.type && a.request_id == b.request_id && a.fields == b.fields);
}

void
set32(std::vector<std::byte>& b, std::size_t p, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i)
        b[p + i] = std::byte((n >> ((3 - i) * 8)) & 255);
}

void
codec() {
    std::mt19937 random(42);
    for (unsigned trial = 0; trial < 600; ++trial) {
        Frame f{Type::message, static_cast<std::uint32_t>(random()), {}};
        for (unsigned i = 0, n = static_cast<unsigned>(random() % 10); i < n; ++i) {
            std::string s;
            for (unsigned j = 0, m = static_cast<unsigned>(random() % 2000); j < m; ++j)
                s += static_cast<char>(32 + random() % (trial % 2 ? 95 : 3));
            f.fields.push_back(std::move(s));
        }
        for (bool compressed : {false, true}) {
            auto bytes = encode(f, compressed);
            Decoder d;
            std::vector<Frame> result;
            for (std::size_t p = 0; p < bytes.size();) {
                auto n = std::min<std::size_t>(1 + random() % 73, bytes.size() - p);
                auto decoded = d.feed(std::span<const std::byte>(bytes).subspan(p, n));
                result.insert(result.end(), decoded.begin(), decoded.end());
                p += n;
                check(d.buffered() <= max_payload + 20);
            }
            check(result.size() == 1 && d.buffered() == 0);
            same(f, result.front());
        }
    }
    Frame a{Type::send, 42, {"lobby", "nonce", std::string(64000, 'a')}};
    auto packed = encode(a);
    check(packed.size() < 2000 && packed[7] == std::byte{1});
    Decoder d;
    auto b = encode(Frame{Type::ping, 99, {}});
    packed.insert(packed.end(), b.begin(), b.end());
    auto frames = d.feed(packed);
    check(frames.size() == 2);
    same(a, frames[0]);
    check(frames[1].request_id == 99);
    Frame maximum{Type::message, 900, {std::string(max_payload - 6, 'x')}};
    auto maximum_wire = encode(maximum);
    Decoder maximum_decoder;
    auto maximum_frames = maximum_decoder.feed(maximum_wire);
    check(maximum_frames.size() == 1);
    same(maximum, maximum_frames[0]);
    auto uncompressed_maximum = encode(maximum, false);
    check(uncompressed_maximum.size() == max_payload + 20);
    Decoder maximum_raw_decoder;
    same(maximum, maximum_raw_decoder.feed(uncompressed_maximum).at(0));
    for (unsigned trial = 0; trial < 1000; ++trial) {
        auto mutated = packed;
        mutated[random() % mutated.size()] ^= std::byte(1u << (random() % 8));
        try {
            Decoder mutation_decoder;
            auto ignored = mutation_decoder.feed(mutated);
            if (mutation_decoder.buffered() > max_payload + 20)
                throw std::logic_error("decoder exceeded bounded buffer");
        } catch (const std::runtime_error&) {}
    }
    auto hello = encode({Type::hello, 1, {"tenant", "user", std::string(1000, 'a')}});
    check(hello[7] == std::byte{0});
    rejects([] { encode({Type::send, 1, {std::string("\xc0\xaf", 2)}}); });
    rejects([] { encode({Type::send, 1, {std::string(max_payload, 'a')}}); });
    rejects([] { encode({Type::send, 1, std::vector<std::string>(129)}); });
    auto normal = encode({Type::message, 1, {"abc"}}, false);
    for (auto [offset, value] : std::vector<std::pair<std::size_t, unsigned>>{
             {0, 0},
             {4, 2},
             {5, 0},
             {5, 17},
             {6, 1},
             {7, 2}
         }) {
        auto bad = normal;
        bad[offset] = std::byte(value);
        rejects([&] {
            Decoder x;
            x.feed(bad);
        });
    }
    {
        auto bad = normal;
        set32(bad, 12, max_payload + 1);
        rejects([&] {
            Decoder x;
            x.feed(bad);
        });
    }
    {
        auto bad = normal;
        bad[20] = std::byte{1};
        bad[21] = std::byte{0};
        rejects([&] {
            Decoder x;
            x.feed(bad);
        });
    }
    {
        auto bad = normal;
        set32(bad, 22, 0xffffffff);
        rejects([&] {
            Decoder x;
            x.feed(bad);
        });
    }
    {
        auto bad = normal;
        bad.back() = std::byte{0xff};
        rejects([&] {
            Decoder x;
            x.feed(bad);
        });
    }
    auto compressed = [&](std::initializer_list<unsigned> payload, std::uint32_t raw) {
        auto out = normal;
        out.resize(20);
        out[7] = std::byte{1};
        set32(out, 12, static_cast<std::uint32_t>(payload.size()));
        set32(out, 16, raw);
        for (auto v : payload)
            out.push_back(std::byte(v));
        return out;
    };
    for (auto bad :
         {compressed({128, 0, 0}, 3),
          compressed({128, 0, 1}, 3),
          compressed({127, 0}, 128),
          compressed({128, 0}, 3),
          compressed({0, 0}, 3),
          compressed({1, 0, 0, 255, 0, 1}, 2)})
        rejects([&] {
            Decoder x;
            x.feed(bad);
        });
    auto secret = compressed({1, 0, 0}, 2);
    secret[5] = std::byte{1};
    rejects([&] {
        Decoder x;
        x.feed(secret);
    });
}

void
unicode() {
    check(valid_utf8("hello"));
    check(valid_utf8("中文😀"));
    check(valid_utf8(std::string("a\0b", 3)));
    for (auto s : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80", "\xe2\x82"})
        check(!valid_utf8(s));
    check(display_width("中文") == 4);
    check(display_width("e\xcc\x81") == 1);
    check(display_width("😀") == 2);
    auto safe = terminal_safe("\x1b[31m中文\n\x7f\xc2\x9b\xe2\x80\xae");
    check(
        safe.find('\x1b') == std::string::npos
        && safe.find('\n') == std::string::npos
        && valid_utf8(safe)
    );
    check(terminal_safe("中文😀") == "中文😀");
    check(valid_utf8(terminal_safe("\xff")));
}

void
sockets() {
    Network runtime;
    auto listener = listen_tcp("127.0.0.1", 0);
    auto port = local_port(listener);
    check(port != 0);
    auto client = connect_tcp("127.0.0.1", port);
    auto peer = accept(listener, nullptr, nullptr);
    check(peer != invalid_socket);
    Poller poller;
    poller.set(peer);
    send_frame(client, {Type::ping, 1, {}});
    auto events = poller.wait(1000);
    check(!events.empty() && events[0].readable);
    Decoder decoder;
    same(receive_frame(peer, decoder), {Type::ping, 1, {}});
    auto first = encode({Type::message, 2, {"中文😀"}});
    auto second = encode({Type::pong, 3, {}});
    first.insert(first.end(), second.begin(), second.end());
    std::size_t sent = 0;
    while (sent < first.size()) {
        auto n = transmit(client, std::span<const std::byte>(first).subspan(sent));
        check(n > 0);
        sent += static_cast<std::size_t>(n);
    }
    same(receive_frame(peer, decoder), {Type::message, 2, {"中文😀"}});
    same(receive_frame(peer, decoder), {Type::pong, 3, {}});
    rejects([&] { receive_frame(peer, decoder, 10); });
    poller.set(peer, true);
    events = poller.wait(1000);
    check(!events.empty() && events[0].writable);
    poller.remove(peer);
    close_socket(client);
    close_socket(peer);
    close_socket(listener);
}
} // namespace

int
main() {
    try {
        codec();
        unicode();
        sockets();
        std::cout << "core protocol / UTF-8 / sockets passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
