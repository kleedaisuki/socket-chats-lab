# Engineering basis and deliberate trade-offs

## Runtime and resource ownership

The reactor exclusively owns nonblocking sockets; workers never write a client socket. Workers return completed domain operations to the reactor. A bounded queue and one active job per session preserve session ordering without one thread per connection. Slow recipients have bounded write queues, and history pagination is also byte-bounded. These are overload controls, not evidence of any particular supported concurrency count.

Linux uses level-triggered epoll. This deliberately avoids edge-triggered rearm and incomplete-drain special cases while retaining readiness multiplexing. The portability path uses poll/WSAPoll. The [Linux epoll manual](https://man7.org/linux/man-pages/man7/epoll.7.html) explains the drain-to-EAGAIN requirement for edge-triggered operation. A later ET optimization would require measurements and fairness regression tests, not merely a different flag.

The architectural precedent is [SEDA overload control](https://www.usenix.org/legacy/event/usits03/tech/full_papers/welsh/welsh_html/usits.html): explicit queues make admission/overload behavior observable and bounded. This implementation is a small reactor plus a single shared worker pool, not a reproduction of a research runtime. Recent scheduling work is much more workload-specific; importing specialized CPU/core scheduling before measuring this PostgreSQL-backed chat workload would add complexity without demonstrated value.

## PostgreSQL concurrency

Each leased libpq connection is used by exactly one worker at a time, as required by [libpq threading rules](https://www.postgresql.org/docs/17/libpq-threading.html). SQL data values use parameters, never string concatenation. Unique tenant/user/nonce keys and [ON CONFLICT](https://www.postgresql.org/docs/current/sql-insert.html) implement retry deduplication at the storage boundary. A row lock serializes writes in each room so history cursors cannot skip a lower-ID message that commits late in the same room. This limits throughput in one extremely hot room but permits independent rooms to progress concurrently.

Tenant identity is obtained from the authenticated session, not from every message payload. All storage operations include that identity. Public rooms are public only inside their tenant. Database credentials remain server-side. Authentication currently uses provisioned opaque tokens; there is no self-registration, password recovery, role administration, or end-to-end encryption.

## Compression and terminal safety

The native codec is a small bounded LZ token format documented separately. Its priority is an inspectable dependency-free implementation, not zstd-equivalent compression density or throughput. Per-frame compression avoids shared mutable dictionaries and cross-tenant dictionary leakage. Authentication frames are never compressed. Although this codec is not zstd, [RFC 8878 section 8](https://www.rfc-editor.org/rfc/rfc8878.html#section-8) provides applicable resource-limit and secret-mixing guidance. Every decoder bounds both advertised and produced output and rejects invalid backreferences.

The terminal is a control interpreter, not a safe raw string sink. Incoming controls are rendered inert. [Microsoft's terminal guidance](https://learn.microsoft.com/en-us/windows/console/classic-vs-vt) recommends VT output and describes the remaining console-input caveats; the native Windows input path must not assume that a byte-oriented cooked UTF-8 console handles all characters. Width estimation supports common CJK/emoji/combining marks, not full Unicode grapheme shaping or every terminal's emoji presentation behavior.

On systems exposing SO_NOSIGPIPE, configuring a socket as nonblocking also applies that per-socket write-safety setting, including accepted sockets. This follows [Apple's explicit accepted-socket guidance](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/NetworkingOverview/CommonPitfalls/CommonPitfalls.html), rather than assuming listener options inherit portably. Linux send uses MSG_NOSIGNAL. This avoids process-wide SIGPIPE handlers and leaves errors on the normal per-peer error path. Native macOS execution remains a CI requirement, not a locally completed test.

## Security boundary

Version 1 is plaintext TCP and loopback IPC. The default listener is loopback; a non-loopback binding requires explicit consent. For a remote machine, forward the server port through SSH and leave the server bound to loopback. Tokens authenticate but do not encrypt. This is not a public-Internet-ready deployment and must not be described as one. Client state is sensitive message history; POSIX requests private permissions (warning on filesystems that ignore chmod). Native Windows applies a protected current-user-only DACL to both the directory and existing sensitive files, refusing reparse points and failing explicitly if it cannot secure state. No transport cryptography is implemented in-house.

## Evidence standard

Properties cover the byte-stream algebra: any fragmentation/coalescing of the same frames must decode identically, and decompression must reproduce the original bounded fields. Process tests cover a real server, authenticated peers, two local IPC clients with colliding request IDs, the daemon JSONL cache, and actual piped TUI commands. PostgreSQL journeys additionally restart the server and verify persistence. Benchmark reports must name OS/compiler/backend, connection count, message sizes, fanout, and latency percentiles. Smoke-test throughput is not a capacity claim.

Operational provisioning is tested against a new dedicated PostgreSQL role/database, not an existing user's application data. `pg_tools.py` moves passwords out of psql process arguments into PGPASSWORD while preserving other connection parameters; generated-role passwords travel on psql stdin. A seeded 1000-case property test covers quoting, whitespace, escapes and Unicode. Private DSN output still contains a secret and must remain outside Git. This is local process-list hygiene, not a defense against malicious code already running as the same operating-system user.
