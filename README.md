# axil-scribe

[![C99](https://img.shields.io/badge/C-C99-555?logo=c)](#)
[![BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue)](LICENSE)
[![Offline First](https://img.shields.io/badge/mode-Offline%20%7C%20Web-16A34A)](#)

> Pure-C, domain-agnostic document OCR, structured field extraction, declarative Domain Packs, and human-in-the-loop (HITL) verification module for [axil](https://github.com/tty-pt/axil).

`axil-scribe` is an open, standalone document capture and verification workbench built entirely on the `tty.pt` pure-C stack. It runs **100% offline on a local workstation/LAN** or as a **hosted web service**, with zero hardcoded vertical logic in the C engine — adapt it to any document domain (invoices, forms, lab notebooks, receipts, or historical cursive parish registers) via pure-data **Domain Packs** (`pack.json` + `templates/*.json` + `dictionaries/*.tsv`).

## Architecture

- **Generic Core (`libscribe`):** Immutable SHA-256 scan store, 1-page → N-entries data model, dual-value audit (`raw_value`, `alt_value`, `verified_value`), declarative cross-field constraints (`date_le`, `num_le`, `date_range`), 2-pass conflict routing, zoomed-crop arbitration escalation, and non-destructive image preprocessing (`none` / `handwriting` / `document`).
- **HTTP & Module Runtime (`libaxil-scribe`):** `axil` + `libxylem` — SSR-first HTML (full No-JS support) + JSON API + accent-sensitive `liblemma` full-text search + open CSV/JSON/printable transcript exports.
- **Isomorphic Verification Workbench (`ux/scribe_ux.c`):** Compiled identically for native SSR and browser `wasm32-wasi` (`libbud` + `libstele`) with zoom/rotate/pan/contrast controls, bounding-box overlays, enum `<datalist>` dropdowns, and pack-driven UI labels.
- **Offline & Cloud VLM Extraction:** Delegates to `axil-auren` (`/v1/chat/completions` with `image_url`, backed locally by `llama.cpp` `mtmd` or optional zero-retention cloud upstreams).
- **Bundled Domain Packs (`packs/`):**
  - [`packs/generic`](packs/generic/) — general documents & invoices (`generic_record`, `invoice_starter`).
  - [`packs/pt-archival-handwriting`](packs/pt-archival-handwriting/) — 17th–20th c. Portuguese cursive ecclesiastical/civil registers (`baptism_starter`, `confirmation_starter`, `wedding_starter`, paleography persona, and ecclesiastical TSV dictionary).

## Quick Start

```sh
# Build native libraries, CLI bench, and isomorphic WASM UX
make
make PREFIX="$HOME/.local" install

# Instant offline demo (no VLM download required)
./scripts/scribe-server --demo

# Generic document & invoice workbench
scribe-server --pack generic

# Portuguese archival handwriting workbench (with real 1786-1894 public-domain samples)
scribe-server --pack pt-archival-handwriting
scribe-server --handwriting-demo
```

Then open **http://127.0.0.1:8095/**.

## Testing & Verification

### 1. Automated Test Suites

**C unit tests + WASM build + dual-pack HTTP E2E (`test.sh`):**
```sh
make test
```
Runs `test_scribe_core` (pack loading, declarative constraints, validation, Corm/Hyle store), `test_scribe_extract` (prompts, 2-pass conflict routing, invoice validation), `test_scribe_ux` (SSR + flat JSON hydration + `<datalist>` enum rendering), and `test.sh` (live `axil` HTTP SSR/JSON/CSV/WASM E2E against both `packs/pt-archival-handwriting` and `packs/generic`).

**NPM package tests (`@tty-pt/scribe`):**
```sh
npm --prefix npm test
```
Verifies pack/template/WASM asset resolution, zero `env.*` WASM imports, and `ScribeClient` HTTP calls.

### 2. Interactive Browser Test — Offline Demo (No VLM Required)

```sh
./scripts/scribe-server --demo
# or after `make PREFIX="$HOME/.local" install`:
scribe-server --demo
```
Starts on **http://127.0.0.1:8095/** with an isolated `/tmp/axil-scribe-demo` store, the `pt-archival-handwriting` pack, a 2-pass conflict mock, an advisory TSV dictionary, and sample synthetic scans in `/tmp/axil-scribe-demo/samples/`.

What to try in the browser:
1. Upload one of `/tmp/axil-scribe-demo/samples/*.png` under **Confirmation Register (Assento de Crisma)** (`confirmation_starter`).
2. Click **Verify** on the entry — inspect the **Portuguese Archival Handwriting** pack badge, full-page diplomatic transcript, cropped bounding-box preview, and the flagged `sponsor` conflict (`Joao Pereyra` vs `João Pereira`) with advisory dictionary suggestions.
3. Edit/confirm fields, click **Confirm & Verify**, then exercise **Search**, **Export CSV**, **Export JSON**, and **Printable Transcript**.

### 3. Interactive Browser Test — Generic Pack (Invoices & General Docs)

Works with `axil-auren` on `:4242` or without a live VLM (uploads open in manual/pending mode):
```sh
SCRIBE_DATA_DIR=/tmp/scribe-generic \
SCRIBE_MODEL=fast \
./scripts/scribe-server --pack generic
```
Starts on **http://127.0.0.1:8095/** with neutral UI labels (**Source Ref**, **Notes / Annotations**, **Full Transcript**, **Deep Re-Extract**).

What to try in the browser:
1. Upload any invoice/receipt image or PDF under **Invoice / Receipt (Starter)** (`invoice_starter`).
2. Check that the `currency` enum field (`EUR`, `USD`, `GBP`, `BRL`, `CHF`) renders an HTML `<datalist>` dropdown while still allowing verbatim OCR text.
3. Verify declarative cross-field constraints by entering a `due_date` earlier than `issue_date` (`date_le`) or a `subtotal` greater than `total_amount` (`num_le`).
4. Click **Export CSV** and confirm the `transcript` column is included alongside all extracted fields.

### 4. Interactive Browser Test — Real 1786–1894 Handwriting (Live VLM)

Requires `axil-auren` running on `127.0.0.1:4242` with `fast`, `very-fast`, and `smart` models available:
```sh
./scripts/scribe-server --handwriting-demo
```
Downloads 5 public-domain Portuguese handwritten parish registers to `/tmp/axil-scribe-handwriting-demo/samples/` and launches with `packs/pt-archival-handwriting`, `SCRIBE_MODEL=fast`, `SCRIBE_MODEL2=very-fast`, `SCRIBE_ESCALATE_MODEL=smart`, and `SCRIBE_ENHANCE=1`. Upload one of the `.png` scans under **Baptism Register (Assento de Baptismo)** (`baptism_starter`) to test live multi-pass extraction, zoomed-crop arbitration, and **Deep Paleography** (`POST /entries/:id/reextract?deep=1`).

### 5. CLI Benchmarks (`scribe-bench`)

```sh
# Synthetic offline benchmark (mock backend)
make bench

# Real handwritten archival benchmark against axil-auren
make bench-handwriting
```
Reports per-document exact-match accuracy, flagged fields, and silent error rate across the benchmark fixtures.

### 6. Testing a Custom Domain Pack

Create a directory containing `pack.json` and `templates/*.json` (plus optional `dictionaries/*.tsv`) modeled on [`packs/generic/pack.json`](packs/generic/pack.json), then launch:
```sh
SCRIBE_DATA_DIR=/tmp/scribe-custom ./scripts/scribe-server --pack /path/to/my-pack
```

## Documentation

- Detailed CLI flags, environment variables, HTTP API, custom templates, and Domain Pack schema: **[docs/USAGE.md](docs/USAGE.md)**
- Architecture & roadmap: **[ROADMAP.md](ROADMAP.md)**

## License

[BSD-2-Clause](LICENSE) © 2026 tty-pt.
