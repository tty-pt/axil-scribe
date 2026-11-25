# @tty-pt/scribe

Node.js & ESM client SDK and prebuilt Axil module locator for [`axil-scribe`](https://github.com/tty-pt/axil-scribe) — pure-C domain-agnostic document OCR, structured field extraction, declarative Domain Packs, and human-in-the-loop (HITL) verification.

## Install

```sh
npm install @tty-pt/scribe
```

## Launch `axil-scribe` via `axil`

```sh
# Generic pack (general documents & invoices)
SCRIBE_PACK_DIR="$(node -e 'import("@tty-pt/scribe").then(m => console.log(m.getPackDir("generic")))')" \
  npx axil -A -p 8095 -m "$(node -e 'import("@tty-pt/scribe").then(m => console.log(m.getModulePath()))')"

# Portuguese archival handwriting pack
SCRIBE_PACK_DIR="$(node -e 'import("@tty-pt/scribe").then(m => console.log(m.getPackDir("pt-archival-handwriting")))')" \
  npx axil -A -p 8095 -m "$(node -e 'import("@tty-pt/scribe").then(m => console.log(m.getModulePath()))')"
```

## Usage (`ScribeClient`)

```js
import fs from "node:fs";
import { createScribeClient } from "@tty-pt/scribe";

const client = createScribeClient({ baseUrl: "http://127.0.0.1:8095" });

// 1. Ingest a scanned image (PNG/JPEG/WebP/PDF)
const bytes = fs.readFileSync("scan.png");
const ingested = await client.ingestBuffer(bytes, {
  contentType: "image/png",
  templateId: "invoice_starter",
  bookRef: "BATCH-2026",
  pageNum: 1,
  extract: true,
});

// 2. Fetch review queue
const queue = await client.queue({ status: "needs_review" });

// 3. Optional high-accuracy deep re-extraction
const entryId = ingested.entries[0].id;
await client.extractEntry(entryId, { deep: true });

// 4. Submit human verification
await client.verifyEntry(entryId, {
  action: "verify",
  reviewedBy: "reviewer-1",
  fields: { currency: "EUR" },
  marginalNotes: "Verified against original document.",
});

// 5. Search (accent-sensitive by default; folded: true for diacritic-insensitive)
const hits = await client.search("Conceição");

// 6. Export open CSV and JSON
const csv = await client.exportCsv({ status: "verified" });
const json = await client.exportJson({ status: "verified" });
```

## Asset & Path Helpers

- `getModulePath()` — locates `libaxil-scribe` (without platform suffix) for `axil -m`.
- `getWasmPath()` — locates `scribe_ux.wasm` for progressive isomorphic WASM hydration.
- `getPacksDir()` — locates bundled declarative domain packs directory (`packs/`).
- `getPackDir(name)` — locates a specific domain pack directory (e.g. `"generic"`, `"pt-archival-handwriting"`).
- `getTemplatesDir()` — locates legacy starter `templates/*.json`.
- `getIncludeDir()` — locates C headers (`ttypt/scribe.h`, `ttypt/scribe-ux.h`, `ttypt/axil-scribe.h`).
- `getPkgConfigPath()` — locates `scribe.pc` / `axil-scribe.pc`.

## License

BSD-2-Clause © 2026 tty-pt
