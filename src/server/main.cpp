#include "storage.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chat/net.hpp>
#include <chat/protocol.hpp>
#include <chat/utf8.hpp>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

namespace {
using namespace chat;
using namespace chat::server;
volatile std::sig_atomic_t stopped = 0;

void
stop(int) {
    stopped = 1;
}

using Clock = std::chrono::steady_clock;
constexpr std::size_t output_limit = 4 * 1024 * 1024;

std::string
env(const char* name) {
    auto* p = std::getenv(name);
    return p ? p : "";
}

bool
identifier(std::string_view s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z')
               || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9')
               || c == '_'
               || c == '-';
    });
}

std::uint64_t
number(std::string_view s) {
    std::uint64_t n{};
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (s.empty()
        || e != std::errc{}
        || p != s.data() + s.size()
        || n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw Failure("invalid", "Invalid number");
    return n;
}

void
require(bool good, const char* message) {
    if (!good)
        throw Failure("invalid", message);
}

bool
secret_equal(std::string_view a, std::string_view b) {
    std::size_t diff = a.size() ^ b.size();
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i)
        diff |= (i < a.size() ? static_cast<unsigned char>(a[i]) : 0)
                ^ (i < b.size() ? static_cast<unsigned char>(b[i]) : 0);
    return diff == 0;
}

class Pool {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> threads_;
    bool stop_ = false;

public:
    explicit Pool(unsigned n) {
        for (unsigned i = 0; i < n; ++i)
            threads_.emplace_back([this] {
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(mutex_);
                        ready_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                        if (jobs_.empty())
                            return;
                        job = std::move(jobs_.front());
                        jobs_.pop_front();
                    }
                    job();
                }
            });
    }

    ~Pool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        ready_.notify_all();
        for (auto& t : threads_)
            t.join();
    }

    bool
    submit(std::function<void()> job) {
        std::lock_guard lock(mutex_);
        if (jobs_.size() >= 1024)
            return false;
        jobs_.push_back(std::move(job));
        ready_.notify_one();
        return true;
    }
};

class Wakeup {
    Socket reader_ = invalid_socket, writer_ = invalid_socket;

public:
    Wakeup() {
        auto listener = listen_tcp("127.0.0.1", 0);
        try {
            writer_ = connect_tcp("127.0.0.1", local_port(listener));
            reader_ = ::accept(listener, nullptr, nullptr);
            if (reader_ == invalid_socket)
                throw std::runtime_error("Cannot create reactor wakeup");
            nonblocking(reader_);
            nonblocking(writer_);
            int yes = 1;
            if (setsockopt(
                    writer_,
                    IPPROTO_TCP,
                    TCP_NODELAY,
                    reinterpret_cast<const char*>(&yes),
                    sizeof yes
                )
                != 0)
                throw std::runtime_error("Cannot configure reactor wakeup");
        } catch (...) {
            close_socket(listener);
            close_socket(reader_);
            close_socket(writer_);
            throw;
        }
        close_socket(listener);
    }

    ~Wakeup() {
        close_socket(reader_);
        close_socket(writer_);
    }

    Wakeup(const Wakeup&) = delete;
    Wakeup& operator=(const Wakeup&) = delete;

    Socket
    socket() const {
        return reader_;
    }

    void
    notify() {
        const std::byte byte{1};
        (void)transmit(writer_, std::span(&byte, 1));
    }

    void
    drain() {
        std::byte bytes[1024];
        while (receive(reader_, bytes) > 0) {}
    }
};

std::size_t
frame_bytes(const Frame& f) {
    std::size_t size = sizeof(Frame) + 22;
    for (const auto& field : f.fields)
        size += sizeof(std::string) + 4 + field.size();
    return size;
}

using WireBuffer = std::shared_ptr<const std::vector<std::byte>>;

struct Session {
    Socket socket;
    Decoder decoder;
    std::deque<Frame> pending;
    std::size_t pending_bytes = 0, input_bytes = 0, running_bytes = 0, preauth_received = 0;
    bool running = false, closed = false, draining = false, welcomed = false, read_eof = false;
    std::deque<WireBuffer> output;
    std::size_t offset = 0, output_bytes = 0;
    std::string tenant, user;
    std::atomic<bool> authenticated{false};
    Clock::time_point accepted = Clock::now(), active = accepted;

    explicit Session(Socket s)
        : socket(s) {}
};

struct Broadcast {
    std::string tenant;
    Frame frame;
};

struct Completion {
    std::shared_ptr<Session> session;
    std::vector<Frame> replies;
    std::vector<Broadcast> broadcasts;
    bool close = false;
};

Frame
message_frame(const Message& m, std::uint32_t request) {
    return {
        Type::message,
        request,
        {std::to_string(m.id), m.room, m.user, std::to_string(m.time), m.text, m.nonce}
    };
}

class Server {
    Storage& storage_;
    std::map<std::pair<std::string, std::string>, std::string> credentials_;
    Poller poller_;
    Socket listener_;
    std::size_t queued_output_ = 0, queued_input_ = 0;
    Wakeup wakeup_;
    std::map<Socket, std::shared_ptr<Session>> sessions_;
    std::mutex completed_mutex_;
    std::deque<Completion> completed_;
    std::condition_variable completed_ready_;
    std::size_t completed_bytes_ = 0;
    bool shutting_down_ = false;
    std::array<std::mutex, 256> room_locks_;
    // Declared last: workers finish before storage-independent completion/session state is
    // destroyed.
    Pool pool_;

    Completion
    execute(std::shared_ptr<Session> s, const Frame& f) {
        Completion out{s, {}, {}, false};
        try {
            require(f.request_id != 0, "Request ID must be nonzero");
            auto count = [&](std::size_t n) {
                require(f.fields.size() == n, "Invalid field count");
            };
            auto room = [&](std::size_t i) {
                require(identifier(f.fields.at(i)), "Invalid room identifier");
                return f.fields[i];
            };
            if (!s->authenticated.load()) {
                if (f.type != Type::hello)
                    throw Failure("auth", "Authenticate first");
                count(3);
                auto it = credentials_.find({f.fields[0], f.fields[1]});
                if (it == credentials_.end() || !secret_equal(it->second, f.fields[2]))
                    throw Failure("auth", "Invalid credentials");
                storage_.join(f.fields[0], "lobby");
                s->tenant = f.fields[0];
                s->user = f.fields[1];
                s->authenticated.store(true);
                out.replies.push_back({Type::welcome, f.request_id, {s->tenant, s->user}});
                return out;
            }
            switch (f.type) {
            case Type::join: {
                count(1);
                auto r = room(0);
                storage_.join(s->tenant, r);
                out.replies.push_back({Type::ack, f.request_id, {r}});
                break;
            }
            case Type::send: {
                count(3);
                auto r = room(0);
                require(identifier(f.fields[1]), "Invalid nonce");
                require(
                    !f.fields[2].empty()
                        && f.fields[2].size() <= 65536
                        && f.fields[2].find('\0') == std::string::npos
                        && valid_utf8(f.fields[2]),
                    "Invalid UTF-8 message"
                );
                auto result = storage_.send(s->tenant, s->user, r, f.fields[1], f.fields[2]);
                out.replies.push_back(
                    {Type::ack, f.request_id, {std::to_string(result.message.id), f.fields[1]}}
                );
                if (result.fresh)
                    out.broadcasts.push_back({s->tenant, message_frame(result.message, 0)});
                break;
            }
            case Type::history: {
                count(3);
                auto r = room(0);
                auto after = number(f.fields[1]);
                auto limit = number(f.fields[2]);
                require(limit >= 1 && limit <= 200, "History limit must be 1..200");
                auto messages = storage_.history(s->tenant, r, after, static_cast<unsigned>(limit));
                std::uint64_t last = after;
                std::size_t bytes = 0;
                for (auto& m : messages) {
                    auto size =
                        m.text.size() + m.room.size() + m.user.size() + m.nonce.size() + 128;
                    if (bytes + size > 1024 * 1024)
                        break;
                    bytes += size;
                    last = m.id;
                    out.replies.push_back(message_frame(m, f.request_id));
                }
                out.replies.push_back({Type::history_end, f.request_id, {r, std::to_string(last)}});
                break;
            }
            case Type::read: {
                count(2);
                auto r = room(0);
                auto cursor = storage_.read(s->tenant, s->user, r, number(f.fields[1]));
                out.replies.push_back(
                    {Type::receipt, f.request_id, {r, s->user, std::to_string(cursor)}}
                );
                out.broadcasts.push_back(
                    {s->tenant, {Type::receipt, 0, {r, s->user, std::to_string(cursor)}}}
                );
                break;
            }
            case Type::list: {
                count(0);
                Frame result{Type::rooms, f.request_id, {}};
                for (auto& r : storage_.list(s->tenant, s->user)) {
                    result.fields.push_back(r.name);
                    result.fields.push_back(std::to_string(r.last));
                    result.fields.push_back(std::to_string(r.unread));
                }
                if (result.fields.size() > 126)
                    throw Failure("overload", "Too many rooms to list");
                out.replies.push_back(std::move(result));
                break;
            }
            case Type::ping:
                count(0);
                out.replies.push_back({Type::pong, f.request_id, {}});
                break;
            default:
                throw Failure("invalid", "Unsupported command");
            }
        } catch (const Failure& e) {
            out.replies.push_back({Type::error, f.request_id, {e.code, e.what()}});
            out.close = e.code == "auth";
        } catch (const std::exception&) {
            out.replies.push_back({Type::error, f.request_id, {"storage", "Operation failed"}});
        }
        return out;
    }

    void
    account_input(Session& s) {
        queued_input_ -= s.input_bytes;
        s.input_bytes = s.pending_bytes + s.running_bytes + s.decoder.buffered();
        queued_input_ += s.input_bytes;
    }

    void
    clear_pending(Session& s) {
        s.pending.clear();
        s.pending_bytes = 0;
        account_input(s);
    }

    void
    close(const std::shared_ptr<Session>& s) {
        if (s->closed)
            return;
        s->closed = true;
        queued_input_ -= s->input_bytes - s->running_bytes;
        s->input_bytes = s->running_bytes;
        queued_output_ -= s->output_bytes;
        s->output_bytes = 0;
        s->output.clear();
        s->pending.clear();
        s->pending_bytes = 0;
        s->decoder = Decoder{};
        poller_.remove(s->socket);
        close_socket(s->socket);
        sessions_.erase(s->socket);
    }

    void
    interest(const std::shared_ptr<Session>& s) {
        if (s->closed)
            return;
        if (!s->output.empty()) {
            poller_.set(s->socket, true, !s->read_eof && !s->draining);
            return;
        }
        if (!s->read_eof && !s->draining) {
            poller_.set(s->socket);
            return;
        }
        poller_.remove(s->socket);
        if (!s->running && s->pending.empty())
            close(s);
    }

    void
    queue_encoded(const std::shared_ptr<Session>& s, WireBuffer data) {
        if (s->closed)
            return;
        if (s->output_bytes + data->size() > output_limit
            || queued_output_ + data->size() > 64 * 1024 * 1024) {
            close(s);
            return;
        }
        // Charge logical delivery bytes per recipient even when physical storage is shared.
        s->output_bytes += data->size();
        queued_output_ += data->size();
        s->output.push_back(std::move(data));
        poller_.set(s->socket, true, !s->read_eof && !s->draining);
    }

    void
    queue(const std::shared_ptr<Session>& s, const Frame& f) {
        if (!s->closed)
            queue_encoded(s, std::make_shared<const std::vector<std::byte>>(encode(f)));
    }

    std::unique_lock<std::mutex>
    publication_lock(const Session& s, const Frame& f) {
        if (!s.authenticated.load() || f.fields.empty())
            return {};
        if (f.type != Type::send
            && f.type != Type::read
            && f.type != Type::history
            && f.type != Type::join)
            return {};
        auto key = s.tenant + ":" + f.fields[0];
        return std::unique_lock(room_locks_[std::hash<std::string>{}(key) % room_locks_.size()]);
    }

    void
    schedule(const std::shared_ptr<Session>& s) {
        if (s->closed || s->draining || s->running || s->pending.empty())
            return;
        auto frame = std::move(s->pending.front());
        s->pending.pop_front();
        s->running_bytes = frame_bytes(frame);
        s->pending_bytes -= s->running_bytes;
        account_input(*s);
        s->running = true;
        if (!pool_.submit([this, s, frame = std::move(frame)] {
                {
                    std::lock_guard lock(completed_mutex_);
                    if (shutting_down_)
                        return;
                }
                // Keep room commit and completion publication in the same order.
                auto publication = publication_lock(*s, frame);
                auto out = execute(s, frame);
                std::size_t bytes = sizeof(Completion);
                for (auto& reply : out.replies)
                    bytes += frame_bytes(reply);
                for (auto& event : out.broadcasts)
                    bytes += frame_bytes(event.frame) + event.tenant.size();
                {
                    std::unique_lock lock(completed_mutex_);
                    completed_ready_.wait(lock, [&] {
                        return shutting_down_ || completed_bytes_ + bytes <= 64 * 1024 * 1024;
                    });
                    if (shutting_down_)
                        return;
                    completed_bytes_ += bytes;
                    completed_.push_back(std::move(out));
                }
                // A full nonblocking wake socket is already readable; losing that byte is harmless.
                wakeup_.notify();
            })) {
            s->running = false;
            s->running_bytes = 0;
            account_input(*s);
            close(s);
        }
    }

    void
    completions() {
        std::deque<Completion> done;
        {
            std::lock_guard lock(completed_mutex_);
            done.swap(completed_);
            completed_bytes_ = 0;
        }
        completed_ready_.notify_all();
        for (auto& out : done) {
            auto& s = out.session;
            s->running = false;
            s->running_bytes = 0;
            account_input(*s);
            for (auto& f : out.replies) {
                queue(s, f);
                if (!s->closed && f.type == Type::welcome)
                    s->welcomed = true;
            }
            for (auto& b : out.broadcasts) {
                std::vector<std::shared_ptr<Session>> targets;
                for (auto& [fd, p] : sessions_)
                    if (p->welcomed && p->tenant == b.tenant)
                        targets.push_back(p);
                if (targets.empty())
                    continue;
                auto data = std::make_shared<const std::vector<std::byte>>(encode(b.frame));
                for (auto& p : targets)
                    queue_encoded(p, data);
            }
            if (out.close && !s->closed) {
                clear_pending(*s);
                s->draining = true;
            } else
                schedule(s);
            interest(s);
        }
    }

    void
    accept_connections() {
        for (unsigned i = 0; i < 64; ++i) {
            auto fd = ::accept(listener_, nullptr, nullptr);
            if (fd == invalid_socket) {
                if (would_block())
                    return;
                return;
            }
            if (sessions_.size() >= 1024) {
                close_socket(fd);
                continue;
            }
            try {
                nonblocking(fd);
                auto s = std::make_shared<Session>(fd);
                sessions_.emplace(fd, s);
                poller_.set(fd);
            } catch (...) {
                close_socket(fd);
                sessions_.erase(fd);
            }
        }
    }

    void
    readable(const std::shared_ptr<Session>& s) {
        if (s->draining || s->read_eof)
            return;
        std::byte data[16384];
        std::size_t budget = 256 * 1024;
        while (budget) {
            auto n = receive(s->socket, data);
            if (n == 0) {
                // FIN ends input, not already decoded commands or their responses.
                s->read_eof = true;
                s->decoder = Decoder{};
                account_input(*s);
                interest(s);
                return;
            }
            if (n < 0) {
                if (!would_block())
                    close(s);
                return;
            }
            budget -= std::min(budget, static_cast<std::size_t>(n));
            s->active = Clock::now();
            if (!s->authenticated.load()) {
                s->preauth_received += static_cast<std::size_t>(n);
                if (s->preauth_received > 8192) {
                    close(s);
                    return;
                }
            }
            for (auto& f : s->decoder.feed(std::span(data, static_cast<std::size_t>(n)))) {
                s->pending_bytes += frame_bytes(f);
                s->pending.push_back(std::move(f));
            }
            account_input(*s);
            if (s->pending.size() > 64
                || s->pending_bytes > 2 * 1024 * 1024
                || queued_input_ > 64 * 1024 * 1024) {
                close(s);
                return;
            }
            schedule(s);
        }
    }

    void
    writable(const std::shared_ptr<Session>& s) {
        std::size_t budget = 256 * 1024;
        while (!s->output.empty() && budget) {
            const auto& data = *s->output.front();
            auto n = transmit(s->socket, std::span(data).subspan(s->offset));
            if (n <= 0) {
                if (n == 0 || !would_block())
                    close(s);
                return;
            }
            auto count = static_cast<std::size_t>(n);
            s->offset += count;
            s->output_bytes -= count;
            queued_output_ -= count;
            budget -= std::min(budget, count);
            if (s->offset == data.size()) {
                s->output.pop_front();
                s->offset = 0;
            }
        }
        interest(s);
    }

public:
    Server(Storage& storage, Socket listener, unsigned threads)
        : storage_(storage),
          listener_(listener),
          pool_(threads) {
        auto spec = env("CHAT_CREDENTIALS");
        std::size_t pos = 0;
        while (pos < spec.size()) {
            auto end = spec.find(';', pos);
            auto entry = spec.substr(pos, end == std::string::npos ? end : end - pos);
            auto first = entry.find(':');
            auto second = entry.find(':', first == std::string::npos ? first : first + 1);
            if (first == std::string::npos || second == std::string::npos)
                throw std::runtime_error("CHAT_CREDENTIALS must contain tenant:user:token entries");
            auto tenant = entry.substr(0, first),
                 user = entry.substr(first + 1, second - first - 1),
                 token = entry.substr(second + 1);
            if (!identifier(tenant)
                || !identifier(user)
                || token.empty()
                || token.size() > 1024
                || !valid_utf8(token))
                throw std::runtime_error("Invalid CHAT_CREDENTIALS entry");
            if (!credentials_.emplace(std::pair{tenant, user}, token).second)
                throw std::runtime_error("Duplicate CHAT_CREDENTIALS identity");
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
        if (credentials_.empty())
            throw std::runtime_error("CHAT_CREDENTIALS is required");
        nonblocking(listener_);
        poller_.set(listener_);
        poller_.set(wakeup_.socket());
    }

    ~Server() {
        {
            std::lock_guard lock(completed_mutex_);
            shutting_down_ = true;
        }
        completed_ready_.notify_all();
        for (auto& [fd, s] : sessions_) {
            s->closed = true;
            queued_output_ -= s->output_bytes;
            s->output_bytes = 0;
            s->output.clear();
            s->pending.clear();
            close_socket(fd);
        }
        close_socket(listener_);
    }

    void
    run() {
        while (!stopped) {
            completions();
            for (auto ready : poller_.wait(1000)) {
                if (ready.socket == wakeup_.socket()) {
                    wakeup_.drain();
                    completions();
                    continue;
                }
                if (ready.socket == listener_) {
                    accept_connections();
                    continue;
                }
                auto it = sessions_.find(ready.socket);
                if (it == sessions_.end())
                    continue;
                auto s = it->second;
                try {
                    if (ready.failed) {
                        close(s);
                        continue;
                    }
                    if (ready.readable)
                        readable(s);
                    if (!s->closed && (ready.writable || (s->read_eof && ready.readable)))
                        writable(s);
                } catch (const std::exception&) {
                    close(s);
                }
            }
            auto now = Clock::now();
            std::vector<std::shared_ptr<Session>> expired;
            for (auto& [fd, s] : sessions_)
                if ((!s->authenticated.load() && now - s->accepted > std::chrono::seconds(10))
                    || now - s->active > std::chrono::minutes(10))
                    expired.push_back(s);
            for (auto& s : expired)
                close(s);
        }
    }
};
} // namespace

int
main(int argc, char** argv) {
    try {
        std::string host = "127.0.0.1";
        unsigned port = 7777, threads = std::clamp(std::thread::hardware_concurrency(), 2u, 16u);
        bool memory = false, allow = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto next = [&]() {
                if (++i >= argc)
                    throw std::runtime_error("Missing option value");
                return std::string(argv[i]);
            };
            if (arg == "--host")
                host = next();
            else if (arg == "--port") {
                auto n = number(next());
                require(n <= 65535, "Invalid port");
                port = static_cast<unsigned>(n);
            } else if (arg == "--threads") {
                auto n = number(next());
                require(n >= 1 && n <= 128, "Threads must be 1..128");
                threads = static_cast<unsigned>(n);
            } else if (arg == "--memory")
                memory = true;
            else if (arg == "--allow-plaintext")
                allow = true;
            else if (arg == "--help") {
                std::cout << "chat-server [--host 127.0.0.1] [--port 7777] [--threads N] "
                             "[--memory] [--allow-plaintext]\nRequires CHAT_CREDENTIALS and "
                             "(unless --memory) CHAT_DB_DSN.\n";
                return 0;
            } else
                throw std::runtime_error("Unknown option: " + arg);
        }
        if (host != "127.0.0.1" && host != "::1" && host != "localhost" && !allow)
            throw std::runtime_error(
                "Remote bind requires --allow-plaintext; use an SSH tunnel "
                "for secure remote access"
            );
        auto dsn = env("CHAT_DB_DSN");
        if (!memory && dsn.empty())
            throw std::runtime_error("CHAT_DB_DSN is required; --memory is explicitly ephemeral");
        auto storage = memory ? memory_storage() : postgres_storage(dsn, std::min(threads, 16u));
        Network network;
        auto listener = listen_tcp(host, static_cast<std::uint16_t>(port));
        auto actual = local_port(listener);
        Server server(*storage, listener, threads);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::cout << "READY " << host << ":" << actual << std::endl;
        server.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "chat-server: " << e.what() << '\n';
        return 1;
    }
}
