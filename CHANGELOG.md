# Changelog

All notable changes to `axil-scribe` are documented in this file.

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
