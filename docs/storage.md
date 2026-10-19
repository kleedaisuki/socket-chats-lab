# Server storage and concurrency

## Operational contract

`chat-server` defaults to PostgreSQL. Build with `CHAT_WITH_POSTGRES=ON` and libpq development files; supply `CHAT_DB_DSN` and `CHAT_CREDENTIALS=tenant:user:token;...` in the process environment. Credentials and connection strings are never logged. The explicit `--memory` flag selects ephemeral, non-durable development storage; no automatic persistence downgrade occurs. `--help` lists process options.

The default listener is loopback plaintext. Non-loopback bind names require `--allow-plaintext`. Use an SSH tunnel or an authenticated TLS transport boundary for remote deployment: this server does not implement TLS and is not an Internet-facing hardened service. Start one server instance per database deployment: live broadcasts are process-local, not distributed PostgreSQL notifications. Database history remains authoritative after a disconnect.

## Execution and resource model

One reactor owns every socket, decoder, request queue and output queue. Linux uses the core epoll poller; other supported platforms use poll/WSAPoll. All database calls run in a global bounded worker pool. A connection has at most one outstanding worker job, preserving command FIFO without a worker thread per client. Reactor completion processing queues acknowledgment before that operation's broadcasts. Welcome is queued before a new connection becomes eligible for broadcasts. Room operations hold one of 256 fixed striped mutexes from execution through completion publication. This keeps each room's commit/history/read order consistent with broadcast publication, preventing a read cursor from skipping an earlier committed but unpublished message. Hash collisions only reduce parallelism between unrelated rooms. Different rooms have no global ordering requirement; use stable IDs to reconcile history.

A peer FIN marks the session input complete, not failed: complete decoded commands continue through the worker FIFO and replies drain before socket close. The socket is removed from read interests at EOF; queued writes use write-only readiness so Linux EPOLLRDHUP cannot cause an idle spin. HUP/RDHUP events are readable rather than fatal, allowing recv to consume bytes preceding FIN; only ERR (or poll NVAL) is fatal. Full disconnect/reset can still prevent reply delivery, while already committed broadcasts remain independent of requester liveness.

Worker completion publication wakes the reactor through one nonblocking loopback TCP pair initialized at startup. TCP_NODELAY prevents wake bytes from waiting for delayed ACK/Nagle interaction. The sender is shared by worker threads; a full write buffer implies unread wake data already exists, so EAGAIN does not lose notification. Completion enqueue precedes wake transmission; the reactor drains wake bytes and then drains completions. The poll timeout is one second for maintenance/signals, not completion dispatch. The wake pair outlives all workers, including shutdown and completion-budget cancellation. This uses the same native socket abstraction on every supported platform instead of platform-specific eventfd/pipe dispatch.

Broadcast serialization/compression runs once per event, not once per recipient. Each recipient queue holds a shared pointer to one immutable wire buffer plus its own write offset. Direct responses use the same queue representation. A slow peer cannot mutate another peer's bytes or advance its offset, and buffers remain alive until the last queued recipient releases them. Existing client/global output budgets still charge wire bytes per logical recipient, intentionally conservatively rather than allowing shared-storage accounting to weaken slow-client limits.

Current explicit limits:

| Resource | Limit |
| --- | --- |
| Concurrent connections | 1024 |
| Worker threads | 1–128; default hardware concurrency clamped to 2–16 |
| Waiting global worker jobs | 1024 |
| Pending commands per client | 64, at most 2 MiB estimated frame/string bytes |
| Global input accounting | 64 MiB across partial decoders, queued commands and submitted/running commands |
| Global queued completions | 64 MiB estimated frame/string bytes; workers wait for reactor drain |
| Pre-authentication incoming wire bytes | 8 KiB total |
| Client queued wire output | 4 MiB |
| Global queued wire output | 64 MiB |
| Read/write work per readiness event | approximately 256 KiB |
| Unauthenticated connection | 10 seconds |
| Idle connection | 10 minutes |
| Tenant public rooms | 42 (fits the protocol's 128-field rooms response) |
| Message body | 1–65536 UTF-8 bytes, no NUL |
| History response | at most requested limit 1–200 and approximately 1 MiB field data |

Slow clients and resource-limit violators are disconnected instead of allowing unbounded reactor memory growth. The history count is an upper bound, not a promise: continue from `history_end`'s returned ID until an empty page. Large room histories therefore do not require large per-client write buffers. No command automatically marks messages as read.

Input charges for running commands survive requester disconnect until completion, so connection churn cannot bypass the global limit. Completion backpressure preserves durable broadcasts instead of dropping already committed results. A detached reactor completion batch can temporarily add another 64 MiB, and executing workers can each hold approximately one history page before admission to the completion queue. These budgets are accounting bounds, not a claim that total process RSS is exactly 64 MiB; object/allocator overhead, memory-mode retained history and encoding buffers remain additional. Shutdown cancels completion admission and skips queued jobs while allowing already executing transactions to finish.

Workers complete before their storage and completion state is destroyed on ordinary SIGINT/SIGTERM shutdown. In-flight transactions finish, but sockets are closed rather than promising response delivery during shutdown; retry sends with the same nonce. PostgreSQL statement and lock timeouts are 10 and 5 seconds. Connection establishment has a 5-second timeout. Durable retries after a lost response must preserve the original nonce.

## PostgreSQL model

At startup the server opens `min(worker_threads,16)` libpq connections. An exclusive lease prevents concurrent use of a `PGconn`; workers otherwise share the pool. A failed connection is reset under its exclusive lease before a later operation, and session encoding/timeouts are reapplied. The operation which first detects a broken idle connection can still return `storage`; clients retry with their original nonce. In-flight operations are never automatically replayed because commit outcome can be unknown. Client encoding is UTF8. All user values are bound through `PQexecParams`, never interpolated SQL. Database failures return generic storage errors instead of exposing SQL, DSN or server diagnostics.

Schema initialization is a transaction protected by a fixed transaction-scoped advisory lock. `scl_schema` records version 1; newer/unsupported versions fail startup. This is the baseline migration rather than a general migration framework. The database role needs CREATE privileges for first start; operational deployments should create/migrate the schema before reducing runtime privileges.

- `scl_rooms`: primary key `(tenant,room)`.
- `scl_messages`: global `bigserial` ID, tenant/room foreign key, author, Unix UTC millisecond timestamp, body and nonce. Unique `(tenant,username,nonce)` enforces user-scoped idempotency across rooms. `(tenant,room,id)` supports ordered keyset history and room queries.
- `scl_reads`: primary key `(tenant,username,room)`, monotonic nonnegative cursor, tenant/room foreign key.

Every statement includes tenant scope; a tenant never discovers another tenant's rooms or content. IDs are globally unique and can have gaps, so clients must not infer counts from ID subtraction. Tenant room creation uses a transaction-scoped advisory lock to enforce capacity across connections. Send transactions first acquire a user-scoped advisory lock and then lock the room row; read transactions lock the room row too. This provides atomic nonce/content conflict detection and keeps validation of read cursors consistent with committed room maximums. Advisory-hash collisions cause harmless extra serialization, not authorization bypass. The unique SQL constraint remains the final idempotency invariant.

Persist succeeds before ACK/broadcast. Exact nonce retries return the original ID without rebroadcast. Reuse with different room/text returns `conflict`. Read updates apply `GREATEST` and reject values above the room's maximum. Unread counts include only messages above that user's cursor written by other authors. History is ascending ID keyset pagination and does not change read state. Tenant rooms are public; `join` creates a room, not a private membership grant.

The in-memory implementation holds one mutex around these same invariants. It intentionally retains messages for the process lifetime and is not intended for sustained unbounded production workloads. PostgreSQL list/unread currently aggregates room history: no incremental counters or retention policy are claimed. Benchmark before large-history deployment, and add rollups/retention only with a compatible semantic contract.

## Completion wakeup latency probe

On 2026-10-19, Windows native GCC 16.2 / Debug CMake build, explicit memory mode, two server worker threads, loopback TCP, one Python client, 10 warm-up pings and 100 sequential measured pings:

| Variant | Median ms | p95 ms | Maximum ms |
| --- | ---: | ---: | ---: |
| Previous 10 ms completion polling | 15.53185 | 16.1628 | 16.5328 |
| Native socket wakeup, first probe | 0.12635 | 0.2218 | 0.3359 |
| Native wakeup, final accounting probe | 0.12275 | 0.2025 | 0.3311 |

The original Windows timer quantization dominated application work. Native wakeup removes that periodic wait (about 123× lower median in the paired probe), without increasing timer resolution or busy-waiting. These are local latency measurements, not a production throughput claim. Reproduce with `python tests/server_latency.py --server PATH_TO_CHAT_SERVER --iterations 100`. The timing probe is informational rather than a flaky CI threshold. Publication/order and pre-auth buffer rejection regressions remain functional tests.

## Encode-once fanout experiment

On 2026-10-19, the same Windows native GCC 16.2 Debug build configuration as above, with the final FIN/accounting behavior and no general TCP_NODELAY change: 16 simultaneous clients, 100 sends per client (1600 total), 256-byte UTF-8 bodies, four server workers, explicit in-memory backend. Each client receives broadcasts while waiting for its ACK. The only A/B source difference is shared immutable encoded buffers versus per-recipient encode. The preserved before executable and source are `.cache/server/broadcast-baseline.exe` and `.cache/server/broadcast-baseline-main.cpp`; raw probe JSONs are `.cache/server/broadcast-{before,after}-{1,2,3}.json`.

| Trial | Before ACK/s | Shared ACK/s | Before send→ACK p50 ms | Shared p50 ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 659.50 | 2604.69 | 24.086 | 5.767 |
| 2 | 647.36 | 2652.47 | 24.639 | 5.492 |
| 3 | 651.64 | 2694.16 | 24.466 | 5.568 |
| Median across trials | 651.64 | 2652.47 | 24.466 | 5.568 |

Median trial throughput improved approximately **4.07×**, with median trial send→ACK p50 approximately **77.2% lower**. The structural cause is eliminating repeated serialization/compression (including LZ hash-table initialization) for identical per-tenant event bytes. No codec or wire format changed. Tests still cover independent per-peer delivery via concurrent publication and FIN draining. These are a local Python-client smoke workload, not maximum production capacity or PostgreSQL throughput results.

Reproduce each trial with `python scripts/benchmark.py --server EXECUTABLE --clients 16 --messages 100 --bytes 256 --threads 4 --output .cache/server/TRIAL.json`. Trials were executed before/after, before/after, before/after without competing benchmarks. The separate unsuccessful general TCP_NODELAY experiment is documented in [performance-network.md](performance-network.md); its reverted changes are absent from this comparison.

## Focused evidence and references

`tests/server_storage.cpp` exercises isolation, exact retry/conflict behavior, own-message exclusion, read monotonicity, future-cursor rejection, 16-way concurrent duplicate sends, ordered history, room capacity, and 400 deterministic generated cursor transitions. `tests/server_publication.py` additionally exercises actual sockets: 20 sends followed immediately by requester FIN still broadcast to a peer; a coalesced 32-send batch followed by FIN receives every ACK and peer broadcast before graceful EOF; 240 concurrent same-room sends arrive at an observer in strictly increasing ID order. This FIN regression passes native Windows WSAPoll, WSL Linux epoll and a forced POSIX poll build. Root integration tests exercise actual processes, sockets and PostgreSQL server-process restart persistence.

Official implementation guidance:

- [libpq threading](https://www.postgresql.org/docs/current/libpq-threading.html): do not concurrently issue commands using one connection object; use exclusive pool leases.
- [Explicit and advisory locks](https://www.postgresql.org/docs/current/explicit-locking.html): transaction-scoped advisory locks release automatically on transaction end; room row locks coordinate sends and read updates.
- [PQexecParams](https://www.postgresql.org/docs/current/libpq-exec.html): pass values separately from SQL command text.
