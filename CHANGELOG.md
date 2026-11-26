# Changelog

All notable changes to `axil-scribe` are documented in this file.

## [Unreleased]

### Added
- **Declarative Domain Packs & Extended Template Schema (`include/ttypt/scribe.h`, `src/scribe_template.c`, `src/scribe_validate.c`, `packs/`)**:
  - Added `scribe_pack_t` (`scribe_pack_load_json()`, `scribe_pack_load_file()`, `scribe_pack_load_dir()`, `scribe_pack_free()`) supporting pack-driven VLM persona, extraction guide, uncertainty markers, preprocessing presets (`none`, `handwriting`, `document`), transcript-first output, default date year bounds, and UI label overrides (`b08c4f0`).
  - Added field types (`SCRIBE_FIELD_NUMBER`, `SCRIBE_FIELD_CURRENCY`, `SCRIBE_FIELD_CODE`, `SCRIBE_FIELD_ENUM`), `dict_category`, `format_hint`, `enum_values`, and declarative cross-field constraints (`SCRIBE_CONSTRAINT_DATE_LE`, `SCRIBE_CONSTRAINT_NUM_LE`, `SCRIBE_CONSTRAINT_DATE_RANGE`) validated via `scribe_validate_entry_ex()` and `scribe_validate_date_range()` (`b08c4f0`).
  - Added bundled domain packs: `packs/generic/` (`generic_record.json`, `invoice_starter.json`) and `packs/pt-archival-handwriting/` (`baptism_starter.json`, `confirmation_starter.json`, `wedding_starter.json`, `dictionaries/pt_ecclesiastical.tsv`) (`b08c4f0`).
- **Multi-Image Preprocessing, Diplomatic Transcripts & Zoomed-Crop Arbitration (`src/scribe_extract.c`, `src/scribe_store.c`, `src/libaxil-scribe.c`, `ux/scribe_ux.c`)**:
  - Added `scribe_extract_page_ex()`, `scribe_extract_opts_t`, multi-image `extract_json_multi`, non-destructive ephemeral preprocessing (`scribe_prepare_ocr_image_preset()`, `scribe_crop_ocr_region()`), competing `alt_value` preservation on conflicts, full-page diplomatic `transcript` persistence, uncertainty marker detection (`scribe_has_uncertain_marker_ex()`), and zoomed-crop escalation / deep re-extraction (`POST /entries/:id/extract?deep=1`) (`b08c4f0`).
  - Wired pack-driven UI vocabulary, enum `<datalist>` dropdowns (`fN_enums` in `include/ttypt/scribe-ux.h` and `ux/scribe_ux.c`), automatic pack dictionary loading, CSV `transcript` column export, and template-driven date year hints (`b08c4f0`, `bde3de8`).
- **Real Handwriting Benchmark, Launcher, NPM Pack Helpers & OS Services (`tests/fetch_real_handwriting.py`, `src/scribe-bench.c`, `scripts/scribe-server`, `npm/`, `services/`, `README.md`, `docs/USAGE.md`)**:
  - Added `tests/fetch_real_handwriting.py` (5 SHA-256-verified public-domain 1786–1894 Portuguese handwritten parish registers + ground-truth fixtures), `make bench-handwriting`, and `scripts/scribe-server --pack <name|dir>` / `--handwriting-demo` with `SCRIBE_PACK_DIR`, `SCRIBE_MODEL2`, `SCRIBE_ESCALATE_MODEL`, `SCRIBE_ENHANCE`, and 180s default VLM timeout (`b08c4f0`).
  - Added `getPacksDir()`, `getPackDir()`, `alt_value`/`transcript` TypeScript fields, and `extractEntry(id, { deep })` to `@tty-pt/scribe`, updated all cross-OS service definitions under `services/`, expanded `tests/test_scribe_{core,extract,ux}.c` and dual-pack `test.sh`, and documented automated, browser demo, benchmark, and custom pack testing in `README.md` and `docs/USAGE.md` (`b08c4f0`, `bde3de8`, `541d822`).

## [0.1.0] - 2026-04-19

### Added
- **Phase 1 — Core Pure-C Library (`libscribe.so`)**:
  - Data-driven JSON template compiler (`scribe_template.c`) compiling `templates/*.json` to `scribe_template_t` and `hyle_schema_desc_t`.
  - Starter templates: `generic_record.json`, `confirmation_starter.json`, `wedding_starter.json`, `baptism_starter.json`.
  - Immutable content-addressed scan intake (`scribe_ingest.c`) with SHA-256 deduplication, PNG/JPEG/WebP sniffing, EXIF orientation support, and multi-page PDF rasterization via `pdftoppm`.
  - Audit-grade persistence & indexing (`scribe_store.c`) on `libcorm` + `liblemma` modeling 1-page → N-entries, dual-value (`raw_value` + `verified_value`) preservation, and accent-sensitive + optional folded search.
- **Phase 2 — VLM Extraction, 2-Pass Conflict Detection & Benchmark Harness**:
  - Pluggable extractor (`scribe_extract.c`) with OpenAI-compatible `auren` socket backend (`/v1/chat/completions` with `image_url`) and deterministic offline `mock` backend.
  - Deterministic field/date/chronology validation (`scribe_validate.c`), 2-pass conflict routing (`SCRIBE_FLAG_CONFLICT` → `SCRIBE_STATUS_NEEDS_REVIEW`), and non-destructive TSV advisory dictionary suggestions.
  - Synthetic archival scan generator (`tests/generate_synthetic_certs.py`) and accuracy benchmark CLI (`bin/scribe-bench`) reporting field-level exact match and silent error rates.
- **Phase 3 — Axil HTTP Service & Pure-C Isomorphic UX**:
  - Axil Xylem HTTP module (`libaxil-scribe.so`, SONAME `axil-scribe.so`) serving `/`, `/queue`, `/ingest`, `/entries/:id`, `/entries/:id/verify`, `/entries/:id/extract`, `/entries/:id/transcript`, `/pages/:id/entries`, `/pages/:id/image`, `/search`, `/export.csv`, `/export.json`, `/assets/scribe.css`, `/assets/scribe_ux.wasm`.
  - Pure-C isomorphic UI (`ux/scribe_ux.c`) dual-compiled for No-JS SSR and browser WASM (`htdocs/scribe_ux.wasm`) with zero domain-specific JS and zero `env.*` WASM imports.
  - Live ephemeral `axil` No-JS E2E suite (`test.sh`).
- **Phase 4 — Cross-OS Packaging, Services, Launcher & NPM SDK**:
  - Turnkey offline launcher `scripts/scribe-server` (including `--demo` mode with built-in 2-pass conflict fixture and synthetic sample certificates).
  - Cross-OS service definitions under `services/` (`systemd`, `launchd`, `freebsd`, `openrc`, `rc.d`, `xdg`, `windows`).
  - Colocated `@tty-pt/scribe` NPM package (`npm/`) with zero-dependency ESM `ScribeClient`, TypeScript types, asset locators, and `node --test` verification suite.
  - Multi-OS GitHub Actions release workflow (`.github/workflows/ci.yml`), `Doxyfile`, and comprehensive `docs/USAGE.md`.
