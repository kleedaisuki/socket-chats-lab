#include "../src/client/session.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

namespace {
using namespace chat;
using namespace std::chrono_literals;

void
require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct Fixture {
    Network network;
    Socket client = invalid_socket, peer = invalid_socket;

    Fixture() {
        auto listener = listen_tcp("127.0.0.1", 0);
        try {
            client = connect_tcp("127.0.0.1", local_port(listener));
            peer = ::accept(listener, nullptr, nullptr);
            if (peer == invalid_socket)
                throw std::runtime_error("accept failed");
        } catch (...) {
            close_socket(listener);
            close_socket(client);
            throw;
        }
        close_socket(listener);
    }

    ~Fixture() {
        close_socket(client);
        close_socket(peer);
    }

    Socket
    transfer() {
        auto result = client;
        client = invalid_socket;
        return result;
    }

    void
    close_peer() {
        close_socket(peer);
        peer = invalid_socket;
    }
};

class Events {
    std::mutex mutex;
    std::condition_variable ready;
    std::size_t count = 0;

public:
    void
    notify() {
        std::lock_guard lock(mutex);
        ++count;
        ready.notify_all();
    }

    std::size_t
    notifications() {
        std::lock_guard lock(mutex);
        return count;
    }

    void
    wait(std::size_t after) {
        std::unique_lock lock(mutex);
        require(
            ready.wait_for(lock, 3s, [&] { return count > after; }),
            "session notification timed out"
        );
    }
};

Frame
hello() {
    return {Type::hello, 1, {"tenant", "user", "secret"}};
}

void
wakeup_and_coalescing() {
    Fixture fixture;
    Events events;
    client::Session session(fixture.transfer(), hello(), [&] { events.notify(); });
    Decoder decoder;
    auto greeting = receive_frame(fixture.peer, decoder);
    require(
        greeting.type == Type::hello && greeting.fields == hello().fields,
        "initial hello lost"
    );
    std::this_thread::sleep_for(40ms);
    require(events.notifications() == 0, "idle transport must not notify UI");
    const auto begin = std::chrono::steady_clock::now();
    session.send({Type::ping, 2, {}});
    require(
        receive_frame(fixture.peer, decoder, 500).request_id == 2,
        "queued request did not wake poller"
    );
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - begin
    )
                            .count();
    std::cout << "idle-wakeup request latency: " << micros << " us\n";
    std::vector<std::byte> burst;
    for (uint32_t i = 0; i < 32; ++i) {
        auto bytes = encode({Type::pong, 100 + i, {}}, false);
        burst.insert(burst.end(), bytes.begin(), bytes.end());
    }
    auto remaining = std::span<const std::byte>(burst);
    while (!remaining.empty()) {
        auto count = transmit(fixture.peer, remaining);
        require(count > 0, "burst write failed");
        remaining = remaining.subspan(static_cast<std::size_t>(count));
    }
    events.wait(0);
    std::this_thread::sleep_for(20ms);
    auto frames = session.take();
    require(frames.size() == 32, "coalesced frames dropped");
    for (std::size_t i = 0; i < frames.size(); ++i)
        require(frames[i].request_id == 100 + i, "coalesced frames reordered");
    require(events.notifications() == 1, "one coalesced batch should have one UI notification");
    const auto notifications = events.notifications();
    send_frame(fixture.peer, {Type::message, 0, {"1", "lobby", "peer", "0", "你好 🌷", "nonce"}});
    events.wait(notifications);
    require(session.take().at(0).fields[4] == "你好 🌷", "UTF-8 event lost");
    const auto before_close = events.notifications();
    fixture.close_peer();
    events.wait(before_close);
    frames = session.take();
    require(
        frames.size() == 1
            && frames[0].type == Type::status
            && frames[0].fields
                   == std::vector<std::string>{"daemon-offline", "Local daemon disconnected"},
        "disconnect must produce one actionable offline event"
    );
    bool rejected = false;
    try {
        session.send({Type::ping, 3, {}});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "offline writes must not be silently replayed");
    std::this_thread::sleep_for(20ms);
    require(session.take().empty(), "disconnect event repeated");
}

void
idle_teardown() {
    Fixture fixture;
    std::atomic<int> notifications = 0;
    auto session =
        std::make_unique<client::Session>(fixture.transfer(), hello(), [&] { ++notifications; });
    Decoder decoder;
    (void)receive_frame(fixture.peer, decoder);
    std::this_thread::sleep_for(30ms);
    auto begin = std::chrono::steady_clock::now();
    session.reset();
    require(std::chrono::steady_clock::now() - begin < 500ms, "teardown blocked on network input");
    auto count = notifications.load();
    std::this_thread::sleep_for(20ms);
    require(count == notifications.load(), "notification outlived Session");
}

void
constructor_ownership() {
    Fixture fixture;
    auto transferred = fixture.transfer();
    bool rejected = false;
    try {
        client::Session session(
            transferred,
            {Type::hello, 1, {std::string(2 * 1024 * 1024, 'a')}},
            [] {}
        );
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "invalid greeting must fail construction");
    Decoder decoder;
    bool closed = false;
    try {
        (void)receive_frame(fixture.peer, decoder, 500);
    } catch (const std::exception& error) {
        closed = std::string_view(error.what()) == "connection closed";
    }
    require(closed, "failed construction leaked transferred socket");
}

void
output_backpressure() {
    Fixture fixture;
    client::Session session(fixture.transfer(), hello(), [] {});
    Decoder decoder;
    (void)receive_frame(fixture.peer, decoder);
    std::mt19937 random(17);
    std::string text(600000, 'a');
    for (auto& ch : text)
        ch = static_cast<char>(32 + random() % 95);
    bool rejected = false;
    for (uint32_t id = 2; id < 66 && !rejected; ++id) {
        try {
            session.send({Type::send, id, {"lobby", "nonce", text}});
        } catch (const std::runtime_error& error) {
            rejected = std::string_view(error.what()) == "Session output queue is full";
            if (!rejected)
                throw;
        }
    }
    require(rejected, "stalled peer must enforce bounded output, including active partial writes");
}

void
fragmented_invalid_protocol() {
    Fixture fixture;
    Events events;
    client::Session session(fixture.transfer(), hello(), [&] { events.notify(); });
    Decoder decoder;
    (void)receive_frame(fixture.peer, decoder);
    auto bytes = encode({Type::pong, 2, {}}, false);
    bytes[5] = std::byte{255};
    require(
        transmit(fixture.peer, std::span<const std::byte>(bytes).first(10)) == 10,
        "fragment write failed"
    );
    std::this_thread::sleep_for(20ms);
    require(events.notifications() == 0, "partial bytes must not trigger UI notification");
    require(
        transmit(fixture.peer, std::span<const std::byte>(bytes).subspan(10)) > 0,
        "fragment completion failed"
    );
    events.wait(0);
    auto frames = session.take();
    require(
        frames.size() == 1
            && frames[0].type == Type::status
            && frames[0].fields[0] == "daemon-offline",
        "unknown protocol type must disconnect rather than escape strict decoder"
    );
}

void
input_backpressure() {
    Fixture fixture;
    Events events;
    client::Session session(fixture.transfer(), hello(), [&] { events.notify(); });
    Decoder decoder;
    (void)receive_frame(fixture.peer, decoder);
    std::vector<std::byte> burst;
    for (uint32_t i = 0; i < 5000; ++i) {
        auto bytes = encode({Type::pong, 10000 + i, {}}, false);
        burst.insert(burst.end(), bytes.begin(), bytes.end());
    }
    std::exception_ptr failure;
    std::thread sender([&] {
        try {
            auto remaining = std::span<const std::byte>(burst);
            while (!remaining.empty()) {
                auto count = transmit(fixture.peer, remaining);
                require(count > 0, "backpressure fixture write failed");
                remaining = remaining.subspan(static_cast<std::size_t>(count));
            }
        } catch (...) {
            failure = std::current_exception();
        }
    });
    uint32_t received = 0;
    try {
        while (received < 5000) {
            const auto before = events.notifications();
            auto frames = session.take();
            require(frames.size() <= 4096, "inbound frame bound exceeded");
            if (frames.empty()) {
                events.wait(before);
                continue;
            }
            for (auto& frame : frames) {
                require(
                    frame.type == Type::pong && frame.request_id == 10000 + received,
                    "backpressure must preserve all frames in FIFO order"
                );
                ++received;
            }
        }
    } catch (...) {
        fixture.close_peer();
        sender.join();
        throw;
    }
    sender.join();
    if (failure)
        std::rethrow_exception(failure);
}
} // namespace

int
main() {
    try {
        wakeup_and_coalescing();
        idle_teardown();
        constructor_ownership();
        input_backpressure();
        output_backpressure();
        fragmented_invalid_protocol();
        std::cout << "PASS session wakeup, batched notifications, UTF-8, disconnect, ownership and "
                     "teardown\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
