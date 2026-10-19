#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace chat
{
enum class Type : std::uint8_t {
    hello = 1,
    welcome = 2,
    send = 3,
    message = 4,
    history = 5,
    history_end = 6,
    read = 7,
    receipt = 8,
    ping = 9,
    pong = 10,
    error = 11,
    list = 12,
    rooms = 13,
    join = 14,
    ack = 15,
    status = 16
};

struct Frame {
    Type type{};
    std::uint32_t request_id{};
    std::vector<std::string> fields;
};

inline constexpr std::size_t max_payload = 1024 * 1024;
std::vector<std::byte> encode(const Frame&, bool compress = true);

class Decoder
{
  public:
    std::vector<Frame> feed(std::span<const std::byte>);

    std::size_t buffered() const
    {
        return bytes_.size();
    }

    std::size_t bytes_needed() const;

  private:
    std::vector<std::byte> bytes_;
};
} // namespace chat
