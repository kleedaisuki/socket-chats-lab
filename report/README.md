# Experimental report

The Chinese manuscript uses the unmodified `acmart` `sigconf,nonacm` layout,
XeLaTeX, and TeX Live's Fandol CJK fonts. It is a systems laboratory report,
not an ACM publication or a claim of a new distributed protocol.

## Build and check

Run from the repository root:

```console
python report/build.py
elan run leanprover/lean4:v4.33.1 lean report/formal/Ordering.lean
python report/check_metrics.py
```

The build requires XeLaTeX, BibTeX, `acmart`, `ctex`, TikZ and `xurl` (TeX Live
2026 was used). Metric calculations require SymPy (verified with 1.14.0).
No Python package is required to build the PDF. The build script uses only the
standard library. It keeps auxiliary and bibliography files under
`.cache/report/`, then copies the finished, checked-in deliverable to
`report/main.pdf`. Run BibTeX in the cache directory to respect TeX's normal
restricted-write policy rather than weakening it.
For editor/latexmk use, `report/.latexmkrc` selects XeLaTeX and the same output
directory when run from `report/` (or with `latexmk -cd report/main.tex`).

## Reading map

- `main.tex`: document assembly and typography.
- `sections/`: main narrative followed by reproducibility and formal appendices.
- `references.bib`: primary-source bibliography.
- `formal/`: Lean model, proved properties, assumptions and implementation map.
- `evidence.md`: durable code/measurement provenance.
- `review.md`: independent scientific review and acceptance follow-up.
- `check_metrics.py`: exact rational summaries and optional local-JSON checks.

The main text concerns ownership, commit/publication ordering, independent
client progress, development organization and discriminating experiments.
Appendices contain commands, resource limits, evidence identities and proofs.
Historical performance measurements are explicitly distinguished from current
manuscript checks. Product code and the user's services are not changed by
report authoring.

## Rendering

Every final page must be inspected after building. A system Poppler render can
be requested with:

```console
pdftoppm -r 120 -png report/main.pdf .cache/report/page
```

The local TeX Live Poppler lacks the Adobe-GB1 CMap data and omits embedded CJK
text while emitting a missing-language-pack error. This is a renderer setup
problem: the embedded Fandol fonts and searchable Chinese text are present.
Use a Poppler installation with `poppler-data`, or render with PyMuPDF:

```console
python -c "import pymupdf; d=pymupdf.open('report/main.pdf'); [p.get_pixmap(matrix=pymupdf.Matrix(1.5,1.5)).save(f'.cache/report/page-{i+1}.png') for i,p in enumerate(d)]"
```

PyMuPDF was used for the complete visual pass. Preview files are generated
artifacts and must remain under `.cache` or `.temp`.
