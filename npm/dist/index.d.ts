export interface ScribeClientOptions {
  baseUrl?: string;
  timeoutMs?: number;
  headers?: Record<string, string>;
}

export interface ScribeFieldValue {
  key: string;
  raw_value: string | null;
  alt_value?: string | null;
  verified_value: string | null;
  flags: number;
  bbox?: [number, number, number, number];
}

export interface ScribePageSummary {
  id: string;
  sha256: string;
  book_ref: string;
  page_num: number;
}

export interface ScribeEntry {
  id: string;
  page_id: string;
  template_id: string;
  status: "pending" | "extracted" | "needs_review" | "verified";
  extractor_model: string;
  reviewed_by: string;
  reviewed_at: number;
  marginal_notes: string | null;
  transcript?: string | null;
  book_ref?: string;
  page_num?: number;
  fields: ScribeFieldValue[];
}

export interface ScribeFilterOptions {
  status?: string;
  templateId?: string;
  template_id?: string;
  bookRef?: string;
  book_ref?: string;
}

export interface ScribeIngestOptions {
  contentType?: string;
  templateId?: string;
  template_id?: string;
  bookRef?: string;
  book_ref?: string;
  pageNum?: number;
  page_num?: number;
  extract?: boolean;
}

export interface ScribeVerifyPayload {
  action?: "verify" | "save";
  reviewedBy?: string;
  reviewed_by?: string;
  marginalNotes?: string;
  marginal_notes?: string;
  fields?: Record<string, string | null>;
  illegible?: Record<string, boolean>;
}

export interface ScribeSearchOptions {
  field?: string;
  folded?: boolean;
}

export interface ScribeExtractOptions {
  deep?: boolean;
}

export class ScribeClient {
  baseUrl: string;
  timeoutMs: number;
  headers: Record<string, string>;

  constructor(options?: ScribeClientOptions);
  queue(filters?: ScribeFilterOptions): Promise<{ ok: boolean; entries: ScribeEntry[] }>;
  ingestBuffer(
    bytes: Uint8Array | ArrayBuffer,
    options?: ScribeIngestOptions
  ): Promise<{ ok: boolean; deduped: boolean; pages: ScribePageSummary[]; entries: ScribeEntry[] }>;
  getEntry(entryId: string): Promise<{ ok: boolean; entry: ScribeEntry }>;
  verifyEntry(entryId: string, payload?: ScribeVerifyPayload): Promise<{ ok: boolean; entry: ScribeEntry }>;
  extractEntry(entryId: string, options?: ScribeExtractOptions): Promise<{ ok: boolean; entries: ScribeEntry[] }>;
  addPageEntry(
    pageId: string,
    options?: { templateId?: string; template_id?: string }
  ): Promise<{ ok: boolean; entry: ScribeEntry }>;
  search(
    query: string,
    options?: ScribeSearchOptions
  ): Promise<{ ok: boolean; query: string; folded: boolean; entries: ScribeEntry[] }>;
  exportCsv(filters?: ScribeFilterOptions): Promise<string>;
  exportJson(filters?: ScribeFilterOptions): Promise<{ version: string; entries: ScribeEntry[] }>;
  getPageImageUrl(pageId: string): string;
  getTranscriptUrl(entryId: string): string;
}

export function createScribeClient(options?: ScribeClientOptions): ScribeClient;
export function getModulePath(): string | null;
export function getWasmPath(): string | null;
export function getPacksDir(): string | null;
export function getPackDir(packName: string): string | null;
export function getTemplatesDir(): string | null;
export function getIncludeDir(): string | null;
export function getPkgConfigPath(): string | null;
