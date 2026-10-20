# Final report validation

Independent validation on 2026-10-21, after the final prose rebuild completed.
Artifact: `report/main.pdf`, 6 pages, SHA-256
`df3efaf18955fb9a7a2cff941195725033a5d9d68a40b15aabefcc0d3a87d03f`.

## Visual inspection

Rendered every page with PyMuPDF, rather than the local Poppler installation
whose missing Adobe-GB1 CMaps omit CJK text:

```console
python -c "import pymupdf; d=pymupdf.open('report/main.pdf'); [p.get_pixmap(matrix=pymupdf.Matrix(1.5,1.5)).save(f'.cache/report/validation-{i+1}.png') for i,p in enumerate(d)]"
```

Opened all six generated images using `view_image`, including a fresh complete
pass after the final rebuild. All Chinese glyphs are present and legible.
No clipped paragraphs, overlapping text, truncated table cells, or displaced
equation numbers were observed. The architecture diagram and arrows fit within
the full text width; its labels and caption are legible. Both main-text tables
and the appendix source-map table fit their columns. Section numbering proceeds
through 1–6 and appendices A–B, with consistent subsection levels. The B.1 state
transitions, numbered invariants, proof, counterexample and B.2 cursor equations
fit without collisions. The reference column ends early on page 4; this is
ordinary unequal final-column fill, not missing content. Main text ends before
the dedicated appendix page break.

The final `.cache/report/main.log` contains no matches for `Overfull`,
`Undefined control`, `Missing character`, or undefined-reference LaTeX warnings.
The remaining font-family and absent-affiliation-city warnings do not affect
the observed layout.

## Executable checks

```console
python report/check_metrics.py
elan run leanprover/lean4:v4.33.1 lean --version
elan run leanprover/lean4:v4.33.1 lean report/formal/Ordering.lean
```

Both checks exited **0**. Lean reports version **4.33.1** and accepted the file
without diagnostics. SymPy **1.14.0** reported exact arithmetic and available
trial records passing, with recomputed ratios: throughput 4.070453011, median
p50 reduction 77.24188670%, ASCII burst 127.1823843, Unicode burst 85.89990022,
and completion wakeup 122.9271864. These are manuscript/model checks, not a new
execution of the historical server or terminal benchmarks.

**Verdict:** the final six-page artifact passes the requested visual, pinned
Lean and symbolic-arithmetic checks. No TeX or production source was modified
by this validation.
