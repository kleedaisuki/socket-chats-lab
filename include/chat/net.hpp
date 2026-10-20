#pragma once
#include "chat/protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace chat {
#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket invalid_socket = INVALID_SOCKET;
#else
using Socket = int;
inline constexpr Socket invalid_socket = -1;
#endif
class Network {
public:
    Network();
    ~Network();
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;
};

void close_socket(Socket);
void nonblocking(Socket);
Socket listen_tcp(std::string_view host, std::uint16_t port, int backlog = 128);
Socket connect_tcp(std::string_view host, std::uint16_t port);
std::uint16_t local_port(Socket);
bool would_block();
int last_socket_error();
std::ptrdiff_t receive(Socket, std::span<std::byte>);
std::ptrdiff_t transmit(Socket, std::span<const std::byte>);
void send_frame(Socket, const Frame&);
Frame receive_frame(Socket, Decoder&, int timeout_ms = 5000);

struct Ready {
    Socket socket;
    bool readable;
    bool writable;
    bool failed;
};

class Poller {
public:
    Poller();
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;
    void set(Socket, bool writable = false, bool readable = true);
    void remove(Socket);
    std::vector<Ready> wait(int timeout_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace chat
