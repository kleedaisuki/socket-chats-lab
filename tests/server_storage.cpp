#include "../src/server/storage.hpp"
#include <atomic>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
using namespace chat::server;

void check(bool condition)
{
    if (!condition)
        throw std::runtime_error("Storage contract failed");
}

template <class F> void fails(const char* code, F&& fn)
{
    try {
        fn();
    } catch (const Failure& e) {
        check(e.code == code);
        return;
    }
    throw std::runtime_error("Expected domain failure");
}

int main()
{
    try {
        auto db = memory_storage();
        db->join("a", "lobby");
        db->join("b", "lobby");
        auto first = db->send("a", "alice", "lobby", "n1", "hello 中文 🌸");
        check(first.fresh && first.message.time > 0);
        check(!db->send("a", "alice", "lobby", "n1", "hello 中文 🌸").fresh);
        fails("conflict", [&] { db->send("a", "alice", "lobby", "n1", "different"); });
        check(db->history("b", "lobby", 0, 200).empty());
        check(db->list("a", "alice")[0].unread == 0);
        check(db->list("a", "bob")[0].unread == 1);
        check(db->read("a", "bob", "lobby", first.message.id) == first.message.id);
        check(db->read("a", "bob", "lobby", 0) == first.message.id);
        check(db->list("a", "bob")[0].unread == 0);
        fails("invalid", [&] { db->read("a", "bob", "lobby", first.message.id + 1); });
        std::atomic<unsigned> fresh = 0;
        std::vector<std::thread> workers;
        for (int i = 0; i < 16; ++i)
            workers.emplace_back([&] {
                if (db->send("a", "alice", "lobby", "shared", "same").fresh)
                    ++fresh;
            });
        for (auto& t : workers)
            t.join();
        check(fresh == 1);
        std::mt19937 rng(74821);
        std::uint64_t cursor = first.message.id;
        for (unsigned i = 0; i < 400; ++i) {
            auto sent = db->send(
                "a", i % 2 ? "alice" : "bob", "lobby", "random" + std::to_string(i), "payload");
            auto requested = rng() % (sent.message.id + 1);
            cursor = std::max(cursor, requested);
            check(db->read("a", "bob", "lobby", requested) == cursor);
        }
        auto messages = db->history("a", "lobby", 0, 200);
        check(messages.size() == 200);
        for (std::size_t i = 1; i < messages.size(); ++i)
            check(messages[i - 1].id < messages[i].id);
        for (unsigned i = 1; i < 42; ++i)
            db->join("a", "r" + std::to_string(i));
        db->join("a", "lobby");
        fails("overload", [&] { db->join("a", "overflow"); });
        std::cout << "Storage contracts and deterministic cursor properties passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
