# axil-scribe Usage Guide

`axil-scribe` is a pure-C, offline-first handwritten and archival document OCR, structured extraction, and human-in-the-loop (HITL) verification workbench built on the `tty.pt` stack.

---

## 1. Instant Browser Demo (Zero Model Download Required)

Test the complete upload → 2-pass conflict detection → split-screen review → accent-sensitive search → CSV/JSON/transcript export workflow right now:

```sh
./scripts/scribe-server --demo
```

Then open **http://127.0.0.1:8095/** in your browser:

1. Upload one of the auto-generated synthetic sample scans from `/tmp/axil-scribe-demo/samples/` (e.g., `01_confirmation_clean.png`) using the **Confirmation Certificate** template.
2. Observe the split-screen verification workbench: `sponsor` is automatically flagged with **CONFLICT** (`Joao Pereyra` vs `João Pereira`) with an advisory dictionary suggestion and bounding-box highlight.
3. Correct/confirm fields, add marginal notes, and click **Verify & Save**.
4. Test accent-sensitive search at `/search` (`Conceição` vs `Conceicao`, with optional diacritic folding) and download open exports at `/export.csv`, `/export.json`, or `/entries/<id>/transcript`.

> `--demo` uses an isolated `/tmp/axil-scribe-demo` directory so it never touches your real archive data.

---

## 2. Production / Offline Parish Launch

```sh
# Build and install rootless into ~/.local
make
make PREFIX="$HOME/.local" install

# Start on http://127.0.0.1:8095 (checks local axil-auren on :4242)
scribe-server
```

### Environment Variables

| Variable | Default | Purpose |
|---|---|---|
| `SCRIBE_PORT` | `8095` | HTTP listening port |
| `SCRIBE_DATA_DIR` | `~/.local/share/axil-scribe/data` | Immutable scans (`scans/`), `libcorm` DB, and `liblemma` index |
| `SCRIBE_TEMPLATES_DIR` | auto-detected `templates/` | Directory of document schema JSON files |
| `SCRIBE_WASM_PATH` | auto-detected `htdocs/scribe_ux.wasm` | Progressive isomorphic WASM UX bundle |
| `SCRIBE_DICT_PATH` | *(unset)* | Optional TSV advisory dictionary (`type\tcanonical\tyear_min\tyear_max`) |
| `SCRIBE_AUREN_HOST` | `127.0.0.1` | `axil-auren` (or OpenAI-compatible VLM) host |
| `SCRIBE_AUREN_PORT` | `4242` | `axil-auren` port |
| `SCRIBE_MODEL` | `qwen2.5-vl-7b` | Primary VLM model identifier |
| `SCRIBE_MODEL2` | *(unset)* | Optional secondary model for 2-pass conflict routing |
| `SCRIBE_MOCK_JSON` | *(unset)* | Optional mock JSON file or literal for offline demos/CI |

---

## 3. HTTP API & `curl` Examples

All HTML routes support No-JS browsers natively. Send `Accept: application/json` for structured JSON responses.

```sh
# Upload a scan (raw binary or multipart/form-data) and run extraction
curl -sS -X POST \
  -H 'Accept: application/json' \
  -H 'Content-Type: image/png' \
  --data-binary @scan.png \
  'http://127.0.0.1:8095/ingest?template_id=confirmation_starter&book_ref=Livro-1954&page_num=12&extract=1'

# List entries needing human review
curl -sS -H 'Accept: application/json' \
  'http://127.0.0.1:8095/queue?status=needs_review'

# Inspect an entry
curl -sS -H 'Accept: application/json' \
  'http://127.0.0.1:8095/entries/<ENTRY_ID>'

# Submit human verification
curl -sS -X POST \
  -H 'Accept: application/json' \
  -H 'Content-Type: application/json' \
  -d '{"action":"verify","reviewed_by":"archivist","fields":{"sponsor":"João Pereira"},"marginal_notes":"Verified"}' \
  'http://127.0.0.1:8095/entries/<ENTRY_ID>/verify'

# Add a second entry on a multi-entry register page (1 page -> N entries)
curl -sS -X POST \
  -H 'Accept: application/json' \
  'http://127.0.0.1:8095/pages/<PAGE_ID>/entries?template_id=confirmation_starter'

# Search (accent-sensitive exact by default; folded=1 for diacritic-insensitive)
curl -sS -H 'Accept: application/json' \
  'http://127.0.0.1:8095/search?q=Concei%C3%A7%C3%A3o&folded=0'

# Export open archival CSV and JSON
curl -sS 'http://127.0.0.1:8095/export.csv?status=verified' -o archive.csv
curl -sS 'http://127.0.0.1:8095/export.json?status=verified' -o archive.json
```

---

## 4. Authoring Custom Templates (Zero C Recompilation)

Drop a new `.json` file into `SCRIBE_TEMPLATES_DIR` and restart `axil-scribe`. Supported field `type` values: `text`, `date`, `person`, `place`, `notes`.

```json
{
  "id": "parish_confirmation_v1",
  "label": "Diocese Confirmation Register",
  "version": 1,
  "prompt_hint": "Transcribe Portuguese archival cursive verbatim. Use null for illegible words. Preserve historical spelling and diacritics.",
  "fields": [
    { "key": "person_name", "label": "Confirmand Full Name", "type": "person", "required": true },
    { "key": "confirmation_date", "label": "Confirmation Date", "type": "date", "required": true },
    { "key": "parish", "label": "Parish", "type": "place", "required": true },
    { "key": "sponsor", "label": "Sponsor / Padrinho", "type": "person", "required": false },
    { "key": "officiant", "label": "Officiant", "type": "person", "required": false }
  ]
}
```

### Advisory Dictionary Format (`SCRIBE_DICT_PATH`)

Tab-separated lines (read-only hints surfaced beside `person` and `place` fields — never auto-overwriting raw transcriptions):

```tsv
person	Pe. Manuel de Sousa	1920	1975
place	São João da Madeira	1800	2026
```

---

## 5. Offline Local VLM Hardware Tier Guide

`axil-scribe` delegates vision extraction to `axil-auren` (or any OpenAI-compatible `/v1/chat/completions` server). Choose your local GGUF + `mmproj` model tier according to available memory:

| Hardware Tier | RAM / VRAM | Recommended Local VLM | Notes |
|---|---|---|---|
| **Entry / Laptop (CPU or 6–8 GB VRAM)** | 8–12 GB | `Qwen2.5-VL-3B-Instruct` (Q4_K_M + mmproj) | Fast single-pass extraction; pair with human review on cursive hands |
| **Standard Workstation (Recommended)** | 16 GB RAM or 8–12 GB VRAM | `Qwen2.5-VL-7B-Instruct` (Q4_K_M/Q5_K_M) or `OlmOCR-2-7B` | Strong cursive + mixed printed-form accuracy; ideal default for parish offices |
| **High-Accuracy Archive Server** | 32–64 GB RAM or 24 GB VRAM | `Qwen2.5-VL-32B-Instruct` (Q4_K_M) + secondary 7B model (`SCRIBE_MODEL2`) | Enables automated 2-pass conflict routing to minimize silent transcription errors |

> **Tip:** Benchmark candidate models on your hardware using `make bench` or `bin/scribe-bench --fixtures <dir> --templates templates --backend auren --model <model>`.

---

## 6. Backup & Restore

All persistent state lives inside `SCRIBE_DATA_DIR`:

- `scans/` — immutable content-addressed scan files keyed by SHA-256 (`scans/<aa>/<sha256>.<ext>`)
- `scribe.corm` (or `corm` DB files) — canonical pages, entries, dual-value fields, and audit metadata
- `lemma/` — full-text search index (rebuildable from `corm`)

To back up cleanly:

```sh
# Stop service (or snapshot filesystem) and archive SCRIBE_DATA_DIR
tar -czf "axil-scribe-backup-$(date +%F).tar.gz" -C "$SCRIBE_DATA_DIR" .
```

To restore, extract into `SCRIBE_DATA_DIR` and start `scribe-server`.

---

## 7. OS Service Installation

Pre-written service definitions are shipped under `services/`:

- **Linux (systemd user service):** `cp services/systemd/axil-scribe.service ~/.config/systemd/user/ && systemctl --user enable --now axil-scribe`
- **macOS (launchd):** `cp services/launchd/pt.tty.axil-scribe.plist ~/Library/LaunchAgents/ && launchctl load -w ~/Library/LaunchAgents/pt.tty.axil-scribe.plist`
- **FreeBSD:** `services/freebsd/axil-scribe`
- **Alpine / OpenRC:** `services/openrc/axil-scribe`
- **OpenBSD (`rc.d`):** `services/rc.d/axil_scribe`
- **Linux Desktop (XDG):** `services/xdg/axil-scribe.desktop`
- **Windows (WinSW / PowerShell):** `services/windows/axil-scribe.xml` + `services/windows/install-service.ps1`
