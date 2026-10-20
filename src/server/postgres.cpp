#include "storage.hpp"
#include <condition_variable>
#include <libpq-fe.h>
#include <mutex>
#include <utility>

namespace chat::server {
namespace {
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

Result
query(PGconn* c, const char* sql, const std::vector<std::string>& args = {}) {
    std::vector<const char*> values;
    for (auto& v : args)
        values.push_back(v.c_str());
    Result r(
        PQexecParams(
            c,
            sql,
            static_cast<int>(values.size()),
            nullptr,
            values.data(),
            nullptr,
            nullptr,
            0
        ),
        PQclear
    );
    if (!r
        || (PQresultStatus(r.get()) != PGRES_TUPLES_OK
            && PQresultStatus(r.get()) != PGRES_COMMAND_OK))
        throw Failure("storage", "Database operation failed");
    return r;
}

std::string
value(const Result& r, int row, int col) {
    return PQgetvalue(r.get(), row, col);
}

Message
message(const Result& r, int row) {
    return {
        std::stoull(value(r, row, 0)),
        value(r, row, 1),
        value(r, row, 2),
        value(r, row, 4),
        value(r, row, 5),
        std::stoll(value(r, row, 3))
    };
}

class Postgres final : public Storage {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<PGconn*> idle_;

    static void
    configure(PGconn* conn) {
        query(conn, "SET client_encoding='UTF8'");
        query(conn, "SET statement_timeout='10s'");
        query(conn, "SET lock_timeout='5s'");
    }

    struct Lease {
        Postgres& owner;
        PGconn* conn;

        explicit Lease(Postgres& o)
            : owner(o) {
            std::unique_lock lock(o.mutex_);
            o.ready_.wait(lock, [&] { return !o.idle_.empty(); });
            conn = o.idle_.back();
            o.idle_.pop_back();
            lock.unlock();
            try {
                if (PQstatus(conn) != CONNECTION_OK) {
                    // Reset only before a new operation; never replay an uncertain commit.
                    PQreset(conn);
                    if (PQstatus(conn) != CONNECTION_OK)
                        throw Failure("storage", "Database unavailable");
                    configure(conn);
                }
            } catch (...) {
                release();
                throw;
            }
        }

        void
        release() {
            std::lock_guard lock(owner.mutex_);
            owner.idle_.push_back(conn);
            owner.ready_.notify_one();
        }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        ~Lease() {
            release();
        }
    };

    struct Transaction {
        PGconn* c;
        bool committed = false;

        explicit Transaction(PGconn* p)
            : c(p) {
            query(c, "BEGIN");
        }

        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        ~Transaction() {
            if (!committed) {
                auto* r = PQexec(c, "ROLLBACK");
                PQclear(r);
            }
        }

        void
        commit() {
            query(c, "COMMIT");
            committed = true;
        }
    };

    void
    lock_room(PGconn* c, const std::string& t, const std::string& r) {
        auto result =
            query(c, "SELECT room FROM scl_rooms WHERE tenant=$1 AND room=$2 FOR UPDATE", {t, r});
        if (!PQntuples(result.get()))
            throw Failure("not_found", "Join the room first");
    }

public:
    Postgres(const std::string& dsn, unsigned count) {
        try {
            for (unsigned i = 0; i < count; ++i) {
                const char* keys[] = {"dbname", "connect_timeout", "client_encoding", nullptr};
                const char* values[] = {dsn.c_str(), "5", "UTF8", nullptr};
                auto* c = PQconnectdbParams(keys, values, 1);
                if (PQstatus(c) != CONNECTION_OK) {
                    PQfinish(c);
                    throw std::runtime_error("Cannot connect to PostgreSQL (check CHAT_DB_DSN)");
                }
                idle_.push_back(c);
                configure(c);
            }
            auto* c = idle_.front();
            Transaction tx(c);
            query(c, "SELECT pg_advisory_xact_lock(734621968)");
            query(
                c,
                "CREATE TABLE IF NOT EXISTS scl_schema(singleton boolean PRIMARY KEY DEFAULT true "
                "CHECK(singleton),version integer NOT NULL)"
            );
            query(
                c,
                "INSERT INTO scl_schema(singleton,version) VALUES(true,1) ON CONFLICT DO NOTHING"
            );
            auto version = query(c, "SELECT version FROM scl_schema WHERE singleton");
            if (value(version, 0, 0) != "1")
                throw std::runtime_error("Unsupported chat database schema version");
            query(
                c,
                "CREATE TABLE IF NOT EXISTS scl_rooms(tenant text NOT NULL,room text NOT "
                "NULL,PRIMARY KEY(tenant,room))"
            );
            query(
                c,
                "CREATE TABLE IF NOT EXISTS scl_messages(id bigserial PRIMARY KEY,tenant text NOT "
                "NULL,room text NOT NULL,username text NOT NULL,time_ms bigint NOT NULL,body text "
                "NOT NULL,nonce text NOT NULL,UNIQUE(tenant,username,nonce),FOREIGN "
                "KEY(tenant,room) REFERENCES scl_rooms(tenant,room))"
            );
            query(
                c,
                "CREATE INDEX IF NOT EXISTS scl_messages_room_id ON scl_messages(tenant,room,id)"
            );
            query(
                c,
                "CREATE TABLE IF NOT EXISTS scl_reads(tenant text NOT NULL,username text NOT "
                "NULL,room text NOT NULL,message_id bigint NOT NULL CHECK(message_id>=0),PRIMARY "
                "KEY(tenant,username,room),FOREIGN KEY(tenant,room) REFERENCES "
                "scl_rooms(tenant,room))"
            );
            tx.commit();
        } catch (...) {
            for (auto* c : idle_)
                PQfinish(c);
            throw;
        }
    }

    ~Postgres() override {
        for (auto* c : idle_)
            PQfinish(c);
    }

    void
    join(const std::string& t, const std::string& r) override {
        Lease l(*this);
        Transaction tx(l.conn);
        query(l.conn, "SELECT pg_advisory_xact_lock(hashtextextended($1,1))", {t});
        auto count = query(
            l.conn,
            "SELECT COUNT(*),COUNT(*) FILTER(WHERE room=$2) FROM scl_rooms WHERE tenant=$1",
            {t, r}
        );
        if (value(count, 0, 1) == "0" && std::stoull(value(count, 0, 0)) >= 42)
            throw Failure("overload", "Tenant room limit reached");
        query(
            l.conn,
            "INSERT INTO scl_rooms(tenant,room) VALUES($1,$2) ON CONFLICT DO NOTHING",
            {t, r}
        );
        tx.commit();
    }

    Sent
    send(
        const std::string& t,
        const std::string& u,
        const std::string& r,
        const std::string& n,
        const std::string& text
    ) override {
        Lease l(*this);
        auto* c = l.conn;
        Transaction tx(c);
        // Serialize a user's nonce across rooms, and room sends with read-cursor updates.
        query(c, "SELECT pg_advisory_xact_lock(hashtextextended($1,0))", {t + ":" + u});
        auto old = query(
            c,
            "SELECT id,room,username,time_ms,body,nonce FROM scl_messages WHERE tenant=$1 AND "
            "username=$2 AND nonce=$3",
            {t, u, n}
        );
        if (PQntuples(old.get())) {
            auto m = message(old, 0);
            if (m.room != r || m.text != text)
                throw Failure("conflict", "Nonce already used for different content");
            tx.commit();
            return {m, false};
        }
        lock_room(c, t, r);
        auto result = query(
            c,
            "INSERT INTO scl_messages(tenant,room,username,time_ms,body,nonce) "
            "VALUES($1,$2,$3,(extract(epoch FROM clock_timestamp())*1000)::bigint,$4,$5) RETURNING "
            "id,room,username,time_ms,body,nonce",
            {t, r, u, text, n}
        );
        auto m = message(result, 0);
        tx.commit();
        return {m, true};
    }

    std::vector<Message>
    history(
        const std::string& t,
        const std::string& r,
        std::uint64_t after,
        unsigned limit
    ) override {
        Lease l(*this);
        auto exists = query(l.conn, "SELECT 1 FROM scl_rooms WHERE tenant=$1 AND room=$2", {t, r});
        if (!PQntuples(exists.get()))
            throw Failure("not_found", "Join the room first");
        auto result = query(
            l.conn,
            "SELECT id,room,username,time_ms,body,nonce FROM scl_messages WHERE tenant=$1 AND "
            "room=$2 AND id>$3::bigint ORDER BY id LIMIT $4::int",
            {t, r, std::to_string(after), std::to_string(limit)}
        );
        std::vector<Message> out;
        for (int i = 0; i < PQntuples(result.get()); ++i)
            out.push_back(message(result, i));
        return out;
    }

    std::uint64_t
    read(
        const std::string& t,
        const std::string& u,
        const std::string& r,
        std::uint64_t id
    ) override {
        Lease l(*this);
        auto* c = l.conn;
        Transaction tx(c);
        lock_room(c, t, r);
        auto maximum = query(
            c,
            "SELECT COALESCE(MAX(id),0) FROM scl_messages WHERE tenant=$1 AND room=$2",
            {t, r}
        );
        if (id > std::stoull(value(maximum, 0, 0)))
            throw Failure("invalid", "Read cursor exceeds room maximum");
        auto result = query(
            c,
            "INSERT INTO scl_reads(tenant,username,room,message_id) VALUES($1,$2,$3,$4::bigint) ON "
            "CONFLICT(tenant,username,room) DO UPDATE SET "
            "message_id=GREATEST(scl_reads.message_id,EXCLUDED.message_id) RETURNING message_id",
            {t, u, r, std::to_string(id)}
        );
        auto cursor = std::stoull(value(result, 0, 0));
        tx.commit();
        return cursor;
    }

    std::vector<Room>
    list(const std::string& t, const std::string& u) override {
        Lease l(*this);
        auto result = query(
            l.conn,
            "SELECT r.room,COALESCE(MAX(m.id),0),COUNT(m.id) FILTER(WHERE "
            "m.id>COALESCE(c.message_id,0) AND m.username<>$2) FROM scl_rooms r LEFT JOIN "
            "scl_messages m ON m.tenant=r.tenant AND m.room=r.room LEFT JOIN scl_reads c ON "
            "c.tenant=r.tenant AND c.room=r.room AND c.username=$2 WHERE r.tenant=$1 GROUP BY "
            "r.room,c.message_id ORDER BY r.room",
            {t, u}
        );
        std::vector<Room> out;
        for (int i = 0; i < PQntuples(result.get()); ++i)
            out.push_back(
                {value(result, i, 0),
                 std::stoull(value(result, i, 1)),
                 std::stoull(value(result, i, 2))}
            );
        return out;
    }
};
} // namespace

std::unique_ptr<Storage>
postgres_storage(const std::string& dsn, unsigned count) {
    return std::make_unique<Postgres>(dsn, count);
}
} // namespace chat::server
