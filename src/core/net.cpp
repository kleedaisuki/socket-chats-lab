#include "chat/net.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#endif
#if defined(__linux__) && !defined(CHAT_FORCE_POLL)
#include <sys/epoll.h>
#define CHAT_EPOLL 1
#endif
namespace chat {
namespace {
[[noreturn]] void
fail(const char* operation) {
    throw std::runtime_error(
        std::string(operation) + ": socket error " + std::to_string(last_socket_error())
    );
}

int
poll_one(Socket s, bool write, int timeout) {
#ifdef _WIN32
    WSAPOLLFD fd{s, static_cast<SHORT>(write ? POLLOUT : POLLIN), 0};
    return WSAPoll(&fd, 1, timeout);
#else
    pollfd fd{s, static_cast<short>(write ? POLLOUT : POLLIN), 0};
    return poll(&fd, 1, timeout);
#endif
}

void
blocking(Socket s) {
#ifdef _WIN32
    u_long mode = 0;
    if (ioctlsocket(s, FIONBIO, &mode))
        fail("blocking");
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags & ~O_NONBLOCK) < 0)
        fail("blocking");
#endif
}
} // namespace

Network::Network() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
        throw std::runtime_error("WSAStartup failed");
#endif
}

Network::~Network() {
#ifdef _WIN32
    WSACleanup();
#endif
}

int
last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool
would_block() {
    auto e = last_socket_error();
#ifdef _WIN32
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EINTR;
#endif
}

void
close_socket(Socket s) {
    if (s == invalid_socket)
        return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

void
nonblocking(Socket s) {
#ifdef SO_NOSIGPIPE
    // Accepted sockets cannot portably rely on listener option inheritance.
    int no_sigpipe = 1;
    if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe) != 0)
        fail("SO_NOSIGPIPE");
#endif
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode))
        fail("nonblocking");
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
        fail("nonblocking");
#endif
}

namespace {
Socket
open_tcp(std::string_view host, std::uint16_t port, int backlog, bool listening) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (listening)
        hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    auto name = std::string(host);
    auto service = std::to_string(port);
    auto rc =
        getaddrinfo(name.empty() ? nullptr : name.c_str(), service.c_str(), &hints, &addresses);
    if (rc)
        throw std::runtime_error("cannot resolve TCP address");
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> guard(addresses, freeaddrinfo);
    for (auto a = addresses; a; a = a->ai_next) {
        auto s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == invalid_socket)
            continue;
#ifdef SO_NOSIGPIPE
        int no_sigpipe = 1;
        if (setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe) != 0) {
            close_socket(s);
            continue;
        }
#endif
        bool success = false;
        if (listening) {
            int yes = 1;
#ifdef _WIN32
            setsockopt(
                s,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&yes),
                sizeof yes
            );
#else
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#endif
            success = bind(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0
                      && listen(s, backlog) == 0;
        } else {
            try {
                nonblocking(s);
            } catch (...) {
                close_socket(s);
                throw;
            }
            auto result = connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
            if (result == 0)
                success = true;
            else if (would_block() && poll_one(s, true, 5000) > 0) {
                int error = 0;
#ifdef _WIN32
                int len = sizeof error;
#else
                socklen_t len = sizeof error;
#endif
                success =
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) == 0
                    && error == 0;
            }
            if (success) {
                try {
                    blocking(s);
                } catch (...) {
                    close_socket(s);
                    throw;
                }
            }
        }
        if (success)
            return s;
        close_socket(s);
    }
    fail(listening ? "listen" : "connect");
}
} // namespace

Socket
listen_tcp(std::string_view h, std::uint16_t p, int b) {
    return open_tcp(h, p, b, true);
}

Socket
connect_tcp(std::string_view h, std::uint16_t p) {
    return open_tcp(h, p, 0, false);
}

std::uint16_t
local_port(Socket s) {
    sockaddr_storage addr{};
#ifdef _WIN32
    int len = sizeof addr;
#else
    socklen_t len = sizeof addr;
#endif
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len))
        fail("getsockname");
    if (addr.ss_family == AF_INET)
        return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
    return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
}

std::ptrdiff_t
receive(Socket s, std::span<std::byte> b) {
    return recv(
        s,
        reinterpret_cast<char*>(b.data()),
        static_cast<int>(std::min<std::size_t>(b.size(), INT_MAX)),
        0
    );
}

std::ptrdiff_t
transmit(Socket s, std::span<const std::byte> b) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    return send(
        s,
        reinterpret_cast<const char*>(b.data()),
        static_cast<int>(std::min<std::size_t>(b.size(), INT_MAX)),
        flags
    );
}

void
send_frame(Socket s, const Frame& f) {
    auto data = encode(f);
    std::span<const std::byte> rest = data;
    while (!rest.empty()) {
        auto n = transmit(s, rest);
        if (n > 0) {
            rest = rest.subspan(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && would_block()) {
            if (poll_one(s, true, 5000) > 0)
                continue;
        }
        fail("send");
    }
}

Frame
receive_frame(Socket s, Decoder& d, int timeout) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    std::array<std::byte, 16384> buffer;
    for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now()
        )
                        .count();
        if (left < 0 || poll_one(s, false, static_cast<int>(left)) <= 0)
            throw std::runtime_error("receive timeout");
        auto n = receive(
            s,
            std::span<std::byte>(buffer).first(std::min(buffer.size(), d.bytes_needed()))
        );
        if (n == 0)
            throw std::runtime_error("connection closed");
        if (n < 0) {
            if (would_block())
                continue;
            fail("receive");
        }
        auto frames = d.feed(std::span<const std::byte>(buffer).first(static_cast<std::size_t>(n)));
        if (!frames.empty())
            return std::move(frames.front());
    }
}

struct Poller::Impl {
#ifdef CHAT_EPOLL
    int fd = epoll_create1(EPOLL_CLOEXEC);
#endif
    struct Interest {
        bool read, write;
    };

    std::unordered_map<Socket, Interest> sockets;
};

Poller::Poller()
    : impl_(std::make_unique<Impl>()) {
#ifdef CHAT_EPOLL
    if (impl_->fd < 0)
        fail("epoll_create1");
#endif
}

Poller::~Poller() {
#ifdef CHAT_EPOLL
    close(impl_->fd);
#endif
}

void
Poller::set(Socket s, bool write, bool read) {
#ifdef CHAT_EPOLL
    epoll_event event{};
    event.data.fd = s;
    event.events = (read ? static_cast<std::uint32_t>(EPOLLIN | EPOLLRDHUP) : 0u)
                   | (write ? static_cast<std::uint32_t>(EPOLLOUT) : 0u);
    if (epoll_ctl(impl_->fd, impl_->sockets.contains(s) ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, s, &event)
        < 0)
        fail("epoll_ctl");
#endif
    impl_->sockets[s] = {read, write};
}

void
Poller::remove(Socket s) {
#ifdef CHAT_EPOLL
    epoll_ctl(impl_->fd, EPOLL_CTL_DEL, s, nullptr);
#endif
    impl_->sockets.erase(s);
}

std::vector<Ready>
Poller::wait(int timeout) {
    std::vector<Ready> ready;
#ifdef CHAT_EPOLL
    std::array<epoll_event, 256> events{};
    int n = epoll_wait(impl_->fd, events.data(), static_cast<int>(events.size()), timeout);
    if (n < 0) {
        if (errno == EINTR)
            return ready;
        fail("epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
        auto e = events[i];
        ready.push_back(
            {e.data.fd,
             (e.events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) != 0,
             (e.events & EPOLLOUT) != 0,
             (e.events & EPOLLERR) != 0}
        );
    }
#else
#ifdef _WIN32
    std::vector<WSAPOLLFD> fds;
#else
    std::vector<pollfd> fds;
#endif
    for (auto [s, interest] : impl_->sockets)
        fds.push_back(
            {s,
             static_cast<short>((interest.read ? POLLIN : 0) | (interest.write ? POLLOUT : 0)),
             0}
        );
#ifdef _WIN32
    if (fds.empty()) {
        Sleep(static_cast<DWORD>(timeout < 0 ? 1000 : timeout));
        return ready;
    }
    int n = WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout);
#else
    int n = poll(fds.data(), fds.size(), timeout);
#endif
    if (n < 0) {
        if (would_block())
            return ready;
        fail("poll");
    }
    for (auto f : fds)
        if (f.revents)
            ready.push_back(
                {f.fd,
                 (f.revents & (POLLIN | POLLHUP)) != 0,
                 (f.revents & POLLOUT) != 0,
                 (f.revents & (POLLERR | POLLNVAL)) != 0}
            );
#endif
    return ready;
}
} // namespace chat
