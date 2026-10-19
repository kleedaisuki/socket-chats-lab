# Socket Chats Lab — implementation contract

## Goal and boundaries
C++23 native sockets only; no third-party networking framework. Server Linux epoll (level-triggered), POSIX poll fallback and Windows WSAPoll. One reactor owns sockets; bounded global worker pool performs database/business work; per-connection FIFO prevents reordering. JSONL client cache, PostgreSQL authoritative storage (libpq database client allowed), explicit in-memory development/test mode. Localhost plaintext by default, SSH tunnel for remote deployment; never claim TLS or Internet readiness. Tenant-wide public rooms, no private membership/DM semantics in v1.

## Shared C++ API (namespace chat)
Files: include/chat/protocol.hpp, net.hpp, utf8.hpp, thread_pool.hpp and src/core/*.cpp. All headers self-contained.

`enum class Type : uint8_t { hello=1,welcome=2,send=3,message=4,history=5,history_end=6,read=7,receipt=8,ping=9,pong=10,error=11,list=12,rooms=13,join=14,ack=15,status=16 };`
`struct Frame { Type type{}; uint32_t request_id{}; std::vector<std::string> fields; };`
`std::vector<std::byte> encode(const Frame&, bool compress=true);`
`class Decoder { public: std::vector<Frame> feed(std::span<const std::byte>); size_t buffered() const; };` Invalid protocol throws std::runtime_error. Maximum raw/wire payload 1 MiB, maximum 128 fields, strict valid UTF-8 strings (binary NUL permitted by codec but rejected in domain identifiers/text). Compression never applies to hello/welcome or secrets. Header 20 bytes: magic SCL1 [0..3], version=1 [4], type [5], flags big-endian uint16 [6..7], request_id BE32 [8..11], wire length BE32 [12..15], raw length BE32 [16..19]. Fields raw: count BE16, repeated length BE32 + UTF-8 bytes. Agent owning protocol documents chosen native LZ codec before integration; Python tests must match.

`bool valid_utf8(std::string_view); size_t display_width(std::string_view); std::string terminal_safe(std::string_view);` Sanitizer neutralizes terminal control injection, preserves CJK and emoji; width is best effort, not full grapheme shaping.

net.hpp: `using Socket = ...; inline constexpr Socket invalid_socket=...; class Network { public: Network(); ~Network(); }; void close_socket(Socket); void nonblocking(Socket); Socket listen_tcp(std::string_view host,uint16_t port,int backlog=128); Socket connect_tcp(std::string_view host,uint16_t port); uint16_t local_port(Socket); bool would_block(); int last_socket_error(); std::ptrdiff_t receive(Socket,std::span<std::byte>); std::ptrdiff_t transmit(Socket,std::span<const std::byte>); void send_frame(Socket,const Frame&); Frame receive_frame(Socket,Decoder&,int timeout_ms=5000);` connect_tcp is bounded/blocking connect, sockets returned blocking until nonblocking called. send_frame for blocking sockets. receive_frame must not drop coalesced frames (retain queue per decoder or implement one-frame reads). Native headers exposed by net.hpp. `struct Ready { Socket socket; bool readable; bool writable; bool failed; }; class Poller { public: Poller(); ~Poller(); void set(Socket,bool writable=false); void remove(Socket); std::vector<Ready> wait(int timeout_ms); };` set registers read always and optional write, modifies existing registrations. Pimpl allowed. On Linux use epoll unless CHAT_FORCE_POLL defined. ThreadPool: ctor(size_t threads,size_t capacity=1024), `bool submit(std::function<void()>);` destructor drains and joins. Prefer implement pool within server if shared API not needed.

## Wire operations
All numbers decimal strings, all timestamps UTC Unix milliseconds. Requests always nonzero, unsolicited events zero.
- hello [tenant,user,token] => welcome [tenant,user]; only once, before commands. Authentication from CHAT_CREDENTIALS `tenant:user:token;tenant:user:token`. Tokens must never enter cache/logs. Identifiers ASCII `[A-Za-z0-9_-]`, length 1..64. Room same plus optional `-`, user/tenant case-sensitive. Default room `lobby`.
- join [room] => ack [room] (create tenant public room idempotently).
- send [room,nonce,text] => ack [message_id,nonce], message broadcast [id,room,user,time_ms,text,nonce]. Nonce ASCII identifier <=64, text valid UTF-8 1..65536 bytes, no NUL. Persist BEFORE ack or broadcast; duplicate (tenant,user,nonce) returns original without rebroadcast. Nonce reuse with different contents is an error.
- history [room,after_id,limit] => message frames with original request_id followed by history_end [room,last_returned_id]; ascending IDs, limit 1..200; after_id defaults explicitly `0`. History does not mark read.
- read [room,id] => receipt [room,user,effective_read_id] broadcast; cursor monotonic and cannot exceed room maximum; only explicit UI foreground display can trigger read, daemon never marks just on receipt.
- list [] => rooms repeating [room,last_id,unread_count] triplets. Default lobby included for tenant; unread excludes own messages.
- ping [] => pong []. Errors [code,human_message] with originating request ID. Error codes: auth, invalid, not_found, overload, storage, conflict. Protocol violations disconnect.

## Process/UI contracts
Executables `chat-server`, `chat-daemon`, `chat`.
Server `--host 127.0.0.1 --port 7777 --threads N [--memory] [--allow-plaintext]`; uses CHAT_DB_DSN and CHAT_CREDENTIALS environment. Default requires PostgreSQL; memory explicit. `--port 0` prints READY host:actualport for harness. Remote binds require --allow-plaintext.
Daemon `--host 127.0.0.1 --port 7777 --tenant t --user u --state-dir PATH [--ipc-port 0]`; token from CHAT_TOKEN. Owner-only state directory contains `endpoint` text `PORT\nRANDOM_IPC_TOKEN\n` and `events.jsonl`. IPC TCP binds only 127.0.0.1, authenticated hello [tenant,user,ipc_token]. Endpoint file atomic write, protected permissions (POSIX 0700/0600, Windows per-user appdata inherited ACL caveat documented). Single state directory must have single writer via lock. Remote reconnect with backoff, finite buffers, persistent JSONL events; no secret caching. Map IPC request IDs so multiple TUI clients cannot collide. Upstream hello must use original credentials, never IPC token. `READY 127.0.0.1:port` after endpoint published.
TUI `chat --state-dir PATH [--plain]` attaches daemon; interactive color layout and line editing, UTF-8, timestamps; /help /join ROOM /rooms /history /read /quit. Plain line mode permits harness/pipes, no ANSI. TUI may provide clear start-daemon instruction rather than silently spawning. Preserve terminal state on normal exit. Never render untrusted ANSI, escape/control strings. Avoid full-screen redraw on every network byte; update on events. Cached offline view encouraged; clearly label offline.

## Ownership
Core worker owns include/chat/{protocol,net,utf8}.hpp and src/core/ plus tests/core_tests.cpp and src/core/CMakeLists.txt.
Server worker owns src/server/, include/chat/server* if needed, tests/server* and docs/storage.md.
Client worker owns src/client/, tests/client* and docs/client.md.
Root owns root CMake, CI, scripts, integration tests, README, architecture/protocol docs and final integration. Communicate contract changes before changing other owners' interfaces. No commits until coordinated.

## Implemented v1 amendments
- Endpoint discovery appends tenant and user as lines 3/4, preserving the original first two lines.
- History count is an upper bound; raw response pages also stop at approximately 1 MiB. Resume at the history-end cursor.
- Wire field count bounds room listings to 42 rooms per tenant, enforced at room creation.
- Windows daemon state uses a protected current-user-only DACL, including preexisting sensitive files; reparse-point state is rejected. POSIX requests private modes and warns when the underlying filesystem ignores chmod (e.g. WSL DrvFS without metadata).
- A portable nonblocking socket wakeup publishes worker completions immediately. A fixed 256-stripe tenant/room gate spans execution through publication so same-room ID, commit and event order agree.
- Global logical input, completion and queued output budgets are each 64 MiB. Detached completion batches, active worker results, allocator overhead and backend data are additional, documented in storage.md; these are not a claim of a 192 MiB process RSS cap.
- Run one server per deployment: live fanout between independent server processes is not implemented, even when sharing PostgreSQL.
- Poller::set has an optional third readable=true argument for write-only EOF draining. HUP/RDHUP means readable EOF, not a fatal error: complete commands received before FIN are executed and acknowledged before graceful close.
