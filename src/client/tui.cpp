#include "common.hpp"
#include "console.hpp"
#include "session.hpp"
#include "theme.hpp"
#include "view.hpp"
#include <chat/utf8.hpp>
#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <set>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

int run_plain(int argc, char** argv);

namespace
{
using namespace client;
using namespace ftxui;
using ui::Delivery;

bool is_terminal()
{
#ifdef _WIN32
    DWORD input = 0, output = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input) &&
           GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output);
#else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Request {
    Type type;
    std::string room, nonce;
    std::uint64_t after = 0, target = 0;
    bool synchronize = false, older = false;
};

class Chat
{
    Network network_;
    ConsoleState console_;
    ScreenInteractive screen_ = ScreenInteractive::Fullscreen();
    ui::State state_;
    std::string draft_, saved_draft_;
    int cursor_ = 0, selected_ = 0;
    int columns_ = 0, rows_ = 0;
    std::vector<std::string> room_names_, room_labels_, history_;
    std::map<std::string, std::string> drafts_;
    std::size_t history_index_ = 0;
    std::map<std::string, std::uint64_t> own_reads_, peer_reads_;
    std::map<std::string, std::uint64_t> sync_cursors_;
    std::set<std::string> sync_needed_;
    std::set<std::string> older_windows_;
    std::map<std::uint32_t, Request> requests_;
    std::uint32_t next_ = 2;
    bool replay_ = false, refresh_rooms_ = false, rooms_seen_ = false;
    Component input_, rooms_, send_button_, component_;
    // The IO worker must join before its screen-notification target is destroyed.
    std::unique_ptr<Session> session_;

    ui::Room& room(const std::string& name)
    {
        auto& value = state_.rooms[name];
        value.name = name;
        return value;
    }

    void labels()
    {
        room_names_.clear();
        room_labels_.clear();
        const auto add = [&](const std::string& name, const ui::Room& value) {
            room_names_.push_back(name);
            auto label = " # " + terminal_safe(name);
            if (value.unread)
                label += "  " + std::to_string(value.unread);
            room_labels_.push_back(std::move(label));
        };
        if (auto found = state_.rooms.find("lobby"); found != state_.rooms.end())
            add(found->first, found->second);
        for (const auto& [name, value] : state_.rooms)
            if (name != "lobby")
                add(name, value);
        const auto found = std::find(room_names_.begin(), room_names_.end(), state_.active_room);
        selected_ = found == room_names_.end() ? 0 : static_cast<int>(found - room_names_.begin());
    }

    bool request(Type type,
                 std::vector<std::string> fields,
                 std::string name = {},
                 std::string nonce = {},
                 std::uint64_t after = 0,
                 std::uint64_t target = 0,
                 bool synchronize = false,
                 bool older = false)
    {
        if (state_.connection != "online") {
            state_.notice = "暂时离线，草稿已保留；连接恢复后再发送。";
            return false;
        }
        if (requests_.size() >= 128) {
            state_.notice = "正在处理的请求较多，请稍后再试；草稿已保留。";
            return false;
        }
        while (next_ == 0 || next_ == 1 || requests_.contains(next_))
            ++next_;
        const auto id = next_++;
        try {
            session_->send({type, id, std::move(fields)});
            requests_.emplace(
                id,
                Request{
                    type, std::move(name), std::move(nonce), after, target, synchronize, older});
            return true;
        } catch (const std::exception&) {
            state_.notice = "未能提交给本地后台，草稿已保留。";
            return false;
        }
    }

    bool pending(Type type, const std::string& name = {}) const
    {
        return std::any_of(requests_.begin(), requests_.end(), [&](const auto& item) {
            return item.second.type == type && (name.empty() || item.second.room == name);
        });
    }

    bool history_page(const std::string& name,
                      std::uint64_t after,
                      std::uint64_t target,
                      bool synchronize,
                      bool older = false)
    {
        if (pending(Type::history, name))
            return false;
        const bool accepted = request(Type::history,
                                      {name, std::to_string(after), "200"},
                                      name,
                                      {},
                                      after,
                                      target,
                                      synchronize,
                                      older);
        if (accepted && older)
            older_windows_.insert(name);
        return accepted;
    }

    void synchronize(const std::string& name)
    {
        auto& value = room(name);
        // A recent global-ID window holds at most 200 messages. Advertised room
        // maxima and unsolicited live events are not contiguous history cursors.
        const auto found = sync_cursors_.find(name);
        const auto after = found != sync_cursors_.end() ? found->second
                           : value.last_id > 200        ? value.last_id - 200
                                                        : 0;
        if (found == sync_cursors_.end() || after < value.last_id)
            history_page(name, after, value.last_id, true);
    }

    bool older_history()
    {
        const auto& value = room(state_.active_room);
        auto oldest = value.last_id;
        for (const auto& item : value.messages)
            if (item.id)
                oldest = std::min(oldest, item.id);
        if (!oldest) {
            synchronize(state_.active_room);
            return true;
        }
        return history_page(
            state_.active_room, oldest > 200 ? oldest - 200 : 0, oldest - 1, false, true);
    }

    void latest()
    {
        auto& value = room(state_.active_room);
        value.scroll = 0;
        if (older_windows_.erase(state_.active_room)) {
            sync_cursors_.erase(state_.active_room);
            sync_needed_.insert(state_.active_room);
            state_.notice = "正在返回最新消息。";
            if (state_.connection == "online" && !pending(Type::history, state_.active_room)) {
                sync_needed_.erase(state_.active_room);
                synchronize(state_.active_room);
            }
        }
    }

    void activate(const std::string& name)
    {
        if (name == state_.active_room)
            return;
        drafts_[state_.active_room] = draft_;
        state_.active_room = name;
        auto& value = room(name);
        value.scroll = 0;
        draft_ = drafts_[name];
        cursor_ = static_cast<int>(draft_.size());
        state_.notice.clear();
        if (state_.connection == "online" && rooms_seen_)
            synchronize(name);
        else
            sync_needed_.insert(name);
        labels();
        input_->TakeFocus();
    }

    void cycle_room(int direction)
    {
        if (room_names_.empty())
            return;
        const auto count = static_cast<int>(room_names_.size());
        activate(room_names_[static_cast<std::size_t>((selected_ + direction + count) % count)]);
    }

    void remember_input(const std::string& text)
    {
        if (history_.empty() || history_.back() != text)
            history_.push_back(text);
        if (history_.size() > 100)
            history_.erase(history_.begin());
        history_index_ = history_.size();
        saved_draft_.clear();
    }

    void recall(int direction)
    {
        if (history_.empty())
            return;
        if (history_index_ == history_.size())
            saved_draft_ = draft_;
        if (direction < 0 && history_index_)
            --history_index_;
        if (direction > 0 && history_index_ < history_.size())
            ++history_index_;
        draft_ = history_index_ == history_.size() ? saved_draft_ : history_[history_index_];
        cursor_ = static_cast<int>(draft_.size());
    }

    void mark_read()
    {
        if (pending(Type::history, state_.active_room)) {
            state_.notice = "正在同步历史，完成后再标记已读。";
            return;
        }
        const auto maximum = ui::visible_message_max(state_, screen_.dimx(), screen_.dimy());
        if (!maximum) {
            state_.notice = "当前没有可标记的已显示消息。";
            return;
        }
        request(Type::read, {state_.active_room, std::to_string(maximum)}, state_.active_room);
    }

    bool command(const std::string& text)
    {
        if (text == "/quit") {
            screen_.Exit();
            return true;
        }
        if (text == "/help") {
            state_.help = !state_.help;
            return true;
        }
        if (text == "/rooms") {
            refresh_rooms_ = true;
            request(Type::list, {});
            state_.notice = "房间与未读数显示在左侧；Tab 切换，窄屏可使用 /join。";
            return true;
        }
        if (text == "/read") {
            mark_read();
            return state_.connection == "online";
        }
        if (text.starts_with("/join ")) {
            auto name = text.substr(6);
            return request(Type::join, {name}, name);
        }
        if (text == "/history")
            return older_history();
        if (text.starts_with("/history ")) {
            const auto after = text.size() > 9 ? text.substr(9) : "0";
            if (after.empty() || after.find_first_not_of("0123456789") != std::string::npos) {
                state_.notice = "用法：/history [消息编号]";
                return false;
            }
            return request(Type::history, {state_.active_room, after, "200"}, state_.active_room);
        }
        state_.notice = "未知命令。按 F1 查看快捷键与命令。";
        return false;
    }

    void submit()
    {
        if (draft_.empty())
            return;
        if (!valid_utf8(draft_) || draft_.size() > 65536) {
            state_.notice = "消息必须是有效 UTF-8，长度不超过 65536 字节。";
            return;
        }
        const auto text = draft_;
        if (text.front() == '/') {
            if (!command(text))
                return;
        } else {
            const auto nonce = random_id();
            if (!trim(text.size() + nonce.size() + state_.user.size())) {
                state_.notice = "待确认消息较多，草稿已保留；稍后再发送。";
                return;
            }
            latest();
            if (!request(Type::send, {state_.active_room, nonce, text}, state_.active_room, nonce))
                return;
            auto& value = room(state_.active_room);
            value.messages.push_back(
                {0, nonce, state_.user, text, now_ms(), Delivery::pending, false});
            value.scroll = 0;
        }
        remember_input(text);
        draft_.clear();
        drafts_[state_.active_room].clear();
        cursor_ = 0;
        input_->TakeFocus();
    }

    void sort_messages(ui::Room& value)
    {
        std::stable_sort(
            value.messages.begin(), value.messages.end(), [](const auto& a, const auto& b) {
                if (!a.id || !b.id)
                    return a.id && !b.id;
                return a.id < b.id;
            });
        while (value.messages.size() > 500) {
            auto victim = value.messages.begin();
            if (older_windows_.contains(value.name)) {
                auto found = std::find_if(
                    value.messages.rbegin(), value.messages.rend(), [](const auto& item) {
                        return item.delivery != Delivery::pending;
                    });
                if (found == value.messages.rend())
                    break;
                victim = std::prev(found.base());
            }
            value.messages.erase(victim);
        }
    }

    void message(const Frame& frame)
    {
        const auto& f = frame.fields;
        if (f.size() != 6)
            return;
        const auto id = static_cast<std::uint64_t>(std::stoull(f[0]));
        auto& value = room(f[1]);
        auto found =
            std::find_if(value.messages.begin(), value.messages.end(), [&](const auto& item) {
                return item.id == id ||
                       (!item.nonce.empty() && item.nonce == f[5] && item.author == f[2]);
            });
        const auto delivery =
            f[2] == state_.user && peer_reads_[f[1]] >= id ? Delivery::read : Delivery::sent;
        ui::Message item{id, f[5], f[2], f[4], std::stoll(f[3]), delivery, replay_};
        if (found == value.messages.end()) {
            value.messages.push_back(std::move(item));
            if (!replay_ && frame.request_id == 0 && f[2] != state_.user && id > own_reads_[f[1]])
                ++value.unread;
        } else {
            item.cached = found->cached && replay_;
            *found = std::move(item);
        }
        value.last_id = std::max(value.last_id, id);
        sort_messages(value);
        if (!replay_ && frame.request_id == 0)
            refresh_rooms_ = true;
    }

    void connection(const Frame& frame)
    {
        if (frame.fields.empty())
            return;
        const auto& next = frame.fields[0];
        if (next == "cache") {
            replay_ = true;
            return;
        }
        replay_ = false;
        const auto before = state_.connection;
        state_.connection = next;
        if (next == "online" && before != "online") {
            state_.notice.clear();
            refresh_rooms_ = true;
            rooms_seen_ = false;
            sync_needed_.insert(state_.active_room);
        } else if (next == "offline") {
            state_.notice = "连接已断开，后台正在重连。可继续编辑草稿。";
        } else if (next == "daemon-offline") {
            state_.notice = "本地后台已停止。草稿仍可编辑，Ctrl+C 退出。";
            for (auto& [name, value] : state_.rooms)
                for (auto& item : value.messages)
                    if (item.delivery == Delivery::pending)
                        item.delivery = Delivery::uncertain;
            requests_.clear();
        } else if (next == "auth-error") {
            state_.notice = "账号认证失败，请检查后台配置。";
        } else if (next == "cache-error") {
            state_.notice = "本地历史保存失败；服务器仍是权威记录。";
            state_.connection = before;
        }
    }

    void receipt(const Frame& frame)
    {
        const auto& f = frame.fields;
        if (f.size() != 3)
            return;
        const auto id = static_cast<std::uint64_t>(std::stoull(f[2]));
        if (f[1] == state_.user) {
            own_reads_[f[0]] = std::max(own_reads_[f[0]], id);
        } else {
            peer_reads_[f[0]] = std::max(peer_reads_[f[0]], id);
            for (auto& item : room(f[0]).messages)
                if (item.author == state_.user && item.id && item.id <= id)
                    item.delivery = Delivery::read;
        }
        if (!replay_)
            refresh_rooms_ = true;
    }

    void room_metadata(const Frame& frame)
    {
        for (std::size_t i = 0; i + 2 < frame.fields.size(); i += 3) {
            auto& value = room(frame.fields[i]);
            value.last_id = std::stoull(frame.fields[i + 1]);
            value.unread = static_cast<std::size_t>(std::stoull(frame.fields[i + 2]));
        }
        rooms_seen_ = true;
    }

    void result(const Frame& frame, const Request& operation)
    {
        if (frame.type == Type::ack && operation.type == Type::join) {
            room(operation.room);
            activate(operation.room);
            refresh_rooms_ = true;
        }
        if (operation.type == Type::send) {
            auto& value = room(operation.room);
            for (auto& item : value.messages) {
                if (item.nonce != operation.nonce || item.author != state_.user)
                    continue;
                if (frame.type == Type::ack && !frame.fields.empty()) {
                    item.id = std::stoull(frame.fields[0]);
                    item.delivery =
                        peer_reads_[operation.room] >= item.id ? Delivery::read : Delivery::sent;
                } else if (frame.type == Type::error) {
                    const bool unknown =
                        !frame.fields.empty() && frame.fields[0] == "delivery_unknown";
                    item.delivery = unknown ? Delivery::uncertain : Delivery::failed;
                }
            }
            sort_messages(value);
        }
        if (frame.type == Type::history_end) {
            room(operation.room).loaded = true;
            const auto cursor = frame.fields.size() < 2
                                    ? operation.after
                                    : static_cast<std::uint64_t>(std::stoull(frame.fields[1]));
            if (operation.synchronize)
                sync_cursors_[operation.room] = std::max(sync_cursors_[operation.room], cursor);
            const bool browsing_older = operation.older && older_windows_.contains(operation.room);
            if ((operation.synchronize || browsing_older) && cursor > operation.after &&
                cursor < operation.target)
                history_page(operation.room,
                             cursor,
                             operation.target,
                             operation.synchronize,
                             operation.older);
            else if (browsing_older)
                state_.notice = "已加载较早历史；PgUp 查看，PgDn 到底返回最新。";
        }
        if (frame.type == Type::error) {
            const auto code = frame.fields.empty() ? "" : frame.fields[0];
            state_.notice = code == "offline"     ? "暂时离线，消息未排队。草稿可继续编辑。"
                            : code == "not_found" ? "房间不存在，使用 /join 房间名 创建。"
                            : code == "delivery_unknown"
                                ? "发送结果未确认，请先查看历史，避免重复发送。"
                                : "操作未成功，请检查房间、输入或连接状态。";
        }
    }

    void event(const Frame& frame)
    {
        switch (frame.type) {
        case Type::message:
            message(frame);
            break;
        case Type::status:
            connection(frame);
            break;
        case Type::receipt:
            receipt(frame);
            break;
        case Type::rooms:
            room_metadata(frame);
            break;
        default:
            break;
        }
        auto found = requests_.find(frame.request_id);
        if (found == requests_.end() || frame.type == Type::message)
            return;
        auto operation = found->second;
        requests_.erase(found);
        result(frame, operation);
    }

    bool trim(std::size_t reserve = 0)
    {
        std::size_t bytes = 0;
        for (const auto& [name, value] : state_.rooms)
            for (const auto& item : value.messages)
                bytes += item.text.size() + item.nonce.size() + item.author.size();
        while (bytes + reserve > 8 * 1024 * 1024) {
            ui::Room* oldest = nullptr;
            std::size_t index = 0;
            for (auto& [name, value] : state_.rooms) {
                for (std::size_t i = 0; i < value.messages.size(); ++i) {
                    if (value.messages[i].delivery == Delivery::pending)
                        continue;
                    const bool selected = name == state_.active_room;
                    const bool was_selected = oldest && oldest->name == state_.active_room;
                    const bool newer = selected && older_windows_.contains(name);
                    const bool preferred =
                        oldest && selected == was_selected &&
                        (newer ? value.messages[i].time_ms > oldest->messages[index].time_ms
                               : value.messages[i].time_ms < oldest->messages[index].time_ms);
                    if (!oldest || (was_selected && !selected) || preferred) {
                        oldest = &value;
                        index = i;
                    }
                }
            }
            if (!oldest)
                return false;
            const auto& item = oldest->messages[index];
            bytes -= item.text.size() + item.nonce.size() + item.author.size();
            oldest->messages.erase(oldest->messages.begin() + static_cast<std::ptrdiff_t>(index));
        }
        return true;
    }

    void network_events()
    {
        for (const auto& frame : session_->take()) {
            try {
                event(frame);
            } catch (const std::exception&) {
                state_.notice = "收到无法解释的消息元信息。";
            }
        }
        if (refresh_rooms_ && !pending(Type::list) && state_.connection == "online") {
            refresh_rooms_ = false;
            request(Type::list, {});
        }
        if (state_.connection == "online" && rooms_seen_ &&
            !pending(Type::history, state_.active_room) && sync_needed_.erase(state_.active_room))
            synchronize(state_.active_room);
        trim();
        labels();
    }

    bool key(Event event)
    {
        if (event == Event::Custom) {
            network_events();
            return true;
        }
        if (event == Event::CtrlC || event == Event::CtrlD) {
            screen_.Exit();
            return true;
        }
        if (event == Event::F1) {
            state_.help = !state_.help;
            return true;
        }
        if (state_.help) {
            if (event == Event::Escape)
                state_.help = false;
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            cycle_room(event == Event::Tab ? 1 : -1);
            return true;
        }
        if (event == Event::CtrlR) {
            mark_read();
            return true;
        }
        if (event == Event::PageUp || event == Event::PageDown) {
            auto& value = room(state_.active_room);
            const auto page = static_cast<std::size_t>(std::max(1, screen_.dimy() / 2));
            const auto maximum = ui::maximum_scroll(state_, screen_.dimx(), screen_.dimy());
            if (event == Event::PageUp && value.scroll >= maximum &&
                !pending(Type::history, value.name))
                older_history();
            value.scroll = event == Event::PageUp ? std::min(value.scroll + page, maximum)
                           : value.scroll > page  ? value.scroll - page
                                                  : 0;
            if (event == Event::PageDown && !value.scroll)
                latest();
            return true;
        }
        if (input_->Focused() && (event == Event::ArrowUp || event == Event::ArrowDown)) {
            recall(event == Event::ArrowUp ? -1 : 1);
            return true;
        }
        if (event.is_character() && input_->Focused() &&
            (draft_.size() + event.character().size() > 65536 ||
             terminal_safe(event.character()) != event.character())) {
            state_.notice = "消息最多 65536 字节；不接受终端控制字符。";
            return true;
        }
        return false;
    }

    void widgets()
    {
        InputOption options;
        options.multiline = false;
        options.cursor_position = &cursor_;
        options.on_enter = [this] { submit(); };
        options.on_change = [this] { state_.notice.clear(); };
        options.transform = ui::theme::editor;
        input_ = Input(&draft_, "输入消息，Enter 发送…", options);
        auto send_style = ButtonOption::Simple();
        send_style.transform = [this](const EntryState& entry) {
            return ui::theme::action(entry, state_.connection == "online");
        };
        send_button_ = Button("发送", [this] { submit(); }, send_style);
        auto menu = MenuOption::Vertical();
        menu.entries_option.transform = ui::theme::room;
        menu.on_change = [this] {
            if (selected_ >= 0 && selected_ < static_cast<int>(room_names_.size()))
                activate(room_names_[static_cast<std::size_t>(selected_)]);
        };
        rooms_ = Menu(&room_labels_, &selected_, menu);
        auto controls =
            Container::Horizontal({rooms_, Container::Horizontal({input_, send_button_})});
        component_ = Renderer(controls, [this] {
            if (columns_ != screen_.dimx() || rows_ != screen_.dimy()) {
                columns_ = screen_.dimx();
                rows_ = screen_.dimy();
                auto& value = room(state_.active_room);
                value.scroll = std::min(value.scroll, ui::maximum_scroll(state_, columns_, rows_));
            }
            auto composer = hbox({input_->Render() | flex, text("  "), send_button_->Render()});
            return ui::conversation_view(
                state_, std::move(composer), screen_.dimx(), screen_.dimy(), rooms_->Render());
        });
        component_ |= CatchEvent([this](Event event) { return key(std::move(event)); });
        input_->TakeFocus();
    }

  public:
    explicit Chat(const std::filesystem::path& directory)
    {
        std::ifstream endpoint(directory / "endpoint");
        std::string port, token;
        if (!std::getline(endpoint, port) || !std::getline(endpoint, token) ||
            !std::getline(endpoint, state_.tenant) || !std::getline(endpoint, state_.user))
            throw std::runtime_error("No usable daemon endpoint. Start chat-daemon first.");
        room("lobby");
        labels();
        widgets();
        screen_.HandlePipedInput(false);
        screen_.ForceHandleCtrlC(false);
        session_ =
            std::make_unique<Session>(connect_tcp("127.0.0.1", port_number(port)),
                                      Frame{Type::hello, 1, {state_.tenant, state_.user, token}},
                                      [this] { screen_.PostEvent(Event::Custom); });
    }

    void run()
    {
        screen_.Loop(component_);
        // Keep IO alive briefly for Enter immediately followed by /quit.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (pending(Type::send) && std::chrono::steady_clock::now() < deadline) {
            network_events();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};
} // namespace

int main(int argc, char** argv)
{
    try {
        if (help_requested(argc, argv)) {
            std::cout << "chat [--state-dir PATH] [--plain]\n"
                         "Native FTXUI conversation client. F1 help; Enter send; Ctrl+R read.\n"
                         "Commands: /rooms /join /history /read /help /quit.\n";
            return 0;
        }
        check_options(argc, argv, {"--state-dir"}, {"--plain"});
        bool plain = !is_terminal();
        for (int i = 1; i < argc; ++i)
            plain = plain || std::string_view(argv[i]) == "--plain";
        if (plain)
            return run_plain(argc, argv);
        Chat app(std::filesystem::path(option(argc, argv, "--state-dir", ".chat")));
        app.run();
    } catch (const std::exception& error) {
        std::cerr << "chat: " << error.what() << '\n';
        return 1;
    }
}
