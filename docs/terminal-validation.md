# Interactive FTXUI terminal validation

## Contract and method

`tests/terminal_journey.py` runs the real server and daemon in isolated memory
mode, then attaches the real FTXUI `chat` executable to a native POSIX controlling
PTY. It never starts the user's existing stack and does not use `--plain`.
All temporary state is beneath the repository's `.temp`; child processes and PTY
handles are cleaned up on both success and failure.

The test reuses `scripts/terminal_probe.py::Projection` to reconstruct the current
bounded terminal screen from cursor movement, erase, UTF-8 and color sequences.
Assertions examine that current screen, not accumulated append-log strings.
The harness answers terminal DA/CPR discovery requests and drives terminal resize
through `TIOCSWINSZ` plus `SIGWINCH`. It checks wire delivery and read receipts with
an independent SCL/1 TCP peer rather than accepting local echo as proof of send.

```sh
python3 tests/terminal_journey.py \
  --server .cache/build/linux/src/server/chat-server \
  --daemon .cache/build/linux/src/client/chat-daemon \
  --client .cache/build/linux/src/client/chat
```

## Covered user journeys

| User behavior | Independently checked outcome |
| --- | --- |
| Attach with cached messages | Fullscreen native input; one visible copy after cache plus server history |
| Inspect conversation | Author and local clock time; no old `[live]`/`[online]` log tags |
| Chinese/emoji Backspace | Whole Unicode text preserved and independently received over TCP |
| Home/Right/Delete/End/Left editing | Exact edited message received by peer |
| Combining-mark Backspace | `Aé`, Backspace, `B` sends `AB` |
| Receive without read action | Server unread remains nonzero |
| `/rooms`, Ctrl+R | Visible unread badge; peer receives the explicit visible-tail cursor |
| F1, Esc | Help overlay opens and closes |
| `/join`, Tab | Only active conversation appears; per-room draft is retained separately |
| PgUp/PgDn, Ctrl+R while scrolled | Older text appears; read cursor never advances beyond projected visible messages |
| 120x32 to 40x12 and back | Bounded narrow composer and timeline remain usable; real narrow-screen send works |
| Upstream stop/restart | Same UI remains editable; offline Enter preserves draft, does not queue/resend; online Enter sends it |
| Peer OSC/BEL payload | Injected title sequence and BEL never reach PTY output |
| Idle | No terminal output during settled 350 ms interval |
| Ctrl+C, Ctrl+D, `/quit` | Original termios attributes, cursor visibility and alternate screen restored |
| Missing daemon endpoint | Visible diagnostic, exit 1, original termios preserved |

## Environment and interpretation

Local environment: WSL2 Ubuntu 24.04.5, Linux
6.18.33.2-microsoft-standard-WSL2, Python 3.12.3, GCC 13.3.0. Test subprocesses use
`TERM=xterm-256color`, UTF-8 locale and `TZ=Asia/Shanghai`. Binaries and isolated
state remain in the repository on DrvFS.

The earlier scrollback client had a verified regression: it emitted 288 bytes
of prompt redraw during an idle 300 ms. Its fix passed the old journey. That
historical result is not evidence of the replacement fullscreen FTXUI client;
the present harness deliberately replaces the old append-log assumptions.

The replacement FTXUI journey passed first against the isolated Release client
at `.cache/terminal-build/src/client/chat` with the existing Linux server/daemon.
Three further complete runs passed using the same client and
`.cache/build-client/src/{server/chat-server,client/chat-daemon}` to avoid racing
the main build while it relinked executables. These runs include the configured
UTC+8 clock assertion and peer OSC/BEL/SGR rejection. The local source override
used the pinned FTXUI 7.0.3 source already available in `.cache/deps`; it does not
establish fresh FetchContent downloading or Windows-specific dependency patching.

## Limits

Projection is a small VT subset, not a complete emulator or a screenshot. Glyph
appearance under actual fonts and Windows console behavior require separate
native validation. Mouse behavior is not exercised. Database persistence and
PostgreSQL are intentionally outside this client-focused in-memory journey.

## Recent-history and failure-state regression suite

`tests/tui_state_journey.py` uses the same three executable arguments. It adds:

```sh
python3 tests/tui_state_journey.py \
  --server .cache/build/linux/src/server/chat-server \
  --daemon .cache/build/linux/src/client/chat-daemon \
  --client .cache/build/linux/src/client/chat
```

- 300 pre-existing lobby messages, interleaved with 30 other-room messages so
  global message IDs have gaps. The daemon and UI start only after seeding, with
  no cached event journal; the first attachment must display `PRESEED_299`.
- A scripted authenticated IPC fixture advertises lobby last ID 350 and other
  room last ID 900. While serving lobby history after 150, it injects other-room
  live ID 900 and lobby live ID 400, then ends the fetched page at 200. The next
  history request must use fetched cursor 200, not any advertised/live maximum.
- The fixture receives a real optimistic send but withholds its ACK. After the
  user edits another draft, the fixture closes IPC. The original UI must convert
  the pending message to delivery-uncertain, retain and allow editing the draft,
  and restore the terminal when Ctrl+C exits.

Against the original isolated FTXUI build, the 300-message regression failed as
intended: the screen stopped at `PRESEED_199` instead of `PRESEED_299`, proving the
first-history-page defect is detected through actual terminal behavior.

The rebuilt main Linux binaries at `.cache/build/linux/src/...` then passed
both complete suites, including the new logical-display-row paging and all
scripted uncertainty/cursor checks. The same test therefore rejects the original
first-page defect and accepts the corrected recent-history synchronization.
Two additional consecutive paired runs also passed: each main-build suite has
three complete successful runs, not a retry-until-pass result. `git diff --check`
reported no whitespace errors for the verification changes.
No existing user's daemon, server, terminal window, or hybrid stack was opened.

### Bounded historical window and structured uncertainty

The state suite additionally invokes `tests/tui_window_journey.py`, using the
shared authenticated fixture in `tests/tui_ipc_fixture.py`. Its scripted daemon
replays exactly 500 cached IDs 301..800, then supplies duplicate recent history
601..800. An explicit older-history fetch adds IDs 102..300. Real PgUp input must
reach `WINDOW_102`, demonstrating that newly fetched history is not immediately
evicted by the 500-message limit. PgDn back to the bottom must trigger another
recent-window fetch and visibly restore `LIVE_800`, rather than claiming the
retained historical window is still current.

The same fixture returns an explicit protocol error `delivery_unknown` for a
real send. The screen must show delivery uncertainty and guidance against
repeating the send, not a definitive failure or the injected raw backend
diagnostic. The user must still be able to edit another draft afterward.

The main Linux build passed the expanded state journey three times after these fixes.
One recorded traversal used 93 PgUp and 97 PgDn events, with exactly two recent
fetches (initial sync and return-to-live). The fixture sent 500 cached entries
plus 199 older entries; the historical window's newest displayed settled ID was
601, the oldest reachable marker was 102, and returning live restored 800.
The structured `delivery_unknown` result was displayed as uncertainty with
human guidance, and all terminal restoration assertions passed.

### Return-to-live while a short older page is still in flight

A held-page fixture reproduces the cancellation race through real PTY input.
After the 500-message cached window, `/history` receives only IDs 102..150;
`history_end(150)` is deliberately withheld. The test waits for the partially
trimmed bottom marker `WINDOW_751`, presses PgDn while already at the bottom,
and waits for the visible “returning to latest” notice before releasing the held
history end. The next request must be a fresh recent sync after 600, never a
stale older continuation after 150. Recent ID 800 and a subsequent unsolicited
live ID 801 must both remain visible, proving normal newest-message retention.

The saved pre-fix main Linux client `.cache/terminal-before-race-chat` failed
exactly at the stale-continuation assertion after all earlier window checks
passed. This is a reproduced ordering defect, not a timeout or VT projection
failure. The saved client is a test artifact inside the repository cache.

The rebuilt main Linux client passed the full expanded state suite with the
same held-page schedule. The recorded proof line was:
`held end=150, PgDn cancels older continuation, recent_fetches=2, live=800/801 retained`.
All preceding recent-history, cursor, uncertainty, older-window traversal and
terminal-restoration checks also passed. The regression therefore fails the
saved pre-fix binary and passes the corrected binary without weakening any
existing scenario. `git diff --check` reported no whitespace errors.
