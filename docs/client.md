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
| /history [AFTER_ID] | Interactive: load earlier history, or one forward page after an explicit ID; plain mode retains the original default 0 / 100-message page |
| /read | Explicitly mark messages already displayed in the current room read |
| /help | Explain available commands |
| /quit, Ctrl-C, Ctrl-D | Exit and restore the terminal; plain mode briefly drains outstanding replies |

Read receipts are never emitted merely because the daemon receives, caches, or
replays a message. /read uses the largest ID actually displayed for that room.
Plain mode displays UTC date/time, live/history/cache origin, room, sender and ID.
The interactive conversation instead groups compact local time, author and own
delivery/read state beside the body, hiding raw IDs and background event logs.
It displays only the active room; cache, history and live versions merge by ID or
author+nonce. `/rooms` updates the sidebar; unread counts exclude own messages.

Interactive mode uses pinned FTXUI 7.0.3 Input/Menu/DOM and its full-screen event
loop. The native socket Session owns a separate bounded IO worker and wakes the
UI with coalesced events, never a per-character socket polling delay. It supports
Unicode-aware editing, Left/Right/Home/End/Delete, input history, Tab room cycling,
PgUp/PgDn history, F1 help and Ctrl+R read. Drafts survive room changes and offline
input. It does not implement multi-line composition. Mode/codepage capture wraps
FTXUI's entire lifetime; original terminal state is restored on normal exit and
exceptions. Windows uses batched UTF-16 input, including split surrogate pairs
and native repeat counts through the narrow exact-source dependency patch.

The conversation retains 500 messages per room and an 8 MiB global text budget;
the state-owned wrapping cache is independently capped at 128 bodies / 8 MiB,
with bounded height metadata. Only viewport rows become DOM nodes. Browsing older
pages evicts the opposite end rather than discarding the page just fetched;
PgDn at the bottom reloads the latest window. A room's advertised maximum and live
events never advance its separate fetched-history cursor. Read is deferred during
history synchronization and is calculated from actual visible layout.

Optimistic sends appear immediately; ACK/history/broadcast reconcile one message.
Errors known to precede queueing retain code `offline`. A forwarded operation
whose response is lost uses `delivery_unknown`, displayed as uncertain rather
than inviting a blind retry. Local daemon loss settles outstanding optimistic
messages to uncertain and preserves the editable draft. No uncertain send is
automatically replayed.

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
Existing IPC clients survive reconnect. The daemon does not fetch missed history
on their behalf; the interactive UI synchronizes its separate fetched cursor.
Plain clients can use /history AFTER_ID to synchronize. Authentication failures never
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
