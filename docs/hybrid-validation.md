# Windows / WSL live-stack verification

## Scope and expected behavior

The manual journey runs native Windows `chat.exe` and `chat-daemon.exe` against
`chat-server` and PostgreSQL in WSL. It is intentionally not registered in generic
CTest/CI: it controls the dedicated local hybrid server and requires Windows/WSL.
It never drops the database, stops PostgreSQL, or stops account daemons.

The expected behavior comes from `docs/client.md` and `docs/storage.md`: reception,
display and cache replay do not imply read; only explicit `/read` advances the
room cursor. Acknowledged messages and read cursors survive server restart.
Existing IPC connections and TUI processes survive upstream reconnect. Offline
commands fail visibly and are not queued. Loopback listeners remain loopback.

## Reproduce

After starting the dedicated stack with `scripts/hybrid.py`, run from native
Windows Python in the repository root:

```console
python tests/hybrid_journey.py
```

The harness uses a new `check_<random>` room on every run, so assertions do not
assume an empty database. It starts two real C++ TUI processes with `--plain`,
writes commands to their stdin and reads stdout concurrently. This verifies the
actual CLI rendering and command path, not just synthetic wire clients. Separate
SCL/1 IPC observers query authoritative unread counts and inspect timestamps and
receipts. Their sockets remain connected throughout the upstream outage.

| Step | Expected observable result |
| --- | --- |
| Alice TUI sends Chinese, emoji and combining Unicode | Bob TUI displays exact text as live; valid UTC millisecond server timestamp; Bob unread 1, Alice unread 0 |
| Bob TUI `/read` | Alice TUI receives Bob read receipt; Bob unread 0 |
| Bob TUI replies in Chinese/emoji | Alice TUI displays exact text; Alice unread 1; history contains exactly both messages |
| Stop only the dedicated WSL chat server | Both original TUIs and observers see offline; new TUI replays cached text labeled cached/offline; offline sends fail explicitly |
| Restart WSL chat server against the same PostgreSQL database | Original IPC sockets reconnect; fresh direct server connection restores exact history and read state; Alice unread remains 1 and Bob 0 |
| Alice TUI `/history`, `/read`; Bob sends again | History rendering, persisted read receipt and new live delivery work; explicit read clears new unread; no duplicate or rejected-offline message in history |
| End | Both daemon PIDs and original TUI PIDs unchanged during test; harness TUIs exit cleanly; account daemons/server/PostgreSQL stay running |

Generated evidence is under `.cache/hybrid/verification/`: `result.json`,
`alice.tui.log`, `bob.tui.log`, and `alice-offline.tui.log`. These files contain no
authentication secrets and are not committed. They include test message content.

## Coverage boundary

`--plain` exercises the real native frontend process and UTF-8 display path but
not interactive Windows console key events, colors, glyph widths, or terminal
restoration. Interactive console checks are a separate step. The deliberate
outage restarts only chat-server: PostgreSQL remains running, so this demonstrates
application-level database persistence, not power-loss durability. This is a
local plaintext loopback deployment, not public-Internet readiness or a load test.

## Executed result: 2026-10-19

Native Windows command `python tests/hybrid_journey.py` exited **0** in **6.84 s**.
The recorded journey ran 14:36:55–14:37:01 UTC (22:36:55–22:37:01 Singapore/China
time), against WSL2 kernel `6.18.33.2-microsoft-standard-WSL2` and the dedicated
PostgreSQL-backed hybrid deployment. All six journey milestones passed.

| Observation | Actual result |
| --- | --- |
| Test room | `check_a245262eff0c` |
| Two persistent Windows test TUI process IDs | Alice `20836`, Bob `34548`; both survived upstream outage and exited cleanly at harness completion |
| Windows daemon process IDs before/after | Alice `1260`, Bob `17848`, unchanged |
| First message | ID `1`, Alice: `Windows 发给 WSL：你好，Klee 🌸 café é` |
| Server timestamp | `1792420615435` UTC milliseconds; TUI rendered `2026-10-19 14:36:55 UTC` |
| Bob unread/read transition | `1` after display, explicit TUI `/read` produced receipt `[room, bob, 1]`, then `0` |
| Reverse message | ID `2`, Bob: `WSL 经 Windows 返回：收到啦 🐈 中文双向验证`; Alice unread `1` |
| Outage | Original TUIs/observers reported offline; third TUI replayed cached text with cached/offline labels; offline send failed and was never queued |
| After server restart | Same IPC sockets reauthenticated; fresh server connection returned exact IDs/text `1,2`; Alice unread `1`, Bob unread `0` persisted |
| After reconnect | Alice TUI history and `/read` succeeded; Bob sent ID `3`, `重连后继续发送 🌷 check_a245262eff0c`; Alice received live, unread `1`, then `/read` cleared to `0` |
| Final authoritative history | Exactly IDs `1,2,3`, no missing/duplicate or rejected-offline message |

The final server and both daemon processes were left running. PostgreSQL was
never stopped or recreated by the test. There were no verification failures and
no product-code changes were needed for this journey.

## Interactive deployment left available

After the automated journey, root opened a new Windows Terminal window split
into Alice and Bob. Windows process metadata confirmed two native MSVC
`chat.exe` processes, IDs `27572` and `30712`, attached to their respective
`.cache/hybrid/alice` and `bob` state directories, without `--plain`.
This confirms interactive frontends were launched; it is not a screenshot-based
assessment of terminal glyphs or colors.

The active WSL chat server was PID `1213`, bound to `127.0.0.1:7777`, with the
existing PostgreSQL 18/main cluster online on WSL port 5432. The account daemons
remained native Windows processes `1260` and `17848`. A PostgreSQL query in WSL
confirmed test message rows `1,2,3` and lobby welcome messages `4,5` in the
dedicated database. User-facing welcome messages were published to the open
frontends without marking them read.

## Operation and infrastructure notes

- `python scripts/hybrid.py start` starts/reuses only this deployment. Existing
  PostgreSQL is checked, not reconfigured. The dedicated database role is not a
  superuser. No existing user database is migrated or dropped.
- Configuration and user tokens are stored only under `.cache/hybrid/`, protected
  by a native current-user-only Windows DACL. Root checked the directory and
  both configuration/runtime files with the same binary-SID ACL oracle used by
  the client suite. Secrets are not passed on command lines or put into logs.
- Windows helpers are detached without visible helper consoles. Account TUI
  windows are opened only by the explicit `open` command. `attach --user alice`
  attaches to an existing daemon in the caller's terminal.
- `status` reports process liveness, the server's Linux PID and Windows TCP
  reachability. Complete authentication and data-path checks belong to `verify`;
  a TCP connection alone is not described as proof of application readiness.
- `stop` retains PostgreSQL, the dedicated database, and frontend cache. Stored
  PIDs are checked against expected executable images before signaling. It does
  not use global process-name termination or stop the shared database cluster.
- A deployment-specific WSL behavior was observed: a detached `wsl.exe` child
  can still connect Linux stdout/stderr to a PTY instead of its Windows log
  handles. `_backend` therefore redirects file descriptors inside Linux before
  exec'ing chat-server. This made readiness logs independent of the console
  bridge. The Linux PID is the authoritative backend lifecycle identifier.
- These are foreground-style applications managed by local Python tooling, not
  installed auto-start Windows/systemd services. Rebooting or `wsl --shutdown`
  requires starting this stack again. No TLS or network exposure was added.
