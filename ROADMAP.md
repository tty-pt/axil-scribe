# axil-scribe Roadmap

Pure-C handwritten and archival document OCR, structured extraction, and human-in-the-loop (HITL) verification engine built on the `tty.pt` stack (`axil`, `libxylem`, `libhyle`, `libcorm`, `liblemma`, `libbud`, `libstele`, `axil-auren`).

---

## 1. Design Principles & Non-Negotiable Invariants

1. **VLM-First, Template-Agnostic Extraction**
   - Traditional OCR engines (Tesseract/EasyOCR) break down on cursive handwriting, mixed printed-form + ink layouts, faded paper, and stamps.
   - `axil-scribe` extracts structured JSON directly via a Vision-Language Model (VLM) using the OpenAI-compatible `/v1/chat/completions` (`image_url`) contract served by [`axil-auren`](https://github.com/tty-pt/axil-auren).
   - Document templates (Confirmation, Wedding, Baptism, generic ledgers) are **data** (`templates/*.json` compiled at load time to `hyle_schema_desc_t`), never hardcoded C structs. Adding or adapting a diocese/archive form requires zero C recompilation.

2. **Offline-First & Dual Deployment**
   - Must run **100% offline/on-premise** on a local PC or parish LAN (`axil` + local `axil-auren` GGUF vision model) with zero external network calls, while also supporting hosted web deployments and optional cloud VLM upstreams through the exact same HTTP interface.

3. **Audit-Grade Provenance (No Silent Hallucinations)**
   - Canonical archival/sacramental records treat a silent transcription error in a surname or date as far worse than flagging a field illegible.
   - Original scans are immutable and content-addressed (`sha256`).
   - Every extracted field stores both `raw_value` (machine output) and `verified_value` (human-approved truth), plus explicit flags (`ILLEGIBLE`, `CONFLICT`, `EDITED`).
   - Modeling **1 page → N entries** from day one supports both standalone certificates and multi-entry bound register books without a schema rewrite.

4. **Pure-C Isomorphic UX & Open Formats**
   - Full **No-JS SSR** functionality (`libbud` + `libhyle-bud`) for upload, verification, search, and export.
   - Progressive **isomorphic WASM** (`<ttypt/stele_wasm.h>`) adds split-screen zoom/pan/rotate and keyboard-first review (`Tab` across flagged fields, `Enter` to confirm) with **zero domain-specific JavaScript**.
   - Open, portable exports (CSV, JSON, printable transcript HTML/PDF) and standalone C library (`libscribe`) with no vendor lock-in.

5. **Abstraction-First TDD & Portable Distribution**
   - Build and unit-test each abstraction (`libscribe` core, schema compiler, extractor client, HTTP handlers, WASM UX) before wiring downstream integrations.
   - Distribute via standard `tty.pt` packaging (`mk/include.mk`, `tty-pt/ci` `all-in-one.yml`, OS service files, and `@tty-pt/scribe` NPM package).

---

## 2. Canonical Data Layout

Define the structures first so every downstream layer (storage, extraction, validation, UI, export) operates on one obvious shape:

```c
/* Template schema loaded from templates/*.json -> compiled to hyle_schema_desc_t */
typedef enum {
	SCRIBE_FIELD_TEXT = 0,
	SCRIBE_FIELD_DATE,
	SCRIBE_FIELD_PERSON,
	SCRIBE_FIELD_PLACE,
	SCRIBE_FIELD_NOTES,
} scribe_field_type_t;

typedef struct {
	const char *key;          /* canonical machine key, e.g. "person_name" */
	const char *label;        /* display label, e.g. "Full Name" */
	scribe_field_type_t type;
	uint8_t required;
} scribe_field_spec_t;

typedef struct {
	const char *id;           /* e.g. "confirmation_starter" */
	const char *label;        /* e.g. "Confirmation Certificate" */
	uint32_t version;
	const char *prompt_hint;  /* domain transcription rules injected into VLM prompt */
	const scribe_field_spec_t *fields;
	size_t field_count;
} scribe_template_t;

/* Immutable source scan / rendered PDF page */
typedef struct {
	char id[32];
	char sha256[65];          /* hex SHA-256 for deduplication & integrity */
	char file_path[256];      /* relative path inside immutable scan store */
	char mime[64];            /* image/png, image/jpeg, image/webp */
	char book_ref[64];        /* optional archival book/volume reference */
	uint32_t page_num;
	uint32_t width;
	uint32_t height;
	int64_t ingested_at;
} scribe_page_t;

/* Workflow status for one logical record (1 page -> N entries) */
typedef enum {
	SCRIBE_STATUS_PENDING = 0,
	SCRIBE_STATUS_EXTRACTED,
	SCRIBE_STATUS_NEEDS_REVIEW,
	SCRIBE_STATUS_VERIFIED,
} scribe_status_t;

typedef enum {
	SCRIBE_FLAG_NONE      = 0,
	SCRIBE_FLAG_ILLEGIBLE = 1u << 0,
	SCRIBE_FLAG_CONFLICT  = 1u << 1,  /* 2-pass extraction disagreement */
	SCRIBE_FLAG_EDITED    = 1u << 2,  /* modified by human reviewer */
	SCRIBE_FLAG_INVALID   = 1u << 3,  /* failed deterministic validation */
} scribe_field_flags_t;

typedef struct {
	char key[64];
	char *raw_value;          /* verbatim VLM transcription (NULL if illegible) */
	char *verified_value;     /* human-verified canonical value */
	uint32_t flags;           /* bitmask of scribe_field_flags_t */
	float bbox[4];            /* optional normalized [x, y, w, h], all 0 if absent */
} scribe_field_val_t;

typedef struct {
	char id[32];
	char page_id[32];         /* foreign key -> scribe_page_t.id */
	char template_id[64];     /* foreign key -> scribe_template_t.id */
	scribe_status_t status;
	char extractor_model[128];
	char reviewed_by[64];
	int64_t reviewed_at;
	char *marginal_notes;     /* handwritten margin annotations added later */
	scribe_field_val_t *fields;
	size_t field_count;
} scribe_entry_t;
```

**Search invariant:** `verified_value` preserves verbatim historical spelling (including diacritics). `liblemma` indexes verbatim fields (accent-sensitive, e.g. `Conceição` ≠ `Conceicao`), with an optional normalized/folded shadow field for forgiving lookup and optional `libsepal` embeddings for fuzzy/semantic search.

---

## 3. Phased Implementation Plan

### Phase 0 — Local Multimodal Vision in `axil-auren` (Ecosystem Prerequisite)

`axil-auren` already handles OpenAI `image_url` (`data:image/...;base64,...`) for cloud upstreams (`auren-upstream.c`) and already vendors `submodules/llama.cpp/tools/mtmd`. Wiring `mtmd` into the local engine unlocks 100% offline VLM inference for `axil-scribe` and the entire `tty.pt` ecosystem.

- [ ] **0.1 Multimodal projector discovery (`auren-registry.c`)**
  - Auto-discover `*mmproj*.gguf` in `~/.models/` (or `$AUREN_MODELS_DIR`) and pair each projector with its matching vision GGUF (e.g. Qwen2.5-VL, OlmOCR-2, Gemma-3 vision).
- [ ] **0.2 Local `mtmd` engine integration (`auren-engine.c`, `Makefile`)**
  - Link vendored `llama.cpp/tools/mtmd` cleanly across Linux (Vulkan/CPU), macOS (Metal), and Windows.
  - Decode image buffers (`stb_image` / `mtmd_helper_bitmap_init_from_buf`) and evaluate multimodal chunks before text generation.
- [ ] **0.3 Local OpenAI `image_url` handler (`openai_chat.c`)**
  - Parse `messages[].content[]` parts with `type: "image_url"` and `data:image/...;base64,...` identically to `auren-upstream.c`.
- [ ] **Verify Phase 0:**
  - `make -C external/axil-auren test` exits `0`.
  - `curl -s http://127.0.0.1:4242/v1/chat/completions` with a base64 test PNG and local GGUF+mmproj model returns valid structured JSON.

---

### Phase 1 — Core C Library (`libscribe`), Template Loader & Storage Engine

Build and verify the standalone pure-C core with zero HTTP or UI coupling.

- [x] **1.1 Repository & build skeleton**
  - Standard `Makefile` using `../mk/include.mk`, `objects-set.mk`, `scribe.pc`, canonical header `include/ttypt/scribe.h`.
- [x] **1.2 Data-driven template compiler (`src/scribe_template.c`)**
  - Load `templates/*.json` and compile into `scribe_template_t` + canonical `hyle_schema_desc_t`.
  - Ship starter templates usable immediately before real church scans arrive:
    - `templates/generic_record.json`
    - `templates/confirmation_starter.json`
    - `templates/wedding_starter.json`
    - `templates/baptism_starter.json`
- [x] **1.3 Immutable scan store & intake normalization (`src/scribe_ingest.c`)**
  - Content-addressed storage (`sha256` deduplication), EXIF auto-orientation, gentle deskew hook.
  - Multi-page PDF intake via `pdftoppm` fallback (200–300 DPI PNG/JPEG per page).
  - Explicitly avoid aggressive binarization/thresholding (preserves faint historical ink for VLMs).
- [x] **1.4 Persistence & indexing (`src/scribe_store.c`)**
  - Store pages, entries, and dual-value fields via `libcorm` + `libhyle` + `libhyle-source`.
  - Index entries in `liblemma` (all writes routed through `hyle put`/`del` invariants); optional `libsepal` semantic index hook.
- [x] **Verify Phase 1:**
  - `make` builds `lib/libscribe.so` with zero warnings.
  - Standalone unit suite (`tests/test_scribe_core`) verifies template compilation, SHA-256 dedup, 1-page → N-entries CRUD, dual-value audit preservation, and accent-sensitive `liblemma` search (exit code `0`).

---

### Phase 2 — Pluggable VLM Extraction, 2-Pass Conflict Detection & Benchmark Harness

Build a verifiable extraction pipeline before touching UI code.

- [x] **2.1 Pluggable extractor interface (`src/scribe_extract.c`)**
  - `scribe_extractor_ops_t` vtable with two built-in backends:
    - **`auren` backend:** calls `/v1/chat/completions` (`image_url` + deterministic schema-derived JSON prompt instructing verbatim transcription, `null` on illegible ink, separate `marginal_notes`, and multi-entry array output). Works unchanged for local offline GGUF and cloud upstreams.
    - **`mock` / replay backend:** deterministic JSON fixture backend for fast offline CI tests.
- [x] **2.2 Deterministic validation & 2-pass conflict routing (`src/scribe_validate.c`)**
  - Format & sanity rules: valid calendar dates, chronological checks (e.g., confirmation/wedding after birth/baptism when present), required field presence -> sets `SCRIBE_FLAG_INVALID`.
  - Optional **2-pass disagreement check**: run two models (or two prompt/temperature passes) and flag any field where outputs diverge (`SCRIBE_FLAG_CONFLICT`), routing the entry to `SCRIBE_STATUS_NEEDS_REVIEW`.
  - Non-destructive dictionary suggestions (known officiants, parishes, localities by era) surfaced as hints only — never auto-overwriting raw transcription.
- [x] **2.3 Synthetic fixture generator & accuracy benchmark (`tests/`)**
  - `tests/generate_synthetic_certs.py` generating synthetic certificates and multi-entry ledger pages (tilted, low-contrast, cursive/handwritten fonts, stamps, margin notes) with ground-truth JSON — zero real PII in the repository.
  - `bin/scribe-bench` measuring **field-level exact match rate** and **silent error rate** (wrong value without `ILLEGIBLE`/`CONFLICT`/`INVALID` flag) across candidate models.
- [x] **Verify Phase 2:**
  - `tests/test_scribe_extract` passes with mock backend (including conflict detection and validation flags).
  - Live `bin/scribe-bench` run against synthetic fixtures produces a repeatable metrics table.

---

### Phase 3 — Axil HTTP Service (`libaxil-scribe.so`), No-JS SSR & Isomorphic WASM Verification UI

Deliver the usable local/web application with full No-JS parity and fast keyboard-driven review.

- [x] **3.1 Axil HTTP module (`src/libaxil-scribe.c`)**
  - `POST /ingest` — upload single image, phone photo, or multi-page PDF; deduplicate and queue extraction.
  - `POST /entries/{id}/extract` — trigger single-pass or 2-pass VLM extraction.
  - `GET /queue` — filterable review queue (`NEEDS_REVIEW`, `EXTRACTED`, `VERIFIED`, template, book_ref).
  - `GET /entries/{id}` & `POST /entries/{id}/verify` — inspect/edit fields, toggle `ILLEGIBLE`, edit `marginal_notes`, add split entries for multi-entry ledger pages, mark `VERIFIED`.
  - `GET /pages/{id}/image` — serve immutable original scan with proper `Cache-Control` and `ETag` (`sha256`).
  - `GET /search` — `liblemma` (+ optional `libsepal`) search across verified and raw fields.
  - `GET /export.csv`, `GET /export.json`, `GET /entries/{id}/transcript` — open archival exports.
  - Optional `axil-auth` integration for multi-volunteer `reviewed_by` attribution, with zero-auth `127.0.0.1` mode for single-machine offline parish use.
- [x] **3.2 No-JS SSR UI (`ux/scribe_ux.c`, `libbud`)**
  - Clean SSR forms and tables derived from template schemas so every workflow (upload, verify, approve, search, export) works with JavaScript disabled.
- [x] **3.3 Progressive isomorphic WASM verification UX (`ux/scribe_ux.c`, `<ttypt/stele_wasm.h>`)**
  - Split-screen verification workbench: zoomable/pannable/rotatable scan canvas on the left, schema-driven verification form on the right.
  - Pure isomorphic action state machine (`scribe_ux_apply_action`) for zoom, rotate, pan, next-flagged-field jump, and illegible toggling, compiled to `htdocs/scribe_ux.wasm`.
  - Optional bounding-box overlay highlighting the active field region on the scan when returned by the VLM.
  - Zero domain-specific JavaScript (strictly generic `hyle`/`bud`/`stele` runtime).
- [x] **Verify Phase 3:**
  - Dual-compile native `.so` + browser `.wasm`.
  - Boundary, WASM import, and no-site-specific-JS checks pass cleanly.
  - End-to-end `./test.sh` exercises upload → extract → conflict flag → No-JS verify → search → CSV/JSON export against a live ephemeral `axil` daemon.

---

### Phase 4 — Portable Cross-OS Packaging, Services, NPM & Ecosystem Integration

Ship `axil-scribe` using the exact distribution machinery of the rest of `tty.pt`.

- [x] **4.1 Cross-platform OS service definitions (`services/`)**
  - Add service units matching `axil-auren`: `systemd/`, `launchd/`, `freebsd/`, `openrc/`, `rc.d/`, `windows/`, `xdg/`.
  - Provide a turnkey offline launcher (`scripts/scribe-server`) starting `axil` (+ checking local `axil-auren` availability).
- [x] **4.2 Colocated NPM package (`npm/`)**
  - Publish `@tty-pt/scribe` containing prebuilt native module, WASM UX artifact, C headers (`include/ttypt/scribe.h`), and `scribe.pc` so `npx axil -A -p 8095 -m axil-scribe` runs with zero local compiler.
- [x] **4.3 Multi-OS CI release pipeline (`.github/workflows/ci.yml`)**
  - Wire `tty-pt/ci/.github/workflows/all-in-one.yml@main` targeting `deb,apk,rpm,pacman,brew,openbsd,winget,npm` with `test_build: "true"`.
- [x] **4.4 Documentation & optional `site` integration**
  - Add `CHANGELOG.md`, `Doxyfile`, `docs/USAGE.md`, and offline hardware tier guide (RAM/VRAM footprints for recommended local VLM GGUFs).
  - Register `axil-scribe` in `site/docs/SIBLING_REPOS.md` and optionally `site/mods/software` (with an optional sandbox demo running strictly on synthetic public sample certificates).
- [x] **Verify Phase 4:**
  - `make PREFIX="$HOME/.local" install` succeeds without root.
  - `npm --prefix npm test` passes and `.github/workflows/ci.yml` passes tag/dry-run build.

---

### Phase 5 — Real-Template Onboarding & Field Calibration

Execute once real church/archival templates and sample scans become available (kept strictly private / outside git):

- [ ] **5.1 1-day golden-set evaluation (20–50 representative scans)**
  - Cover clean modern cursive, older/faded ink, stamped forms, phone photos, and multi-entry bound registers.
  - Measure field-level exact match and silent error rate via `bin/scribe-bench` across local VLMs (e.g., Qwen2.5-VL-7B/32B, OlmOCR-2) vs. cloud VLMs.
- [ ] **5.2 Template & prompt refinement (data-only)**
  - Author exact diocese/parish `templates/*.json` schemas and language/abbreviations prompt hints (including Portuguese/Latin/local ecclesiastical conventions if applicable).
  - Populate local parish/officiant suggestion dictionaries.
- [ ] **5.3 Operational hardening**
  - Document backup/restore of immutable scans + `libcorm` database, volunteer review throughput targets, and privacy/offline deployment checklist.

---

## 4. Recommended Execution Order

```text
Phase 0 (axil-auren mtmd vision) ──┐
                                   ├──▶ Phase 2 (extract + 2-pass + synthetic bench) ──▶ Phase 3 (axil HTTP + SSR/WASM UI) ──▶ Phase 4 (CI/OS/NPM packaging) ──▶ Phase 5 (real-template calibration)
Phase 1 (libscribe core + store) ──┘
```

*Phase 0 and Phase 1 are independent and can proceed in parallel — Phase 1 + Phase 2 also work immediately against mock fixtures and `axil-auren`'s existing cloud `image_url` path before Phase 0 completes.*
