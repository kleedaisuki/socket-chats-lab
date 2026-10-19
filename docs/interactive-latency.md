# Interactive Windows latency: native ConPTY evidence

## Workload and method

The user's report concerns native interactive Windows input, not `--plain` pipes.
`scripts/terminal_probe.py` hosts the real client through `CreatePseudoConsole`
and `STARTUPINFOEX` using Python standard-library `ctypes`. A dedicated reader
thread continuously drains output. The hidden fixture launches the real memory
server and daemon; an independent Bob protocol peer verifies exact server-observed
message bodies. No user hybrid services or visible terminal windows are started.
Fixture processes are terminated in `finally`; accounts, logs, VT and results stay
under `.cache`/`.temp`. Background services use `CREATE_NO_WINDOW` explicitly.

The common workload uses a 140-column, 36-row pseudo-console, three rounds of 26
individually observed ASCII keys, three 108-codepoint ASCII bursts and three
76-codepoint Chinese/emoji bursts (240 UTF-8 bytes). Unique prefixes/suffixes prevent
old transcript text satisfying a new draft observation. Key latency starts before
input-pipe `WriteFile` and ends at the output reader timestamp where the projected
screen contains the new draft. Cursor movement, CR, erase, scroll, UTF-8
fragmentation and wide/combining cells are interpreted; stdout is not treated as
an append-only screen. Unknown CSI commands are recorded. Both authoritative
captures have none. Server delivery is checked separately, never inferred from
local echo. SVG/PNG projections use the captured screen and SGR colors, not a
handwritten UI mockup.

This measures input-to-terminal-output latency, not physical keyboard hardware,
monitor scanout, a particular IME candidate window, or Windows Terminal font
rasterization. The observed approximately 30 ms single-key floor includes ConPTY
output scheduling; the decisive improvement is removal of **per-character
serialization for buffered input**, not a claim of nanosecond key response.

### Host requirements learned during investigation

1. Tool/CI hosts supply redirected standard handles. The probe temporarily clears
   OS std handles around `CreateProcessW` and immediately restores them. Otherwise
   Windows can pass host pipes through and silently execute the plain path.
2. ConPTY negotiates Win32 input mode with `CSI ? 9001 h`. The probe honors it by
   emitting `CSI Vk;Sc;Uc;Kd;Cs;Rc _` records with UTF-16 units, native virtual-key
   values, and matching key-down/key-up records, as Windows Terminal does.
   Unicode text uses `VK_PACKET`. Plain UTF-8 injection after this negotiation
   initially dropped emoji in **both** old and new clients; that exploratory
   failure was a test-host defect, not evidence of a production emoji defect.
3. Matching key-up events matter: down-only repeated characters may be merged by
   the console host into repeat-count records. The authoritative comparison models
   complete key presses and does not accidentally benchmark this synthetic case.
   The separate native fixture intentionally verifies repeat-count handling.

## Authoritative comparison: 2026-10-19

Environment: Windows 11 build 26200 x64, Intel family 6 model 154, 20 logical CPUs,
Python 3.14.6. Both clients are MSVC Debug builds. The same unchanged server and
daemon executables, machine, ConPTY size and input-host logic are used. Timing
runs are local and noninstrumented; they do not measure a remote network or SQL.

| Native interactive workload | Original client | FTXUI client | Interpretation |
| --- | ---: | ---: | --- |
| ASCII single key, 78 observations | p50 30.844 ms; p95 32.085 ms | p50 30.642 ms; p95 31.656 ms | Output scheduling dominates individual keys |
| 108-character ASCII burst, 3 runs | 3446.937 / 3392.855 / 3410.650 ms | 26.817 / 29.107 / 23.721 ms | Median **3410.650 → 26.817 ms**, approximately **127×** faster |
| Chinese/emoji 76-codepoint burst, 3 runs | 2410.523 / 2372.430 / 2448.607 ms | 27.883 / 42.240 / 28.062 ms | Median **2410.523 → 28.062 ms**, approximately **86×** faster |
| Additional sequential Chinese/emoji input | Stalled at draft `U0-你`; next `好` absent after 3 s | All 24 keys complete; p50 30.509 ms, p95 31.486 ms, max 31.902 ms | New native path also handles separately delivered committed Unicode |
| Exact bodies observed by independent Bob | All 9 common-workload messages | All 13 messages, including Unicode keys and cursor edits | No dropped/reordered payload accepted as success |
| Left/Delete/Home/End edit | Not a baseline capability | Exact server body `前原始🙂AC后` | Real native editing, not editor-model-only tests |
| Exit | Old stalled probe forcibly cleaned up | Ctrl-C returns zero, fixture cleaned up | No visible test application left running |

The original native loop always waits on network polling for 25 ms before reading
one `_getwch()` key. Buffered bursts are therefore serialized behind approximately
one OS scheduling interval per character. Its high-surrogate path also calls a
blocking second `_getwch()`. The measurements establish the user-visible bulk
input mechanism; the isolated legacy sequential-Unicode stall is recorded as an
additional observed limitation without claiming its exact CRT-internal cause.
The replacement removes this custom console/polling engine and uses native FTXUI
input and event-driven redraws instead.

### Exact artifacts and commands

Original client SHA-256:
`75768f37798a66ca6a2ef6a8677baa320879a6b4d23f60181683f4f3da4e4097`.
Preserved executable/source: `.cache/tui-baseline/chat-before.exe` and
`.cache/tui-baseline/tui-before.cpp`.
Measured replacement SHA-256:
`c54a6c52dfbf9d2616632557e01b49c15ebb8876bc81fdf55a31288109710ca2`.

```powershell
python scripts/terminal_probe.py `
  --server .cache/build/windows/src/server/chat-server.exe `
  --daemon .cache/build/windows/src/client/chat-daemon.exe `
  --client .cache/tui-baseline/chat-before.exe `
  --output .cache/tui-baseline/conpty-authoritative --rounds 3 --allow-failures

python scripts/terminal_probe.py `
  --server .cache/build/windows/src/server/chat-server.exe `
  --daemon .cache/build/windows/src/client/chat-daemon.exe `
  --client .cache/build/msvc/src/client/Debug/chat.exe `
  --output .cache/tui-current/final-color --rounds 3 --editing --max-burst-ms 500
```

Each output directory contains `result.json`, `conversation.vt`,
`conversation.screen.txt` and `conversation.svg`; the final capture is retained
separately. JSON includes executable and harness hashes and every latency sample.
The old result intentionally reports `baseline_failure_reproduced` after preserving
all common-workload measurements and the subsequent Unicode stall. New acceptance
must omit `--allow-failures` and return `passed`.

## Large-history acceptance and visual inspection

The final same-binary acceptance also seeds **102 real messages**, including six
**63,623-byte CJK bodies**, before attaching the UI. The initial replay includes
100 cached events under the daemon's 512 KiB replay budget. It then repeats every
native-input, exact-delivery and editing check under a **500 ms per-burst hard
budget**, rather than merely reporting timings:

```powershell
python scripts/terminal_probe.py `
  --server .cache/build/windows/src/server/chat-server.exe `
  --daemon .cache/build/windows/src/client/chat-daemon.exe `
  --client .cache/build/msvc/src/client/Debug/chat.exe `
  --output .cache/tui-current/final-color-stress --rounds 3 `
  --editing --stress-history --max-burst-ms 500
```

Result: passed; ASCII burst 24.628 / 26.432 / 26.546 ms; Unicode burst
27.417 / 27.918 / 27.452 ms; Unicode single-key p95 **31.125 ms**. All 13 sent
bodies were exact at Bob; clean Ctrl-C exit; no unknown CSI commands. Earlier
new-client Unicode p95 samples were around 59–64 ms during other development
activity; these runs remain in the raw artifacts. The quiet final acceptance
improves that distribution, but burst completion is the robust common-workload
performance result, not a blanket promise about every individual key.

For visual inspection, `--showcase` constructs a fresh **real** Alice/Bob exchange
of four normal Chinese messages, a Bob read receipt, unread messages in both
`lobby` and `research`, and an unsent emoji draft. It captures the same final
client through native ConPTY at 120×32 and 40×12; this is not manually authored
UI artwork or the repetitive benchmark transcript. Final captures:

- `.cache/tui-current/showcase-120x32/conversation.vt`, `.screen.txt`, `.svg`, `.png`.
- `.cache/tui-current/showcase-40x12/conversation.vt`, `.screen.txt`, `.svg`, `.png`.

Both PNG projections were rendered with hidden Edge and inspected. The wide
capture simultaneously shows a room sidebar with unread badges, grouped authors
and local times, own-message read state, online status, a stable composer and the
flat one-line Send action. The narrow capture collapses the sidebar, retains the
latest readable message and preserves the draft, Send and essential shortcuts.
Actual 80×24 and 40×12 native input/Unicode/editing runs also pass. The original
`.cache/tui-baseline/conpty-final/conversation.png` is retained only as old-layout
evidence; it predates the host-encoding correction and is not Unicode evidence.

The tool host sets `NO_COLOR=1` and `TERM=dumb`. The probe now supplies an explicit
Unicode environment block to the child: `NO_COLOR` removed, `TERM=xterm-256color`,
`COLORTERM=truecolor`. This advertises the genuine terminal's capabilities rather
than inheriting tool-log preferences. Production still respects a real user's
`NO_COLOR` preference. Final captures contain actual RGB foreground/background
SGR sequences. SVG backgrounds are painted before glyphs so the following cell
cannot cover the right half of a wide Chinese character.

## Native key-record and restoration fixture

```powershell
python scripts/terminal_probe.py `
  --fixture .cache/build/ftxui-only/ftxui-native-input.exe `
  --output .cache/ftxui-native-input-verified
```

Independent run returned zero and wrote:
`PASS split-surrogate repeat-count Chinese-batch mode-restoration`.
The fixture injects `WriteConsoleInputW` records into the native input queue:
high surrogate in one batch and low surrogate in a later batch, Chinese text,
and a repeat-count record. It compares pre-construction and post-destruction modes
and code pages. This complements the full application's ConPTY journey.

The repeat-count stage temporarily disables Windows VT input conversion so the
console host does not itself reencode repeat 7 as one event before FTXUI observes
it; the original record's repeat 7 is asserted before consumption. Other stages
retain the normal FTXUI path. An earlier fixture run found lazy color-capability
initialization left the output mode changed. The final test uses the same outer
`ConsoleGuard` as production, constructed before any FTXUI object and restored
after their destruction. It does not move baseline sampling forward to hide the
change. CTest exposes this as `windows_console_input` through the ConPTY runner.

## Platform references

- [Microsoft: Creating a Pseudoconsole session](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)
  describes pipes/STARTUPINFOEX and warns against synchronous input/output on one
  thread because full pipes can deadlock.
- [Microsoft native ConPTY sample](https://github.com/microsoft/terminal/blob/main/samples/ConPTY/EchoCon/EchoCon/EchoCon.cpp)
  documents the attribute-list and pseudoconsole-handle setup.
- [Microsoft Terminal input implementation](https://github.com/microsoft/terminal/blob/main/src/terminal/input/terminalInput.cpp)
  defines Win32 input record encoding, including UTF-16 units and key-up events.

Additional independent showcase metadata check:
`.cache/tui-current/showcase-final-metadata/result.json` reports
`lobby_messages_after_exit=4`, `unread_after_view={lobby:2,research:1}` and
`showcase_passed`. The draft therefore was not silently sent on Ctrl-C, and simply
viewing the conversation did not implicitly mark either room read.
