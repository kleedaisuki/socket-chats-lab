# Interactive terminal validation

## Contract and method

`tests/terminal_journey.py` exercises the workflow documented in `client.md`.
It starts the real in-memory server and client daemon, then attaches the real
`chat` executable to a POSIX pseudo-terminal using Python's standard-library
`pty`, `termios`, and `select`. This is not the `--plain` pipe path.
An independent protocol peer verifies text and receipts received over TCP rather
than accepting terminal echo as evidence of a successful send.

All temporary state lives beneath the repository's `.temp` directory. Processes,
PTY descriptors, and temporary files are cleaned up on success and failure.

```sh
python3 tests/terminal_journey.py \
  --server .cache/build/linux/src/server/chat-server \
  --daemon .cache/build/linux/src/client/chat-daemon \
  --client .cache/build/linux/src/client/chat
```

## Coverage

| User behavior | Independent expected outcome |
| --- | --- |
| Attach interactive terminal | Native noncanonical/no-echo/no-ISIG mode; colored prompt |
| Type `你好🙂错`, backspace twice, append `🌷，PTY 世界`, Enter | Bob receives exactly `你好🌷，PTY 世界` |
| Receive peer message | Explicit UTC timestamp, live origin, room and author |
| Receive and display without `/read` | Account retains one unread message |
| `/rooms` | Latest message ID and one unread in table output |
| `/read` | Bob receives Alice's exact displayed cursor; unread becomes zero |
| Receive OSC title change, BEL and SGR injection | No OSC/BEL or unauthorized escape reaches PTY output |
| Idle after all replies drain | Zero output over 300 ms; no timer-driven prompt redraw |
| Ctrl-C, Ctrl-D, `/quit` | Exit 0, all original `termios` attributes restored |
| Attach with absent endpoint | Visible diagnostic, exit 1, all original attributes restored |

The injection assertion permits only application-generated SGR and erase-line
CSI sequences. UTF-8 send correctness is checked on the remote socket. Read
correctness is checked both at the remote recipient and against server account
unread state. Tests wait for observable replies instead of fixed startup sleeps.
The short idle interval follows a separate reply-drain interval.

## Local observations (2026-10-19)

Environment: Ubuntu 24.04.5 under WSL2, Linux
6.18.33.2-microsoft-standard-WSL2, Python 3.12.3, GCC 13.3.0. Project binaries
and temporary state are on the repository's DrvFS mount.

The initial complete workflow passed before the idle-output guard was added.
Adding that guard exposed a concrete defect in the original TUI loop:
`run()` passed empty input to `input()`, which unconditionally redrew the prompt.
With no pending user action, 288 output bytes were observed in 300 ms (12 prompt
redraws, approximately 40/second). The regression assertion fails this original
behavior without relying on source inspection. Production fixes belong to the
client implementation, not the validation harness.

The client worker's corrected binary at
`.cache/build-client/src/client/chat` was then tested with the same server,
daemon, and full harness: **PASS**, including zero idle output and restoration
on all four exit paths. The corrected command differs only in
`--client .cache/build-client/src/client/chat`. Thus the added guard demonstrably
rejects the original defect and accepts its fix. The primary build must be
rebuilt before its older binary can obtain the same result.

## Limits

This validates POSIX PTY byte streams, not a screenshot or an actual font's glyph
rendering. CJK/emoji visual width, combining grapheme editing, Windows console
input and restoration, terminal resizing, and remote terminal emulator behavior
are not established by this test. Database durability and the PostgreSQL backend
are covered by separate integration workflows; this journey deliberately uses
memory storage to isolate interactive client behavior.
