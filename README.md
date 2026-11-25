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

## Verification & Documentation

- Run full native + WASM + C unit + dual-pack E2E suite: `make test`
- Run live real-handwriting VLM benchmark against `axil-auren`: `make bench-handwriting`
- Detailed CLI, environment variables, HTTP API, custom templates, and Domain Pack authoring: **[docs/USAGE.md](docs/USAGE.md)**
- Architecture & roadmap: **[ROADMAP.md](ROADMAP.md)**

## License

[BSD-2-Clause](LICENSE) © 2026 tty-pt.
