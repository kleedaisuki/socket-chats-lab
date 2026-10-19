#include "../src/client/view.hpp"
#include "chat/utf8.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <random>
#include <stdexcept>

namespace
{
void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

client::ui::State fixture()
{
    using namespace client::ui;
    State state;
    state.tenant = "thought-lab";
    state.user = "Klee";
    state.connection = "在线";
    Room room;
    room.name = "lobby";
    room.loaded = true;
    room.unread = 2;
    room.messages = {
        {812341,
         "hidden-nonce-1",
         "Alice",
         "早上好，Klee！今天来研究一下流式协议？🌷",
         1792383000000,
         Delivery::sent,
         true},
        {812342,
         "hidden-nonce-2",
         "Klee",
         "好呀。先把聊天做得舒服，让复杂性留在程序里。",
         1792383060000,
         Delivery::read,
         false},
        {812343,
         "hidden-nonce-3",
         "Alice",
         "收到！中文长句应该自然换行，而不是横着消失。\n输入时，消息和草稿都要稳稳地留在原处。",
         1792383120000,
         Delivery::sent,
         false},
        {0,
         "hidden-nonce-4",
         "Klee",
         "这才像一个可以每天使用的聊天窗口。",
         1792383180000,
         Delivery::pending,
         false}};
    state.rooms.emplace("lobby", std::move(room));
    Room other;
    other.name = "research";
    other.unread = 3;
    other.messages.push_back({919191, {}, "Other", "PRIVATE_OTHER_ROOM_SENTINEL", 0});
    state.rooms.emplace("research", std::move(other));
    return state;
}

ftxui::Screen render(const client::ui::State& state, int columns, int rows)
{
    auto screen =
        ftxui::Screen::Create(ftxui::Dimension::Fixed(columns), ftxui::Dimension::Fixed(rows));
    auto view =
        client::ui::conversation_view(state, ftxui::text("正在输入：你好，世界 🌷"), columns, rows);
    ftxui::Render(screen, std::move(view));
    return screen;
}

std::string plain(const ftxui::Screen& screen)
{
    std::string output;
    for (int y = 0; y < screen.dimy(); ++y) {
        for (int x = 0; x < screen.dimx(); ++x)
            output += screen.CellAt(x, y).character;
        output += '\n';
    }
    return output;
}
} // namespace

int main(int argc, char** argv)
{
    auto state = fixture();
    if (argc == 2 && std::string(argv[1]) == "--benchmark") {
        auto sample = state.rooms.at("lobby").messages.front();
        for (int i = 0; i < 1000; ++i)
            state.rooms.at("lobby").messages.push_back(sample);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i)
            render(state, 120, 32);
        const auto duration =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        std::cout << "FTXUI build+render 120x32, 1004 cached messages, 100 frames: "
                  << duration / 100 << " ms/frame\n";
        sample.text.clear();
        for (int i = 0; i < 21845; ++i)
            sample.text += "中";
        for (int count : {1, 36}) {
            state.rooms.at("lobby").messages.assign(static_cast<std::size_t>(count), sample);
            state.layout_cache.reset();
            const auto cold_start = std::chrono::steady_clock::now();
            render(state, 120, 32);
            const auto cold_ms = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - cold_start)
                                     .count();
            std::vector<double> samples;
            for (int i = 0; i < 100; ++i) {
                const auto large_start = std::chrono::steady_clock::now();
                render(state, 120, 32);
                samples.push_back(std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - large_start)
                                      .count());
            }
            std::sort(samples.begin(), samples.end());
            std::cout << count << " x65535byte CJK messages: cold=" << cold_ms
                      << " ms, warm p50=" << samples[50] << " p95=" << samples[95] << " ms/frame\n";
        }
        sample.text.clear();
        for (int i = 0; i < 340; ++i)
            sample.text += "中";
        auto& room = state.rooms.at("lobby");
        room.messages.clear();
        for (int i = 0; i < 500; ++i) {
            sample.text.replace(0, 3, std::to_string(100 + i));
            room.messages.push_back(sample);
        }
        room.scroll = 4970;
        state.layout_cache.reset();
        const auto top_start = std::chrono::steady_clock::now();
        render(state, 120, 32);
        const auto top_cold =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - top_start)
                .count();
        std::vector<double> top_samples;
        for (int i = 0; i < 100; ++i) {
            const auto frame_start = std::chrono::steady_clock::now();
            render(state, 120, 32);
            top_samples.push_back(std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - frame_start)
                                      .count());
        }
        std::sort(top_samples.begin(), top_samples.end());
        std::cout << "500 distinct1KiBCJK messages, scroll4970: cold=" << top_cold
                  << " p50=" << top_samples[50] << " p95=" << top_samples[95] << " ms/frame\n";
        return 0;
    }
    if (argc == 5 && std::string(argv[1]) == "--snapshot") {
        auto screen = render(state, std::stoi(argv[2]), std::stoi(argv[3]));
        std::ofstream output(argv[4], std::ios::binary);
        require(bool(output), "snapshot file cannot be opened");
        output << screen.ToString();
        std::ofstream text(std::string(argv[4]) + ".txt", std::ios::binary);
        text << plain(screen);
        return 0;
    }
    for (const auto& [columns, rows] :
         {std::pair{120, 32}, {88, 24}, {60, 18}, {40, 12}, {12, 5}}) {
        auto screen = render(state, columns, rows);
        auto content = plain(screen);
        require(content.find("PRIVATE_OTHER_ROOM_SENTINEL") == std::string::npos,
                "inactive room leaked");
        require(content.find("81234") == std::string::npos, "message IDs leaked");
        require(content.find("hidden-nonce") == std::string::npos, "nonce leaked");
        require(content.find('\033') == std::string::npos, "control entered cell data");
        require(chat::valid_utf8(content), "invalid UTF-8 rendered");
        require(screen.dimx() == columns && screen.dimy() == rows, "dimension changed");
        if (columns >= 40)
            require(content.find("正在输入") != std::string::npos, "composer clipped away");
        if (columns >= 88)
            require(content.find("research") != std::string::npos, "wide room sidebar missing");
    }
    auto normal = plain(render(state, 120, 32));
    require(normal.find("早上好") != std::string::npos, "conversation body missing");
    require(normal.find("在线") != std::string::npos, "connection missing");
    require(normal.find("已读") != std::string::npos, "delivery missing");
    require(normal.find("发送中") != std::string::npos, "optimistic status missing");
    require(client::ui::visible_message_max(state) == 812343, "tail read marker incorrect");
    state.rooms.at("lobby").scroll = 7;
    require(client::ui::visible_message_max(state) == 812342, "scrolled read marker incorrect");
    state.rooms.at("lobby").scroll = 999;
    require(client::ui::visible_message_max(state) == 0, "history start read marker incorrect");
    state.help = true;
    require(plain(render(state, 120, 32)).find("键盘") != std::string::npos,
            "help overlay missing");
    state.help = false;
    state.rooms.at("lobby").scroll = 0;
    auto oversized = fixture();
    oversized.rooms.at("lobby").messages.back().text = std::string(10000, 'x');
    require(client::ui::visible_message_max(oversized, 40, 12) == 0,
            "hidden committed message marked read behind oversized optimistic send");
    require(client::ui::visible_message_max(oversized, 12, 5) == 0,
            "tiny fallback marked invisible messages read");
    auto long_message = fixture();
    std::string large = "START";
    for (int i = 0; i < 21837; ++i)
        large += "中";
    large += "END";
    long_message.rooms.at("lobby").messages = {{99, {}, "Alice", large, 0}};
    const auto tail = plain(render(long_message, 40, 12));
    require(tail.find("END") != tail.npos && tail.find("START") == tail.npos,
            "maximum Chinese message tail not virtualized");
    bool saw_start = false;
    for (std::size_t scroll = 0; scroll < 1300; scroll += 5) {
        long_message.rooms.at("lobby").scroll = scroll;
        const auto page = plain(render(long_message, 40, 12));
        saw_start |= page.find("START") != page.npos;
    }
    require(saw_start, "line paging cannot reach start of long Chinese message");
    const auto limit = client::ui::maximum_scroll(long_message, 40, 12);
    long_message.rooms.at("lobby").scroll = limit;
    const auto oldest = plain(render(long_message, 40, 12));
    require(oldest.find("START") != oldest.npos && oldest.find("已到历史开头") == oldest.npos,
            "maximum scroll overshoots into an empty history viewport");
    for (int i = 0; i < 100; ++i)
        long_message.rooms.at("lobby").scroll =
            std::min(limit, long_message.rooms.at("lobby").scroll + 6);
    require(long_message.rooms.at("lobby").scroll == limit,
            "repeated page-up advances beyond populated history");
    long_message.rooms.at("lobby").scroll = 0;
    for (const int columns : {120, 40, 88, 60, 120})
        require(plain(render(long_message, columns, 24)).find("END") != std::string::npos,
                "width change lost long message tail");
    for (int columns = 30; columns < 180; ++columns)
        render(long_message, columns, 24);
    const auto metrics = client::ui::layout_metrics(long_message);
    require(metrics.entries <= 128 && metrics.bytes <= 8 * 1024 * 1024,
            "layout cache exceeded bounded retention");
    auto skipped = fixture();
    skipped.rooms.at("lobby").messages.clear();
    for (int i = 0; i < 500; ++i)
        skipped.rooms.at("lobby").messages.push_back(
            {static_cast<std::uint64_t>(i + 1), {}, "Alice", "短消息" + std::to_string(i), 0});
    const auto original_limit = client::ui::maximum_scroll(skipped, 40, 12);
    skipped.rooms.at("lobby").messages.back().text = "changed\nbody\nheight";
    require(client::ui::maximum_scroll(skipped, 40, 12) == original_limit + 2,
            "skipped-message height memo ignored changed canonical body");
    skipped.rooms.at("lobby").scroll = client::ui::maximum_scroll(skipped, 40, 12);
    require(plain(render(skipped, 40, 12)).find("短消息0") != std::string::npos,
            "deep history maximum scroll lost oldest message");
    long_message.rooms.at("lobby").messages.front().text = "changed canonical body";
    require(plain(render(long_message, 40, 12)).find("changed canonical body") != std::string::npos,
            "same identity cache ignored changed canonical body");
    std::string numbered;
    for (int i = 1000; i < 1100; ++i)
        numbered += "LINE" + std::to_string(i) + "\n";
    long_message.rooms.at("lobby").messages.front().text = numbered;
    std::string visited;
    for (std::size_t scroll = 0; scroll < 110; ++scroll) {
        long_message.rooms.at("lobby").scroll = scroll;
        visited += plain(render(long_message, 40, 12));
    }
    for (int i = 1000; i < 1100; ++i)
        require(visited.find("LINE" + std::to_string(i)) != visited.npos,
                "row paging silently skipped canonical body lines");
    state.rooms.at("lobby").messages = {
        {1, {}, "\033]52;escape\a", "safe\033[2Jvisible\r\n中文\xe2\x80\xae", 0}};
    state.user = "Klee\033[2J";
    std::mt19937 random(731);
    for (int trial = 0; trial < 120; ++trial) {
        const int columns = 1 + static_cast<int>(random() % 160);
        const int rows = 1 + static_cast<int>(random() % 45);
        auto screen = render(state, columns, rows);
        auto output = plain(screen);
        require(output.find('\033') == std::string::npos, "untrusted escape rendered");
        require(output.find('\r') == std::string::npos, "untrusted carriage return rendered");
        require(chat::valid_utf8(output), "property: malformed UTF-8");
    }
    std::cout
        << "PASS FTXUI conversation fixtures and 120 seeded bounded-layout/control-safety cases\n";
}
