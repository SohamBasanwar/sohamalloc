# Presentation material (2026-11-04)

| File | What |
|---|---|
| `sohamalloc-how-it-works.pdf` | 25-slide deck (16:9): how sohamalloc works, its evaluation, limits and status |
| `sohamalloc-progress-report.pdf` | 17-page progress report: each change with its references, effect and negative results; comparison with other allocators; future steps |
| `src/` | The generators (`deck.py`, `report.py`, shared `common.py`) |

Rebuild (needs Python with `reportlab`, and the Lato and DejaVu fonts):

```sh
python3 docs/presentation/src/deck.py
python3 docs/presentation/src/report.py
```

Every number in both PDFs comes from the research log, `docs/IMPLEMENTATION_NOTES.md` or the data under `docs/research-log/data/`.
