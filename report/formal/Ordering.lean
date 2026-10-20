import Std

/-!
Single-room commit/admission/drain model and read-cursor algebra.
Only fresh successful sends enter this log; retries and failures do not append.
"Drained" means processed by the reactor, not received by a remote peer.
-/

namespace SocketChats

structure State where
  committed : List Nat
  admitted : List Nat
  drained : List Nat
  pending : List Nat
  deriving Repr, DecidableEq

def initial : State := ⟨[], [], [], []⟩

inductive Step : State → State → Prop where
  | commit (s : State) (id : Nat)
      (idle : s.pending = [])
      (fresh : ∀ old ∈ s.committed, old < id) :
      Step s { s with committed := s.committed ++ [id], pending := [id] }
  | admit (s : State) (id : Nat) (active : s.pending = [id]) :
      Step s { s with admitted := s.admitted ++ [id], pending := [] }
  | drain (s : State) (id : Nat) (tail : List Nat)
      (next : s.admitted = s.drained ++ id :: tail) :
      Step s { s with drained := s.drained ++ [id] }

def Invariant (s : State) : Prop :=
  s.committed = s.admitted ++ s.pending ∧
  s.drained.IsPrefix s.admitted ∧
  s.pending.length ≤ 1 ∧
  s.committed.Pairwise (· < ·)

theorem initial_invariant : Invariant initial := by
  simp [Invariant, initial, List.IsPrefix]

theorem step_preserves {s t : State} (h : Step s t)
    (inv : Invariant s) : Invariant t := by
  rcases inv with ⟨logs, hpref, bounded, ordered⟩
  cases h with
  | commit id idle fresh =>
      have equal : s.committed = s.admitted := by simpa [idle] using logs
      refine ⟨?_, hpref, by simp, ?_⟩
      · simp [equal]
      · apply List.pairwise_append.mpr
        exact ⟨ordered, by simp, by simpa using fresh⟩
  | admit id active =>
      refine ⟨?_, ?_, by simp, ordered⟩
      · simpa [active] using logs
      · exact hpref.trans ⟨[id], rfl⟩
  | drain id tail next =>
      refine ⟨logs, ?_, bounded, ordered⟩
      exact ⟨tail, by simpa [List.append_assoc] using next.symm⟩

inductive Reachable : State → Prop where
  | initial : Reachable initial
  | step {s t : State} : Reachable s → Step s t → Reachable t

theorem reachable_invariant {s : State} (h : Reachable s) : Invariant s := by
  induction h with
  | initial => exact initial_invariant
  | step _ transition ih => exact step_preserves transition ih

theorem drained_prefix_committed {s : State} (h : Reachable s) :
    s.drained.IsPrefix s.committed := by
  obtain ⟨logs, hpref, _, _⟩ := reachable_invariant h
  exact hpref.trans ⟨s.pending, logs.symm⟩

theorem drained_ids_increase {s : State} (h : Reachable s) :
    s.drained.Pairwise (· < ·) := by
  obtain ⟨suffix, equal⟩ := drained_prefix_committed h
  have ordered := (reachable_invariant h).2.2.2
  rw [← equal] at ordered
  exact (List.pairwise_append.mp ordered).1

theorem admission_gap_at_most_one {s : State} (h : Reachable s) :
    s.committed.length ≤ s.admitted.length + 1 := by
  obtain ⟨logs, _, bounded, _⟩ := reachable_invariant h
  rw [logs, List.length_append]
  omega

/- A database-only lock permits commit order [1,2] but admission order [2,1]. -/
theorem reversed_admission_not_prefix :
    ¬ ([2, 1] : List Nat).IsPrefix [1, 2] := by
  decide

def advance (cursor requested : Nat) : Nat := max cursor requested

theorem cursor_monotone (cursor requested : Nat) :
    cursor ≤ advance cursor requested := Nat.le_max_left _ _

theorem cursor_bounded (cursor requested roomMax : Nat)
    (oldValid : cursor ≤ roomMax) (requestValid : requested ≤ roomMax) :
    advance cursor requested ≤ roomMax :=
  Nat.max_le.mpr ⟨oldValid, requestValid⟩

theorem cursor_retry_idempotent (cursor requested : Nat) :
    advance (advance cursor requested) requested = advance cursor requested := by
  simp [advance, Nat.max_assoc]

theorem cursor_updates_commute (cursor first second : Nat) :
    advance (advance cursor first) second = advance (advance cursor second) first := by
  simp only [advance]
  omega

end SocketChats
