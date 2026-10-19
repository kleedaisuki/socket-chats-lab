#pragma once
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace chat::server
{
struct Failure : std::runtime_error {
    std::string code;

    Failure(std::string c, std::string m) : runtime_error(std::move(m)), code(std::move(c)) {}
};

struct Message {
    std::uint64_t id{};
    std::string room, user, text, nonce;
    std::int64_t time{};
};

struct Room {
    std::string name;
    std::uint64_t last{}, unread{};
};

struct Sent {
    Message message;
    bool fresh{};
};

class Storage
{
  public:
    virtual ~Storage() = default;
    virtual void join(const std::string&, const std::string&) = 0;
    virtual Sent send(const std::string&,
                      const std::string&,
                      const std::string&,
                      const std::string&,
                      const std::string&) = 0;
    virtual std::vector<Message>
    history(const std::string&, const std::string&, std::uint64_t, unsigned) = 0;
    virtual std::uint64_t
    read(const std::string&, const std::string&, const std::string&, std::uint64_t) = 0;
    virtual std::vector<Room> list(const std::string&, const std::string&) = 0;
};

std::unique_ptr<Storage> memory_storage();
std::unique_ptr<Storage> postgres_storage(const std::string&, unsigned);
} // namespace chat::server
