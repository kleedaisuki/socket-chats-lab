# Independent report review

Reviewed 2026-10-21. Scope: `main.tex`, the five main-text sections, the evidence
ledger, and the current ordering model. This review does not modify the manuscript.

## Overall assessment

The manuscript is a strong systems laboratory report, organized around a useful
unifying judgment: share stable facts while separating lifetimes and progress.
Its contribution is the explanation and measured evolution of an implemented
system, not a claim to invent a new concurrency or distributed-consensus method.
The design, development-process, and evaluation sections support that scope.
The main text appropriately emphasizes decisions and their costs rather than
listing APIs. No decision-critical defect was identified in the reviewed text.

## Verified strengths

- The room gate spans `execute` through completion admission and notification
  (`src/server/main.cpp::schedule`), while the reactor consumes FIFO batches.
  PostgreSQL separately locks the room before allocation/insertion. The report
  correctly distinguishes these mechanisms, single-process ordering, network
  delivery, and crash recovery rather than conflating them.
- The closed-origin behavior, exact nonce retry, independent output offsets,
  logical-recipient accounting, and explicit foreground read action agree with
  inspected implementation and `docs/storage.md` / `docs/client.md`.
- R1/R2 and T1/T4/T5 are accurately reconstructed from the independent review
  records, including the later held-history cancellation race. Historical
  findings are presented as design evolution, not as current unresolved bugs.
- Fanout table values match retained JSON and `docs/storage.md`; the text
  correctly distinguishes the median of trial percentiles from a pooled
  percentile. The ConPTY results distinguish burst completion from individual
  keystroke latency and local draft visibility from independent peer delivery.
- The negative TCP_NODELAY result identifies source-identity confounding rather
  than attributing the earlier binary's behavior to one socket option.
- The Lean model proves safety over arbitrary finite reachable histories;
  it does not pretend to prove C++ refinement, liveness, exactly-once delivery,
  or actual human attention. The fresh-ID and ordinary-process-lifetime
  assumptions in `formal/README.md` make the correspondence understandable.

## Small revisions and acceptance criteria

1. **Keep hosted evidence revision-scoped.** The last evaluation paragraph says
   the final cross-platform matrix passed the relevant 13 suites. The ledger
   correctly identifies `eb24fa738f5eccf7785db6fb940a09f004171609`; carry that
   identity into the reproduction appendix and explicitly refer the main-text
   sentence to that recorded run. Acceptance: a reader cannot mistake historical
   hosted execution for a claim that this newly edited report tree was rebuilt
   on all five platforms.
2. **Expose the throughput denominator in the appendix.** The ACK/s figure
   includes authentication and thread startup, as the raw JSON notes state.
   The main text's workload description is sufficient, but reproduction must
   retain this definition. Acceptance: a reader knows it is not a steady-state
   server-only throughput measurement.
3. **Make evidence classes visible without defensive repetition.** The completion
   wakeup numbers survive in a dated project document, whereas fanout and ConPTY
   have retained raw JSON. The ledger already makes this distinction. Preserve
   it once in the appendix; no extra caveat is needed in every main-text claim.
4. **Complete the artifact check.** At initial review time the bibliography,
   reproduction appendix, and formal appendix were being authored. Acceptance:
   resolve all citations/references, execute the pinned Lean check and SymPy
   calculations, then inspect the rendered acmart PDF for table/diagram width,
   CJK glyph coverage, and appendix pagination. These are delivery checks, not
   objections to the current argument.

## Optional clarification

The description of visible-read is already limited to a user-confirmed room
prefix, not every preceding message having been displayed. Retain that explicit
distinction when integrating the formal appendix. A bounded initial history
window and a prefix cursor are compatible only with that interpretation.

## Review evidence inspected

`docs/architecture.md`, `docs/storage.md`, `docs/review.md`,
`docs/tui-review.md`, `docs/validation.md`; relevant `schedule`, `completions`,
`mark_read`, and `visible_message_max` implementations; current
`report/formal/Ordering.lean` and `README.md`;
`.cache/server/broadcast-after-2.json` and
`.cache/tui-current/final-color/result.json`, together with ledger tables and
retained document summaries. Execution of product tests is attributed to the
recorded validation runs, not claimed as a new independent rerun here.

## Reproduction appendix follow-up

The first integrated reproduction appendix now states the hosted code identity
and separates historical runs from report-time activity; item 1 is satisfied in
the appendix. Two concrete corrections were reported to the author:

- Point the new ConPTY measurement to
  `.cache/tui-current/final-color/result.json`, matching the table and ledger,
  rather than the earlier `conpty-authoritative/result.json` path.
- State that the ACK/s elapsed interval includes authentication/thread startup.

The alternating before/after trial order is supported by `docs/storage.md`
(encode-once fanout experiment); it is not an unsupported inference. The
reproduction commands and workload shape match the inspected driver arguments.

## Final appendix acceptance follow-up

The integrated appendices were rechecked against the current proof and source
paths. The ConPTY path, ACK/s denominator, hosted revision identity, and cache/
reconnect test filenames are corrected. Each cited implementation/test/script
path exists in the working tree.

`sections/formal.tex` faithfully states the transition guards, unchanged
components, four-part invariant, three consequences, and the four independent
cursor properties in `formal/Ordering.lean`. The database-only counterexample is
clearly a relaxed-gate scenario, not a legal trace of the proved transition
system. Its concrete list non-prefix claim is exactly what the cited Lean
theorem checks. The implementation correspondence and exclusions are stated
once, with no unsupported full-program verification claim.

This reviewer independently executed the pinned Lean 4.33.1 command successfully
(exit 0, no diagnostics), and `python report/check_metrics.py` successfully
rechecked exact SymPy arithmetic and available raw trial records. Numeric
ratios in the appendix agree with that output and are correctly described as
arithmetic summaries of measured values. No remaining logical/prose blocker
was identified. PDF typography and final rendered-page inspection remain owned
by the integrating author; successful compilation alone is not treated here as
proof of visual quality.
