/**
 * @file index.js
 * @brief Universal ESM client SDK and native/WASM asset locator for @tty-pt/scribe.
 */

let getPlatformPrebuildKeys = () => [];

if (typeof window === "undefined" && typeof process !== "undefined") {
  const candidates = [
    "@tty-pt/stele",
    new URL("../../../libstele/npm/dist/index.js", import.meta.url).href,
    new URL("../../../site/external/libstele/npm/dist/index.js", import.meta.url).href,
  ];
  for (const cand of candidates) {
    try {
      const stele = await import(cand);
      if (typeof stele?.getPlatformPrebuildKeys === "function") {
        getPlatformPrebuildKeys = stele.getPlatformPrebuildKeys;
        break;
      }
    } catch {}
  }
}

function getNodeBuiltins() {
  if (typeof process === "undefined" || !process.versions?.node) {
    return null;
  }
  const getModule = globalThis.process?.getBuiltinModule;
  if (!getModule) return null;
  const fs = getModule("fs");
  const path = getModule("path");
  const url = getModule("url");
  const pkgRoot = path.resolve(path.dirname(url.fileURLToPath(import.meta.url)), "..");
  return { fs, path, pkgRoot };
}

function buildQuery(params) {
  const sp = new URLSearchParams();
  for (const [k, v] of Object.entries(params)) {
    if (v === undefined || v === null || v === "") continue;
    sp.set(k, String(v));
  }
  const qs = sp.toString();
  return qs ? `?${qs}` : "";
}

/**
 * HTTP client for the axil-scribe service (zero third-party runtime dependencies).
 */
export class ScribeClient {
  constructor(options = {}) {
    this.baseUrl = (options.baseUrl || "http://127.0.0.1:8095").replace(/\/+$/, "");
    this.timeoutMs = options.timeoutMs || 60000;
    this.headers = options.headers || {};
  }

  async _request(path, init = {}, expectJson = true) {
    const url = `${this.baseUrl}${path}`;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);

    const headers = {
      ...(expectJson ? { Accept: "application/json" } : {}),
      ...this.headers,
      ...init.headers,
    };

    try {
      const res = await fetch(url, {
        ...init,
        headers,
        signal: controller.signal,
      });

      if (!res.ok) {
        const errText = await res.text().catch(() => "");
        throw new Error(`Scribe error ${res.status} (${res.statusText}): ${errText}`);
      }

      return expectJson ? await res.json() : await res.text();
    } finally {
      clearTimeout(timer);
    }
  }

  /**
   * Fetch filtered review queue entries as JSON.
   */
  async queue(filters = {}) {
    const qs = buildQuery({
      status: filters.status,
      template_id: filters.templateId ?? filters.template_id,
      book_ref: filters.bookRef ?? filters.book_ref,
    });
    return this._request(`/queue${qs}`, { method: "GET" }, true);
  }

  /**
   * Ingest a raw image or PDF buffer via POST /ingest.
   */
  async ingestBuffer(bytes, options = {}) {
    const qs = buildQuery({
      template_id: options.templateId ?? options.template_id,
      book_ref: options.bookRef ?? options.book_ref,
      page_num: options.pageNum ?? options.page_num,
      extract: options.extract === undefined ? undefined : (options.extract ? 1 : 0),
    });
    const contentType = options.contentType || "application/octet-stream";
    return this._request(
      `/ingest${qs}`,
      {
        method: "POST",
        headers: { "Content-Type": contentType },
        body: bytes,
      },
      true
    );
  }

  /**
   * Retrieve a single entry and its source page metadata.
   */
  async getEntry(entryId) {
    return this._request(`/entries/${encodeURIComponent(entryId)}`, { method: "GET" }, true);
  }

  /**
   * Submit human-in-the-loop verification or edits for an entry.
   */
  async verifyEntry(entryId, payload = {}) {
    const body = {
      action: payload.action || "verify",
      ...(payload.reviewedBy || payload.reviewed_by
        ? { reviewed_by: payload.reviewedBy ?? payload.reviewed_by }
        : {}),
      ...(payload.marginalNotes !== undefined || payload.marginal_notes !== undefined
        ? { marginal_notes: payload.marginalNotes ?? payload.marginal_notes }
        : {}),
      ...(payload.fields ? { fields: payload.fields } : {}),
      ...(payload.illegible ? { illegible: payload.illegible } : {}),
    };
    return this._request(
      `/entries/${encodeURIComponent(entryId)}/verify`,
      {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      },
      true
    );
  }

  /**
   * Trigger extraction / re-extraction for an entry's page.
   * Pass { deep: true } to force high-accuracy escalation mode (?deep=1).
   */
  async extractEntry(entryId, options = {}) {
    const qs = buildQuery({
      deep: options.deep ? 1 : undefined,
    });
    return this._request(
      `/entries/${encodeURIComponent(entryId)}/extract${qs}`,
      { method: "POST" },
      true
    );
  }

  /**
   * Add a new entry on an existing page (1 page -> N entries for multi-record scans).
   */
  async addPageEntry(pageId, options = {}) {
    const qs = buildQuery({
      template_id: options.templateId ?? options.template_id,
    });
    return this._request(
      `/pages/${encodeURIComponent(pageId)}/entries${qs}`,
      { method: "POST" },
      true
    );
  }

  /**
   * Search indexed records (exact accent-sensitive by default; pass { folded: true } for diacritic folding).
   */
  async search(query, options = {}) {
    const qs = buildQuery({
      q: query,
      field: options.field,
      folded: options.folded ? 1 : 0,
    });
    return this._request(`/search${qs}`, { method: "GET" }, true);
  }

  /**
   * Export matching records as CSV text.
   */
  async exportCsv(filters = {}) {
    const qs = buildQuery({
      status: filters.status,
      template_id: filters.templateId ?? filters.template_id,
      book_ref: filters.bookRef ?? filters.book_ref,
    });
    return this._request(`/export.csv${qs}`, { method: "GET" }, false);
  }

  /**
   * Export matching records as structured JSON.
   */
  async exportJson(filters = {}) {
    const qs = buildQuery({
      status: filters.status,
      template_id: filters.templateId ?? filters.template_id,
      book_ref: filters.bookRef ?? filters.book_ref,
    });
    return this._request(`/export.json${qs}`, { method: "GET" }, true);
  }

  /**
   * Build the immutable scan image URL for a page.
   */
  getPageImageUrl(pageId) {
    return `${this.baseUrl}/pages/${encodeURIComponent(pageId)}/image`;
  }

  /**
   * Build the printable HTML transcript URL for an entry.
   */
  getTranscriptUrl(entryId) {
    return `${this.baseUrl}/entries/${encodeURIComponent(entryId)}/transcript`;
  }
}

export function createScribeClient(options) {
  return new ScribeClient(options);
}

/**
 * Resolve the path to the bundled `libaxil-scribe` Xylem module (without `.so`/`.dylib`/`.dll` suffix)
 * for loading into `axil -m <modulePath>`. Returns null when unavailable (e.g. browser).
 */
export function getModulePath() {
  const nb = getNodeBuiltins();
  if (!nb) return null;
  const { fs, path, pkgRoot } = nb;
  const keys = getPlatformPrebuildKeys();
  const dirs = [
    ...keys.map((k) => path.join(pkgRoot, "prebuilds", k)),
    path.join(pkgRoot, "build", "Release"),
    path.resolve(pkgRoot, "..", "lib"),
  ];
  for (const d of dirs) {
    const stem = path.join(d, "libaxil-scribe");
    if (
      fs.existsSync(`${stem}.so`) ||
      fs.existsSync(`${stem}.dylib`) ||
      fs.existsSync(`${stem}.dll`)
    ) {
      return stem;
    }
  }
  return null;
}

/**
 * Resolve the path to the compiled `scribe_ux.wasm` isomorphic UX module.
 */
export function getWasmPath() {
  const nb = getNodeBuiltins();
  if (!nb) return null;
  const { fs, path, pkgRoot } = nb;
  const candidates = [
    path.join(pkgRoot, "dist", "scribe_ux.wasm"),
    path.resolve(pkgRoot, "..", "htdocs", "scribe_ux.wasm"),
  ];
  for (const p of candidates) {
    if (fs.existsSync(p)) return p;
  }
  return null;
}

/**
 * Resolve the directory containing declarative domain packs (`generic`, `pt-archival-handwriting`, etc.).
 */
export function getPacksDir() {
  const nb = getNodeBuiltins();
  if (!nb) return null;
  const { fs, path, pkgRoot } = nb;
  const candidates = [
    path.join(pkgRoot, "packs"),
    path.resolve(pkgRoot, "..", "packs"),
  ];
  for (const d of candidates) {
    if (fs.existsSync(d)) return d;
  }
  return null;
}

/**
 * Resolve the directory for a specific named domain pack (e.g. "generic", "pt-archival-handwriting").
 * Returns null if the pack or its `pack.json` does not exist.
 */
export function getPackDir(packName) {
  const nb = getNodeBuiltins();
  if (!nb || !packName) return null;
  const { fs, path } = nb;
  const packsDir = getPacksDir();
  if (!packsDir) return null;
  const cand = path.join(packsDir, packName);
  if (fs.existsSync(path.join(cand, "pack.json"))) return cand;
  return null;
}

/**
 * Resolve the directory containing legacy starter template JSON files.
 */
export function getTemplatesDir() {
  const nb = getNodeBuiltins();
  if (!nb) return null;
  const { fs, path, pkgRoot } = nb;
  const candidates = [
    path.join(pkgRoot, "templates"),
    path.resolve(pkgRoot, "..", "templates"),
  ];
  for (const d of candidates) {
    if (fs.existsSync(d)) return d;
  }
  return null;
}

/**
 * Resolve the C headers include directory (`ttypt/scribe.h`, etc.).
 */
export function getIncludeDir() {
  const nb = getNodeBuiltins();
  if (!nb) return null;
  const { fs, path, pkgRoot } = nb;
  const candidates = [
    path.join(pkgRoot, "include"),
    path.resolve(pkgRoot, "..", "include"),
  ];
  for (const d of candidates) {
    if (fs.existsSync(d)) return d;
  }
  return null;
}

/**
 * Resolve the directory containing `scribe.pc` and `axil-scribe.pc`.
 */
export function getPkgConfigPath() {
  const nb = getNodeBuiltins();
  if (!nb) return null;
  const { fs, path, pkgRoot } = nb;
  const candidates = [pkgRoot, path.resolve(pkgRoot, "..")];
  for (const d of candidates) {
    if (fs.existsSync(path.join(d, "scribe.pc"))) return d;
  }
  return null;
}
