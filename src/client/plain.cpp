#include "common.hpp"
#include <chat/utf8.hpp>
#include <ctime>
#include <map>
#include <set>
#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace
{
using namespace client;
class PlainInput
{
  public:
    explicit PlainInput(bool) {}
    bool interactive() const
    {
        return false;
    }
    std::optional<std::string> read()
    {
#ifdef _WIN32
        auto input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD available = 0;
        const auto type = GetFileType(input);
        if (type == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
                return std::nullopt;
            if (!available)
                return std::string{};
        } else if (type == FILE_TYPE_CHAR && WaitForSingleObject(input, 0) != WAIT_OBJECT_0)
            return std::string{};
        char bytes[4096];
        DWORD count = 0;
        if (!ReadFile(input, bytes, sizeof bytes, &count, nullptr) || count == 0)
            return std::nullopt;
        return std::string(bytes, count);
#else
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (::poll(&input, 1, 0) <= 0)
            return std::string{};
        char bytes[4096];
        const auto count = ::read(STDIN_FILENO, bytes, sizeof bytes);
        if (count <= 0)
            return std::nullopt;
        return std::string(bytes, static_cast<std::size_t>(count));
#endif
    }
};
std::string timestamp(const std::string& value)
{
    try {
        auto seconds = static_cast<std::time_t>(std::stoll(value) / 1000);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &seconds);
#else
        gmtime_r(&seconds, &tm);
#endif
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << " UTC";
        return out.str();
    } catch (...) {
        return "?";
    }
}
class Tui
{
    Network network_;
    PlainInput terminal_;
    Channel channel_;
    Poller poller_;
    std::string room_ = "lobby", line_, user_, status_ = "connecting";
    std::map<std::string, uint64_t> displayed_;
    std::map<uint32_t, std::string> joins_;
    std::set<uint32_t> outstanding_;
    uint32_t request_ = 2;
    bool running_ = true, cache_ = false, discard_escape_ = false;
    bool line_overflow_ = false;
    int escape_bytes_ = 0;

    void prompt()
    {
        if (terminal_.interactive())
            std::cout << "\r\033[2K\033[36m[" << terminal_safe(room_) << "] > \033[0m"
                      << terminal_safe(line_) << std::flush;
    }
    void print(const std::string& text, const char* color = "\033[37m")
    {
        if (terminal_.interactive())
            std::cout << "\r\033[2K" << color << text << "\033[0m\n";
        else
            std::cout << text << '\n';
        std::cout.flush();
        prompt();
    }
    uint32_t send(Type type, std::vector<std::string> fields)
    {
        auto id = request_++;
        if (request_ == 0)
            request_ = 2;
        outstanding_.insert(id);
        channel_.queue({type, id, std::move(fields)});
        poller_.set(channel_.socket, true);
        return id;
    }
    void help()
    {
        print("Commands: /rooms | /join ROOM | /history [AFTER_ID] | /read | /quit | /help");
        print("Type a message to send. /read explicitly marks displayed messages read; receipt is "
              "never automatic.");
    }
    void command(std::string text)
    {
        if (text.empty())
            return;
        if (!valid_utf8(text)) {
            print("Input is not valid UTF-8.", "\033[31m");
            return;
        }
        if (text == "/quit") {
            running_ = false;
            return;
        }
        if (text == "/help") {
            help();
            return;
        }
        if (text == "/rooms") {
            send(Type::list, {});
            return;
        }
        if (text == "/history" || text.starts_with("/history ")) {
            auto after = text.size() > 9 ? text.substr(9) : "0";
            if (after.empty() || after.find_first_not_of("0123456789") != after.npos) {
                print("Usage: /history [AFTER_ID]");
                return;
            }
            send(Type::history, {room_, after, "100"});
            return;
        }
        if (text.starts_with("/join ")) {
            auto room = text.substr(6);
            joins_[send(Type::join, {room})] = room;
            return;
        }
        if (text == "/read") {
            auto id = displayed_[room_];
            if (!id) {
                print("No displayed messages in this room to mark read.");
                return;
            }
            send(Type::read, {room_, std::to_string(id)});
            return;
        }
        if (text[0] == '/') {
            print("Unknown command. Type /help.");
            return;
        }
        send(Type::send, {room_, random_id(), std::move(text)});
    }
    void event(const Frame& frame)
    {
        auto& f = frame.fields;
        if (frame.type == Type::ack || frame.type == Type::rooms || frame.type == Type::error ||
            frame.type == Type::receipt || frame.type == Type::pong ||
            frame.type == Type::history_end)
            outstanding_.erase(frame.request_id);
        if (frame.type == Type::welcome) {
            print("Attached as " + terminal_safe(user_) + ". /help for commands.");
            return;
        }
        if (frame.type == Type::status && f.size() >= 2) {
            status_ = f[0];
            cache_ = status_ == "cache";
            print("[" + terminal_safe(status_) + "] " + terminal_safe(f[1]), "\033[33m");
            return;
        }
        if (frame.type == Type::message && f.size() >= 6) {
            std::string origin = cache_ ? "cached" : frame.request_id ? "history" : "live";
            print("[" + timestamp(f[3]) + "] [" + origin + "] #" + terminal_safe(f[1]) + " " +
                      terminal_safe(f[2]) + " · " + terminal_safe(f[0]) + ": " +
                      terminal_safe(f[4]),
                  "\033[32m");
            try {
                displayed_[f[1]] =
                    std::max(displayed_[f[1]], static_cast<uint64_t>(std::stoull(f[0])));
            } catch (...) {
            }
            return;
        }
        if (frame.type == Type::rooms) {
            print("ROOM                         LAST ID       UNREAD", "\033[36m");
            for (size_t i = 0; i + 2 < f.size(); i += 3) {
                auto name = terminal_safe(f[i]);
                auto width = display_width(name);
                print(name + std::string(width < 29 ? 29 - width : 1, ' ') +
                      terminal_safe(f[i + 1]) + "    " + terminal_safe(f[i + 2]));
            }
            return;
        }
        if (frame.type == Type::receipt && f.size() >= 3) {
            print("[read] #" + terminal_safe(f[0]) + " " + terminal_safe(f[1]) + " through " +
                      terminal_safe(f[2]),
                  "\033[35m");
            return;
        }
        if (frame.type == Type::history_end) {
            print("[history complete] " + (f.empty() ? room_ : terminal_safe(f[0])));
            return;
        }
        if (frame.type == Type::error) {
            joins_.erase(frame.request_id);
            std::string message = "[error]";
            for (auto& field : f)
                message += " " + terminal_safe(field);
            print(message, "\033[31m");
            return;
        }
        if (frame.type == Type::ack) {
            auto found = joins_.find(frame.request_id);
            if (found != joins_.end()) {
                room_ = found->second;
                joins_.erase(found);
                print("[joined] #" + terminal_safe(room_));
            } else if (!f.empty())
                print("[sent] " + terminal_safe(f[0]));
        }
    }
    void input(std::string_view bytes)
    {
        if (bytes.empty())
            return;
        for (unsigned char ch : bytes) {
            if (!running_)
                break;
            if (terminal_.interactive() && ch == 27) {
                discard_escape_ = true;
                escape_bytes_ = 0;
                continue;
            }
            if (discard_escape_) {
                ++escape_bytes_;
                if ((escape_bytes_ > 1 && ch >= 0x40 && ch <= 0x7e) || escape_bytes_ > 16)
                    discard_escape_ = false;
                continue;
            }
            if (ch == 3 || ch == 4) {
                running_ = false;
                break;
            }
            if (ch == '\r' || ch == '\n') {
                if (ch == '\r' && !terminal_.interactive())
                    continue;
                auto text = std::move(line_);
                line_.clear();
                if (terminal_.interactive())
                    std::cout << "\r\033[2K";
                if (line_overflow_)
                    print("Input exceeds 65536 UTF-8 bytes; message was not sent.", "\033[31m");
                else
                    command(std::move(text));
                line_overflow_ = false;
                prompt();
                continue;
            }
            if (ch == 8 || ch == 127) {
                if (!line_.empty()) {
                    do {
                        auto tail = static_cast<unsigned char>(line_.back());
                        line_.pop_back();
                        if ((tail & 0xc0) != 0x80)
                            break;
                    } while (!line_.empty());
                }
            } else if (ch >= 32) {
                if (line_.size() < 65536)
                    line_ += static_cast<char>(ch);
                else
                    line_overflow_ = true;
            }
        }
        prompt();
    }

  public:
    Tui(int argc, char** argv)
        : terminal_([&] {
              for (int i = 1; i < argc; ++i)
                  if (std::string_view(argv[i]) == "--plain")
                      return true;
              return false;
          }())
    {
        auto dir = std::filesystem::path(option(argc, argv, "--state-dir", ".chat"));
        std::ifstream endpoint(dir / "endpoint");
        std::string port, token, tenant;
        if (!std::getline(endpoint, port) || !std::getline(endpoint, token) ||
            !std::getline(endpoint, tenant) || !std::getline(endpoint, user_))
            throw std::runtime_error("No usable daemon endpoint. Start chat-daemon --tenant TENANT "
                                     "--user USER --state-dir " +
                                     dir.string() + " with CHAT_TOKEN set.");
        channel_.socket = connect_tcp("127.0.0.1", port_number(port));
        nonblocking(channel_.socket);
        channel_.queue({Type::hello, 1, {tenant, user_, token}});
        poller_.set(channel_.socket, true);
        if (terminal_.interactive()) {
            print("Socket Chats · UTF-8 terminal · Ctrl-C or /quit exits", "\033[36m");
            help();
        }
    }
    ~Tui()
    {
        if (channel_.socket != invalid_socket)
            close_socket(channel_.socket);
    }
    void run()
    {
        prompt();
        while (running_) {
            for (auto ready : poller_.wait(25)) {
                if (ready.failed)
                    throw std::runtime_error("Daemon disconnected; restart it and attach again.");
                if (ready.writable)
                    channel_.flush();
                if (ready.readable)
                    for (auto& frame : channel_.read())
                        event(frame);
                poller_.set(channel_.socket, !channel_.output.empty());
            }
            auto bytes = terminal_.read();
            if (!bytes) {
                if (!line_.empty() && !line_overflow_)
                    command(std::exchange(line_, {}));
                running_ = false;
            } else
                input(*bytes);
        }
        // Flush complete commands before exiting a piped session.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while ((!channel_.output.empty() || !outstanding_.empty()) &&
               std::chrono::steady_clock::now() < deadline) {
            for (auto ready : poller_.wait(25)) {
                if (ready.failed)
                    return;
                if (ready.writable)
                    channel_.flush();
                if (ready.readable)
                    for (auto& frame : channel_.read())
                        event(frame);
                poller_.set(channel_.socket, !channel_.output.empty());
            }
        }
    }
};
} // namespace
int run_plain(int argc, char** argv)
{
    try {
        if (help_requested(argc, argv)) {
            std::cout
                << "chat [--state-dir PATH] [--plain]\n"
                   "Attach to chat-daemon. Commands: /help /rooms /join /history /read /quit.\n";
            return 0;
        }
        check_options(argc, argv, {"--state-dir"}, {"--plain"});
        Tui tui(argc, argv);
        tui.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "chat: " << error.what() << '\n';
        return 1;
    }
}
