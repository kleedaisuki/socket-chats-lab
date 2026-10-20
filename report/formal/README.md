# Checked room-ordering and cursor model

`Ordering.lean` is a self-contained Lean 4 proof using `Std`; no Mathlib,
downloaded dependencies, admitted proofs, or user axioms are required. It was
checked with Lean **4.33.1** on 2026-10-21. It proves an invariant for arbitrary
finite transition histories, rather than enumerating traces up to a fixed depth.
The bounded part of the abstraction is one committed, not-yet-admitted fresh
event per room.

## Reproduce

From the repository root, with that Lean toolchain installed:

```console
elan run leanprover/lean4:v4.33.1 lean --version
elan run leanprover/lean4:v4.33.1 lean report/formal/Ordering.lean
```

Successful checking exits with status zero and prints no diagnostics. Optional
compiled proof output belongs under `.cache/`, not beside the source:

```console
elan run leanprover/lean4:v4.33.1 lean -o .cache/report/formal/Ordering.olean report/formal/Ordering.lean
```

Create `.cache/report/formal/` before using the optional output command. The
toolchain is specified explicitly so the repository does not impose a Lean
dependency on ordinary C++ builds.

## Model and implementation correspondence

Fix one `(tenant, room)` during the ordinary lifetime of one server. Project
execution onto **fresh, successful send events** for that room. Authentication,
failed sends, exact nonce retries, history/read commands, unrelated rooms,
socket-byte writes, and scheduling stalls do not append to this projection.
Each message ID is an arbitrary natural number; contiguous IDs are not assumed.

| Model component | Meaning | Implementation anchor |
| --- | --- | --- |
| `committed` / C | Fresh messages whose storage operation has committed | `Postgres::send` in `src/server/postgres.cpp`, `tx.commit()` before return |
| `pending` / P | One committed fresh message whose completion has not yet entered the shared queue | Worker between `execute()` and `completed_.push_back()` |
| `admitted` / A | Completion-queue admission order, projected onto fresh room messages | `Server::schedule` in `src/server/main.cpp` |
| `drained` / D | Completion processing order in the reactor, projected onto those messages | `Server::completions`, FIFO `done` iteration |
| `commit` transition | Append an ID while no earlier committed event remains pending | Room-stripe lock spans execution through completion admission |
| `admit` transition | Append the pending ID to A and clear P | `completed_.push_back` occurs before publication-lock destruction |
| `drain` transition | Append the next A element to D | Reactor drains a FIFO batch without reordering its entries |

The in-memory backend supplies the same model's `committed` operation boundary
with its mutex-protected successful insertion, without durable-storage meaning.

The model assumes fresh IDs strictly exceed all earlier committed IDs of that
room. The PostgreSQL implementation acquires the room row lock before its
`INSERT ... RETURNING id`; the schema creates the default `bigserial` sequence.
This correspondence assumes its default sequence behavior is preserved: no
external sequence rewinds, altered caching/allocation semantics, or concurrent
writers bypassing the server contract. The memory backend increments its next
ID while holding its storage mutex. Sequence gaps are allowed and have no effect
on the proof. PostgreSQL documents both atomic sequence allocation and gaps after
aborts in [Sequence Manipulation Functions](https://www.postgresql.org/docs/18/functions-sequence.html).

The application gate has 256 mutex stripes, but this proof projects onto one
room. Stripe collisions add waiting; they do not create a transition that
bypasses the gate. The global queue may contain other rooms' completions between
two projected entries, without changing their relative order.

## Proved statements

`Invariant` is the conjunction

```text
C = A ++ P
D is a prefix of A
length(P) <= 1
C is pairwise strictly increasing
```

`initial_invariant`, `step_preserves`, and `reachable_invariant` prove this
conjunction by induction over all reachable states. The three transition cases
have a direct interpretation: commit appends only to C and P; admission moves
the pending event into A; reactor processing consumes A in order.

The useful consequences are:

- `drained_prefix_committed`: D is a prefix of C. No fresh completion can
  overtake an earlier committed fresh completion of the same room in reactor
  processing order.
- `drained_ids_increase`: reactor-processed fresh room IDs are strictly
  increasing, including when allocated IDs contain gaps.
- `admission_gap_at_most_one`: `length(C) <= length(A) + 1`. Completion
  backpressure can retain the gate, but cannot permit a second committed fresh
  room event to bypass the first event's admission.
- `reversed_admission_not_prefix`: `[2,1]` is not a prefix of `[1,2]`. This is
  the checked log-level counterexample for releasing the application gate at
  commit: worker 1 commits 1 and pauses; worker 2 commits and admits 2; worker 1
  subsequently admits 1. A database transaction lock alone does not constrain
  the post-commit application queue.

The list model is a safety argument at the application publication boundary, not
a proof that the full server implements linearizability. Its use of a sequential
abstract log follows the specification style introduced by Herlihy and Wing,
[Linearizability: A Correctness Condition for Concurrent Objects](https://cs.brown.edu/~mph/HerlihyW90/p463-herlihy.pdf),
ACM TOPLAS 12(3), 1990, pp. 463–492.

## Cursor algebra

The independent `advance(r,x) = max(r,x)` model matches PostgreSQL's `GREATEST`
update and the in-memory read cursor. Its four checked statements establish:

1. The old cursor never decreases (`cursor_monotone`).
2. If both old and requested cursors are at most the committed room maximum M,
   the updated cursor is also at most M (`cursor_bounded`).
3. Repeating a request is idempotent (`cursor_retry_idempotent`).
4. Applying two requests in either order yields the same cursor
   (`cursor_updates_commute`).

The bound assumes the existing cursor is valid and the requested value is
validated against a room maximum serialized with sends; the implementation
locks the room row before validation and the `GREATEST` update. These properties
do not equate cursor distance with message count, and they do not assert that a
user has displayed every earlier historical message. An initial history window
can omit earlier messages by design.

## Verification boundary

Theorems concern the stated transition relation and cursor function. The links
to C++ are source-level correspondence, supported separately by
`tests/server_publication.py` and `tests/server_storage.cpp`; this is not a
compiler-checked refinement of the implementation. Reactor processing is not
network receipt or terminal display. Slow-client limits, disconnect, and
shutdown can prevent byte delivery; the model claims neither liveness nor
exactly-once network delivery. The ordinary-lifetime model excludes crash and
shutdown transitions, for which committed database history and nonce retry
provide recovery rather than guaranteed queued-event delivery.

No symbolic computation is used in this proof. Consequently no SymPy derivation
is needed here.
