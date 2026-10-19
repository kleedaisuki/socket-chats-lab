# Client and daemon

## Running

The daemon owns the remote connection and local persistent state. The terminal
client attaches to that daemon; closing the terminal does not stop message
reception. Multiple terminal clients can attach to one account daemon.

~~~sh
export CHAT_TOKEN='your-server-issued-secret'
chat-daemon --host 127.0.0.1 --port 7777 \
  --tenant alpha --user alice --state-dir .cache/alice
# Another terminal:
chat --state-dir .cache/alice
~~~

Use one state directory per tenant/user account. Reusing a directory for another
account is rejected, not silently allowed to reveal the previous account's cache.
CHAT_TOKEN is read only by the daemon, never written to the endpoint or journal.
The IPC token is independently generated and is not the remote credential.
Do not put state directories in source control or shared temporary directories.

Transport is plaintext native TCP. IPC binds only 127.0.0.1.
For remote connections, use a trusted SSH tunnel; neither executable implements
TLS. The daemon is a foreground process suitable for supervision, not an installer
or OS service manager.

## Terminal workflow

| Input | Behavior |
| --- | --- |
| Message text | Send to the current room, with a generated idempotency nonce |
| /rooms | Server-authoritative room, latest ID, unread count |
| /join ROOM | Create/join tenant-public room; switch only after acknowledgement |
| /history [AFTER_ID] | Display up to 100 messages after ID (default 0), ascending |
| /read | Explicitly mark messages already displayed in the current room read |
| /help | Explain available commands |
| /quit, Ctrl-C, Ctrl-D | Flush outstanding commands and exit |

Read receipts are never emitted merely because the daemon receives, caches, or
replays a message. /read uses the largest ID actually displayed for that room.
Messages display UTC date/time, live/history/cache origin, room, sender, and ID.
Read receipts display user and cursor; /rooms provides unread counts excluding
one's own messages. History and cached events can include rooms other than the
current sending room, and their room labels remain explicit.

Interactive mode uses native terminal input, color, incremental prompt updates,
UTF-8 input, and whole-codepoint backspace. Terminal mode is restored on normal
exit and exceptions; POSIX Ctrl-C is handled as input so normal cleanup runs.
It is a scrollback-oriented TUI, not a full-screen terminal emulator. Arrow-key
editing/history, grapheme-cluster editing, and multi-line composition are not
implemented. CJK/emoji width is best effort. Windows uses UTF-16 console input
converted to UTF-8, and restores previous console modes/code pages.

The --plain option emits no ANSI and supports pipes. End-of-input flushes
commands and drains replies for up to two seconds. Input larger than 65536 bytes
is rejected rather than silently truncated. Received text passes through the
shared terminal sanitizer: untrusted escape/control sequences cannot set titles,
change colors, or execute terminal control actions.

## IPC and persistence contracts

The state directory contains:

- lock: OS-held exclusive writer lock (flock on POSIX, unshared file handle on
  Windows). Crashes release the lock; stale files do not block restart.
- identity: tenant and user, preventing cross-account cache reuse.
- endpoint: atomically replaced UTF-8 lines: port, ipc_token, tenant, user.
  The first two lines retain the original discovery contract. The token is
  replaced on every daemon start.
- events.jsonl: event-only records of the following shape and type 8 read receipts.

~~~json
{"type":4,"fields":["id","room","user","time_ms","text","nonce"]}
~~~

JSON escaping preserves UTF-8, embedded newlines, and controls. Authentication and
hello frames are never stored. POSIX requests directory mode 0700 and endpoint
mode 0600. If the filesystem ignores permission changes (notably WSL DrvFS without
metadata), startup warns: use a private native Linux directory, enable WSL mount
metadata, or protect the directory with Windows ACLs. On Windows the daemon
creates a protected DACL allowing only the current process user's SID full file
access, inheritable by new files/subdirectories. Existing identity, endpoint,
journal, temporary files, and the open lock handle are hardened too. Startup
fails clearly if these permissions cannot be applied. Reparse points in the
state directory or sensitive files are rejected. Elevated administrators can
still recover files with normal Windows ownership/security privileges; this is
not intended to protect against an administrator or another process of the same
user. LOCALAPPDATA remains a convenient state location, but a shared parent's
inherited broad ACL is no longer accepted as the privacy mechanism.

The cache is a convenience, not authoritative durable storage. Writes are flushed
but not fsync'd; a crash can lose recent cache records. Damaged records are skipped
on load; PostgreSQL history remains authoritative. In-memory cache is bounded to
1000 events and 8 MiB serialized data. After the journal exceeds 16 MiB it is
compacted to that cache with a replacement file. New terminal clients receive at
most 100 cached events / 512 KiB, oldest first, bracketed by explicit cache and
online/offline statuses. A cache event does not imply the remote server is reachable.

## Reconnect, limits, and failure behavior

One reactor owns established sockets: 2 MiB output per peer, 64 IPC clients, 4096
pending remote requests, 5-second IPC authentication timeout. Each IPC request
gets a distinct upstream ID, so overlapping IDs from different clients cannot
cross-deliver replies. History mapping survives until history-end. Unsolicited
events fan out to all authenticated clients.

Remote resolution/connect runs asynchronously: an unreachable server cannot
freeze cached browsing. Reconnect uses 1/2/4/.../30-second backoff, a 10-second
authentication deadline, heartbeat pings every 15 seconds, and reconnection after
45 seconds without any remote frame. The core connector separately bounds TCP
address attempts; OS DNS can take longer but does not block the IPC reactor.

Offline commands fail explicitly and are **not** queued. Interrupted in-flight
requests get a delivery-uncertainty error and are not automatically resent.
Inspect /history before manually retrying a send whose acknowledgement was lost.
Existing IPC clients survive reconnect; missed history is not automatically
fetched. Use /history AFTER_ID to synchronize. Authentication failures never
print credentials; replace CHAT_TOKEN and restart the daemon to change them.

## Verification

- client_cache: 2000 seeded JSONL round trips with UTF-8, all ASCII controls,
  escaping, empty strings, and different field counts.
- client_reconnect: real server/daemon/CLI processes exercise offline replay,
  upstream reconnect on the same IPC socket, no implicit read, writer exclusion,
  cross-account rejection, and terminal injection safety. Windows additionally
  seeds broad Everyone access and inspects native security descriptors to confirm
  migration to current-user-only access on the directory and sensitive files.
- journey_memory: shared integration suite also exercises colliding request IDs
  from simultaneous clients, UTF-8 messages, and real piped commands.

## Platform references

- [Microsoft SetConsoleMode](https://learn.microsoft.com/en-us/windows/console/setconsolemode):
  disable processed input so Ctrl-C is received as input and normal cleanup can
  restore console modes; enable virtual-terminal output only on a console handle.
- [POSIX termios](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/termios.h.html):
  use noncanonical input and explicit VMIN/VTIME rather than busy-waiting on
  buffered line-oriented standard input.
- [Microsoft SetNamedSecurityInfoW](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-setnamedsecurityinfow)
  and [ACL construction](https://learn.microsoft.com/en-us/windows/win32/secauthz/creating-or-modifying-an-acl):
  replace, rather than merge, existing broad permissions; protect the directory
  DACL and make the sole current-user ACE inheritable. The exclusive lock must
  request WRITE_DAC and use its existing handle for security updates.
