#pragma once
#include <cstddef>
#include <cstdint>
#include <ftxui/dom/elements.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace client::ui
{
class LayoutCache;
enum class Delivery { pending, sent, read, failed, uncertain };
struct Message {
    std::uint64_t id = 0;
    std::string nonce, author, text;
    std::int64_t time_ms = 0;
    Delivery delivery = Delivery::sent;
    bool cached = false;
};
struct Room {
    std::string name;
    std::uint64_t last_id = 0;
    std::size_t unread = 0;
    std::vector<Message> messages;
    std::uint64_t displayed = 0;
    bool loaded = false;
    std::size_t scroll = 0;
};
struct State {
    std::string tenant, user, active_room = "lobby", connection = "connecting", notice;
    std::map<std::string, Room> rooms;
    bool help = false;
    mutable std::shared_ptr<LayoutCache> layout_cache;
};
// Room.scroll counts wrapped display rows back from the live tail.
ftxui::Element conversation_view(const State&,
                                 ftxui::Element composer,
                                 int columns,
                                 int rows,
                                 ftxui::Element rooms_element = nullptr);
std::uint64_t visible_message_max(const State&, int columns = 120, int rows = 32);
std::size_t maximum_scroll(const State&, int columns, int rows);
struct LayoutMetrics {
    std::size_t entries = 0, bytes = 0;
};
LayoutMetrics layout_metrics(const State&);
} // namespace client::ui
