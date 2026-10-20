#include "storage.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <tuple>

namespace chat::server {
namespace {
class Memory final : public Storage {
    std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, std::vector<Message>> rooms_;
    std::map<std::tuple<std::string, std::string, std::string>, Message> nonces_;
    std::map<std::tuple<std::string, std::string, std::string>, std::uint64_t> reads_;
    std::uint64_t next_ = 1;

    auto&
    room(const std::string& t, const std::string& r) {
        auto it = rooms_.find({t, r});
        if (it == rooms_.end())
            throw Failure("not_found", "Join the room first");
        return it->second;
    }

public:
    void
    join(const std::string& t, const std::string& r) override {
        std::lock_guard lock(mutex_);
        if (!rooms_.contains({t, r})) {
            std::size_t count = 0;
            for (auto& [key, messages] : rooms_)
                if (key.first == t)
                    ++count;
            if (count >= 42)
                throw Failure("overload", "Tenant room limit reached");
            rooms_.try_emplace({t, r});
        }
    }

    Sent
    send(
        const std::string& t,
        const std::string& u,
        const std::string& r,
        const std::string& n,
        const std::string& text
    ) override {
        std::lock_guard lock(mutex_);
        auto key = std::tuple{t, u, n};
        auto old = nonces_.find(key);
        if (old != nonces_.end()) {
            if (old->second.room != r || old->second.text != text)
                throw Failure("conflict", "Nonce already used for different content");
            return {old->second, false};
        }
        auto& messages = room(t, r);
        Message m{
            next_++,
            r,
            u,
            text,
            n,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            )
                .count()
        };
        messages.push_back(m);
        nonces_.emplace(std::move(key), m);
        return {m, true};
    }

    std::vector<Message>
    history(
        const std::string& t,
        const std::string& r,
        std::uint64_t after,
        unsigned limit
    ) override {
        std::lock_guard lock(mutex_);
        std::vector<Message> out;
        for (const auto& m : room(t, r)) {
            if (m.id > after)
                out.push_back(m);
            if (out.size() == limit)
                break;
        }
        return out;
    }

    std::uint64_t
    read(
        const std::string& t,
        const std::string& u,
        const std::string& r,
        std::uint64_t id
    ) override {
        std::lock_guard lock(mutex_);
        auto& messages = room(t, r);
        auto maximum = messages.empty() ? 0 : messages.back().id;
        if (id > maximum)
            throw Failure("invalid", "Read cursor exceeds room maximum");
        auto& cursor = reads_[{t, u, r}];
        cursor = std::max(cursor, id);
        return cursor;
    }

    std::vector<Room>
    list(const std::string& t, const std::string& u) override {
        std::lock_guard lock(mutex_);
        std::vector<Room> out;
        for (const auto& [key, messages] : rooms_) {
            if (key.first != t)
                continue;
            auto cursor = reads_[{t, u, key.second}];
            std::uint64_t unread = 0;
            for (const auto& m : messages)
                if (m.id > cursor && m.user != u)
                    ++unread;
            out.push_back({key.second, messages.empty() ? 0 : messages.back().id, unread});
        }
        return out;
    }
};
} // namespace

std::unique_ptr<Storage>
memory_storage() {
    return std::make_unique<Memory>();
}
#ifndef CHAT_WITH_POSTGRES
std::unique_ptr<Storage>
postgres_storage(const std::string&, unsigned) {
    throw std::runtime_error(
        "PostgreSQL support unavailable; rebuild with CHAT_WITH_POSTGRES=ON or explicitly use "
        "--memory"
    );
}
#endif
} // namespace chat::server
