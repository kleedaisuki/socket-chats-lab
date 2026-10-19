# Independent implementation review

Review date: 2026-10-19. Scope: server reactor/worker ownership, completion publication, PostgreSQL tenant predicates/nonce/read transactions, daemon IPC identity, bounded queues, reconnect and TUI read cursors. Static source review only; root owns Windows/WSL builds and end-to-end execution. Line numbers below refer to the snapshot reviewed and may move during fixes.

## Findings

### R1 — P1: A committed send loses its broadcast when its initiating connection closes

**Location:** `src/server/main.cpp:351-355`, `Server::completions()`.

The early `if (s->closed) continue` discards both request replies and domain broadcasts. A worker can commit a send while the reactor closes its originating socket (EOF, disconnect, idle expiry, or output overload); its completion is subsequently ignored. Every other healthy peer misses that persisted message indefinitely unless it explicitly queries history. Read receipt publications have the same lifecycle coupling.

**Correction:** Publish committed domain events regardless of requester liveness. Restrict the closed-session check to queuing replies, authentication publication, draining and scheduling another command. Avoid tying durable publication to an ephemeral socket.

**Confidence:** High; direct executable path, no timing reproduction performed. Suggested targeted test: block the worker just after storage commit, disconnect sender, unblock completion, assert an independent authenticated recipient receives the committed message once.

### R2 — P2: Room commit order is not completion/publication order, so the displayed maximum is not a safe read cursor

**Locations:** `src/server/main.cpp:335-337` (execute then completion enqueue), `src/server/postgres.cpp:220-230` (room lock / insert / commit), `src/client/tui.cpp:149,170` (read maximum displayed message ID).

A permitted scheduling trace: worker A commits room message ID 1 and is descheduled before enqueuing its completion; worker B acquires the now-released room lock, commits ID 2 and queues its completion first. Recipient sees ID 2 and `/read` submits 2. Storage legitimately accepts that cursor, although ID 1 has not appeared in the UI. Thus ID 1 becomes read without foreground display. The same publication inversion is possible with the memory backend after its storage mutex is released. Per-session FIFO does not serialize different senders.

The PostgreSQL room lock already prevents the separate lower-ID-late-commit history anomaly; do not remove it. The missing property is an ordered room event publication boundary. A coherent remedy should order publication with persistence or establish a synchronization watermark the client can distinguish from an arbitrary live event ID. Another local `max()` guard cannot prove completeness.

**Confidence:** High on the legal interleaving; deterministic scheduling test not yet run. Suggested test: hold ID 1 between commit and enqueue, publish ID 2, verify the chosen publication/read-cursor contract before unblocking ID 1.

### R3 — P2: PostgreSQL connection loss permanently poisons the pool

**Locations:** `src/server/postgres.cpp:12-35` (`query`), `:58-74` (`Lease` acquisition/return).

Every failed connection is returned unchanged to `idle_`; connection establishment occurs only in the constructor. After PostgreSQL restart or terminated database sessions, subsequent commands repeatedly reuse dead `PGconn` objects. Daemon TCP reconnect cannot recover storage, and even a healthy restarted database requires restarting chat-server.

**Correction:** When leasing a bad connection, reset/recreate it with bounded retry/backoff and reapply connection-local UTF-8/timeouts. Do not automatically replay an uncertain transaction commit; return an error and let the existing nonce contract resolve a retried send. Preserve a usable pool after a transient outage.

**Confidence:** High. PostgreSQL documents explicit `PQreset` / `PQresetStart` as the recovery mechanism for a lost working connection: https://www.postgresql.org/docs/current/libpq-connect.html#LIBPQ-PQRESET . No database restart experiment performed in this review.

## Resolved during review

- **R0 — P1: All broadcasts initially suppressed.** `Session::welcomed` was initialized false and never assigned, while broadcast targets required it. Reported immediately to root. The subsequent snapshot sets it after queuing a welcome in `Server::completions()` (`src/server/main.cpp:356-359`). Static inspection confirms that specific missing assignment is corrected; end-to-end coverage remains owned by root.

## Reviewed strengths and limits

- Commands are serialized per session by reactor-owned `running` plus a pending FIFO; worker-produced completions publish through a mutex.
- Tenant/user identity derives from server authentication, not command payload. Examined storage queries consistently constrain tenant; no concrete cross-tenant SQL data leak found.
- Nonce uniqueness includes tenant/user; PostgreSQL advisory lock serializes that user's retries before duplicate lookup and payload conflict checking. Room row locks serialize read maxima with sends. Read cursors use `GREATEST`, not last-writer-wins.
- Reactor input/output admission is finite and includes per-turn I/O budgets. Daemon peer outputs and pending map are bounded. Bounds are not a throughput or latency benchmark.
- IPC binds loopback and authenticates a separate random token; upstream authentication retains the server token. State directory account identity and single-writer lock prevent accidental local account sharing. POSIX private directory protects journal; Windows ACL inheritance remains the documented deployment caveat.
- Reconnect explicitly labels offline state and does not silently queue uncertain sends. Missed-message synchronization is manual and clearly announced, not claimed automatic.
- No production source was edited by this reviewer. This review does not certify all protocol fuzz cases, Windows ACL behavior, network partitions, crash durability, or capacity.

## Follow-up inspection of server fixes

A subsequent source snapshot removes the closed-origin early return from `Server::completions()` and leaves `queue()` / `schedule()` to reject closed recipients. **R1 is resolved by static inspection.** Broadcast processing is no longer conditional on requester liveness.

The same snapshot adds `std::array<std::mutex, 256> room_locks_` and `publication_lock()` (`src/server/main.cpp:325-335`). Authenticated send/read/history/join operations acquire the stripe keyed by tenant plus room before execution and keep it through `completed_.push_back` (`:349-354`). Tenant identifiers cannot contain the colon separator, so valid domain keys are unambiguous. Completion FIFO and reactor processing preserve that publication order. The reactor never acquires a room stripe. Member destruction keeps pool teardown ahead of mutex teardown. **R2 is resolved for operations originating in this single server process by static inspection.** PostgreSQL room row locks remain necessary for storage correctness and were retained. Multiple chat-server processes still do not provide shared real-time fanout; that is outside the stated single-reactor service boundary.

Stripe collisions can serialize otherwise independent rooms, and waiting hot-room jobs occupy shared workers. These are explicit throughput/fairness trade-offs rather than a new correctness finding; capacity claims require workload measurements. No new serious client defect was found in the additional pass through IPC drop lifetimes, multiple TUI request routing, authentication, heartbeat/reconnect, journal identity, and terminal control sanitation.

## Root integration follow-up

R3 is now resolved: an exclusively acquired bad PostgreSQL connection is reset before new work, UTF-8 and connection-local timeouts are reapplied, and in-flight writes are never automatically replayed. Root executed `tests/postgres_recovery.py` against a new dedicated PostgreSQL 18 database: all chat pool sessions were terminated, the same running chat-server recovered, an existing nonce returned its original message ID, and a new send succeeded. The cluster and unrelated databases were not stopped.

Windows state security was subsequently upgraded from inherited ACL reliance to an explicit protected current-user-only DACL. The Windows client process suite starts with an Everyone-accessible fixture and verifies every sensitive object's resulting DACL. Historical findings above describe the original reviewed snapshots, not outstanding defects.
