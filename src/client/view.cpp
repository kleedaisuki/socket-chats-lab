#include "view.hpp"
#include "chat/utf8.hpp"
#include <algorithm>
#include <ctime>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <functional>
#include <list>
#include <utility>

namespace client::ui
{
namespace
{
using namespace ftxui;
const auto accent = Color::RGB(108, 190, 181);
const auto muted = Color::RGB(143, 157, 173);
const auto ink = Color::RGB(225, 231, 239);
const auto panel = Color::RGB(25, 33, 45);

std::string safe(std::string_view value)
{
    return chat::terminal_safe(value);
}

std::string connection(std::string_view value)
{
    if (value == "online" || value == "connected")
        return "● 在线";
    if (value == "connecting")
        return "连接中…";
    if (value == "reconnecting")
        return "重新连接中…";
    if (value == "offline" || value == "disconnected")
        return "离线 · 正在重试";
    return safe(value);
}

std::string local_time(std::int64_t millis, const char* format)
{
    if (millis <= 0)
        return {};
    const auto seconds = static_cast<std::time_t>(millis / 1000);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &seconds) != 0)
        return {};
#else
    if (localtime_r(&seconds, &local) == nullptr)
        return {};
#endif
    char buffer[64]{};
    std::strftime(buffer, sizeof(buffer), format, &local);
    return buffer;
}

std::string delivery(Delivery value)
{
    switch (value) {
    case Delivery::pending:
        return "发送中…";
    case Delivery::sent:
        return "已发送";
    case Delivery::read:
        return "已读";
    case Delivery::failed:
        return "发送失败 · 请重试";
    case Delivery::uncertain:
        return "待确认 · 请勿重复发送";
    }
    return {};
}

Element metadata(const Message& value, const State& state)
{
    const bool own = value.author == state.user;
    Elements items{text(own ? "你" : safe(value.author)) | bold |
                       color(own ? accent : Color::RGB(160, 184, 231)),
                   text("  " + local_time(value.time_ms, "%H:%M")) | color(muted)};
    if (own)
        items.push_back(text("  · " + delivery(value.delivery)) |
                        color(value.delivery == Delivery::failed ? Color::RedLight : muted));
    return hbox(std::move(items));
}

struct Visibility {
    std::vector<std::pair<std::uint64_t, Box>> messages;
    Box viewport;
};

Element timeline(const State& state, int columns, int rows, Visibility* visibility);

Element room_list(const State& state)
{
    Elements items;
    for (const auto& [name, room] : state.rooms) {
        auto row = hbox(
            {text(" # " + safe(name)) | flex,
             text(room.unread ? " " + std::to_string(room.unread) + " " : " ") | color(accent)});
        if (name == state.active_room)
            row = row | bold | bgcolor(Color::RGB(42, 63, 73));
        items.push_back(row);
        items.push_back(text(""));
    }
    return vbox(std::move(items)) | frame | flex;
}

Element help()
{
    return vbox({text("键盘，也可以很轻松") | bold | color(accent),
                 text(""),
                 text("Enter        发送消息"),
                 text("Tab          切换房间"),
                 text("↑ / ↓        输入历史"),
                 text("PgUp / PgDn  翻阅消息"),
                 text("Ctrl+R       标记当前可见消息已读"),
                 text("F1 / Esc     关闭帮助"),
                 text("Ctrl+C       退出"),
                 text(""),
                 paragraph("命令：/join 房间  /rooms  /history  /read  /quit")}) |
           border | bgcolor(panel) | center;
}
} // namespace

class LayoutCache
{
  public:
    LayoutMetrics metrics() const
    {
        return {entries_.size(), bytes_ + height_bytes()};
    }
    std::size_t height(const Message& message, int width)
    {
        // This fingerprint is only an ephemeral layout memo, not authentication.
        const auto fingerprint = std::hash<std::string_view>{}(message.text);
        const auto found = std::find_if(heights_.begin(), heights_.end(), [&](const Height& item) {
            return item.identity == &message && item.width == width;
        });
        if (found != heights_.end() && found->fingerprint == fingerprint &&
            found->source_size == message.text.size())
            return found->rows;
        const auto rows = lines(message, width).size();
        Height item{&message, width, fingerprint, message.text.size(), rows};
        if (found != heights_.end())
            *found = item;
        else if (heights_.size() == 4096)
            heights_[replacement_++ % heights_.size()] = item;
        else
            heights_.push_back(item);
        trim(0);
        return rows;
    }
    const std::vector<std::string>& lines(const Message& message, int width)
    {
        auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return entry.identity == &message && entry.width == width &&
                   entry.source == message.text;
        });
        if (found != entries_.end()) {
            entries_.splice(entries_.begin(), entries_, found);
            return entries_.front().lines;
        }
        Entry entry;
        entry.identity = &message;
        entry.width = width;
        entry.source = message.text;
        entry.lines = wrap(message.text, width);
        entry.bytes = sizeof(Entry) + 2 * sizeof(void*) + entry.source.capacity() +
                      entry.lines.capacity() * sizeof(std::string);
        for (const auto& line : entry.lines)
            entry.bytes += line.capacity();
        trim(entry.bytes);
        bytes_ += entry.bytes;
        entries_.push_front(std::move(entry));
        return entries_.front().lines;
    }

  private:
    struct Entry {
        const Message* identity = nullptr;
        int width = 0;
        std::string source;
        std::vector<std::string> lines;
        std::size_t bytes = 0;
    };
    std::list<Entry> entries_;
    std::size_t bytes_ = 0;
    struct Height {
        const Message* identity;
        int width;
        std::size_t fingerprint, source_size, rows;
    };
    std::vector<Height> heights_;
    std::size_t replacement_ = 0;

    std::size_t height_bytes() const
    {
        return heights_.capacity() * sizeof(Height);
    }

    void trim(std::size_t additional)
    {
        while (!entries_.empty() && ((additional && entries_.size() >= 128) ||
                                     bytes_ + additional + height_bytes() > 8 * 1024 * 1024)) {
            bytes_ -= entries_.back().bytes;
            entries_.pop_back();
        }
    }

    static std::vector<std::string> wrap(std::string_view input, int width)
    {
        std::vector<std::string> output;
        std::size_t start = 0;
        do {
            const auto end = input.find('\n', start);
            const auto part = input.substr(start, end == input.npos ? end : end - start);
            std::string line;
            int used = 0;
            for (const auto& glyph : ftxui::Utf8ToGlyphs(safe(part))) {
                if (glyph.empty())
                    continue;
                const int cells = ftxui::string_width(glyph);
                if (used + cells > width && !line.empty()) {
                    output.push_back(std::move(line));
                    line.clear();
                    used = 0;
                }
                line += glyph;
                used += cells;
            }
            output.push_back(std::move(line));
            if (end == input.npos)
                break;
            start = end + 1;
        } while (start <= input.size());
        return output;
    }
};

LayoutMetrics layout_metrics(const State& state)
{
    return state.layout_cache ? state.layout_cache->metrics() : LayoutMetrics{};
}

namespace
{
int body_width(int columns, int rows)
{
    return std::max(2, columns - (columns >= 88 && rows >= 16 ? 23 : 0) - 1);
}

bool begins_day(const Room& room, std::size_t index)
{
    const auto date = local_time(room.messages[index].time_ms, "%Y年%m月%d日");
    return !date.empty() &&
           (index == 0 || date != local_time(room.messages[index - 1].time_ms, "%Y年%m月%d日"));
}

Element timeline(const State& state, int columns, int rows, Visibility* visibility)
{
    auto found = state.rooms.find(state.active_room);
    if (found == state.rooms.end() || found->second.messages.empty())
        return vbox({filler(),
                     text("这里，是对话的开始。") | color(accent) | hcenter,
                     text("写下第一句话吧。") | color(muted) | hcenter,
                     filler()}) |
               flex;
    if (!state.layout_cache)
        state.layout_cache = std::make_shared<LayoutCache>();
    const auto& room = found->second;
    const int width = body_width(columns, rows);
    const auto budget = static_cast<std::size_t>(std::max(rows + 4, 8));
    if (visibility)
        visibility->messages.reserve(budget);
    Elements content;
    auto skip = room.scroll;
    for (auto i = room.messages.size(); i > 0 && content.size() < budget; --i) {
        const auto& value = room.messages[i - 1];
        const auto date = local_time(value.time_ms, "%Y年%m月%d日");
        const bool dated = begins_day(room, i - 1);
        const auto prefix = dated ? std::size_t{2} : std::size_t{0};
        const auto height = prefix + state.layout_cache->height(value, width) + 2;
        if (skip >= height) {
            skip -= height;
            continue;
        }
        const auto& body = state.layout_cache->lines(value, width);
        for (auto row = height - skip; row > 0 && content.size() < budget; --row) {
            const auto index = row - 1;
            Element item;
            std::uint64_t id = value.id;
            if (dated && index == 0) {
                item = hbox({separator() | flex,
                             text("  " + date + "  ") | color(muted),
                             separator() | flex});
                id = 0;
            } else if ((dated && index == 1) || index == height - 1) {
                item = text("");
                id = 0;
            } else if (index == prefix) {
                item = metadata(value, state);
            } else {
                item = text(body[index - prefix - 1]) | color(ink);
            }
            if (visibility) {
                visibility->messages.emplace_back(id, Box{});
                item = item | reflect(visibility->messages.back().second);
            }
            content.push_back(std::move(item));
        }
        skip = 0;
    }
    if (content.empty())
        content.push_back(text("已到历史开头 · PgDn 返回") | color(muted));
    std::reverse(content.begin(), content.end());
    auto result = vbox(std::move(content)) | focusPositionRelative(0, 1) | yframe | flex;
    return visibility ? result | reflect(visibility->viewport) : result;
}
} // namespace

static ftxui::Element make_view(const State& state,
                                ftxui::Element composer,
                                int columns,
                                int rows,
                                ftxui::Element rooms_element,
                                Visibility* visibility)
{
    using namespace ftxui;
    if (columns < 30 || rows < 9)
        return vbox({text("Socket Chats") | color(accent),
                     text("# " + safe(state.active_room)),
                     separator(),
                     std::move(composer) | flex,
                     text("Enter 发送 · Ctrl+C 退出") | dim}) |
               frame;
    auto header = hbox({text("  SOCKET / CHATS") | bold | color(accent),
                        filler(),
                        text(safe(state.tenant) + " / " + safe(state.user) + "  ") | color(muted)});
    auto found = state.rooms.find(state.active_room);
    auto unread = found == state.rooms.end() ? 0 : found->second.unread;
    auto title =
        hbox({text("# " + safe(state.active_room)) | bold,
              text(unread ? "  · " + std::to_string(unread) + " 条未读" : "") | color(accent),
              filler(),
              text(connection(state.connection)) | color(muted)});
    auto conversation =
        vbox({title, separator(), timeline(state, columns, rows, visibility)}) | flex;
    Element main = conversation;
    if (columns >= 88 && rows >= 16) {
        auto sidebar = vbox({text(" 房间") | bold | color(muted),
                             text(""),
                             rooms_element ? std::move(rooms_element) | flex : room_list(state),
                             text(" Tab 切换") | color(muted)}) |
                       size(WIDTH, EQUAL, 21);
        main = hbox({sidebar, separator(), text(" "), conversation});
    }
    auto draft = vbox({hbox({text(" 写消息 ") | color(accent), separator() | flex}),
                       std::move(composer) | size(HEIGHT, LESS_THAN, rows >= 20 ? 4 : 2) | frame});
    const auto shortcuts = columns < 70
                               ? " Enter 发送 · Tab 房间 · F1 帮助"
                               : " Enter 发送   Tab 房间   PgUp 历史   Ctrl+R 已读   F1 帮助";
    auto footer = state.notice.empty() ? shortcuts : " " + safe(state.notice);
    auto result = vbox({header, separator(), main | flex, draft, text(footer) | color(muted)}) |
                  color(ink) | bgcolor(panel);
    if (state.help)
        result = dbox({result, help()});
    return result | size(WIDTH, EQUAL, std::max(columns, 1)) |
           size(HEIGHT, EQUAL, std::max(rows, 1));
}

ftxui::Element conversation_view(const State& state,
                                 ftxui::Element composer,
                                 int columns,
                                 int rows,
                                 ftxui::Element rooms_element)
{
    return make_view(state, std::move(composer), columns, rows, std::move(rooms_element), nullptr);
}

std::uint64_t visible_message_max(const State& state, int columns, int rows)
{
    if (columns < 30 || rows < 9 || state.help)
        return 0;
    Visibility visibility;
    // Reserve the maximum composer height: explicit read never advances past
    // messages proven visible even when a multiline draft consumes more space.
    auto draft = ftxui::vbox({ftxui::text(""), ftxui::text(""), ftxui::text(""), ftxui::text("")});
    auto view = make_view(state, draft, columns, rows, nullptr, &visibility);
    auto screen =
        ftxui::Screen::Create(ftxui::Dimension::Fixed(columns), ftxui::Dimension::Fixed(rows));
    ftxui::Render(screen, std::move(view));
    std::uint64_t maximum = 0;
    for (const auto& [id, box] : visibility.messages) {
        if (box.y_max >= visibility.viewport.y_min && box.y_min <= visibility.viewport.y_max)
            maximum = std::max(maximum, id);
    }
    return maximum;
}

std::size_t maximum_scroll(const State& state, int columns, int rows)
{
    const auto found = state.rooms.find(state.active_room);
    if (found == state.rooms.end() || columns < 30 || rows < 9)
        return 0;
    if (!state.layout_cache)
        state.layout_cache = std::make_shared<LayoutCache>();
    const auto& room = found->second;
    std::size_t total = 0;
    for (std::size_t i = 0; i < room.messages.size(); ++i)
        total += state.layout_cache->height(room.messages[i], body_width(columns, rows)) + 2 +
                 (begins_day(room, i) ? 2 : 0);
    // Use the smallest timeline produced by the maximum composer height. The
    // oldest page remains populated even if the draft grows after paging.
    const auto viewport = static_cast<std::size_t>(std::max(1, rows - (rows >= 20 ? 10 : 8)));
    return total > viewport ? total - viewport : 0;
}
} // namespace client::ui
