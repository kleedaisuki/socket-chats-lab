#include "common.hpp"
#include <cstdlib>
#include <future>
#include <memory>
#include <set>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
using namespace client;
using Clock = std::chrono::steady_clock;
void replace_file(const std::filesystem::path& from, const std::filesystem::path& to)
{
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish local state file");
#else
    std::filesystem::rename(from, to);
#endif
}
class StateLock
{
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
  public:
    explicit StateLock(const std::filesystem::path& dir)
    {
        std::filesystem::create_directories(dir);
#ifdef _WIN32
        StatePermissions permissions;
        permissions.protect(dir);
        const auto lock_path = dir / "lock";
        auto attributes = GetFileAttributesW(lock_path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Refusing a reparse point in private Windows state");
        handle_ = CreateFileW((dir / "lock").c_str(),
                              GENERIC_READ | GENERIC_WRITE | WRITE_DAC,
                              0,
                              nullptr,
                              OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("State directory already in use or not writable");
        try {
            permissions.protect(handle_);
            for (auto name : {"identity", "endpoint", "events.jsonl", "endpoint.tmp", "events.tmp"})
                permissions.protect(dir / name, false);
        } catch (...) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw;
        }
#else
        if (chmod(dir.c_str(), 0700) != 0)
            throw std::runtime_error("Cannot protect state directory");
        struct stat info{};
        if (stat(dir.c_str(), &info) != 0 || (info.st_mode & 0077) != 0)
            std::cerr
                << "WARNING: filesystem does not enforce private POSIX permissions. Use a private "
                   "filesystem or WSL metadata mount; protect this directory with Windows ACLs.\n";
        handle_ = open((dir / "lock").c_str(), O_CREAT | O_RDWR, 0600);
        if (handle_ < 0)
            throw std::runtime_error("Cannot open state directory lock");
        if (flock(handle_, LOCK_EX | LOCK_NB) != 0) {
            close(handle_);
            handle_ = -1;
            throw std::runtime_error("State directory already in use");
        }
#endif
    }
    ~StateLock()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
#else
        if (handle_ >= 0)
            close(handle_);
#endif
    }
};
struct Pending {
    Socket peer;
    uint32_t request;
};
class Daemon
{
    Network network_;
    std::filesystem::path dir_;
    StateLock lock_;
    std::string host_, tenant_, user_, token_, ipc_token_ = random_id();
    uint16_t port_;
    Socket listener_ = invalid_socket;
    Poller poller_;
    std::unique_ptr<Channel> upstream_;
    std::future<Socket> connecting_;
    std::unordered_map<Socket, Channel> peers_;
    std::unordered_map<uint32_t, Pending> pending_;
    std::deque<Frame> cache_;
    uint32_t next_ = 2;
    Clock::time_point retry_ = Clock::now();
    int backoff_ = 1;
    bool online_ = false;
    size_t journal_bytes_ = 0;
    size_t cache_bytes_ = 0;
    Clock::time_point last_remote_ = Clock::now(), last_ping_ = Clock::now();
    uint32_t heartbeat_ = 0;

    void queue(Socket socket, const Frame& frame)
    {
        auto found = peers_.find(socket);
        if (found == peers_.end())
            return;
        try {
            found->second.queue(frame);
            poller_.set(socket, true);
        } catch (...) {
            drop(socket);
        }
    }
    void broadcast(const Frame& frame)
    {
        std::vector<Socket> sockets;
        for (auto& [socket, peer] : peers_)
            if (peer.authenticated)
                sockets.push_back(socket);
        for (auto socket : sockets)
            queue(socket, frame);
    }
    void status(std::string state, std::string detail)
    {
        broadcast({Type::status, 0, {std::move(state), std::move(detail)}});
    }
    void drop(Socket socket)
    {
        poller_.remove(socket);
        close_socket(socket);
        peers_.erase(socket);
        std::erase_if(pending_, [socket](auto& pair) { return pair.second.peer == socket; });
    }
    void disconnected()
    {
        if (upstream_) {
            poller_.remove(upstream_->socket);
            close_socket(upstream_->socket);
            upstream_.reset();
        }
        online_ = false;
        heartbeat_ = 0;
        auto pending = std::move(pending_);
        pending_.clear();
        for (auto& [id, item] : pending) {
            (void)id;
            queue(item.peer,
                  {Type::error,
                   item.request,
                   {"offline",
                    "Connection lost; delivery may be uncertain. Check history before retrying."}});
        }
        retry_ = Clock::now() + std::chrono::seconds(backoff_);
        backoff_ = std::min(backoff_ * 2, 30);
        status("offline", "Reconnecting; cached messages are not live.");
    }
    void connect()
    {
        // Name resolution and the bounded TCP connect must not freeze local cached browsing.
        if (!connecting_.valid()) {
            connecting_ = std::async(std::launch::async, [host = host_, port = port_] {
                return connect_tcp(host, port);
            });
            return;
        }
        if (connecting_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try {
            auto socket = connecting_.get();
            upstream_ = std::make_unique<Channel>();
            upstream_->socket = socket;
            nonblocking(socket);
            upstream_->queue({Type::hello, 1, {tenant_, user_, token_}});
            poller_.set(socket, true);
        } catch (...) {
            disconnected();
        }
    }
    void remember(Frame frame)
    {
        if (frame.type != Type::message && frame.type != Type::receipt)
            return;
        frame.request_id = 0;
        if (frame.type == Type::message && !frame.fields.empty()) {
            auto duplicate = std::find_if(cache_.begin(), cache_.end(), [&](auto& old) {
                return old.type == Type::message && !old.fields.empty() &&
                       old.fields[0] == frame.fields[0];
            });
            if (duplicate != cache_.end())
                return;
        }
        const auto line = journal_line(frame) + "\n";
        cache_bytes_ += line.size();
        cache_.push_back(frame);
        while (cache_.size() > 1000 || cache_bytes_ > 8 * 1024 * 1024) {
            cache_bytes_ -= journal_line(cache_.front()).size() + 1;
            cache_.pop_front();
        }
        std::ofstream out(dir_ / "events.jsonl", std::ios::app | std::ios::binary);
        out << line;
        out.flush();
        if (!out) {
            status("cache-error", "Cannot persist local history; server remains authoritative.");
            return;
        }
        journal_bytes_ += line.size();
        if (journal_bytes_ <= 16 * 1024 * 1024)
            return;
        out.close();
        auto temp = dir_ / "events.tmp";
        std::ofstream compact(temp, std::ios::trunc | std::ios::binary);
        journal_bytes_ = 0;
        for (auto& item : cache_) {
            auto text = journal_line(item) + "\n";
            compact << text;
            journal_bytes_ += text.size();
        }
        compact.close();
        if (compact)
            replace_file(temp, dir_ / "events.jsonl");
    }
    void remote(Frame frame)
    {
        last_remote_ = Clock::now();
        if (!online_) {
            if (frame.type == Type::welcome) {
                online_ = true;
                backoff_ = 1;
                last_ping_ = Clock::now();
                status("online", "Connected; use /history to synchronize missed messages.");
            } else if (frame.type == Type::error) {
                status("auth-error",
                       "Server rejected authentication; check CHAT_TOKEN and restart daemon.");
                disconnected();
            }
            return;
        }
        remember(frame);
        if (frame.request_id == heartbeat_ && heartbeat_ != 0) {
            heartbeat_ = 0;
            return;
        }
        if (frame.request_id == 0) {
            broadcast(frame);
            return;
        }
        auto found = pending_.find(frame.request_id);
        if (found == pending_.end())
            return;
        auto item = found->second;
        bool done = frame.type != Type::message;
        if (done)
            pending_.erase(found);
        frame.request_id = item.request;
        queue(item.peer, frame);
    }
    void local(Socket socket, Frame frame)
    {
        auto found = peers_.find(socket);
        if (found == peers_.end())
            return;
        if (found->second.closing)
            return;
        if (!found->second.authenticated) {
            if (frame.type != Type::hello || frame.request_id == 0 ||
                frame.fields != std::vector<std::string>{tenant_, user_, ipc_token_}) {
                found->second.closing = true;
                queue(
                    socket,
                    {Type::error, frame.request_id, {"auth", "Invalid local daemon credentials"}});
                return;
            }
            found->second.authenticated = true;
            queue(socket, {Type::welcome, frame.request_id, {tenant_, user_}});
            queue(socket,
                  {Type::status,
                   0,
                   {"cache", "Cached local history follows; not a live synchronization."}});
            // A replay is deliberately bounded so a new peer cannot exhaust its output queue.
            size_t bytes = 0;
            std::vector<Frame> replay;
            for (auto it = cache_.rbegin(); it != cache_.rend(); ++it) {
                auto size = encode(*it).size();
                if (bytes + size > 512 * 1024 || replay.size() == 100)
                    break;
                bytes += size;
                replay.push_back(*it);
            }
            for (auto it = replay.rbegin(); it != replay.rend(); ++it)
                queue(socket, *it);
            queue(socket,
                  {Type::status,
                   0,
                   {online_ ? "online" : "offline",
                    online_ ? "Connected." : "Reconnecting; cached view only."}});
            return;
        }
        if (frame.request_id == 0 || frame.type == Type::hello) {
            drop(socket);
            return;
        }
        if (frame.type != Type::send && frame.type != Type::history && frame.type != Type::read &&
            frame.type != Type::list && frame.type != Type::join && frame.type != Type::ping) {
            queue(socket,
                  {Type::error, frame.request_id, {"invalid", "Unsupported client operation"}});
            return;
        }
        if (!online_) {
            queue(socket,
                  {Type::error,
                   frame.request_id,
                   {"offline", "Server offline; command not queued."}});
            return;
        }
        if (pending_.size() >= 4096) {
            queue(socket,
                  {Type::error, frame.request_id, {"overload", "Too many pending requests"}});
            return;
        }
        while (next_ == 0 || next_ == 1 || next_ == heartbeat_ || pending_.contains(next_))
            ++next_;
        const auto id = next_++;
        pending_.emplace(id, Pending{socket, frame.request_id});
        frame.request_id = id;
        try {
            upstream_->queue(frame);
            poller_.set(upstream_->socket, true);
        } catch (...) {
            disconnected();
        }
    }
    void accept_peer()
    {
        sockaddr_storage address{};
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        auto socket = ::accept(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        if (socket == invalid_socket)
            return;
        if (peers_.size() >= 64) {
            close_socket(socket);
            return;
        }
        try {
            nonblocking(socket);
            Channel channel;
            channel.socket = socket;
            peers_.emplace(socket, std::move(channel));
            poller_.set(socket);
        } catch (...) {
            close_socket(socket);
        }
    }
    void publish()
    {
        auto temp = dir_ / "endpoint.tmp";
        std::ofstream out(temp, std::ios::trunc | std::ios::binary);
        out << local_port(listener_) << "\n"
            << ipc_token_ << "\n"
            << tenant_ << "\n"
            << user_ << "\n";
        out.close();
        if (!out)
            throw std::runtime_error("Cannot write daemon endpoint");
#ifndef _WIN32
        if (chmod(temp.c_str(), 0600) != 0)
            throw std::runtime_error("Cannot protect endpoint");
#endif
        replace_file(temp, dir_ / "endpoint");
    }

  public:
    Daemon(int argc, char** argv)
        : dir_(option(argc, argv, "--state-dir", ".chat")), lock_(dir_),
          host_(option(argc, argv, "--host", "127.0.0.1")), tenant_(option(argc, argv, "--tenant")),
          user_(option(argc, argv, "--user")),
          port_(port_number(option(argc, argv, "--port", "7777")))
    {
        auto secret = std::getenv("CHAT_TOKEN");
        if (tenant_.empty() || user_.empty() || !secret || !*secret)
            throw std::runtime_error("--tenant, --user and CHAT_TOKEN are required");
        token_ = secret;
        const auto identity = tenant_ + "\n" + user_ + "\n";
        std::ifstream previous(dir_ / "identity", std::ios::binary);
        if (previous) {
            std::string saved((std::istreambuf_iterator<char>(previous)), {});
            if (saved != identity)
                throw std::runtime_error(
                    "State directory belongs to a different account; use a separate --state-dir.");
        } else {
            std::ofstream saved(dir_ / "identity", std::ios::binary);
            saved << identity;
            saved.close();
            if (!saved)
                throw std::runtime_error("Cannot save state account identity");
        }
        std::ifstream input(dir_ / "events.jsonl", std::ios::binary);
        std::string line;
        while (std::getline(input, line)) {
            journal_bytes_ += line.size() + 1;
            if (line.size() > 1024 * 1024)
                continue;
            try {
                auto frame = journal_frame(line);
                (void)encode(frame);
                cache_bytes_ += journal_line(frame).size() + 1;
                cache_.push_back(std::move(frame));
                while (cache_.size() > 1000 || cache_bytes_ > 8 * 1024 * 1024) {
                    cache_bytes_ -= journal_line(cache_.front()).size() + 1;
                    cache_.pop_front();
                }
            } catch (...) {
            }
        }
        listener_ = listen_tcp("127.0.0.1", port_number(option(argc, argv, "--ipc-port", "0")));
        nonblocking(listener_);
        poller_.set(listener_);
        publish();
        std::cout << "READY 127.0.0.1:" << local_port(listener_) << std::endl;
    }
    ~Daemon()
    {
        if (connecting_.valid())
            try {
                close_socket(connecting_.get());
            } catch (...) {
            }
        if (upstream_)
            close_socket(upstream_->socket);
        for (auto& [socket, peer] : peers_) {
            (void)peer;
            close_socket(socket);
        }
        close_socket(listener_);
        std::error_code ignored;
        std::filesystem::remove(dir_ / "endpoint", ignored);
    }
    void run()
    {
        for (;;) {
            if (!upstream_ && Clock::now() >= retry_)
                connect();
            if (upstream_ && !online_ &&
                Clock::now() - upstream_->created > std::chrono::seconds(10))
                disconnected();
            if (online_ && Clock::now() - last_remote_ > std::chrono::seconds(45))
                disconnected();
            if (online_ && !heartbeat_ && Clock::now() - last_ping_ > std::chrono::seconds(15)) {
                while (next_ == 0 || next_ == 1 || pending_.contains(next_))
                    ++next_;
                heartbeat_ = next_++;
                last_ping_ = Clock::now();
                try {
                    upstream_->queue({Type::ping, heartbeat_, {}});
                    poller_.set(upstream_->socket, true);
                } catch (...) {
                    disconnected();
                }
            }
            std::vector<Socket> expired;
            for (auto& [socket, peer] : peers_)
                if (!peer.authenticated && Clock::now() - peer.created > std::chrono::seconds(5))
                    expired.push_back(socket);
            for (auto socket : expired)
                drop(socket);
            for (auto ready : poller_.wait(100)) {
                if (ready.socket == listener_) {
                    accept_peer();
                    continue;
                }
                if (upstream_ && ready.socket == upstream_->socket) {
                    try {
                        if (ready.failed)
                            throw std::runtime_error("Remote failed");
                        if (ready.writable)
                            upstream_->flush();
                        if (ready.readable)
                            for (auto frame : upstream_->read()) {
                                remote(std::move(frame));
                                if (!upstream_)
                                    break;
                            }
                        if (upstream_)
                            poller_.set(upstream_->socket, !upstream_->output.empty());
                    } catch (...) {
                        disconnected();
                    }
                    continue;
                }
                auto found = peers_.find(ready.socket);
                if (found == peers_.end())
                    continue;
                try {
                    if (ready.failed)
                        throw std::runtime_error("Peer failed");
                    if (ready.writable)
                        found->second.flush();
                    if (found->second.closing && found->second.output.empty()) {
                        drop(ready.socket);
                        continue;
                    }
                    if (ready.readable) {
                        auto frames = found->second.read();
                        for (auto& frame : frames)
                            local(ready.socket, std::move(frame));
                    }
                    found = peers_.find(ready.socket);
                    if (found != peers_.end())
                        poller_.set(ready.socket, !found->second.output.empty());
                } catch (...) {
                    if (peers_.contains(ready.socket))
                        drop(ready.socket);
                }
            }
        }
    }
};
} // namespace
int main(int argc, char** argv)
{
    try {
        if (help_requested(argc, argv)) {
            std::cout << "chat-daemon --tenant TENANT --user USER [--host 127.0.0.1] [--port 7777] "
                         "[--state-dir PATH] [--ipc-port 0]\n"
                         "Requires CHAT_TOKEN. Foreground daemon; transport is plaintext.\n";
            return 0;
        }
        check_options(
            argc, argv, {"--tenant", "--user", "--host", "--port", "--state-dir", "--ipc-port"});
        Daemon daemon(argc, argv);
        daemon.run();
    } catch (const std::exception& error) {
        std::cerr << "chat-daemon: " << error.what() << '\n';
        return 1;
    }
}
