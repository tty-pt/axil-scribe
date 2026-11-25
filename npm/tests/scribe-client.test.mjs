import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import http from "node:http";
import {
  ScribeClient,
  createScribeClient,
  getModulePath,
  getWasmPath,
  getTemplatesDir,
  getIncludeDir,
  getPkgConfigPath,
} from "../dist/index.js";

test("asset helpers resolve native module, wasm, templates, headers, and pkg-config", () => {
  const modPath = getModulePath();
  assert.ok(modPath && modPath.endsWith("libaxil-scribe"), `expected libaxil-scribe path, got ${modPath}`);

  const wasmPath = getWasmPath();
  assert.ok(wasmPath && fs.existsSync(wasmPath), `expected scribe_ux.wasm, got ${wasmPath}`);

  const tplDir = getTemplatesDir();
  assert.ok(tplDir && fs.existsSync(path.join(tplDir, "confirmation_starter.json")), `expected templates dir, got ${tplDir}`);

  const incDir = getIncludeDir();
  assert.ok(incDir && fs.existsSync(path.join(incDir, "ttypt", "scribe.h")), `expected include dir, got ${incDir}`);

  const pcDir = getPkgConfigPath();
  assert.ok(pcDir && fs.existsSync(path.join(pcDir, "scribe.pc")), `expected pkg-config dir, got ${pcDir}`);
});

test("scribe_ux.wasm exports isomorphic Stele kernel entrypoints and has zero env.* imports", () => {
  const wasmPath = getWasmPath();
  const bytes = fs.readFileSync(wasmPath);
  const mod = new WebAssembly.Module(bytes);

  const exportNames = new Set(WebAssembly.Module.exports(mod).map((e) => e.name));
  for (const required of [
    "wasm_init",
    "bud_app_render",
    "stl_kernel_mount",
    "stl_kernel_action",
  ]) {
    assert.ok(exportNames.has(required), `missing WASM export: ${required}`);
  }

  const imports = WebAssembly.Module.imports(mod);
  const envImports = imports.filter((i) => i.module === "env");
  assert.deepEqual(envImports, [], `forbidden env.* WASM imports: ${JSON.stringify(envImports)}`);
});

test("ScribeClient: end-to-end mocked HTTP protocol", async () => {
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      const bodyBuf = Buffer.concat(chunks);
      const url = new URL(req.url, `http://${req.headers.host}`);

      if (req.method === "GET" && url.pathname === "/queue") {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({
          ok: true,
          entries: [{
            id: "ent-1",
            page_id: "pg-1",
            template_id: url.searchParams.get("template_id") || "confirmation_starter",
            status: url.searchParams.get("status") || "needs_review",
            fields: [],
          }],
        }));
        return;
      }

      if (req.method === "POST" && url.pathname === "/ingest") {
        assert.equal(req.headers["content-type"], "image/png");
        assert.ok(bodyBuf.length > 0);
        res.writeHead(201, { "Content-Type": "application/json" });
        res.end(JSON.stringify({
          ok: true,
          deduped: false,
          pages: [{ id: "pg-1", sha256: "abc123", book_ref: url.searchParams.get("book_ref") || "", page_num: 1 }],
          entries: [{ id: "ent-1", page_id: "pg-1", template_id: "confirmation_starter", status: "needs_review", fields: [] }],
        }));
        return;
      }

      if (req.method === "GET" && url.pathname === "/entries/ent-1") {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({
          ok: true,
          entry: { id: "ent-1", page_id: "pg-1", template_id: "confirmation_starter", status: "needs_review", fields: [] },
        }));
        return;
      }

      if (req.method === "POST" && url.pathname === "/entries/ent-1/verify") {
        const payload = JSON.parse(bodyBuf.toString("utf8"));
        assert.equal(payload.action, "verify");
        assert.equal(payload.reviewed_by, "archivist");
        assert.equal(payload.fields.sponsor, "João Pereira");
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({
          ok: true,
          entry: { id: "ent-1", page_id: "pg-1", template_id: "confirmation_starter", status: "verified", reviewed_by: "archivist", fields: [] },
        }));
        return;
      }

      if (req.method === "POST" && url.pathname === "/entries/ent-1/extract") {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ ok: true, entries: [{ id: "ent-1", status: "extracted", fields: [] }] }));
        return;
      }

      if (req.method === "POST" && url.pathname === "/pages/pg-1/entries") {
        res.writeHead(201, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ ok: true, entry: { id: "ent-2", page_id: "pg-1", status: "pending", fields: [] } }));
        return;
      }

      if (req.method === "GET" && url.pathname === "/search") {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({
          ok: true,
          query: url.searchParams.get("q"),
          folded: url.searchParams.get("folded") === "1",
          entries: [{ id: "ent-1", status: "verified", fields: [] }],
        }));
        return;
      }

      if (req.method === "GET" && url.pathname === "/export.csv") {
        res.writeHead(200, { "Content-Type": "text/csv; charset=utf-8" });
        res.end("entry_id,status,person_name\nent-1,verified,Maria\n");
        return;
      }

      if (req.method === "GET" && url.pathname === "/export.json") {
        res.writeHead(200, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ version: "0.1.0", entries: [{ id: "ent-1", status: "verified", fields: [] }] }));
        return;
      }

      res.writeHead(404);
      res.end("Not Found");
    });
  });

  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const { port } = server.address();
  const client = createScribeClient({ baseUrl: `http://127.0.0.1:${port}` });
  assert.ok(client instanceof ScribeClient);

  try {
    const ingested = await client.ingestBuffer(new Uint8Array([0x89, 0x50, 0x4e, 0x47]), {
      contentType: "image/png",
      templateId: "confirmation_starter",
      bookRef: "Livro-1954",
      pageNum: 1,
      extract: true,
    });
    assert.equal(ingested.ok, true);
    assert.equal(ingested.entries[0].id, "ent-1");

    const q = await client.queue({ status: "needs_review", templateId: "confirmation_starter" });
    assert.equal(q.entries.length, 1);

    const got = await client.getEntry("ent-1");
    assert.equal(got.entry.id, "ent-1");

    const verified = await client.verifyEntry("ent-1", {
      action: "verify",
      reviewedBy: "archivist",
      fields: { sponsor: "João Pereira" },
      marginalNotes: "Checked",
    });
    assert.equal(verified.entry.status, "verified");

    const extracted = await client.extractEntry("ent-1");
    assert.equal(extracted.entries[0].status, "extracted");

    const added = await client.addPageEntry("pg-1", { templateId: "confirmation_starter" });
    assert.equal(added.entry.id, "ent-2");

    const hits = await client.search("Conceição", { folded: false });
    assert.equal(hits.query, "Conceição");
    assert.equal(hits.folded, false);
    assert.equal(hits.entries.length, 1);

    const csv = await client.exportCsv({ status: "verified" });
    assert.ok(csv.includes("entry_id,status,person_name"));

    const exported = await client.exportJson({ status: "verified" });
    assert.equal(exported.version, "0.1.0");
    assert.equal(exported.entries.length, 1);

    assert.equal(client.getPageImageUrl("pg-1"), `http://127.0.0.1:${port}/pages/pg-1/image`);
    assert.equal(client.getTranscriptUrl("ent-1"), `http://127.0.0.1:${port}/entries/ent-1/transcript`);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});
