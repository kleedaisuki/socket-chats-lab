# Socket Chats Lab

A C++23 native-socket chat application: PostgreSQL server, per-account client
daemon, and UTF-8 color terminal client. No third-party networking framework,
JSON package, terminal toolkit, or compression library is used. libpq is the
optional build-time PostgreSQL database client; PostgreSQL is the default runtime
backend. The explicit memory backend is only for local demos and tests.

```text
 chat TUI ─┐                      ┌─ bounded shared worker pool ─ PostgreSQL
           ├─ authenticated IPC ─ chat-daemon ─ TCP/SCL1 ─ server reactor
 chat TUI ─┘       loopback          │                     epoll / poll / WSAPoll
                                 events.jsonl
```

## Quick start: local two-user demo

Requires CMake 3.25+, Ninja, a C++23 compiler, and Python 3.10+. Tested locally
with Windows MinGW GCC 16 and Ubuntu WSL GCC 13. The GitHub Actions matrix also
builds Windows MSVC and macOS's native compiler.

From the repository root:

```console
python scripts/dev.py build --portable
python scripts/dev.py test --portable
python scripts/dev.py demo --portable
```

The demo starts the server and **two account daemons**, attaches Alice here, and
prints the command for Bob in a second terminal. Both users belong to tenant
`lab`. Type a message, then try `/rooms`, `/history`, `/read`, `/join research`,
and `/quit`. The server's memory data disappears when the demo stops; the local
cache is explicitly labeled cached, never presented as authoritative live state.

On Windows, use Windows Terminal with a Unicode font. MinGW users need its `bin`
directory on PATH (for the compiler and runtime DLLs). Visual Studio users may
use a Developer PowerShell or configure a Visual Studio CMake generator directly:

```console
cmake -S . -B .cache/build/vs -DCHAT_WITH_POSTGRES=OFF
cmake --build .cache/build/vs --config Debug --parallel
ctest --test-dir .cache/build/vs -C Debug --output-on-failure
```

WSL source checkouts on `/mnt/c` or `/mnt/d` without mount metadata ignore POSIX
chmod. The daemon warns rather than promising permissions that do not exist.
Enable WSL mount metadata or use host ACLs to protect these files. Native Windows
daemons enforce a protected current-user-only DACL on their state. All generated
project files stay in this repository's `.cache/` or `.temp/` directories.

## PostgreSQL-backed use

### Windows clients with a WSL backend

For the local split deployment, `scripts/hybrid.py` uses the existing native
MSVC build in `.cache/build/msvc` and Linux build in `.cache/build/linux`, plus
an already-running WSL PostgreSQL instance. Run it with **Windows Python**:

```console
python scripts/hybrid.py start
python scripts/hybrid.py open
python scripts/hybrid.py status
```

`open` creates an Alice/Bob Windows Terminal split. The server listens on WSL
loopback port 7777; Windows account daemons connect through localhost forwarding.
The launcher creates a dedicated `socket_chat_hybrid` database/role, refuses to
take over an existing database without its configuration, and keeps credentials,
logs and account state under a protected `.cache/hybrid/` directory.

```console
python scripts/hybrid.py verify
python scripts/hybrid.py attach --user alice
python scripts/hybrid.py restart-server
python scripts/hybrid.py stop
```

`verify` briefly interrupts only this chat server to test real native Windows
TUI/daemon reconnect and PostgreSQL persistence. It leaves the stack running.
`stop` stops the application processes, **not PostgreSQL**, and retains data.
See [live hybrid validation](docs/hybrid-validation.md) for evidence and limits.

### Database setup for other deployments

Install your operating system's libpq development package and PostgreSQL client
tools. For example, on Ubuntu: `sudo apt install g++ cmake ninja-build libpq-dev
postgresql-client`. Run PostgreSQL separately, with a UTF-8 database owned by a
dedicated chat role. The server transactionally initializes its versioned tables.
It does not need PostgreSQL superuser privileges.

```console
python scripts/dev.py build
```

Optional provisioning: set `CHAT_ADMIN_DSN` to an administrator connection and run
`python scripts/database.py`. It creates a new isolated role/database, never takes
over an existing one, and writes a sensitive DSN configuration under
`.cache/database/`. Read that configuration's `dsn` value into `CHAT_DB_DSN`.
Alternatively, provision the database using your existing deployment tooling.
The provisioner requests POSIX private modes; on Windows its output inherits the
directory ACL, so restrict `.cache/database/` to your account before production
use. This separate provisioning file is not daemon state and is not covered by
the daemon's native DACL hardening.

PowerShell example, using environment variables rather than token CLI arguments:

```powershell
$env:CHAT_DB_DSN = 'host=127.0.0.1 dbname=chat user=chat password=YOUR_DB_PASSWORD'
$env:CHAT_CREDENTIALS = 'alpha:alice:ALICE_SECRET;alpha:bob:BOB_SECRET;beta:alice:OTHER_SECRET'
.cache/build/dev/src/server/chat-server.exe --port 7777
# Another terminal:
$env:CHAT_TOKEN = 'ALICE_SECRET'
.cache/build/dev/src/client/chat-daemon.exe --tenant alpha --user alice --state-dir .cache/alice
# Another terminal:
.cache/build/dev/src/client/chat.exe --state-dir .cache/alice
```

POSIX example:

```sh
export CHAT_DB_DSN='host=127.0.0.1 dbname=chat user=chat password=YOUR_DB_PASSWORD'
export CHAT_CREDENTIALS='alpha:alice:ALICE_SECRET;alpha:bob:BOB_SECRET'
.cache/build/dev/src/server/chat-server --threads 4
# Another terminal:
export CHAT_TOKEN='ALICE_SECRET'
.cache/build/dev/src/client/chat-daemon --tenant alpha --user alice --state-dir .cache/alice
# Another terminal:
.cache/build/dev/src/client/chat --state-dir .cache/alice
```

Never reuse the demonstration credentials in a real deployment. Use independent,
high-entropy user tokens. One state directory is bound to one tenant/user; another
account cannot silently reuse its cache. Stop the daemon to replace `CHAT_TOKEN`.

## What is implemented

| Area | Implementation |
| --- | --- |
| Protocol | Versioned 20-byte binary header, request IDs, length-prefixed UTF-8 fields, incremental stream parsing, optional bounded per-frame native LZ compression |
| Server I/O | Nonblocking sockets, Linux level-triggered epoll; POSIX poll and Windows WSAPoll; partial writes, fairness budgets and completion wakeup |
| Concurrency | One bounded global worker pool, session FIFO, ordered same-room commit/publication, bounded input/completion/output queues |
| Tenancy | Provisioned tenant/user token authentication; every query scoped by authenticated tenant; tenant-public rooms |
| Reliability | Persist before ack/broadcast; tenant/user/nonce deduplication; PostgreSQL transaction/pool recovery; server restart persistence |
| Metadata | UTC millisecond timestamps, sender and message ID, monotonic read cursors, unread counts excluding own messages, history pagination |
| Daemon | Private authenticated loopback IPC, multiple TUI clients, request-ID isolation, account lock, bounded JSONL cache, reconnect/backoff and heartbeats |
| TUI | Color scrollback, UTF-8 input, codepoint backspace, event-driven prompt updates, explicit read, terminal sanitization and state restoration; plain mode for pipes |
| Build/ops | Target-based CMake subdirectories, presets, Python build/demo/provision/benchmark tools, CTest and GitHub Actions |

## Security and scope

**Transport is plaintext, not TLS.** Listeners default to `127.0.0.1`; the server
requires explicit `--allow-plaintext` for a non-loopback bind. For remote use,
keep the server bound to loopback and establish an SSH tunnel. Tokens authenticate
but do not encrypt. This version is not a public-Internet-ready deployment.

Rooms are public within a tenant, not private membership groups. There are no
attachments, DMs, account registration/recovery, moderation tools, or end-to-end
encryption. Horizontal live fanout across multiple server processes is not
implemented: run one chat server per database deployment. PostgreSQL persists
data; it is not a distributed event bus.

The TUI is a deliberately small scrollback-oriented interface, not a full-screen
terminal emulator. It does not implement grapheme-cluster editing, arrow-key line
editing, or multi-line composition. `/read` is explicit so background receipt
does not incorrectly mark messages read. Offline sends fail clearly, not queue
silently; after reconnect use `/history AFTER_ID` to synchronize. An interrupted
send can have an unknown delivery outcome; inspect history before manual retry.

## Testing and measurements

`ctest` runs randomized protocol/cache properties, malformed-input and real socket
tests, publication-order/disconnected-sender regressions, server/daemon/plain-TUI
journeys, and daemon reconnection/privacy tests. POSIX builds also run a **real
pseudo-terminal** journey with Chinese/emoji editing, read receipts, hostile
terminal controls, idle redraw checks, and terminal-mode restoration.

For a **fresh, dedicated test database**, set `CHAT_DB_DSN` and run:

```console
python tests/journey.py --server .cache/build/dev/src/server/chat-server --daemon .cache/build/dev/src/client/chat-daemon --client .cache/build/dev/src/client/chat --postgres
python tests/postgres_recovery.py --server .cache/build/dev/src/server/chat-server
```

The recovery test intentionally terminates sessions for that database/user, not
the PostgreSQL cluster. Do not point it at an active production database.

```console
python scripts/benchmark.py --server .cache/build/portable/src/server/chat-server --clients 16 --messages 100 --bytes 256
```

Results include throughput, payload bytes, fanout, p50/p95/p99, backend and OS;
they are reproducible local smoke measurements, not advertised capacity. Add
`.exe` to executable paths on Windows. See [validation record](docs/validation.md)
for the actual configurations and results executed during development.

GitHub Actions has passed all five hosted jobs: Linux/Windows/macOS native
execution, Linux PostgreSQL persistence/recovery and forced-poll, and ASan/UBSan.
See the [validation record](docs/validation.md#hosted-github-actions-validation)
for the actual run, tested commit, and the corrected Windows SID-alias assertion.

## Design documentation

- [Architecture and wire operations](docs/architecture.md)
- [Frame/compression format and UTF-8 policy](docs/protocol-codec.md)
- [Storage, ordering, resource budgets and latency](docs/storage.md)
- [Client daemon, cache, terminal and IPC](docs/client.md)
- [Engineering sources and trade-offs](docs/engineering-notes.md)
- [Review and addressed findings](docs/review.md)
- [Interactive terminal validation](docs/terminal-validation.md)

License: [GPL-3.0](LICENSE), retained from the initial repository.
