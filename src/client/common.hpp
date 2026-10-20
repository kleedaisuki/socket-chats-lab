#pragma once
#include <algorithm>
#include <chat/net.hpp>
#include <chat/protocol.hpp>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <unordered_map>
#include <utility>
#ifdef _WIN32
#include <aclapi.h>
#include <windows.h>
#endif

namespace client {
using namespace chat;

inline bool
help_requested(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h")
            return true;
    return false;
}

inline void
check_options(
    int argc,
    char** argv,
    std::initializer_list<std::string_view> values,
    std::initializer_list<std::string_view> flags = {}
) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (std::find(flags.begin(), flags.end(), arg) != flags.end())
            continue;
        if (std::find(values.begin(), values.end(), arg) == values.end())
            throw std::runtime_error("Unknown option: " + std::string(arg));
        if (++i >= argc)
            throw std::runtime_error("Missing value for " + std::string(arg));
    }
}
#ifdef _WIN32
class StatePermissions {
    PACL acl_ = nullptr;

    static void
    check(DWORD error) {
        if (error != ERROR_SUCCESS)
            throw std::runtime_error(
                "Cannot secure Windows state for the current user (Win32 error "
                + std::to_string(error)
                + ")"
            );
    }

public:
    StatePermissions() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            check(GetLastError());
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size == 0) {
            auto error = GetLastError();
            CloseHandle(token);
            check(error);
        }
        std::vector<std::byte> info(size);
        const auto success = GetTokenInformation(token, TokenUser, info.data(), size, &size);
        const auto error = success ? ERROR_SUCCESS : GetLastError();
        CloseHandle(token);
        check(error);
        auto user = reinterpret_cast<TOKEN_USER*>(info.data());
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_ALL_ACCESS;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);
        check(SetEntriesInAclW(1, &access, nullptr, &acl_));
    }

    StatePermissions(const StatePermissions&) = delete;
    StatePermissions& operator=(const StatePermissions&) = delete;

    ~StatePermissions() {
        if (acl_)
            LocalFree(acl_);
    }

    void
    protect(const std::filesystem::path& path, bool required = true) const {
        auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (!required && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
                return;
            check(error);
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Refusing a reparse point in private Windows state");
        auto name = path.wstring();
        check(SetNamedSecurityInfoW(
            name.data(),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            acl_,
            nullptr
        ));
    }

    void
    protect(HANDLE file) const {
        check(SetSecurityInfo(
            file,
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            acl_,
            nullptr
        ));
    }
};
#endif
inline std::string
random_id() {
    std::random_device source;
    std::ostringstream out;
    for (int i = 0; i < 8; ++i)
        out << std::hex << std::setw(8) << std::setfill('0') << source();
    return out.str();
}

inline std::string
option(int argc, char** argv, std::string_view key, std::string fallback = {}) {
    for (int i = 1; i < argc; ++i)
        if (argv[i] == key) {
            if (i + 1 == argc)
                throw std::runtime_error("Missing value for " + std::string(key));
            return argv[i + 1];
        }
    return fallback;
}

inline uint16_t
port_number(const std::string& value) {
    size_t end = 0;
    auto port = std::stoul(value, &end);
    if (end != value.size() || port > 65535)
        throw std::runtime_error("Invalid port");
    return static_cast<uint16_t>(port);
}

struct Channel {
    Socket socket = invalid_socket;
    Decoder decoder;
    std::deque<std::vector<std::byte>> output;
    size_t offset = 0, queued = 0;
    bool authenticated = false;
    bool closing = false;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();

    void
    queue(const Frame& frame) {
        auto bytes = encode(frame);
        if (queued + bytes.size() > 2 * 1024 * 1024)
            throw std::runtime_error("Slow peer exceeded output limit");
        queued += bytes.size();
        output.push_back(std::move(bytes));
    }

    void
    flush() {
        while (!output.empty()) {
            auto& bytes = output.front();
            auto n = transmit(socket, std::span<const std::byte>(bytes).subspan(offset));
            if (n < 0 && would_block())
                return;
            if (n <= 0)
                throw std::runtime_error("Connection lost while writing");
            offset += static_cast<size_t>(n);
            queued -= static_cast<size_t>(n);
            if (offset == bytes.size()) {
                output.pop_front();
                offset = 0;
            }
        }
    }

    std::vector<Frame>
    read() {
        std::byte data[32768];
        auto n = receive(socket, data);
        if (n < 0 && would_block())
            return {};
        if (n <= 0)
            throw std::runtime_error("Connection closed");
        return decoder.feed(std::span<const std::byte>(data, static_cast<size_t>(n)));
    }
};

inline std::string
json_string(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch < 32) {
            out += "\\u00";
            out += hex[ch >> 4];
            out += hex[ch & 15];
        } else
            out += static_cast<char>(ch);
    }
    return out + '"';
}

inline std::string
journal_line(const Frame& frame) {
    std::string out = "{\"type\":" + std::to_string(static_cast<int>(frame.type)) + ",\"fields\":[";
    for (size_t i = 0; i < frame.fields.size(); ++i) {
        if (i)
            out += ',';
        out += json_string(frame.fields[i]);
    }
    return out + "]}";
}

inline Frame
journal_frame(std::string_view line) {
    auto marker = line.find(",\"fields\":[");
    if (!line.starts_with("{\"type\":") || marker == line.npos)
        throw std::runtime_error("Bad cache record");
    Frame frame{static_cast<Type>(std::stoi(std::string(line.substr(8, marker - 8)))), 0, {}};
    size_t i = marker + 11;
    while (i < line.size() && line[i] != ']') {
        if (line[i++] != '"')
            throw std::runtime_error("Bad cache string");
        std::string field;
        while (i < line.size() && line[i] != '"') {
            char ch = line[i++];
            if (ch == '\\') {
                if (i >= line.size())
                    throw std::runtime_error("Bad cache escape");
                ch = line[i++];
                if (ch == 'u') {
                    if (i + 4 > line.size() || line.substr(i, 2) != "00")
                        throw std::runtime_error("Bad cache escape");
                    ch = static_cast<char>(
                        std::stoi(std::string(line.substr(i + 2, 2)), nullptr, 16)
                    );
                    i += 4;
                } else if (ch != '"' && ch != '\\')
                    throw std::runtime_error("Bad cache escape");
            }
            field += ch;
        }
        if (i >= line.size())
            throw std::runtime_error("Bad cache string");
        ++i;
        frame.fields.push_back(std::move(field));
        if (i < line.size() && line[i] == ',')
            ++i;
    }
    if (frame.type != Type::message && frame.type != Type::receipt)
        throw std::runtime_error("Unsupported cache event");
    return frame;
}
} // namespace client
