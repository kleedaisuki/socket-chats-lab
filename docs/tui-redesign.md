# TUI redesign and input-latency investigation

## User requirement and baseline
The user reports the actual Windows interactive client feels delayed character-by-character and its log-like display is unreadable. This is authoritative workload context. Stop/clean only this lab's processes, preserve PostgreSQL data, then fix both input responsiveness and human-readable conversation UX. Plain pipe success is not evidence of interactive Windows responsiveness.

2026-10-19 cleanup: stopped both recorded hybrid Windows daemons and the recorded WSL server; closed the dedicated Socket Chats Windows Terminal window; removed stale account endpoint files. PostgreSQL, account configuration, persisted chat data and prior evidence are retained. Process enumeration found no remaining chat/chat-daemon processes.

Static baseline cause to test: interactive Windows Terminal::read takes exactly one UTF-16 key per loop, while Tui::run always calls socket Poller::wait(25) first. This imposes approximately 25+ ms per character even for buffered paste and excludes keyboard readiness from the wait. Windows timer quantization can further increase this. Pending high-surrogate reads call a blocking _getwch. Every character redraws the full prompt, and protocol/cache/history events are printed directly with IDs and repeated metadata. Existing --plain tests do not cover these paths.

## Architecture and ownership
**User clarification / revised decision:** only third-party networking frameworks are forbidden. Use mature FTXUI v7.0.3 from a pinned, SHA256-verified source archive through CMake FetchContent, instead of building a new terminal/input/layout engine. FTXUI owns terminal event handling, the Input/Menu widgets, Unicode editing, alternate screen, resize and screen rendering. Native socket IO remains our own. The earlier native Terminal API and custom VT Renderer sketches below are superseded, not implementation requirements. Retain only the conversation data models and product acceptance requirements.

- terminal worker now owns dependency integration, exact-source small Windows compatibility patch, and FTXUI mechanism investigation.
- view worker now owns FTXUI DOM-based `conversation_view(State, composer, columns, rows, optional_rooms_component)` and pure rendered view tests, not a custom VT renderer.
- root owns a bounded native transport thread that posts batched events to the FTXUI main loop, app state/deduplication/optimistic send and plain-mode compatibility.
- perf worker remains responsible for real ConPTY baseline/current evidence, not `--plain` stand-ins.

### Superseded initial sketch (retained for decision traceability)
- terminal worker owns src/client/terminal.hpp/.cpp and terminal-focused tests. Native batched input and combined input/network readiness, no periodic per-character polling.
- view worker owns src/client/view.hpp/.cpp and tests/view_tests.cpp. Pure model -> bounded screen layout + incremental VT frame diff, no I/O or protocol business logic.
- root owns src/client/tui.cpp orchestration, CMake integration and docs. Keep --plain textual output and CLI contract compatible. All transport/storage/tenant contracts stay unchanged.
- perf/validation worker owns actual Windows ConPTY input-to-visible-output probe and POSIX interactive compatibility tests, not production code. Measure baseline and revised client with the same terminal journey; persist actual evidence.

## terminal.hpp contract (namespace client)
enum class Key { text, enter, backspace, del, left, right, up, down, home, end, page_up, page_down, escape, tab, reverse_tab, ctrl_c, ctrl_d, ctrl_l, ctrl_u, ctrl_w, ctrl_r, help, resize, eof };
struct Input { Key key; std::string text; };
struct Size { std::size_t columns, rows; };
struct Ready { bool input=false, readable=false, writable=false, closed=false; };
class Terminal { public: explicit Terminal(bool plain); ~Terminal(); bool interactive() const; Size size() const; std::vector<Input> read(); Ready wait(chat::Socket,bool want_write,int timeout_ms=250); void write(std::string_view); };
May use pimpl; headers self-contained. Noninteractive read returns text chunks including LF and eof on end, preserving UTF-8 stream fragmentation for root buffering. Interactive read drains a bounded BATCH of available events/text, preserving partial UTF-16/UTF-8 sequences without blocking. Win32 ReadConsoleInputW, event waiting via WSAEventSelect + WaitForMultipleObjects; POSIX poll(stdin,socket). Up/down are editor history; PgUp/PgDn scroll messages; Tab cycles rooms; F1 help; Ctrl+R explicit mark-read. Win32 raw modes disable line/echo/processed/QuickEdit, enable resize; restore all previous modes/codepages. Enter/leave alternate screen only in interactive mode, enable VT and leave normal scrollback intact. Bounds: at most256 native records/read and input payload <=256KiB. Key event repeats honored, surrogate state carried between batches, pasted burst must not be throttled to one char/wait. No third-party libraries.

## view.hpp contract (namespace client::ui)
enum class Delivery { pending, sent, read, failed, uncertain };
struct Message { std::uint64_t id=0; std::string nonce,author,text; std::int64_t time_ms=0; Delivery delivery=Delivery::sent; bool cached=false; };
struct Room { std::string name; std::uint64_t last_id=0; std::size_t unread=0; std::vector<Message> messages; std::uint64_t displayed=0; bool loaded=false; std::size_t scroll=0; };
struct State { std::string tenant,user,active_room="lobby",connection="connecting",notice; std::map<std::string,Room> rooms; bool help=false; };
struct Composer { std::string text; std::size_t cursor=0; std::string hint; }; // cursor is UTF-8 byte boundary
struct Screen { std::vector<std::string> lines; std::size_t cursor_row=0,cursor_column=0; std::uint64_t visible_max=0; }; // row,column zero-based terminal cells; lines may contain app SGR only
Screen compose(const State&,const Composer&,std::size_t columns,std::size_t rows);
class Renderer { public: std::string draw(const Screen&); void reset(); }; // retains prior frame, changes only dirty rows, emits nothing if unchanged

## Visual/product requirements
A real full-screen conversation view: concise account/connection header, room sidebar when wide enough (collapse cleanly on small terminals), active-room message viewport, fixed multirow composer and discoverable shortcuts/status footer. Message body dominates; author/time grouped and subdued, own delivery state inline. Hide raw message IDs/nonces/cache replay events in normal UI. Deduplicate by ID/nonce and display only active room. No per-event debug log, no per-message protocol status spam. Chinese-friendly local date/time, CJK/emoji width-aware clipping and wrapping, neutralize untrusted controls. Date/history dividers once where useful; no repeated full UTC timestamps on every line. Maintain sensible narrow/short fallback down to40x12 without writing outside dimensions. Input remains visible during new messages and resize.

## Behavioral requirements
Immediate local echo and optimistic pending message on Enter; authoritative ACK transitions sent, matching broadcast/history merges rather than duplicates. Failed/offline send must retain draft or show an explicit failed/uncertain state, not silently lose it. Explicit /read and Ctrl+R only mark foreground-displayed messages; no daemon auto-read. Cache/history/live data become one coherent conversation. Existing /rooms /join /history /read /help /quit commands remain supported, --plain remains stable for scripts. Support cursor left/right/home/end, backspace/delete, input history, Tab room switch, PgUp/PgDn scroll, F1 help, Ctrl+C/D clean quit. Render once per input/network batch with a single buffered output write; no full-screen redraw for each key and no idle redraw.

## Completion evidence
Must include native Windows pseudo-console burst typing/IME-equivalent Unicode, key-to-output latency distribution and input burst completion vs baseline, no dropped/reordered chars, real send/read/reconnect regression, POSIX PTY modes/restoration, bounded screen properties at normal/narrow/short sizes, captured actual new VT-rendered output inspected visually, and all supported platform builds/tests. Do not reopen a visible live app against the user's request to shut down without explicit approval; hidden test fixtures must clean themselves. Do not claim glyph appearance was inspected from protocol logs.

## FTXUI dependency decision and native input mechanisms

The user subsequently explicitly allowed mature third-party terminal/UI components
managed by CMake FetchContent; networking frameworks remain forbidden. The earlier
native-terminal and custom-screen contracts above are superseded by the root's
FTXUI integration, not additional engines to maintain alongside it.

Pinned dependency: FTXUI v7.0.3 (MIT), release commit prefix `f921fad`, downloaded
from <https://codeload.github.com/ArthurSonzogni/FTXUI/tar.gz/refs/tags/v7.0.3>.
Archive SHA-256:
`e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88`.
CMake configuration lives in `cmake/dependencies.cmake`; default FetchContent
source/build/download files remain inside each build tree's `_deps` (the normal
project build trees live below `.cache/build`). Windows and WSL must not share
one dependency build directory. Only static screen/dom/component libraries are
built; examples, upstream tests/fuzzers, docs, modules, install and developer
instrumentation are disabled. Application networking, protocol and database
implementation remains unchanged.

Source inspection of `src/ftxui/component/app.cpp` in this exact release found:

- `ScreenInteractive` is an alias of `App`. `RunOnce` fetches console input,
  drains pending events/tasks and draws once for the batch. `RunOnceBlocking`
  caps frame cadence at approximately 60 FPS; it does not impose a network wait
  on each character. This removes the original one-character-per-25-ms scaling,
  but measured latency remains a separate acceptance gate.
- Windows uses `GetNumberOfConsoleInputEvents` then `ReadConsoleInputW` on the
  available native records; key-up records are ignored. Native VT-input mode
  routes special-key escape sequences through its existing parser. POSIX reads
  terminal bytes through poll and the same parser. The Input component handles
  UTF-8 cursor positions, glyph-aware movement/deletion and viewport framing.
- Thread-safe `PostEvent` queues through the protected task runner, allowing
  daemon socket reads on a separate owned I/O thread without blocking typing.
- Existing teardown restores saved Win32 modes and POSIX termios,
  signal handlers and alternate-screen state. FTXUI does not set Windows console
  code pages; the application must retain an UTF-8 code-page RAII guard.
  The final application/PTY tests
  must still prove those paths actually run.

Three precise Windows defects in the pinned source require the reproducible
`PATCH_COMMAND` implemented by `scripts/patch_ftxui.py`: a local pending UTF-16
string loses a high surrogate when its low surrogate arrives in the next batch;
`wRepeatCount` is ignored; and raw mode leaves QuickEdit enabled (selection can
freeze output) and leaves Ctrl-C to OS processing. The patch stores only the
pending high surrogate in `App::Internal`, feeds valid pairs through upstream's
existing UTF-8 converter/parser, honors key repeat records, replaces malformed
isolated surrogates with U+FFFD, and disables QuickEdit/processed input while
preserving the upstream saved-mode restoration. It does not replace FTXUI's event
loop, parser, widgets or renderer. Exact-context and idempotence checks fail
configuration on source drift, so upgrading the dependency requires reviewing
these decisions instead of silently carrying a patch against changed code.

Dependency patch checks: `python tests/dependency_patch.py` passes 3 tests for
idempotence, reject-before-write on drift and duplicate-context rejection. The
archive was freshly downloaded and the same patch applied twice successfully.
Native split-surrogate and key-repeat behavior requires the actual Win32 console
fixture; patch-text tests do not establish interactive correctness.

Primary references:
- Release: <https://github.com/ArthurSonzogni/FTXUI/releases/tag/v7.0.3>
- Pinned native/event loop source:
  <https://github.com/ArthurSonzogni/FTXUI/blob/v7.0.3/src/ftxui/component/app.cpp>
- Pinned Input component:
  <https://github.com/ArthurSonzogni/FTXUI/blob/v7.0.3/src/ftxui/component/input.cpp>
- Console records and batch semantics:
  <https://learn.microsoft.com/en-us/windows/console/readconsoleinput>
- Console modes/QuickEdit:
  <https://learn.microsoft.com/en-us/windows/console/setconsolemode>

## Conversation view implementation (FTXUI pivot)
The user explicitly permitted a mature source-built TUI library during implementation. The pure view now returns an FTXUI DOM (`conversation_view`); it does not generate or diff VT sequences. The model remains protocol-independent. `Room.scroll` is the number of wrapped display rows excluded from the newest tail while browsing history (updated after the large-message performance investigation below). Only the active room enters the message DOM; IDs, nonces and replay bookkeeping are never normal visual elements.

Visual hierarchy: quiet account/application header; 21-cell room navigation only at >=88 columns and >=16 rows; active-room heading with unread count and connection; conversation body with one date divider, compact local author/time line and separated bodies; bottom-fixed supplied Input component; one-line shortcuts/notice. The layout drops the sidebar at narrow widths and uses a minimal composer-first fallback below30x9. Teal is the primary accent, subdued blue-grey metadata and near-white body text on a calm dark background. Pending/read/failure are attached to one's own message, not emitted as separate logs. No decorative double boxes or full-width status blocks compete with message content.

FTXUI v7 paragraph splits ASCII spaces only. The final view therefore uses FTXUI glyph segmentation/width helpers to wrap sanitized message bodies into cached lines, then instantiates text nodes only for visible rows. Long URLs and Chinese both wrap instead of disappearing; canonical message text is never truncated. Untrusted strings pass through the existing core `terminal_safe` helper before entering any DOM text. FTXUI supplies glyph segmentation, cell widths, wrapping, clipping and terminal rendering; no handwritten terminal renderer is retained.

Explicit-read visibility is measured with FTXUI's actual layout: message `reflect` boxes are intersected with the timeline's viewport, using maximum composer height conservatively. Tiny fallback/help views return zero. This prevents an oversized optimistic message (ID zero) from marking an earlier committed message that is actually entirely outside the viewport. Call `visible_message_max(state, columns, rows)` with current terminal dimensions; defaults120x32 only preserve source convenience.

Focused evidence (Windows MinGW, 2026-10-19): linked tests/view_tests.cpp with src/client/view.cpp, src/core/utf8.cpp and the actual pinned FTXUI DOM/screen static libraries; passed fixture assertions at120x32,88x24,60x18,40x12,12x5 plus120 seeded dimension/control-safety cases. Tests cover active-room isolation, hiddenIDs/nonces, composer retention, local metadata/delivery labels, history/read bounds, help and oversized optimistic-message read safety. This is view evidence, not interactive-input latency evidence.

Genuine rendered fixture capture: `chat-view-tests --snapshot 120 32 .cache/tui-redesign/wide.ansi` (and40x12 narrow.ansi). This renders the production DOM into a real FTXUI Screen then writes its ANSI representation plus a plain cell-content companion. Use ANSI as authoritative for alignment/styles; the text companion intentionally omits styling and blank cell padding. All captures remain ignored in.cache, not checked-in generated assets. Root still must inspect a rendered terminal capture and validate the integrated native-input journey before declaring the user goal complete.

Historical short-message-only hot-path check, superseded by row virtualization below: construction was limited to `clamp(terminal_rows+4,8,200)` newest eligible messages, not the entire cache. Each message takes at least three rows, so this retains enough content to fill the actual viewport while avoiding cache-sized work per key. The repeatable `chat-view-tests --benchmark` fixture uses1004 cached messages (Chinese+emoji sample),120x32 and100 full DOM-build+Screen-render iterations; Windows MinGW `-O2` measured0.911307ms/frame in this session. This isolates view cost, not terminal I/O or end-to-end keystroke latency. Snapshot files regenerated after this change; focused tests remain passing.

### Native console fixture observations

`tests/ftxui_console_input.cpp` injects real `INPUT_RECORD`s using
`WriteConsoleInputW`, rather than inferring UTF-16 behavior from UTF-8 pipe input.
The split-batch emoji and batched Chinese cases pass with the patched dependency.
On this Windows console host, injecting a record with `wRepeatCount=7` while
`ENABLE_VIRTUAL_TERMINAL_INPUT` is active immediately produces a record with
repeat count 1, confirmed by `PeekConsoleInputW` before FTXUI reads it. The
fixture therefore temporarily disables only the VT-input bit during its native
repeat-record case, asserts the actual buffered count is 7, and then restores
VT mode. With those real native records, all seven characters reach the Input
component; this is not a relaxed one-character expectation.

The strict fixture also exposed a separate upstream side effect: before/after
Win32 output mode changes from 7 to 15, despite input mode correctly returning
503 to 503. `src/ftxui/screen/terminal.cpp::TerminalInfo::ComputeColorSupport`
probes VT support via `SetConsoleMode` without restoring its original mode;
this can run during lazy initialization before `App::Install` saves modes.
The application must retain an outer console guard that captures both Win32
modes and both code pages before constructing any FTXUI object and restores them
after all FTXUI objects are destroyed. A guard that only restores code pages is
insufficient. The native fixture must use that same production guard and retain
its strict equality assertion; do not mask the side effect by resampling the
baseline after color initialization.

## Native Session transport implementation

Interactive IO is implemented in src/client/session.hpp/.cpp, separate from the
FTXUI event/render thread. Session takes ownership of the connected daemon socket
(including constructor failure), makes it nonblocking, queues the authentication
frame, and starts one owned worker. The worker blocks in native Poller::wait(-1)
with a portable loopback TCP wake pair. UI sends and inbound-drain notifications
write a wake byte immediately; there is no periodic 25 ms network/input cadence.
Only the wake writer uses TCP_NODELAY to avoid delaying internal wake bytes; the
application socket policy and core ABI remain unchanged.

Outgoing encoded buffers, including the active partial write, are bounded to
2 MiB and 512 frames. send throws on queue overflow or a stopped/offline session;
the UI must preserve draft or show explicit uncertain/failed delivery. Inbound
frames are bounded to 8 MiB and 4096 records, including a reserved terminal status
slot. Conservative read batches pause socket readability before these bounds,
and take wakes the worker to resume. Notification is once per receive batch and
coalesced until take, never once per byte or per UI poll tick. Peer EOF/invalid
protocol/write failure produces one status event [daemon-offline, Local daemon
disconnected], without replaying pending writes. Destruction requests stop, wakes,
and joins before closing transport state or releasing the callback.

Tests/session_tests.cpp uses real native loopback connections: initial hello,
idle-to-send wake, 32 coalesced frames with one notification, UTF-8, exactly one
disconnect status, rejection of subsequent writes, prompt idle teardown, callback
lifetime, constructor-failure socket ownership, and 5000 FIFO events across input
backpressure. Native Windows GCC23 and WSL GCC23 standalone runs passed. Single
idle-wakeup observations were below 1 ms; these are diagnostic samples, not the
ConPTY typing-latency acceptance distribution owned by the performance worker.

The production guard is now `src/client/console.hpp::ConsoleState`, shared by
both the interactive application and the fixture. It captures Win32 input/output
modes and both code pages before FTXUI construction, sets UTF-8 code pages, and
restores the exact saved state after FTXUI teardown (including exception
unwinding). POSIX delegates raw terminal handling to FTXUI and the guard is a
no-op. If UTF-8 setup partially fails, the constructor restores captured state
before throwing.

On 2026-10-19 both native MinGW GCC 16.2 and MSVC 19.51 dependency/fixture builds
completed, and both fixtures passed in a real hidden ConPTY:

```console
python tests/dependency_patch.py
python scripts/terminal_probe.py --fixture .cache/build/ftxui-only/ftxui-native-input.exe --output .cache/ftxui-native-input-verified
python scripts/terminal_probe.py --fixture .cache/build/ftxui-msvc-only/Debug/ftxui-native-input.exe --output .cache/ftxui-native-msvc-verified
```

Both returned `PASS split-surrogate repeat-count Chinese-batch mode-restoration`.
The checks include exact before/after mode and code-page equality measured
outside the shared guard, not a resampled post-initialization baseline. The
ConPTY launcher stores raw VT capture, projected screen and fixture result and
closes its pseudo-console/process handles in a finally path. These fixtures
establish native input conversion and cleanup; complete chat/latency/UX behavior
is evaluated separately by the root's application journey tests.


## Maximum-message performance investigation and row virtualization
A targeted review identified that a bounded *message count* does not bound glyph DOM work: a legal65,535-byte Chinese message contains21,845 glyphs. The pre-virtualization actual build+render benchmark at120x32, MinGW `-O2`, measured17.1306ms/frame with one such message and618.004ms/frame with36 (five full frames each). The earlier0.91ms short-message figure did not cover this workload.

Final architecture: each `ui::State` owns an opaque `LayoutCache` shared handle, used solely on the UI thread. It retains at most128 LRU entries and8MiB accounted allocation (canonical source copy, vector/string capacities, entry/list overhead). Cache hits require both current message identity, width and exact canonical body equality; source equality prevents address reuse/ACK-body replacement from yielding stale content. Metadata is rendered fresh, so read/ACK changes without a body change reuse wrapped lines. No global caches or cross-account cache registry exist; cache storage is released with the owning state. Existing aggregate model construction remains source-compatible because the handle is appended with a default value.

Timeline construction walks messages from the newest tail backwards and materializes at most`terminal_rows+4` single-row text/metadata/divider nodes. Cached wrapping uses FTXUI glyph segmentation and width measurements; FTXUI still owns the actual screen, clipping, widgets and native input. `Room.scroll` now counts **logical wrapped rows**, allowing PgUp/PgDn to navigate within a64KiB message instead of skipping it wholesale. Explicit read intersects reflected per-row IDs with the actual FTXUI viewport; date dividers and blank spacer rows carry no read IDs. Full canonical bodies remain in the model, and every wrapped line is reachable through paging.

Repeatable command: `chat-view-tests --benchmark`. Final measured run, same Windows MinGW `-O2` machine and120x32:

| Workload | Cold frame | Warm p50 | Warm p95 |
| --- | ---: | ---: | ---: |
|1 x65,535-byte Chinese message |2.1905ms |0.2534ms |0.3523ms |
|36 x65,535-byte Chinese messages |1.7158ms |0.2693ms |0.3863ms |

Warm percentiles use100 measured full DOM-build+Screen-render frames after a separate cold frame. The1004 short-message fixture averages0.176677ms/frame over100 frames. These are view-only costs; native terminal input/transport latency still needs the separate integration probes.

Focused tests now additionally exercise: a65,519-byte Chinese message with distinct START/END markers (tail visible, head reachable by row paging), all100 numbered explicit-newline body rows reachable, width changes120→40→88→60→120,150 distinct widths while asserting retained cache≤128entries/8MiB, canonical body replacement under the same message identity, invisible committed IDs behind oversized pending content, and tiny/help no-read behavior. Windows standalone linked tests passed after virtualization; integration CI should compile the same production source/test target. Regenerate snapshots after each layout change; narrow footer now shortens its shortcut labels instead of clipping half a command.

## History-top clamping and skipped-message height memo
The follow-up history workload exposed another cache policy problem: a deep scroll walks past hundreds of messages, so looking up/wrapping *every skipped body* churned the128-entry wrapped-line LRU. A500-message cohort with distinct~1KiB Chinese bodies measured~10.57ms warm p50 /11.97ms warm p95 before the fix (100 frames, original deep-scroll position6000, including an overshoot scenario). The final controlled populated near-top position4970 measured cold11.029ms, warm p50=0.5623ms and p95=0.7406ms at120x32. Cold includes the first complete history-height discovery; normal typing thereafter reuses height metadata.

A separate at-most4096-record height memo stores message identity, width, body length, standard-library source fingerprint and row count. Its allocation is included in the existing8MiB budget; it retains no duplicate bodies. This is an ephemeral non-security memoization fingerprint, never an authentication/tenant/protocol identifier. Exact canonical-body equality still guards the wrapped-body LRU. Fully skipped messages use only height metadata, so they do not evict the viewport's wrapped lines. Mutation, width and pointer reuse are detected by the height key/fingerprint; the root's canonical message model remains authoritative.

`maximum_scroll(state, columns, rows)` sums cached logical row counts only for the active room and subtracts the smallest actual message viewport (maximum composer height). Clamp PageUp to this value, and clamp again when terminal dimensions change: otherwise a previously valid narrow-window offset could overshoot after a wider resize. Repeated PageUp thus stops on a populated oldest page rather than accumulating an enormous offset requiring many PageDown presses. Tiny fallback returns0. Tests assert the maximum Chinese-message page contains START (not an empty history warning),100 repeated clamped PageUps stay at the same bound,500-message oldest-page remains reachable, and changed bodies invalidate skipped-row height metadata. Full GCC warning set and linked focused view tests passed.
