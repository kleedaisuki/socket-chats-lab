# Independent FTXUI client review

Date: 2026-10-19. Scope: `src/client/tui.cpp`, `session.hpp/.cpp`, `view.hpp/.cpp`, `console.hpp`, `plain.cpp`, `cmake/dependencies.cmake`, and `scripts/patch_ftxui.py`. Read the revised FTXUI decision in `docs/tui-redesign.md` and actual ConPTY baseline in `docs/interactive-latency.md` first. Superseded native Terminal/VT renderer sketches were not treated as current requirements. Static source review; no application services were launched and the user's stopped stack remained untouched. Root/perf own builds, integrated tests and native latency measurements. Line numbers refer to the reviewed snapshot.

## Findings

### T1 — P1: Metadata maximum is incorrectly reused as fetched-history progress

**Locations:** `src/client/tui.cpp:121-139` (`load_history`, `activate`), `:285` (live message updates maximum), `:307` (reconnect), `:338-343` (`room_metadata`), `:371-372` (marks loaded after any page).

Initial synchronization requests the oldest 200 messages (`after=0, limit=200`), then marks the room loaded after a single `history_end`. Separately, room-list metadata writes the latest server message ID into `Room.last_id`; live events also raise that field. Future activation/reconnect starts history after this maximum. Example: a room contains IDs 1..1000, list reports 1000, initial history yields 1..200, loaded becomes true, and the next synchronization starts after 1000. IDs 201..1000 never enter the conversation. The initial view shows old conversation rather than recent conversation, and the automatic cursor cannot repair it. A page may also be byte-limited before 200 records, so record count alone is insufficient as completion evidence.

**Remedy:** Separate room maximum from the contiguous fetched history cursor and synchronize by the returned `history_end` cursor until the chosen bounded catch-up/latest-view contract is satisfied. Do not advance fetched history merely from metadata or a live high ID. For a recent-tail UX, use an explicit tail-window API or bounded full catch-up instead of treating an oldest-page result as a latest-window result.

**Confidence:** High; direct state transitions. No process reproduction performed. Suggested journey: seed >200 messages and a byte-limited page, attach with an existing cache, interleave a newer live message, reconnect, and assert the recent conversation and any promised history are neither skipped nor duplicated.

### T2 — P2: Local daemon disconnect leaves optimistic sends pending forever

**Locations:** `src/client/tui.cpp:291-318`, especially the `daemon-offline` branch; `src/client/session.cpp:144-163`.

`Session::disconnect` clears outgoing buffers and emits one terminal `daemon-offline` status, not per-request errors. The UI branch changes only connection/notice, leaving `requests_` and all optimistic `Delivery::pending` messages unchanged. If the daemon exits after Enter but before ACK, the message remains labelled sending indefinitely although no further reply can arrive on that Session. The actual result can be uncertain if the daemon forwarded it before dying.

**Remedy:** On terminal local-session failure, settle all outstanding send models as uncertain (or failed only when provably unsent), clear outstanding requests, preserve the editable draft, and keep the result visible. Do not blindly retry an uncertain send with a new nonce.

**Confidence:** High; direct terminal path. Suggested journey: inject a send, terminate only a hidden fixture daemon before ACK, verify explicit uncertain/failed state rather than perpetual sending.

### T3 — P2: The 8 MiB model budget cannot evict failed/uncertain ID-zero messages

**Location:** `src/client/tui.cpp:409-425` (`trim`), combined with `result` retaining failed optimistic messages and `sort_messages` capping only per-room count.

Eviction considers only a room's front item when its ID is nonzero. A room containing failed sends with ID zero is never eligible, so the loop exits with bytes still above 8 MiB. A user can repeatedly submit legal 64 KiB messages while storage operations return errors, accumulating up to 500 ID-zero failed items in one room (~32 MiB of body alone); multiple rooms multiply the retained memory. Pending sends should not be silently discarded, but terminal failed items do not justify ignoring the advertised global bound.

**Remedy:** Enforce a single explicit byte/count admission or eviction policy that covers committed and terminal failed/uncertain entries. Reserve room for active pending items; reject additional submissions with draft retained if all retained entries must remain. Any eviction that removes a failed send should preserve a useful recovery affordance instead of silently losing its contents.

**Confidence:** High on budget violation; no allocation stress run. Test byte bounds with both committed and ID-zero terminal messages, not only live server history.

## Performance lead requiring measurement, not a proven defect

`view.cpp::timeline` bounds candidate message count to `rows+4`, but `message`/`flow` still construct DOM for complete texts, including many oversized messages entirely above the viewport. `flow` uses one node per non-ASCII glyph: a legal 64 KiB Chinese message creates roughly 21k glyph nodes. Existing short-body render benchmarks do not establish keystroke latency under large-body history. Ask the real ConPTY/perf workload to include such a room before claiming latency robustness. If measured expensive, bound layout work by visible content/bytes or cache immutable message layout, without silently dropping message text.

`Session::send` performs protocol encoding/compression on the caller/UI thread before entering its output mutex. Socket operations are isolated correctly; the stronger claim that all outgoing transport processing occurs off the UI thread would be inaccurate. Its practical cost should be judged by measurements rather than assumed blocking severity.

## Positive source checks and limits

- Session's joined worker and last-declared Session member ensure notification callbacks cannot outlive `screen_`; constructor notifications only post events, and the loop starts after construction.
- UI model mutations occur on the UI/main thread, not in the Session callback. Socket poll/read/write is owned by Session's worker. Wake sockets avoid a fixed per-character socket wait.
- Outbound budget counts the active partial write as well as queued frames. Input backpressure reserves room for terminal status and uses bounded wire reads/decompression reserve. Notification coalescing and `take()` clearing share the same mutex, avoiding a straightforward lost-notification race.
- ACK, history and live messages merge by authoritative ID or author+nonce; committed items sort by ID and optimistic items remain at the tail. Source review found no unconditional duplicate append on the normal ACK/broadcast path.
- Explicit-read uses actual view layout rather than every received ID; hidden/help/tiny views return zero. No automatic read is triggered by a network receipt.
- Offline submission refuses to clear the current draft; per-room draft switching preserves text. Existing failed/uncertain pending status handling has the T2/T3 gaps above.
- Console guard is constructed before FTXUI and destroyed after it, restoring original modes/code pages beyond upstream teardown. FTXUI source patch is pinned, hash-verified, exact-context and idempotent; no unrelated networking dependency was introduced.
- Plain mode retains its separate piped behavior and UTF-8 line limit; no native keyboard responsiveness claim was inferred from this path.
- Native Windows key-repeat/surrogate behavior, FTXUI drawing appearance, IME experience, terminal restoration and latency require actual fixture evidence. This source review does not substitute for those measurements.

## Follow-up review after root fixes

The subsequent `tui.cpp` snapshot has separate `sync_cursors_`, per-request `after`/`target`/synchronize context, a bounded initial recent global-ID window, and continuation until the returned history cursor reaches the snapshot target. Live events/cache replay/list maxima do not advance the fetched cursor. Explicit read is deferred while a history request is pending. **T1 is resolved by static source inspection for initial recent-tail synchronization.** This is a deliberately bounded recent window, not a promise to initially fetch all historical messages.

Terminal `daemon-offline` now settles pending model entries to uncertain and clears outstanding request bookkeeping without clearing the editable draft. **T2 is resolved by static source inspection for local daemon/socket loss.**

`trim` now searches all nonpending messages, including ID-zero terminal failures/uncertain sends, for time-based global eviction. Submission reserves the upcoming message bytes before queueing and requests have an admission cap of 128. **T3 is resolved by static source inspection.** The body/author/nonce budget is not a total-process RSS bound; drafts, editor history, transport and view cache have their own finite budgets.

The view now caches wrapped lines and constructs only viewport-budget rows, rather than rebuilding complete glyph DOMs for oversized bodies on each key. This addresses the earlier performance mechanism concern in source; actual timing remains owned by perf/root.

### T4 — P2: Remote loss still advertises uncertain delivery as definite failure

**Locations:** `src/client/daemon.cpp::disconnected` (snapshot lines 165-169), `src/client/tui.cpp::result` send-error branch.

The actual daemon emits error code `offline` for outstanding forwarded requests when its upstream connection disappears, with explanatory text that delivery may be uncertain. The rewritten UI recognizes only `delivery_unknown` as uncertain; `offline` makes the optimistic message failed and displays a notice claiming it was not queued. A send can already be committed when the upstream socket is lost before ACK. This presentation invites an unnecessary fresh-nonce retry and duplicate visible messages.

**Remedy:** Distinguish known-not-queued `offline` errors from indeterminate forwarded sends at the daemon boundary. Emit the agreed `delivery_unknown` code for the latter and retain `offline` for commands rejected before upstream submission. Keep plain-mode explanatory text useful.

**Confidence:** High; checked the actual daemon source, not inferred from prose. Root notified immediately.

### T5 — P2: Older-history merge immediately discards fetched messages once the 500-item tail is full

**Locations:** `src/client/tui.cpp::older_history`, `message`, `sort_messages`.

`older_history` now fetches an earlier ID window correctly. However every individual received item invokes `sort_messages`, which always truncates to the newest 500 items. If the room already contains 500 retained newer messages, each older retrieved message is immediately erased before rendering. `/history` can report that older history was merged while none becomes available. Repeating it fetches the same window because the oldest retained item never changes.

**Remedy:** Make the finite retention window direction-aware while explicitly browsing older pages, evicting the opposite end or retaining a separate bounded page. At minimum communicate a real retention boundary rather than successful loading. Preserve pending outgoing entries if moving the historical window.

**Confidence:** High; direct sort/truncate path. Suggested journey: retain 500 committed messages, request earlier history, assert that the older sentinel appears in the browseable model rather than disappearing on receipt.

Root reports POSIX state journeys (300 preseeded messages, global-ID gaps and held-ACK daemon EOF) and full Linux/MSVC suites passing. Those tests were not rerun by this reviewer; they cover the original fix paths, not independently establish T4/T5 resolution.

## Final focused source recheck (T4/T5)

**T4 resolved.** `src/client/daemon.cpp:165-169` now emits `delivery_unknown` when disconnecting upstream with outstanding requests. The known-not-forwarded path in `local` retains `offline` (`:326-331`). `src/client/tui.cpp:469-472,494-499` maps the former to `Delivery::uncertain` plus avoid-duplicate guidance and the latter to definite rejection. This closes the original code mismatch; no diagnostic string matching is used.

**T5 original retention defect resolved.** `history_page` enters `older_windows_` only after accepting the request (`tui.cpp:147-156`). `sort_messages` keeps the finite 500-message bound while evicting the newest nonpending item in older mode (`:347-358`). The global byte trim also prefers evicting inactive rooms, then newer settled entries when the selected room is in older mode (`:529-561`). `latest` clears older mode and its fetched cursor, then schedules the bounded recent window (`:188-200`); PgDn at bottom and a new send call this path. Thus the formerly discarded older messages can enter a bounded historical window and normal return-to-live refetches the latest messages.

**Focused test evidence examined:** `tests/tui_window_journey.py::window_and_delivery` seeds 500 cached messages (301..800), verifies live/history deduplication, fetches 102..300, asserts newest eviction, pages to sentinel 102, returns to 800 and verifies a fresh recent request. It also injects `delivery_unknown`, asserts uncertain rather than failed presentation, hides raw backend diagnostics and confirms an editable draft afterward. `tests/tui_state_journey.py:101-103` invokes this journey. Root reports final Windows MSVC, Windows MinGW and WSL Linux suites all passing 13/13. This reviewer inspected source/test assertions, did not rerun those suites, and did not launch services.

### Conditional follow-up: return-to-live during a multi-page older request

A narrower race remains in the reviewed frozen snapshot: `latest()` can erase older mode while an older byte-short page is still in flight, leaving `sync_needed_` to restore the recent window later. The old request's `history_end` still carries `operation.older=true`, so continuation through `history_page(..., older=true)` re-enters older mode. Once this chain finishes, the deferred recent request can execute under older eviction policy and drop its newest messages again. The existing window fixture waits for the complete single older page before PgDn, so it does not cover cancellation during multi-page catch-up.

Root was notified. A coherent narrow correction is to treat the cleared older-mode flag as cancellation of that request chain, not re-enter it from a stale request context; deferred latest synchronization can then proceed. Suggested discriminating fixture: hold a byte-short older page, issue return-to-bottom, release its `history_end`, and verify no older continuation reactivates retention before the latest sentinel is restored. This is a conditional P2 follow-up, not a P1 delivery blocker or a reopening of the fixed ordinary 500-message merge path.

### Final disposition: in-flight older-page cancellation resolved

The final source snapshot guards history continuation with `browsing_older = operation.older && older_windows_.contains(operation.room)` (`src/client/tui.cpp:484-493`). Both older-page continuation and the older-loaded notice now require the current browsing intent, not merely the stale request context. `latest()` clears that intent, so the held older response cannot re-enter the historical eviction policy; deferred recent synchronization then proceeds normally. Static reinspection confirms the reported cancellation mechanism is corrected.

Validator executed the deterministic regression in `tests/tui_window_journey.py::partial_page_return` (held `history_end(150)`, bottom PgDn, then release). The saved pre-fix Linux binary failed specifically on the unexpected continuation after 150. The rebuilt Linux binary passed the full expanded `tui_state_journey`, including exactly two recent fetches and retention of IDs 800 and 801 after cancellation. Execution evidence and the preserved failing-binary reference are recorded in `docs/terminal-validation.md`, section “Return-to-live while a short older page is still in flight”. This reviewer inspected the current guarded source and fixture assertions; execution is attributed to the validator, not independently rerun here.

**Final focused review status:** T1–T5 and the conditional cancellation follow-up are resolved within the reviewed contracts. No remaining concrete blocker was identified in this focused recheck. Production source was not modified by the reviewer, no user services were started, and this review document is now frozen for the root's coordinated commit.
