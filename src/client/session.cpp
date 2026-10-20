#include "session.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

namespace client {
namespace {
using namespace chat;
constexpr std::size_t output_limit = 2 * 1024 * 1024;
constexpr std::size_t input_limit = 8 * 1024 * 1024;
constexpr std::size_t input_reserve = 2 * 1024 * 1024;

class OwnedSocket {
    Socket value_ = invalid_socket;

public:
    explicit OwnedSocket(Socket value = invalid_socket)
        : value_(value) {}

    ~OwnedSocket() {
        close_socket(value_);
    }

    OwnedSocket(const OwnedSocket&) = delete;
    OwnedSocket& operator=(const OwnedSocket&) = delete;

    OwnedSocket(OwnedSocket&& other) noexcept
        : value_(std::exchange(other.value_, invalid_socket)) {}

    Socket
    get() const {
        return value_;
    }

    void
    reset(Socket value) {
        close_socket(value_);
        value_ = value;
    }
};

class Wakeup {
    OwnedSocket reader_, writer_;

public:
    Wakeup() {
        OwnedSocket listener(listen_tcp("127.0.0.1", 0));
        writer_.reset(connect_tcp("127.0.0.1", local_port(listener.get())));
        reader_.reset(::accept(listener.get(), nullptr, nullptr));
        if (reader_.get() == invalid_socket)
            throw std::runtime_error("Cannot accept session wakeup");
        nonblocking(reader_.get());
        nonblocking(writer_.get());
        const int yes = 1;
        if (setsockopt(
                writer_.get(),
                IPPROTO_TCP,
                TCP_NODELAY,
                reinterpret_cast<const char*>(&yes),
                sizeof(yes)
            )
            != 0)
            throw std::runtime_error("Cannot configure session wakeup");
    }

    Socket
    reader() const {
        return reader_.get();
    }

    void
    notify() noexcept {
        const std::byte byte{1};
        // EWOULDBLOCK means an earlier wake is already waiting for the reader.
        (void)transmit(writer_.get(), std::span(&byte, 1));
    }

    void
    drain() {
        std::byte bytes[1024];
        while (receive(reader_.get(), bytes) > 0) {}
    }
};

std::size_t
frame_size(const Frame& frame) {
    std::size_t bytes = sizeof(Frame) + frame.fields.size() * sizeof(std::string);
    for (const auto& field : frame.fields)
        bytes += field.size();
    return bytes;
}
} // namespace

struct Session::Impl {
    OwnedSocket socket;
    Wakeup wake;
    Poller poller;
    Decoder decoder;
    std::function<void()> notify;
    std::mutex mutex;
    std::deque<std::vector<std::byte>> output;
    std::vector<Frame> input;
    std::size_t output_bytes = 0, output_frames = 0, input_bytes = 0;
    bool stopping = false, offline = false, notification_pending = false;
    Frame offline_frame{Type::status, 0, {"daemon-offline", "Local daemon disconnected"}};
    std::thread worker;

    Impl(OwnedSocket connected, Frame hello, std::function<void()> callback)
        : socket(std::move(connected)),
          notify(std::move(callback)) {
        nonblocking(socket.get());
        auto bytes = encode(hello);
        if (bytes.size() > output_limit)
            throw std::runtime_error("Session hello exceeds output limit");
        output_bytes = bytes.size();
        output_frames = 1;
        output.push_back(std::move(bytes));
        input.reserve(4096);
        poller.set(wake.reader());
        poller.set(socket.get(), true);
        worker = std::thread([this] { run(); });
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify();
        if (worker.joinable())
            worker.join();
    }

    void
    alert() noexcept {
        try {
            if (notify)
                notify();
        } catch (...) {}
    }

    void
    disconnect() noexcept {
        bool signal = false;
        {
            std::lock_guard lock(mutex);
            if (offline || stopping)
                return;
            offline = true;
            output.clear();
            output_bytes = 0;
            output_frames = 0;
            // A reserved slot keeps this visible even when the UI consumer is stalled.
            input.push_back(std::move(offline_frame));
            if (!notification_pending) {
                notification_pending = true;
                signal = true;
            }
        }
        if (signal)
            alert();
    }

    bool
    receive_batch() {
        std::size_t slots = 0;
        {
            std::lock_guard lock(mutex);
            if (input.size() >= 4095 || input_bytes > input_limit - input_reserve)
                return false;
            slots = 4095 - input.size();
        }
        std::array<std::byte, 1024> bytes;
        // At most one partly buffered frame plus one frame per 22 incoming bytes.
        const auto amount = std::min(bytes.size(), std::max(std::size_t{1}, (slots - 1) * 22));
        auto count = receive(socket.get(), std::span(bytes).first(amount));
        if (count < 0 && would_block())
            return false;
        if (count <= 0)
            throw std::runtime_error("Daemon disconnected");
        auto frames = decoder.feed(std::span(bytes).first(static_cast<std::size_t>(count)));
        if (frames.empty())
            return true;
        {
            std::lock_guard lock(mutex);
            for (auto& frame : frames) {
                const auto size = frame_size(frame);
                if (input.size() >= 4095 || size > input_limit - input_bytes)
                    throw std::runtime_error("Session input limit exceeded");
                input_bytes += size;
                input.push_back(std::move(frame));
            }
        }
        return true;
    }

    void
    run() noexcept {
        std::vector<std::byte> current;
        std::size_t offset = 0;
        try {
            for (;;) {
                bool readable = false;
                {
                    std::lock_guard lock(mutex);
                    if (stopping)
                        return;
                    if (current.empty() && !output.empty()) {
                        current = std::move(output.front());
                        output.pop_front();
                        offset = 0;
                    }
                    readable = input.size() < 4095 && input_bytes <= input_limit - input_reserve;
                }
                poller.set(socket.get(), !current.empty(), readable);
                // Keyboard events are FTXUI's responsibility; transport has no polling cadence.
                for (auto ready : poller.wait(-1)) {
                    if (ready.socket == wake.reader()) {
                        wake.drain();
                        continue;
                    }
                    if (ready.readable && !readable)
                        throw std::runtime_error("Daemon closed while input was paused");
                    if (ready.readable) {
                        for (int batch = 0; batch < 64 && receive_batch(); ++batch) {}
                    }
                    if (ready.writable && !current.empty()) {
                        auto count = transmit(
                            socket.get(),
                            std::span<const std::byte>(current).subspan(offset)
                        );
                        if (count < 0 && !would_block())
                            throw std::runtime_error("Daemon write failed");
                        if (count == 0)
                            throw std::runtime_error("Daemon write closed");
                        if (count > 0) {
                            offset += static_cast<std::size_t>(count);
                            std::lock_guard lock(mutex);
                            output_bytes -= static_cast<std::size_t>(count);
                            if (offset == current.size()) {
                                current.clear();
                                --output_frames;
                            }
                        }
                    }
                    if (ready.failed)
                        throw std::runtime_error("Daemon socket failed");
                }
                bool signal = false;
                {
                    std::lock_guard lock(mutex);
                    if (!input.empty() && !notification_pending) {
                        notification_pending = true;
                        signal = true;
                    }
                }
                if (signal)
                    alert();
            }
        } catch (...) {
            disconnect();
        }
    }
};

Session::Session(chat::Socket connected, chat::Frame hello, std::function<void()> notify) {
    OwnedSocket owned(connected);
    impl_ = std::make_unique<Impl>(std::move(owned), std::move(hello), std::move(notify));
}

Session::~Session() = default;

void
Session::send(chat::Frame frame) {
    auto bytes = chat::encode(frame);
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->offline || impl_->stopping)
            throw std::runtime_error("Local daemon disconnected");
        if (impl_->output_frames >= 512 || bytes.size() > output_limit - impl_->output_bytes)
            throw std::runtime_error("Session output queue is full");
        auto size = bytes.size();
        impl_->output.push_back(std::move(bytes));
        impl_->output_bytes += size;
        ++impl_->output_frames;
    }
    impl_->wake.notify();
}

std::vector<chat::Frame>
Session::take() {
    std::vector<chat::Frame> frames;
    {
        std::lock_guard lock(impl_->mutex);
        frames.reserve(impl_->input.size());
        for (auto& frame : impl_->input)
            frames.push_back(std::move(frame));
        impl_->input.clear();
        impl_->input_bytes = 0;
        impl_->notification_pending = false;
    }
    impl_->wake.notify();
    return frames;
}
} // namespace client
