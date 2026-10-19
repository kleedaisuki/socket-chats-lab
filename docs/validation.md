# Executed validation record

Date: 2026-10-19. Repository started with only the license and an initial Git
commit. All generated artifacts were kept under `.cache/` or `.temp/`.

## Final native builds and application tests

| Configuration | Executed result | Representative elapsed time |
| --- | --- | ---: |
| Windows 11, MinGW GCC 16.2, Debug, WSAPoll, memory backend | 8/8 CTest suites passed | 6.21 s |
| Windows 11, MSVC 19.51 / Visual Studio 18, Debug, WSAPoll, memory backend | 8/8 passed | 10.03 s |
| WSL2 Ubuntu 24.04, GCC 13.3, Debug, Linux epoll, PostgreSQL-enabled binary | 9/9 passed | 8.15 s |
| WSL2 GCC 13.3, Debug, CHAT_FORCE_POLL=ON | 9/9 passed | 8.22 s |
| WSL2 GCC 13.3, Debug, ASan + UBSan, memory backend | 9/9 passed | 12.72 s |

The additional POSIX suite is a real PTY journey, not an assumption that piped
input exercises terminal modes. The suite names are `ops`, `cli`,
`journey_memory`, `server_publication`, `terminal_journey` (POSIX only), `core`,
`server_storage`, `client_cache`, and `client_reconnect`.

Commands (substitute the matching build directory):

```console
cmake -S . -B .cache/build/windows -G Ninja -DCHAT_WITH_POSTGRES=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build .cache/build/windows --parallel 6
ctest --test-dir .cache/build/windows --output-on-failure
cmake -S . -B .cache/build/msvc -G "Visual Studio 18 2026" -A x64 -DCHAT_WITH_POSTGRES=OFF
cmake --build .cache/build/msvc --config Debug --parallel 6
ctest --test-dir .cache/build/msvc -C Debug --output-on-failure
```

WSL uses the same checkout on its DrvFS mount and separate
`.cache/build/linux`, `poll`, and `sanitize` trees. Linux build adds
`CHAT_WITH_POSTGRES=ON`; poll adds `CHAT_FORCE_POLL=ON`; sanitizer adds
`CHAT_SANITIZERS=ON` and disables the optional database dependency.

## What the suites actually check

- Core: 600 seeded frame vectors, compressed/uncompressed round trips, arbitrary
  fragmentation/coalescing, exact 1 MiB boundary, 1000 mutated streams, malformed
  LZ/length/flags/UTF-8 rejection, terminal-safe controls, real loopback sockets
  and readiness/timeouts.
- Storage: isolation, nonce/content conflicts, concurrent duplicate sends,
  ordering, read monotonicity/future rejection, own-message unread exclusion,
  room capacity and 400 seeded cursor transitions.
- Process journeys: tenant authentication/isolation, real compressed Chinese and
  emoji messages, timestamps, history paging, two daemon IPC clients with
  deliberately colliding request IDs, persistent JSONL, real plain-TUI commands.
- Publication: 20 sender FIN cases, a 32-command coalesced batch followed by FIN
  with all ACKs/events before EOF, 240 concurrent ordered room events, pre-auth
  byte-budget rejection.
- Daemon: same-IPC reconnect after server loss, cached offline view, explicit-only
  read receipts, writer exclusion, account identity rejection, terminal injection
  safety, and Windows DACL migration from an Everyone-accessible fixture to the
  current user only.
- PTY: Chinese/emoji input and codepoint backspace verified by another actual
  peer, asynchronous receive during typing, `/rooms` and `/read`, OSC/BEL
  neutralization, zero idle redraw, Ctrl-C/Ctrl-D/quit/exception terminal restore.
- Ops: 1000 seeded PostgreSQL connection-string quoting cases, password removal
  from psql arguments, malformed input rejection. CLI help/errors require no
  credentials and leave no account state behind.

## PostgreSQL execution

Executed using PostgreSQL/libpq 18.6 in WSL, with new randomly named dedicated
roles and UTF-8 databases on the existing local cluster. No unrelated database
was stopped, reset or migrated.

`tests/journey.py --postgres` passed, including exact equality of all returned
message fields and room/unread metadata before and after a chat-server restart.
`tests/postgres_recovery.py` then terminated only that dedicated database/user's
chat pool sessions. The running server recovered without restart; retrying an
existing nonce returned the original ID, and a new write succeeded.

The actual `scripts/database.py` provisioner also created an independent
non-superuser role/database and sensitive connection configuration; its output
was used for another real PostgreSQL journey. All fixture databases, roles,
temporary elevated fixture privileges and generated credentials were removed
after testing. The application role needs no superuser privilege.

On the project's WSL DrvFS mount, chmod is ignored. A separate filesystem probe
detects this and reports permission-bit assertions as unavailable rather than
claiming POSIX protection. Native Windows's explicit DACL checks passed. Linux
CI's ordinary local filesystem runs the strict POSIX permission assertions.

## Installation, launcher and source hygiene

- MSVC `cmake --install ... --prefix .cache/install/msvc` succeeded; installed
  `chat --help` and `chat-daemon --help` both succeeded.
- Actual `python scripts/dev.py demo --portable --build-dir .cache/build/windows`
  started a server and two daemons, printed Bob's attach command, attached Alice,
  and exited cleanly with piped `/quit`.
- `python -m compileall -q scripts tests`, `.clang-format --dry-run --Werror`
  across C++ sources, and `git diff --check` passed.
- Credentials, local caches and build directories are ignored. No generated
  artifacts were placed outside the repository.

## Performance evidence and rejected experiments

The measured completion-wakeup fix removed Windows's approximately 15.5 ms
periodic completion delay; root's repeated single-client check measured a
0.163 ms median / 0.208 ms p95. Encoding each broadcast once and sharing immutable
wire buffers improved median throughput across three matched Debug trials from
651.64 to 2652.47 ACK/s and reduced median send/ack latency from 24.466 to
5.568 ms (16 clients, 1600 total messages, 256-byte bodies, same-room fanout,
4 workers, Windows loopback).

These are local workload measurements with a Python peer driver, not production
capacity or release-build claims. `docs/storage.md` retains the paired trial
values and reproducible commands. `docs/performance-network.md` records the
TCP_NODELAY investigation: same-source controls found no benefit, so the tuning
was removed rather than keeping an unsupported optimization. A historical
binary comparison was explicitly treated as confounded, not causal evidence.

## Hosted GitHub Actions validation

`.github/workflows/ci.yml` defines five cloud jobs: native Linux, Windows, macOS;
Linux PostgreSQL/poll; Linux ASan/UBSan. Workflows run on push, pull request, or
manual dispatch, with read-only repository permissions.

The application was committed and pushed to the independent branch
`codex/native-socket-chat`; `main` was not modified. Hosted
[run 39267732139](https://github.com/kleedaisuki/socket-chats-lab/actions/runs/39267732139)
completed successfully for code commit
`c0319c8e0dc7148b9f4d4214593d56105cc3f7c0` on 2026-10-19.

| Hosted job | Actual compiler/image | Result |
| --- | --- | --- |
| Native Linux | GCC 13.3, Ubuntu 24.04 | 9/9 CTest suites passed |
| Native Windows | MSVC 19.51, Windows 2025 / VS 2026 | 8/8 passed, including native ACL migration and reconnect |
| Native macOS | AppleClang 21, macOS 26 ARM64 | 9/9 passed, including real PTY, socket and publication journeys |
| Linux PostgreSQL and poll | GCC 13.3, Ubuntu 24.04, PostgreSQL 18 service | 9/9 PostgreSQL-enabled suites, both explicit persistence/recovery journeys, then 9/9 forced-poll suites passed |
| Linux ASan and UBSan | GCC 13.3, Ubuntu 24.04 | 9/9 instrumented suites passed |

The workflow conclusion and every one of its five job conclusions were
`success`. This is hosted execution evidence, not inferred from a checked-in
workflow or successful local builds. Job logs were retained under `.cache/ci/`
for this checkout; canonical logs are available from the linked Actions run.

### Windows runner-specific test failure and correction

The [first hosted run](https://github.com/kleedaisuki/socket-chats-lab/actions/runs/39267284479)
passed all non-Windows jobs but failed the Windows `client_reconnect` privacy
assertion. The runner's current Administrator SID serialized in the security
descriptor as `LA`; the test compared that display alias with the full token SID
string and incorrectly rejected an owner-only DACL (`D:PAI(A;OICI;FA;;;LA)`).

The fix inspects native ACL entries with `GetAce` and compares binary trustee
SIDs with `EqualSid`, rather than parsing formatted identity text. Null/empty
DACLs, non-allow entries and every other trustee are still rejected; protected
directory inheritance is still required. The original Everyone-accessible
migration fixture is retained. Local MinGW and MSVC reconnect journeys passed,
and an additional negative probe confirmed an Everyone-accessible directory is
still rejected. The subsequent hosted Windows runner passed the same full suite
under its aliased account; no production permissions were weakened.

See Microsoft's [ACCESS_ALLOWED_ACE layout](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-access_allowed_ace)
and [SID aliases](https://learn.microsoft.com/en-us/windows/win32/secauthz/sid-strings)
for the binary trustee layout and the `LA` representation.

Interactive Windows glyph appearance was not screenshot-verified. Unicode
grapheme shaping remains explicitly out of scope; hosted macOS PTY execution
does not imply a complete terminal-emulator or grapheme-editing implementation.


## FTXUI redesign: additional local validation (2026-10-19)

The initial-build and hosted results above describe the original client. After
replacing its interactive terminal path with pinned FTXUI 7.0.3 and independent
native socket IO, the following full local suites passed:

| Configuration | Executed result | Elapsed time |
| --- | --- | ---: |
| Windows MSVC Debug | 13/13 | 18.24 s |
| Windows MinGW Debug | 13/13 | 11.80 s |
| WSL2 Linux Debug, PostgreSQL-enabled build | 13/13 | 38.11 s |

Windows includes native ConPTY server-daemon-client input/edit/send journeys and
native console surrogate/repeat/mode restoration fixtures. POSIX includes real
PTY editing, resize, offline-draft and reconnect journeys plus scripted history
and delivery-state regressions. Shared suites cover bounded asynchronous socket
queues and property-style conversation rendering tests. The expanded POSIX state
journey checks a full 500-message window moving backward and returning to latest,
and indeterminate delivery without suggesting a duplicate-producing resend.

See `docs/interactive-latency.md` for same-host original/new native input timing
and `docs/terminal-validation.md` for precise behavioral assertions and limits.
No visible user applications were restarted for these isolated fixtures. The
hybrid application's PostgreSQL cluster and existing account data were retained.
