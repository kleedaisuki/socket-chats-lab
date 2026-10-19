#pragma once
#include <chat/net.hpp>
#include <chat/protocol.hpp>
#include <functional>
#include <memory>
#include <vector>

namespace client
{
// The transferred socket and notify callback are owned until teardown joins the IO worker.
// send throws on a stopped session or its bounded output queue overflowing.
class Session
{
  public:
    Session(chat::Socket connected, chat::Frame hello, std::function<void()> notify);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    void send(chat::Frame);
    std::vector<chat::Frame> take();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace client
