# axil-scribe Usage Guide

`axil-scribe` is a pure-C, offline-first, domain-agnostic document OCR, structured field extraction, and human-in-the-loop (HITL) verification workbench built on the `tty.pt` stack. All domain knowledge (prompts, templates, constraints, dictionaries, preprocessing presets, and UI labels) lives in declarative **Domain Packs** (`pack.json`).

---

## 1. Instant Browser Demos

### Zero-Model Offline Demo (`--demo`)

Test the complete upload → 2-pass conflict detection → split-screen review → accent-sensitive search → CSV/JSON/transcript export workflow without running a VLM:

```sh
./scripts/scribe-server --demo
```

Then open **http://127.0.0.1:8095/** in your browser:

1. Upload one of the auto-generated synthetic sample scans from `/tmp/axil-scribe-demo/samples/` (e.g., `01_confirmation_clean.png`) using the **Confirmation Certificate** template.
2. Observe the split-screen verification workbench: `sponsor` is automatically flagged with **CONFLICT** (`Joao Pereyra` vs `João Pereira`) with an advisory dictionary suggestion and bounding-box highlight.
3. Correct/confirm fields, add notes, and click **Confirm & Verify**.
4. Test accent-sensitive search at `/search` (`Conceição` vs `Conceicao`, with optional diacritic folding) and download open exports at `/export.csv`, `/export.json`, or `/entries/<id>/transcript`.

### Live Real-Handwriting Demo (`--handwriting-demo`)

Download 5 public-domain 1786–1894 Portuguese handwritten parish registers and launch with `packs/pt-archival-handwriting` and the 2-pass + zoomed-crop escalation pipeline against local `axil-auren`:

```sh
./scripts/scribe-server --handwriting-demo
```

> Both demo flags use isolated `/tmp/axil-scribe-*` directories so they never touch your persistent data directory.

---

## 2. Production / Offline Launch & Domain Packs

```sh
# Build and install rootless into ~/.local
make
make PREFIX="$HOME/.local" install

# Generic document & invoice workbench
scribe-server --pack generic

# 17th-20th c. Portuguese archival cursive registers
scribe-server --pack pt-archival-handwriting

# Custom pack directory anywhere on disk
scribe-server --pack /path/to/my-pack
```

### Environment Variables

| Variable | Default | Purpose |
|---|---|---|
| `SCRIBE_PORT` | `8095` | HTTP listening port |
| `SCRIBE_PACK_DIR` | *(unset; CLI `--pack`)* | Declarative Domain Pack directory containing `pack.json`, `templates/`, and optional `dictionaries/*.tsv` |
| `SCRIBE_DATA_DIR` | `~/.local/share/axil-scribe/data` | Immutable scans (`scans/`), `libcorm` DB, and `liblemma` index |
| `SCRIBE_TEMPLATES_DIR` | `<pack>/templates` or `templates/` | Explicit template JSON directory override |
| `SCRIBE_WASM_PATH` | auto-detected `htdocs/scribe_ux.wasm` | Progressive isomorphic WASM UX bundle |
| `SCRIBE_DICT_PATH` | *(unset)* | Optional extra TSV advisory dictionary (`category\tcanonical\tyear_min\tyear_max`) loaded in addition to `<pack>/dictionaries/*.tsv` |
| `SCRIBE_AUREN_HOST` | `127.0.0.1` | `axil-auren` (or OpenAI-compatible VLM) host |
| `SCRIBE_AUREN_PORT` | `4242` | `axil-auren` port |
| `SCRIBE_MODEL` | `qwen2.5-vl-7b` | Primary VLM model identifier |
| `SCRIBE_MODEL2` | *(unset)* | Optional secondary model for 2-pass conflict routing |
| `SCRIBE_ESCALATE_MODEL` | *(unset)* | Optional stronger VLM used for zoomed-crop arbitration of conflicted/uncertain fields and `?deep=1` re-extraction |
| `SCRIBE_ENHANCE` | `0` (`1` if pack uses `handwriting`/`document` or set explicitly) | Send a non-destructive contrast-normalized copy alongside the original scan (zero binarization) |
| `SCRIBE_TIMEOUT_MS` | `180000` | VLM HTTP socket timeout in milliseconds |
| `SCRIBE_MAX_BODY` | `33554432` | Maximum upload body size in bytes (32 MiB) |
| `SCRIBE_MOCK_JSON` | *(unset)* | Optional mock JSON file or literal for offline demos/CI |

---

## 3. HTTP API & `curl` Examples

All HTML routes support No-JS browsers natively. Send `Accept: application/json` (or `?format=json`) for structured JSON responses.

```sh
# Upload a scan (raw binary or multipart/form-data) and run extraction
curl -sS -X POST \
  -H 'Accept: application/json' \
  -H 'Content-Type: image/png' \
  --data-binary @scan.png \
  'http://127.0.0.1:8095/ingest?template_id=invoice_starter&book_ref=BATCH-2026&page_num=1&extract=1'

# List entries needing human review
curl -sS -H 'Accept: application/json' \
  'http://127.0.0.1:8095/queue?status=needs_review'

# Inspect an entry
curl -sS -H 'Accept: application/json' \
  'http://127.0.0.1:8095/entries/<ENTRY_ID>'

# Trigger high-accuracy deep re-extraction (?deep=1)
curl -sS -X POST -H 'Accept: application/json' \
  'http://127.0.0.1:8095/entries/<ENTRY_ID>/extract?deep=1'

# Submit human verification
curl -sS -X POST \
  -H 'Accept: application/json' \
  -H 'Content-Type: application/json' \
  -d '{"action":"verify","reviewed_by":"reviewer-1","fields":{"currency":"EUR"},"marginal_notes":"Verified"}' \
  'http://127.0.0.1:8095/entries/<ENTRY_ID>/verify'

# Add a second entry on a multi-record page (1 page -> N entries)
curl -sS -X POST \
  -H 'Accept: application/json' \
  'http://127.0.0.1:8095/pages/<PAGE_ID>/entries?template_id=generic_record'

# Search (accent-sensitive exact by default; folded=1 for diacritic-insensitive)
curl -sS -H 'Accept: application/json' \
  'http://127.0.0.1:8095/search?q=Concei%C3%A7%C3%A3o&folded=0'

# Export open CSV (includes transcript + raw/verified field columns) and JSON
curl -sS 'http://127.0.0.1:8095/export.csv?status=verified' -o export.csv
curl -sS 'http://127.0.0.1:8095/export.json?status=verified' -o export.json
```

---

## 4. Authoring Domain Packs & Templates (Zero C Recompilation)

A **Domain Pack** is a self-contained directory passed via `--pack <dir>` or `SCRIBE_PACK_DIR`:

```text
my-pack/
├── pack.json              # Persona, extraction rules, preprocessing, date bounds, UI labels
├── templates/
│   └── my_template.json   # Structured field schemas & declarative constraints
└── dictionaries/
    └── terms.tsv          # Optional read-only advisory suggestions
```

### `pack.json` Schema

```json
{
  "id": "my-domain",
  "label": "My Document Pack",
  "system_persona": "You are a precise document transcription and structured extraction specialist.",
  "extraction_guide": "1. Transcribe verbatim.\n2. Use [?] for uncertain characters.",
  "uncertainty_markers": "[?],[ileg,[illeg",
  "preprocess_preset": "document",
  "transcript_first": true,
  "min_year": 1900,
  "max_year": 2100,
  "ui": {
    "source_label": "Batch / Folder",
    "notes_label": "Reviewer Notes",
    "transcript_label": "Full Transcript",
    "deep_btn_label": "Deep Re-Extract"
  }
}
```

- `preprocess_preset`: `"none"` (EXIF auto-orient only), `"handwriting"` (soft contrast-stretch + mild unsharp, zero binarization), or `"document"` (contrast normalization for photographed forms/receipts).
- `transcript_first`: when `true`, prompts the VLM to transcribe the full visible text into `"transcript"` before filling structured `"fields"` (strongly improves cursive and dense-layout accuracy).

### Template JSON Schema

Supported field `type` values: `text`, `date`, `number`, `currency`, `code`, `enum`, `person`, `place`, `notes`.

Optional per-field properties:
- `format_hint` — short format guidance shown in both the VLM prompt and HTML input placeholder (e.g. `"ISO YYYY-MM-DD"`, `"Decimal e.g. 1234.56"`).
- `dict_category` — advisory dictionary lookup category (e.g. `"vendor"`, `"person"`, `"place"`, `"parish"`).
- `enum_values` — array of allowed string values (validated deterministically and rendered as an HTML `<datalist>` dropdown while preserving verbatim OCR values).

Optional top-level `constraints` array (deterministic cross-field validation):
- `{ "op": "date_le", "lhs": "issue_date", "rhs": "due_date" }`
- `{ "op": "num_le", "lhs": "subtotal", "rhs": "total" }`
- `{ "op": "date_range", "field": "baptism_date", "min_year": 1500, "max_year": 2050 }`

```json
{
  "id": "invoice_starter",
  "label": "Invoice / Receipt",
  "version": 1,
  "prompt_hint": "Extract vendor, invoice number, dates, currency, and monetary totals verbatim.",
  "fields": [
    { "key": "vendor_name", "label": "Vendor", "type": "text", "required": true, "dict_category": "vendor" },
    { "key": "invoice_number", "label": "Invoice #", "type": "code", "required": true },
    { "key": "issue_date", "label": "Issue Date", "type": "date", "required": true, "format_hint": "YYYY-MM-DD" },
    { "key": "due_date", "label": "Due Date", "type": "date", "required": false, "format_hint": "YYYY-MM-DD" },
    { "key": "currency", "label": "Currency", "type": "enum", "required": true, "enum_values": ["EUR", "USD", "GBP", "BRL", "CHF", "OTHER"] },
    { "key": "subtotal", "label": "Subtotal", "type": "currency", "required": false, "format_hint": "Decimal e.g. 120.00" },
    { "key": "total", "label": "Total", "type": "currency", "required": true, "format_hint": "Decimal e.g. 147.60" }
  ],
  "constraints": [
    { "op": "date_le", "lhs": "issue_date", "rhs": "due_date" },
    { "op": "num_le", "lhs": "subtotal", "rhs": "total" }
  ]
}
```

### Advisory Dictionary Format (`dictionaries/*.tsv` or `SCRIBE_DICT_PATH`)

Tab-separated lines (read-only hints surfaced beside matching `dict_category` / `person` / `place` fields — never auto-overwriting raw transcriptions):

```tsv
vendor	Acme Paper Supplies Ltd.	2010	2030
person	Pe. Manuel de Sousa	1920	1975
place	São João da Madeira	1800	2026
```

---

## 5. Offline Local VLM Hardware Tier Guide

`axil-scribe` delegates vision extraction to `axil-auren` (or any OpenAI-compatible `/v1/chat/completions` server). Choose your local GGUF + `mmproj` model tier according to available memory:

| Hardware Tier | RAM / VRAM | Recommended Local VLM | Notes |
|---|---|---|---|
| **Entry / Laptop (CPU or 6–8 GB VRAM)** | 8–12 GB | `Qwen2.5-VL-3B-Instruct` (Q4_K_M + mmproj) | Fast single-pass extraction; pair with human review on cursive hands |
| **Standard Workstation (Recommended)** | 16 GB RAM or 8–12 GB VRAM | `Qwen2.5-VL-7B-Instruct` (Q4_K_M/Q5_K_M) or `OlmOCR-2-7B` | Strong cursive + mixed printed-form accuracy |
| **High-Accuracy Server** | 32–64 GB RAM or 24 GB VRAM | `Qwen2.5-VL-32B-Instruct` (`SCRIBE_ESCALATE_MODEL`) + fast 7B/3B (`SCRIBE_MODEL` / `SCRIBE_MODEL2`) | Enables 2-pass conflict routing + zoomed-crop arbitration to minimize silent errors |

> **Tip:** Benchmark candidate models and packs using:
> - `make bench` (synthetic offline mock suite)
> - `make bench-handwriting` (real 1786–1894 cursive Portuguese registers with `packs/pt-archival-handwriting`)
> - `bin/scribe-bench --fixtures <dir> --pack <pack-dir> --backend auren --model <model> [--model2 <m2>] [--escalate <m3>] [--enhance]`

---

## 6. Backup & Restore

All persistent state lives inside `SCRIBE_DATA_DIR`:

- `scans/` — immutable content-addressed scan files keyed by SHA-256 (`scans/<aa>/<sha256>.<ext>`)
- `scribe.corm` (or `corm` DB files) — canonical pages, entries, dual-value fields, transcripts, and audit metadata
- `lemma/` — full-text search index (rebuildable from `corm`)

To back up cleanly:

```sh
# Stop service (or snapshot filesystem) and archive SCRIBE_DATA_DIR
tar -czf "axil-scribe-backup-$(date +%F).tar.gz" -C "$SCRIBE_DATA_DIR" .
```

To restore, extract into `SCRIBE_DATA_DIR` and start `scribe-server`.

---

## 7. OS Service Installation

Pre-written service definitions are shipped under `services/` (defaulting to `packs/generic`; override `SCRIBE_PACK_DIR` for another pack):

- **Linux (systemd user service):** `cp services/systemd/axil-scribe.service ~/.config/systemd/user/ && systemctl --user enable --now axil-scribe`
- **macOS (launchd):** `cp services/launchd/pt.tty.axil-scribe.plist ~/Library/LaunchAgents/ && launchctl load -w ~/Library/LaunchAgents/pt.tty.axil-scribe.plist`
- **FreeBSD:** `services/freebsd/axil-scribe`
- **Alpine / OpenRC:** `services/openrc/axil-scribe`
- **OpenBSD (`rc.d`):** `services/rc.d/axil_scribe`
- **Linux Desktop (XDG):** `services/xdg/axil-scribe.desktop`
- **Windows (WinSW / PowerShell):** `services/windows/axil-scribe.xml` + `services/windows/install-service.ps1`
