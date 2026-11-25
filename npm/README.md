# @tty-pt/scribe

Node.js & ESM client SDK and prebuilt Axil module locator for [`axil-scribe`](https://github.com/tty-pt/axil-scribe) — pure-C handwritten & archival document OCR, structured field extraction, and human-in-the-loop (HITL) verification.

## Install

```sh
npm install @tty-pt/scribe
```

## Launch `axil-scribe` via `axil`

```sh
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
  templateId: "confirmation_starter",
  bookRef: "Livro-1954",
  pageNum: 12,
  extract: true,
});

// 2. Fetch review queue
const queue = await client.queue({ status: "needs_review" });

// 3. Submit human verification
const entryId = ingested.entries[0].id;
await client.verifyEntry(entryId, {
  action: "verify",
  reviewedBy: "archivist",
  fields: { person_name: "Maria das Graças Silva" },
  marginalNotes: "Verified against original parish book.",
});

// 4. Search (accent-sensitive by default; folded: true for diacritic-insensitive)
const hits = await client.search("Conceição");

// 5. Export open archival formats
const csv = await client.exportCsv({ status: "verified" });
const json = await client.exportJson({ status: "verified" });
```

## Asset & Path Helpers

- `getModulePath()` — locates `libaxil-scribe` (without platform suffix) for `axil -m`.
- `getWasmPath()` — locates `scribe_ux.wasm` for progressive isomorphic WASM hydration.
- `getTemplatesDir()` — locates bundled starter `templates/*.json`.
- `getIncludeDir()` — locates C headers (`ttypt/scribe.h`, `ttypt/scribe-ux.h`, `ttypt/axil-scribe.h`).
- `getPkgConfigPath()` — locates `scribe.pc` / `axil-scribe.pc`.

## License

BSD-2-Clause © 2026 tty-pt
