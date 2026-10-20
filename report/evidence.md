# Report evidence ledger

This ledger connects the report's argument to implementation, retained measurements,
and primary literature. Reviewed against the working tree on 2026-10-21. Paths are
repository-relative; generated evidence remains in `.cache/`.

## Narrative and design decisions

The useful systems contribution is the composition of ownership boundaries:
socket ownership, database commit, ordered publication, persistent account state,
and foreground-visible read state. The report should follow this chain rather
than enumerate commands. The three-process design deliberately separates a
terminal's short lifetime from an account's ongoing network/cache lifetime.

| Decision | Concrete mechanism | Trade-off and useful implication | Evidence |
| --- | --- | --- | --- |
| Keep sockets single-owned | Server reactor owns decoder, pending FIFO, readiness and output; workers produce `Completion` values | Blocking SQL cannot freeze socket readiness; completion/wakeup becomes an explicit correctness boundary | `src/server/main.cpp`, `schedule`, `completions`, `readable`, `writable` |
| Serialize a session, not the whole service | `running` admits at most one job from each session; global pool queue caps waiting jobs at 1024 | Preserves command order across blocking work without a connection-per-thread design | `src/server/main.cpp`, `Pool::submit`, `schedule` |
| Align room order across SQL and publication | 256 striped room mutexes cover `execute` through `completed_.push_back`; SQL `FOR UPDATE` locks room before message insertion | Same-room commit and completion enqueue orders agree; collisions and hot-room waiters reduce parallelism | `src/server/main.cpp:publication_lock`, `schedule`; `src/server/postgres.cpp:lock_room`, `send`, `read` |
| Publish a committed event independently of its requesting socket | `completions` processes broadcasts even if response `queue` ignores a closed sender | A peer FIN does not erase an already committed event | `src/server/main.cpp:completions`; `tests/server_publication.py` |
| Use a durable idempotency key | SQL unique `(tenant,username,nonce)`; user advisory lock; exact room/text comparison | Lost replies can be resolved with the original nonce; changed content is a conflict, not a second message | `src/server/postgres.cpp:send`; `tests/server_storage.cpp` |
| Treat IDs as ordered keys, not counts | Room-scoped `id > cursor ORDER BY id`; own-message exclusion in unread query | Global sequence gaps and other rooms do not inflate unread counts | `src/server/postgres.cpp:history`, `list` |
| Reuse an immutable fanout representation | `shared_ptr<const vector<byte>>`, recipient-owned write offsets | Serialization/compression occurs once per broadcast, while output admission still charges each logical recipient | `src/server/main.cpp:WireBuffer`, `queue_encoded`, `completions`, `writable` |
| Separate account and terminal lifetimes | Daemon maintains upstream connection, authenticated loopback IPC, JSONL journal, request-ID remapping | Terminal can close/reopen without losing incoming account events; local permissions and cache identity become important contracts | `src/client/daemon.cpp`; `docs/client.md` |
| Make read an explicit semantic action | `/read` / Ctrl+R use visible foreground layout; daemon receipt/cache replay is not read | Connectivity and caching cannot impersonate attention; history synchronization and visibility need separate cursors | `src/client/tui.cpp`, `view.cpp`; `docs/terminal-validation.md` |
| Adopt a mature non-network component | Pinned FTXUI 7.0.3 through FetchContent; bounded native socket IO thread posts UI events | Removes a home-grown terminal event engine while preserving networking/wire/CLI contracts | `cmake/dependencies.cmake`, `scripts/patch_ftxui.py`, `src/client/session.cpp`, `tui.cpp` |

### Precise ordering language

The room stripe is held until the worker enqueues the completion and issues its
wakeup, not until bytes reach a client. The completion deque and reactor output
queues preserve that enqueue order. SQL room locks separately prevent a lower-ID
message committing after a higher-ID message in the same room. These are two
distinct mechanisms: a database transaction alone cannot order worker scheduling
after commit. History/join/read also pass through the room stripe. Different
rooms need no global publication order. Live fanout is scoped to one server process.

`Postgres::send` commits before returning `{message,true}`; only then does
`Server::execute` construct its ACK/broadcast. Exact retries return
`{original,false}` and generate no second broadcast. `Postgres::read` locks the
room, rejects a future cursor, applies `GREATEST(existing,requested)`, and commits
before returning. These mechanisms substantiate the report's stated invariants.

### Resource model

The server separately budgets logical input, queued completion data and queued
wire output at 64 MiB each; per-client output is 4 MiB. This is a pipeline
admission policy, not a process-RSS formula. Shared immutable buffers reduce
physical copies without relaxing recipient-specific slow-peer accounting.
An executing history operation and a detached completion batch can temporarily
hold additional data. Relevant code: `account_input`, `queue_encoded`,
`schedule`, `completions`; full limits in `docs/storage.md`.

## Development organization: lessons supported by the project record

1. **Contract before parallel ownership.** `docs/architecture.md` declares wire
   types, APIs, tenant semantics, memory bounds and module owners. Core, server,
   client and integration work can proceed concurrently against explicit shared
   interfaces. The value is semantic independence, not a claim that more agents
   automatically improve quality.
2. **Independent review across module boundaries.** `docs/review.md` identifies
   a requester-lifetime/publication coupling and the commit-to-completion order
   gap. Both are composition failures invisible in a storage-only correctness
   argument. The fixes become real-socket FIN and concurrent-publication tests.
3. **User friction changes the unit of validation.** The supplied report of
   character-by-character Windows delay is accepted as workload evidence.
   `--plain` success did not answer it. `docs/tui-redesign.md` records the change
   from a custom terminal engine proposal to pinned FTXUI, with native ConPTY
   acceptance rather than editor-model-only testing.
4. **Experiments eliminate work as well as justify it.** Same-source controls
   rejected broad TCP_NODELAY tuning. The next optimization removes repeated
   compression work structurally, without changing the protocol.
5. **Test the observer.** The ConPTY host must honor Win32 input negotiation,
   UTF-16 key records and key-up events. The macOS PTY host must drain output
   continuously and sample restored modes before controlling-session hangup.
   These corrections are test infrastructure, not weakened product assertions.

Useful document sequence: `architecture.md` -> `review.md` -> `storage.md` ->
`performance-network.md` -> `tui-redesign.md` -> `interactive-latency.md` ->
`terminal-validation.md` -> `validation.md`. Git history provides revision order;
these design documents provide the rationale and experiment context.

## Measurements read directly from retained JSON

### Encode-once fanout

Raw files: `.cache/server/broadcast-{before,after}-{1,2,3}.json`.
Windows 11 build 26200, Python 3.14.6; native GCC Debug; explicit memory backend;
4 workers, 16 authenticated clients, 100 sends/client, 1600 total messages,
256 UTF-8 bytes/message, same-room tenant-wide fanout, one outstanding send/client.
The mostly repeated text is compressible. Python decoding and scheduling are
included in end-to-end results. ACK/s includes authentication/thread startup.

| Pair | Before ACK/s | After ACK/s | Before p50 ms | After p50 ms | Before p95 ms | After p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 659.502980 | 2604.686417 | 24.085850 | 5.767400 | 26.950300 | 8.233200 |
| 2 | 647.362689 | 2652.465367 | 24.638750 | 5.492150 | 26.564800 | 9.672000 |
| 3 | 651.635977 | 2694.161483 | 24.466100 | 5.567850 | 26.420300 | 10.036700 |

Median trial throughput: 651.635977 -> 2652.465367 ACK/s (about 4.07x).
Median of trial p50 values: 24.466100 -> 5.567850 ms (about 77.2% lower).
This summary is a median across trials, not a percentile of pooled samples.
Retained baseline source/executable:
`.cache/server/broadcast-baseline-main.cpp`, `broadcast-baseline.exe`.
Current implementation confirms encoding once before the recipient loop.

Reproduce a trial:

```console
python scripts/benchmark.py --server EXECUTABLE --clients 16 --messages 100 --bytes 256 --threads 4 --output .cache/server/TRIAL.json
```

### Native Windows interactive input

Raw files: `.cache/tui-baseline/conpty-authoritative/result.json` and
`.cache/tui-current/final-color/result.json`. Same ConPTY 140x36 workload,
3 rounds, MSVC Debug clients; unchanged server/daemon. ASCII single keys have
78 observations; ASCII bursts contain 108 codepoints; Unicode bursts contain
76 codepoints / 240 UTF-8 bytes. Independent Bob checks exact delivered bodies.

| Quantity | Original | FTXUI |
| --- | ---: | ---: |
| Single-key p50 ms | 30.844450 | 30.641500 |
| Single-key p95 ms | 32.085000 | 31.655500 |
| ASCII burst run 1 / 2 / 3 ms | 3446.936800 / 3392.855200 / 3410.650400 | 26.816500 / 29.107400 / 23.720600 |
| Unicode burst run 1 / 2 / 3 ms | 2410.522800 / 2372.430200 / 2448.607300 | 27.883000 / 42.239500 / 28.062400 |
| Exact server-observed common workload messages | 9 | 9 (13 including additional Unicode/edit tests) |
| Final probe status | baseline_failure_reproduced | passed |

The report's causal result is removal of per-character serialization: the old
loop waits on socket polling before consuming one `_getwch()` key; the new loop
drains native input as a batch and has independent socket IO. Individual-key
latency remains near ConPTY's output scheduling interval. This explains the
127x ASCII / 86x Unicode burst improvements without conflating them with key
latency or physical keyboard/IME display latency.

Retained client SHA-256:

- Original: `75768f37798a66ca6a2ef6a8677baa320879a6b4d23f60181683f4f3da4e4097`.
- FTXUI: `c54a6c52dfbf9d2616632557e01b49c15ebb8876bc81fdf55a31288109710ca2`.

The second value and all server/daemon/harness hashes occur in the current JSON.
`docs/interactive-latency.md` provides full commands and final history-stress
acceptance. Real showcase captures live under
`.cache/tui-current/showcase-{120x32,40x12}/conversation.{vt,screen.txt,svg,png}`.

### Completion wakeup

`docs/storage.md` records the Windows 100-ping sequential probe: median
15.53185 ms for periodic completion polling versus 0.12635 ms for native socket
wakeup (first paired probe); final accounting probe median 0.12275 ms, p95
0.2025 ms. Use this as a mechanism illustration with the document's provenance,
not as newly rerun raw data. Reproduction: `python tests/server_latency.py
--server EXECUTABLE --iterations 100`. The production wakeup is a nonblocking
loopback TCP pair; completion enqueue precedes notify, and an already full
sender implies unread wake data. The reactor never waits for its maintenance
timeout to discover completed ordinary work.

### Rejected socket tuning

`.cache/benchmark-{nagle,nodelay,reverted}-{1,2,3}.json` and
`docs/performance-network.md` retain the full controls. Current-source TCP_NODELAY
p50s: 25.121, 24.549, 24.638 ms; current-source reverted p50s: 24.945, 24.347,
24.417 ms. A faster preserved earlier binary was confounded by other source
differences and was not the causal control. Broad tuning was removed. The
report can make this a concise example of hypothesis rejection and selecting
the next informative intervention.

## End-to-end and property evidence

The retained hosted logs `.cache/ci/tui-accepted-{linux,windows,macos,postgres,
sanitizers}.log` were inspected and each contains all 13 suites passed; postgres
contains two successful 13-suite runs (database-enabled and forced poll).
The hosted code identity is `eb24fa738f5eccf7785db6fb940a09f004171609`,
[Actions run 39286126175](https://github.com/kleedaisuki/socket-chats-lab/actions/runs/39286126175).
These are execution results for that revision, not implied results for every
later tree. Suite scope and dedicated-database cleanup are in `docs/validation.md`.

Seeded properties in current code: `tests/core_tests.cpp` uses mt19937 seed 42,
600 round-trip/fragmentation cases and 1000 mutations;
`tests/server_storage.cpp` uses seed 74821 for 400 cursor transitions.
`tests/server_publication.py` checks real socket FIN draining, a 32-command
coalesced batch with all ACKs/events before EOF, and 240 concurrent same-room
events. Terminal journeys check independent peer payloads, actual PTY/ConPTY
input, terminal restoration, offline drafts, history navigation and visibility-
bound read semantics. Unit/property tests and process journeys are complementary:
the first explores representation space, the second crosses ownership boundaries.

## Primary sources and their role in the argument

All links below were inspected or resolved on 2026-10-21. Keep related-work
discussion proportional: explain mechanisms that guided decisions rather than
presenting the lab as a new distributed consensus or scheduling system.

### SEDA

Matt Welsh, David Culler, and Eric Brewer. 2001. *SEDA: An Architecture for
Well-Conditioned, Scalable Internet Services*. SOSP '01, 230--243.
Proceedings DOI: `10.1145/502034.502057` (journal-proceedings record also uses
`10.1145/502059.502057`).
[Author-hosted paper](https://people.eecs.berkeley.edu/~prabal/teaching/resources/eecs582/welsh01seda.pdf),
[author publication record](https://people.eecs.berkeley.edu/~brewer/papers/).
The inspected paper explains explicit event queues, stage resource control and
load conditioning. Our reactor/shared pool borrows queue visibility and bounded
admission; it does not reproduce SEDA's dynamically controlled stage network.

### Property-based testing

Koen Claessen and John Hughes. 2000. *QuickCheck: A Lightweight Tool for Random
Testing of Haskell Programs*. ICFP '00, 268--279.
DOI: `10.1145/351240.351266`.
[Paper text](https://www.cis.upenn.edu/~bcpierce/courses/552-2008/resources/icfp-quickcheck.pdf).
The paper motivates executable properties and controlled generators; this
supports the byte-stream decomposition/round-trip and cursor-state properties.
The repository uses native seeded C++/Python tests rather than the QuickCheck
library. Mentioning that distinction is sufficient; no framework comparison is
needed.

### Operational amplification and retry scope

Lexiang Huang, Matthew Magnusson, Abishek Bangalore Muralikrishna, Salman
Estyak, Rebecca Isaacs, Abutalib Aghayev, Timothy Zhu, and Aleksey Charapko.
2022. *Metastable Failures in the Wild*. OSDI '22, 73--90. USENIX Association.
[Publisher page and authoritative BibTeX](https://www.usenix.org/conference/osdi22/presentation/huang-lexiang).
The study connects incident triggers to sustaining work amplification. For this
project the useful connection is bounded admission and avoiding automatic
replay of uncertain writes: reconnect is a transport policy, while idempotent
operation recovery belongs to the nonce/storage contract. This is an engineering
interpretation, not an experiment showing metastability in the lab.

### Platform and database mechanisms

- [PostgreSQL 18 sequence functions](https://www.postgresql.org/docs/18/functions-sequence.html):
  atomic unique allocation, gaps after abort, and commit before external use.
  Supports the distinction between identifier allocation and committed order.
- [PostgreSQL 18 explicit locks](https://www.postgresql.org/docs/18/explicit-locking.html):
  room row locking and transaction-scoped advisory locks.
- [PostgreSQL libpq threading](https://www.postgresql.org/docs/18/libpq-threading.html):
  exclusive connection leases, not concurrent operations on one PGconn.
- [Linux epoll manual](https://man7.org/linux/man-pages/man7/epoll.7.html):
  readiness modes and ET drain requirements; level-triggered operation avoids
  unnecessary rearm/drain-state machinery for this workload.
- [Microsoft pseudoconsole sessions](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session):
  pipe/STARTUPINFOEX setup and synchronous pipe deadlock concerns.
- [Microsoft Terminal native input source](https://github.com/microsoft/terminal/blob/main/src/terminal/input/terminalInput.cpp):
  Win32 input encoding used by the corrected ConPTY observer.

## Recommended report shape

Main text: workload and thesis -> ownership architecture -> commit/publication/
visibility design -> bounded representations and lifecycle trade-offs -> measured
redesigns -> contract-first parallel development and cross-boundary review ->
discussion of practical extensions. Place full protocol tables, commands,
machine/toolchain identities, Lean model/proofs, symbolic calculations and
evidence inventory in appendices. Use the paired fanout and native-input tables
as the central quantitative results, with one sentence defining the workload in
the main text and complete reproduction instructions in the appendix.
