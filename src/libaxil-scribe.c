#define _GNU_SOURCE
/*
 * Copyright (c) 2026, tty-pt
 * libaxil-scribe: Axil HTTP service module for axil-scribe.
 *
 * Provides No-JS SSR HTML via pure isomorphic libbud UX (ux/scribe_ux.c),
 * JSON API responses, binary-safe multipart/raw scan intake, immutable ETag
 * scan serving, liblemma search, and open CSV/JSON/printable transcript exports.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <bud/bud.h>
#include <json-c/json.h>
#include <ttypt/axil.h>
#include <ttypt/xy-mod.h>

#include "ttypt/scribe.h"
#include "ttypt/scribe-ux.h"
#include "ttypt/axil-scribe.h"

/* ── Global Module Context ─────────────────────────────────────── */

#define SCRIBE_MAX_MOCK_RESPONSES 4

typedef struct {
	scribe_store_t *store;
	scribe_registry_t *registry;
	scribe_dict_t *dict;
	scribe_pack_t *pack;
	char data_dir[256];
	char pack_dir[256];
	char templates_dir[256];
	char wasm_path[256];
	char auren_host[128];
	uint16_t auren_port;
	char model_primary[128];
	char model_secondary[128];
	char model_escalate[128];
	int enhance_handwriting;
	int timeout_ms;
	char *mock_responses[SCRIBE_MAX_MOCK_RESPONSES];
	size_t mock_response_count;
	int initialized;
} axil_scribe_ctx_t;

static axil_scribe_ctx_t g_ctx;

/* ── Dynamic String Builder ────────────────────────────────────── */

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} strbuf_t;

static int
sb_reserve(strbuf_t *sb, size_t extra)
{
	size_t need = sb->len + extra + 1;
	size_t ncap;
	char *grown;

	if (need <= sb->cap)
		return 0;
	ncap = sb->cap ? sb->cap * 2 : 1024;
	while (ncap < need)
		ncap *= 2;
	grown = realloc(sb->data, ncap);
	if (!grown)
		return -1;
	sb->data = grown;
	sb->cap = ncap;
	return 0;
}

static int
sb_append_n(strbuf_t *sb, const char *s, size_t n)
{
	if (sb_reserve(sb, n) != 0)
		return -1;
	if (n > 0)
		memcpy(sb->data + sb->len, s, n);
	sb->len += n;
	sb->data[sb->len] = '\0';
	return 0;
}

static int
sb_append(strbuf_t *sb, const char *s)
{
	return sb_append_n(sb, s ? s : "", s ? strlen(s) : 0);
}

static int
sb_append_csv(strbuf_t *sb, const char *val)
{
	const char *p;
	int quote = 0;

	if (!val)
		val = "";
	for (p = val; *p; p++) {
		if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
			quote = 1;
			break;
		}
	}
	if (!quote)
		return sb_append(sb, val);
	if (sb_append_n(sb, "\"", 1) != 0)
		return -1;
	for (p = val; *p; p++) {
		if (*p == '"') {
			if (sb_append_n(sb, "\"\"", 2) != 0)
				return -1;
		} else {
			if (sb_append_n(sb, p, 1) != 0)
				return -1;
		}
	}
	return sb_append_n(sb, "\"", 1);
}

static void
sb_free(strbuf_t *sb)
{
	free(sb->data);
	sb->data = NULL;
	sb->len = 0;
	sb->cap = 0;
}

/* ── File & Directory Helpers ──────────────────────────────────── */

static int
ensure_dir(const char *path)
{
	char tmp[512];
	size_t len;
	size_t i;

	if (!path || !path[0])
		return -1;
	len = strlen(path);
	if (len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);
	for (i = 1; i < len; i++) {
		if (tmp[i] == '/') {
			tmp[i] = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			tmp[i] = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static uint8_t *
read_file_all(const char *path, size_t *out_len)
{
	FILE *fp;
	struct stat st;
	uint8_t *buf;
	size_t rd;

	if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return NULL;
	fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	rd = fread(buf, 1, (size_t)st.st_size, fp);
	fclose(fp);
	if (rd != (size_t)st.st_size) {
		free(buf);
		return NULL;
	}
	buf[rd] = '\0';
	if (out_len)
		*out_len = rd;
	return buf;
}

static void
load_mock_json_config(const char *spec)
{
	uint8_t *file_buf = NULL;
	const char *json_text = spec;
	struct json_object *root;

	if (!spec || !spec[0])
		return;
	if (spec[0] != '{' && spec[0] != '[') {
		file_buf = read_file_all(spec, NULL);
		if (!file_buf)
			return;
		json_text = (const char *)file_buf;
	}

	root = json_tokener_parse(json_text);
	if (!root) {
		free(file_buf);
		return;
	}

	if (json_object_is_type(root, json_type_array)) {
		size_t n = json_object_array_length(root);
		size_t i;
		for (i = 0; i < n && g_ctx.mock_response_count < SCRIBE_MAX_MOCK_RESPONSES; i++) {
			struct json_object *item = json_object_array_get_idx(root, i);
			const char *s = json_object_is_type(item, json_type_string)
			                ? json_object_get_string(item)
			                : json_object_to_json_string_ext(item, JSON_C_TO_STRING_PLAIN);
			if (s)
				g_ctx.mock_responses[g_ctx.mock_response_count++] = strdup(s);
		}
	} else {
		const char *s = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
		if (s)
			g_ctx.mock_responses[g_ctx.mock_response_count++] = strdup(s);
	}

	json_object_put(root);
	free(file_buf);
}

/* ── Content Negotiation & HTTP Utilities ──────────────────────── */

static int
wants_json(socket_t cfd)
{
	char val[256];

	if (axil_req_param(cfd, NULL, "format", val, sizeof(val)) > 0 &&
	    strcasecmp(val, "json") == 0)
		return 1;
	if (axil_header_get(cfd, "Accept", val, sizeof(val)) == 0 &&
	    strstr(val, "application/json") != NULL &&
	    strstr(val, "text/html") == NULL)
		return 1;
	return 0;
}

static size_t
get_content_length(socket_t cfd, const char *body)
{
	char cl[64];
	char *endp = NULL;
	unsigned long long v;

	if (axil_header_get(cfd, "Content-Length", cl, sizeof(cl)) == 0) {
		v = strtoull(cl, &endp, 10);
		if (endp && endp != cl)
			return (size_t)v;
	}
	return body ? strlen(body) : 0;
}

static void
respond_error(socket_t cfd, int code, const char *msg)
{
	if (wants_json(cfd)) {
		struct json_object *o = json_object_new_object();
		json_object_object_add(o, "ok", json_object_new_boolean(0));
		json_object_object_add(o, "error", json_object_new_string(msg ? msg : "error"));
		axil_respond_json(cfd, code, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
		json_object_put(o);
	} else {
		axil_respond_plain(cfd, code, msg ? msg : "error");
	}
}

static void
respond_binary(socket_t cfd, int code, const char *mime,
               const uint8_t *data, size_t len,
               const char *etag, const char *cache_control)
{
	char cl[64];
	void *defer;

	snprintf(cl, sizeof(cl), "%zu", len);
	axil_header_set(cfd, "Content-Type", mime ? mime : "application/octet-stream");
	axil_header_set(cfd, "Content-Length", cl);
	if (etag && etag[0])
		axil_header_set(cfd, "ETag", etag);
	if (cache_control && cache_control[0])
		axil_header_set(cfd, "Cache-Control", cache_control);

	defer = axil_respond_defer(cfd, code);
	if (!defer)
		return;
	if (!(axil_flags(cfd) & DF_HEAD) && data && len > 0)
		axil_write(cfd, (void *)data, len);
	axil_respond_defer_done(defer);
}

static void
resolve_reviewer(socket_t cfd, const char *submitted, char *out, size_t outsz)
{
	char remote[128];

	if (!(axil_flags(cfd) & DF_AUTH_AUTO) &&
	    axil_env_get(cfd, remote, sizeof(remote), "REMOTE_USER") == 0 &&
	    remote[0] != '\0') {
		snprintf(out, outsz, "%.*s", (int)(outsz > 0 ? outsz - 1 : 0), remote);
		return;
	}
	if (submitted && submitted[0]) {
		snprintf(out, outsz, "%.*s", (int)(outsz > 0 ? outsz - 1 : 0), submitted);
		return;
	}
	snprintf(out, outsz, "local");
}

/* ── CSS Stylesheet (Split Chunks to Respect ISO C String Limits) ─ */

static const char *const SCRIBE_CSS_CHUNKS[] = {
	":root{--bg:#f7f5ef;--card:#fff;--ink:#1f2421;--muted:#5c645f;"
	"--line:#d8d3c5;--accent:#1b5e4a;--accent-ink:#fff;"
	"--conflict:#b45309;--conflict-bg:#fef3c7;"
	"--invalid:#b91c1c;--invalid-bg:#fee2e2;"
	"--illeg:#6b21a8;--illeg-bg:#f3e8ff;"
	"--ok:#15803d;--ok-bg:#dcfce7;}\n"
	"*{box-sizing:border-box;}body{margin:0;background:var(--bg);color:var(--ink);"
	"font:14px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;}\n"
	".scribe-header{display:flex;align-items:center;justify-content:space-between;"
	"padding:10px 18px;background:#16241f;color:#f4f1ea;border-bottom:2px solid var(--accent);}\n"
	".scribe-brand{color:#fff;font-weight:700;font-size:16px;text-decoration:none;letter-spacing:.03em;}\n"
	".scribe-pack-badge{background:rgba(255,255,255,.16);color:#e6f2ed;font-size:11px;"
	"padding:2px 8px;border-radius:99px;font-weight:600;}\n"
	".scribe-nav{display:flex;gap:14px;flex-wrap:wrap;}\n"
	".scribe-nav a{color:#d5e2dc;text-decoration:none;padding:4px 8px;border-radius:4px;}\n"
	".scribe-nav a.is-active,.scribe-nav a:hover{background:rgba(255,255,255,.14);color:#fff;}\n"
	".scribe-main{max-width:1440px;margin:0 auto;padding:16px;}\n",

	".scribe-card{background:var(--card);border:1px solid var(--line);border-radius:8px;"
	"padding:16px;margin-bottom:16px;}\n"
	".scribe-card h2{margin:0 0 12px;font-size:16px;}\n"
	".scribe-flash{background:var(--ok-bg);border:1px solid var(--ok);color:#14532d;"
	"padding:10px 14px;border-radius:6px;margin-bottom:14px;}\n"
	".scribe-form-row,.scribe-filter-form,.scribe-search-form,.scribe-inline-form{"
	"display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:10px;}\n"
	".scribe-input,.scribe-select,.scribe-textarea{border:1px solid #bfb8a6;border-radius:5px;"
	"padding:6px 9px;font:inherit;background:#fff;color:var(--ink);}\n"
	".scribe-input-sm{width:76px;}.scribe-textarea{width:100%;resize:vertical;}\n"
	".scribe-btn{display:inline-block;border:1px solid #aeb8b2;background:#f3f1eb;color:var(--ink);"
	"padding:6px 12px;border-radius:5px;text-decoration:none;cursor:pointer;font:inherit;}\n"
	".scribe-btn:hover{background:#e7e3d8;}\n"
	".scribe-btn-primary{background:var(--accent);border-color:var(--accent);color:var(--accent-ink);}\n"
	".scribe-btn-primary:hover{background:#144939;}\n"
	".scribe-btn-sm{padding:4px 8px;font-size:12px;}\n",

	".scribe-table-wrap{overflow-x:auto;}.scribe-table{width:100%;border-collapse:collapse;}\n"
	".scribe-table th,.scribe-table td{padding:8px 10px;border-bottom:1px solid var(--line);"
	"text-align:left;vertical-align:top;}\n"
	".scribe-table th{background:#f3f0e7;font-weight:600;}\n"
	".scribe-badge{display:inline-block;padding:2px 7px;border-radius:99px;font-size:11px;font-weight:600;}\n"
	".scribe-status-verified{background:var(--ok-bg);color:var(--ok);}\n"
	".scribe-status-needs_review{background:var(--conflict-bg);color:var(--conflict);}\n"
	".scribe-status-extracted{background:#dbeafe;color:#1d4ed8;}\n"
	".scribe-status-pending{background:#e5e7eb;color:#374151;}\n"
	".scribe-flag-conflict{background:var(--conflict-bg);color:var(--conflict);}\n"
	".scribe-flag-invalid{background:var(--invalid-bg);color:var(--invalid);}\n"
	".scribe-flag-illegible{background:var(--illeg-bg);color:var(--illeg);}\n"
	".scribe-flag-edited{background:#e0f2fe;color:#0369a1;}\n",

	".scribe-workbench{display:grid;grid-template-columns:minmax(360px,1.15fr) minmax(360px,1fr);"
	"gap:16px;align-items:start;}\n"
	"@media(max-width:920px){.scribe-workbench{grid-template-columns:1fr;}}\n"
	".scribe-scan-pane,.scribe-form-pane{background:var(--card);border:1px solid var(--line);"
	"border-radius:8px;padding:14px;}\n"
	".scribe-toolbar{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin-bottom:10px;}\n"
	".scribe-scan-viewport{overflow:auto;border:1px solid var(--line);border-radius:6px;"
	"background:#232624;min-height:420px;max-height:74vh;display:flex;align-items:flex-start;"
	"justify-content:center;padding:8px;}\n"
	".scribe-scan-stage{position:relative;display:inline-block;transform-origin:top center;}\n"
	".scribe-scan-img{display:block;max-width:100%;height:auto;}\n"
	".scribe-bbox{position:absolute;border:2px solid #f59e0b;background:rgba(245,158,11,.18);"
	"pointer-events:none;}\n"
	".scribe-page-actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px;align-items:center;}\n"
	".scribe-meta-bar{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:12px;"
	"font-size:12px;color:var(--muted);}\n",

	".scribe-field{border:1px solid var(--line);border-radius:6px;padding:10px;margin-bottom:10px;"
	"background:#fcfbf8;}\n"
	".scribe-field.is-conflict{border-left:4px solid var(--conflict);background:#fffbeb;}\n"
	".scribe-field.is-invalid{border-left:4px solid var(--invalid);background:#fef2f2;}\n"
	".scribe-field.is-illegible{border-left:4px solid var(--illeg);background:#faf5ff;}\n"
	".scribe-field.is-active{outline:2px solid var(--accent);}\n"
	".scribe-field-head{display:flex;justify-content:space-between;align-items:center;"
	"gap:8px;margin-bottom:4px;}\n"
	".scribe-label{font-weight:600;display:block;margin-bottom:4px;}\n"
	".scribe-req{color:var(--invalid);}.scribe-flags{display:flex;gap:4px;flex-wrap:wrap;}\n"
	".scribe-raw-value{font-size:12px;color:var(--muted);margin-bottom:5px;font-family:ui-monospace,monospace;}\n"
	".scribe-alt-value{font-size:12px;color:var(--conflict);margin-bottom:5px;font-family:ui-monospace,monospace;}\n"
	".scribe-transcript-box{border:1px solid var(--line);border-radius:6px;background:#faf8f2;"
	"padding:8px 10px;margin-bottom:12px;}\n"
	".scribe-transcript-box summary{cursor:pointer;font-weight:600;font-size:13px;color:var(--accent);}\n"
	".scribe-transcript-box pre{margin:8px 0 0;white-space:pre-wrap;font-size:12px;line-height:1.5;"
	"font-family:ui-serif,Georgia,serif;color:var(--ink);max-height:240px;overflow:auto;}\n"
	".scribe-field .scribe-input{width:100%;margin-bottom:5px;}\n"
	".scribe-check{display:inline-flex;align-items:center;gap:5px;font-size:12px;color:var(--muted);}\n"
	".scribe-suggestions{margin-top:5px;display:flex;gap:6px;flex-wrap:wrap;align-items:center;}\n"
	".scribe-chip{display:inline-block;padding:2px 8px;border-radius:99px;background:#eef2ff;"
	"border:1px solid #c7d2fe;color:#3730a3;font-size:12px;}\n"
	".scribe-submit-bar{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px;}\n"
	".scribe-muted{color:var(--muted);}\n"
	".scribe-transcript{max-width:860px;margin:24px auto;background:#fff;padding:24px;"
	"border:1px solid var(--line);}\n"
	"@media print{.scribe-header,.scribe-no-print{display:none!important;}"
	"body{background:#fff;}.scribe-transcript{border:none;margin:0;padding:0;}}\n",
	NULL
};

/* ── UX State & JSON Serialization Builders ────────────────────── */

static void
populate_pack_ui(scribe_ux_state_t *st)
{
	if (!g_ctx.pack)
		return;
	if (g_ctx.pack->label && g_ctx.pack->label[0])
		snprintf(st->pack_label, sizeof(st->pack_label), "%s", g_ctx.pack->label);
	else if (g_ctx.pack->id && g_ctx.pack->id[0])
		snprintf(st->pack_label, sizeof(st->pack_label), "%s", g_ctx.pack->id);
	if (g_ctx.pack->ui_source_label && g_ctx.pack->ui_source_label[0])
		snprintf(st->ui_source_label, sizeof(st->ui_source_label), "%s", g_ctx.pack->ui_source_label);
	if (g_ctx.pack->ui_notes_label && g_ctx.pack->ui_notes_label[0])
		snprintf(st->ui_notes_label, sizeof(st->ui_notes_label), "%s", g_ctx.pack->ui_notes_label);
	if (g_ctx.pack->ui_transcript_label && g_ctx.pack->ui_transcript_label[0])
		snprintf(st->ui_transcript_label, sizeof(st->ui_transcript_label), "%s", g_ctx.pack->ui_transcript_label);
	if (g_ctx.pack->ui_deep_label && g_ctx.pack->ui_deep_label[0])
		snprintf(st->ui_deep_label, sizeof(st->ui_deep_label), "%s", g_ctx.pack->ui_deep_label);
}

static void
populate_template_options(scribe_ux_state_t *st)
{
	size_t n = scribe_registry_count(g_ctx.registry);
	size_t i;

	populate_pack_ui(st);
	st->template_count = 0;
	for (i = 0; i < n && st->template_count < SCRIBE_UX_MAX_TEMPLATES; i++) {
		const scribe_template_t *tpl = scribe_registry_at(g_ctx.registry, i);
		if (!tpl)
			continue;
		snprintf(st->templates[st->template_count].id,
		         sizeof(st->templates[st->template_count].id), "%s", tpl->id);
		snprintf(st->templates[st->template_count].label,
		         sizeof(st->templates[st->template_count].label), "%s",
		         tpl->label ? tpl->label : tpl->id);
		st->template_count++;
	}
}

static uint32_t
count_flagged_fields(const scribe_entry_t *e)
{
	size_t i;
	uint32_t c = 0;

	for (i = 0; i < e->field_count; i++) {
		if (e->fields[i].flags & SCRIBE_UX_FLAG_REVIEW)
			c++;
	}
	return c;
}

static void
build_entry_summary(const scribe_entry_t *e, char *out, size_t outsz)
{
	size_t i;
	size_t used = 0;

	out[0] = '\0';
	for (i = 0; i < e->field_count && used + 8 < outsz; i++) {
		const char *v = e->fields[i].verified_value
		                ? e->fields[i].verified_value
		                : e->fields[i].raw_value;
		int w;

		if (!v || !v[0])
			continue;
		w = snprintf(out + used, outsz - used, "%s%s", used ? " · " : "", v);
		if (w <= 0)
			break;
		used += (size_t)w;
		if (used >= outsz)
			used = outsz - 1;
	}
}

static void
append_queue_item(scribe_ux_state_t *st, const scribe_entry_t *e,
                  const scribe_page_t *p)
{
	scribe_ux_queue_item_t *it;

	if (st->item_count >= SCRIBE_UX_MAX_QUEUE)
		return;
	it = &st->items[st->item_count++];
	memset(it, 0, sizeof(*it));
	snprintf(it->entry_id, sizeof(it->entry_id), "%s", e->id);
	snprintf(it->page_id, sizeof(it->page_id), "%s", e->page_id);
	if (p) {
		snprintf(it->book_ref, sizeof(it->book_ref), "%s", p->book_ref);
		it->page_num = p->page_num;
	}
	snprintf(it->template_id, sizeof(it->template_id), "%s", e->template_id);
	snprintf(it->status, sizeof(it->status), "%s", scribe_status_str(e->status));
	it->flagged_count = count_flagged_fields(e);
	build_entry_summary(e, it->summary, sizeof(it->summary));
}

static struct json_object *
ux_state_to_flat_json(const scribe_ux_state_t *st)
{
	struct json_object *o = json_object_new_object();
	const char *vname = (st->view == SCRIBE_UX_VIEW_ENTRY) ? "entry"
	                  : (st->view == SCRIBE_UX_VIEW_SEARCH) ? "search"
	                  : "queue";
	size_t i;

	json_object_object_add(o, "view", json_object_new_string(vname));
	json_object_object_add(o, "flash", json_object_new_string(st->flash));
	json_object_object_add(o, "pack_label", json_object_new_string(st->pack_label));
	json_object_object_add(o, "ui_source_label", json_object_new_string(st->ui_source_label));
	json_object_object_add(o, "ui_notes_label", json_object_new_string(st->ui_notes_label));
	json_object_object_add(o, "ui_transcript_label", json_object_new_string(st->ui_transcript_label));
	json_object_object_add(o, "ui_deep_label", json_object_new_string(st->ui_deep_label));
	json_object_object_add(o, "entry_id", json_object_new_string(st->entry_id));
	json_object_object_add(o, "page_id", json_object_new_string(st->page_id));
	json_object_object_add(o, "book_ref", json_object_new_string(st->book_ref));
	json_object_object_add(o, "page_num", json_object_new_int((int)st->page_num));
	json_object_object_add(o, "template_id", json_object_new_string(st->template_id));
	json_object_object_add(o, "template_label", json_object_new_string(st->template_label));
	json_object_object_add(o, "status", json_object_new_string(st->status));
	json_object_object_add(o, "extractor_model", json_object_new_string(st->extractor_model));
	json_object_object_add(o, "reviewed_by", json_object_new_string(st->reviewed_by));
	json_object_object_add(o, "image_url", json_object_new_string(st->image_url));
	json_object_object_add(o, "marginal_notes", json_object_new_string(st->marginal_notes));
	json_object_object_add(o, "transcript", json_object_new_string(st->transcript));
	json_object_object_add(o, "zoom_pct", json_object_new_int(st->zoom_pct));
	json_object_object_add(o, "rotate_deg", json_object_new_int(st->rotate_deg));
	json_object_object_add(o, "pan_x", json_object_new_int(st->pan_x));
	json_object_object_add(o, "pan_y", json_object_new_int(st->pan_y));
	json_object_object_add(o, "active_field", json_object_new_int(st->active_field));
	json_object_object_add(o, "high_contrast", json_object_new_int(st->high_contrast ? 1 : 0));
	json_object_object_add(o, "field_count", json_object_new_int((int)st->field_count));

	for (i = 0; i < st->field_count; i++) {
		const scribe_ux_field_t *f = &st->fields[i];
		char k[32];

		snprintf(k, sizeof(k), "f%zu_key", i);
		json_object_object_add(o, k, json_object_new_string(f->key));
		snprintf(k, sizeof(k), "f%zu_label", i);
		json_object_object_add(o, k, json_object_new_string(f->label));
		snprintf(k, sizeof(k), "f%zu_type", i);
		json_object_object_add(o, k, json_object_new_string(f->type));
		snprintf(k, sizeof(k), "f%zu_hint", i);
		json_object_object_add(o, k, json_object_new_string(f->format_hint));
		snprintf(k, sizeof(k), "f%zu_enums", i);
		json_object_object_add(o, k, json_object_new_string(f->enum_options));
		snprintf(k, sizeof(k), "f%zu_req", i);
		json_object_object_add(o, k, json_object_new_int(f->required ? 1 : 0));
		snprintf(k, sizeof(k), "f%zu_raw", i);
		json_object_object_add(o, k, json_object_new_string(f->raw_value));
		snprintf(k, sizeof(k), "f%zu_alt", i);
		json_object_object_add(o, k, json_object_new_string(f->alt_value));
		snprintf(k, sizeof(k), "f%zu_ver", i);
		json_object_object_add(o, k, json_object_new_string(f->verified_value));
		snprintf(k, sizeof(k), "f%zu_flags", i);
		json_object_object_add(o, k, json_object_new_int((int)f->flags));
		snprintf(k, sizeof(k), "f%zu_bx", i);
		json_object_object_add(o, k, json_object_new_int((int)(f->bbox[0] * 1000.0f)));
		snprintf(k, sizeof(k), "f%zu_by", i);
		json_object_object_add(o, k, json_object_new_int((int)(f->bbox[1] * 1000.0f)));
		snprintf(k, sizeof(k), "f%zu_bw", i);
		json_object_object_add(o, k, json_object_new_int((int)(f->bbox[2] * 1000.0f)));
		snprintf(k, sizeof(k), "f%zu_bh", i);
		json_object_object_add(o, k, json_object_new_int((int)(f->bbox[3] * 1000.0f)));
	}
	return o;
}

static void
respond_ssr_page(socket_t cfd, int code, const char *title,
                 const scribe_ux_state_t *st)
{
	bud_arena *arena = bud_arena_new(16384);
	bud_arena *prev = bud_arena_push(arena);
	bud_node *tree = scribe_ux_render(st);
	char *inner_html = tree ? bud_render_html(tree) : NULL;
	struct json_object *state_json = ux_state_to_flat_json(st);
	const char *json_str = state_json
	                       ? json_object_to_json_string_ext(state_json, JSON_C_TO_STRING_PLAIN)
	                       : "{}";
	strbuf_t doc = { NULL, 0, 0 };

	bud_arena_pop(prev);
	bud_arena_free(arena);

	sb_append(&doc, "<!doctype html>\n<html lang=\"en\">\n<head>\n"
	                "<meta charset=\"utf-8\">\n"
	                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
	                "<title>");
	sb_append(&doc, title ? title : "axil-scribe");
	sb_append(&doc, "</title>\n"
	                "<link rel=\"stylesheet\" href=\"/assets/scribe.css\">\n"
	                "</head>\n<body>\n"
	                "<div id=\"bud-root\" data-wasm=\"/assets/scribe_ux.wasm\" "
	                "data-module=\"scribe_ux\">");
	if (inner_html)
		sb_append(&doc, inner_html);
	sb_append(&doc, "</div>\n"
	                "<script type=\"application/json\" id=\"bud-state\">");
	sb_append(&doc, json_str);
	sb_append(&doc, "</script>\n</body>\n</html>\n");

	bud_free_string(inner_html);
	if (state_json)
		json_object_put(state_json);

	axil_header_set(cfd, "Content-Type", "text/html; charset=utf-8");
	axil_header_set(cfd, "Cache-Control", "no-store");
	axil_respond(cfd, code, doc.data ? doc.data : "");
	sb_free(&doc);
}

static struct json_object *
entry_to_json(const scribe_entry_t *e, const scribe_page_t *p)
{
	struct json_object *o = json_object_new_object();
	struct json_object *farr = json_object_new_array();
	size_t i;

	json_object_object_add(o, "id", json_object_new_string(e->id));
	json_object_object_add(o, "page_id", json_object_new_string(e->page_id));
	json_object_object_add(o, "template_id", json_object_new_string(e->template_id));
	json_object_object_add(o, "status", json_object_new_string(scribe_status_str(e->status)));
	json_object_object_add(o, "extractor_model", json_object_new_string(e->extractor_model));
	json_object_object_add(o, "reviewed_by", json_object_new_string(e->reviewed_by));
	json_object_object_add(o, "reviewed_at", json_object_new_int64(e->reviewed_at));
	json_object_object_add(o, "marginal_notes",
	                       json_object_new_string(e->marginal_notes ? e->marginal_notes : ""));
	if (e->transcript && e->transcript[0]) {
		json_object_object_add(o, "transcript", json_object_new_string(e->transcript));
	}
	if (p) {
		struct json_object *po = json_object_new_object();
		char img_url[96];
		snprintf(img_url, sizeof(img_url), "/pages/%s/image", p->id);
		json_object_object_add(po, "id", json_object_new_string(p->id));
		json_object_object_add(po, "sha256", json_object_new_string(p->sha256));
		json_object_object_add(po, "book_ref", json_object_new_string(p->book_ref));
		json_object_object_add(po, "page_num", json_object_new_int((int)p->page_num));
		json_object_object_add(po, "mime", json_object_new_string(p->mime));
		json_object_object_add(po, "width", json_object_new_int((int)p->width));
		json_object_object_add(po, "height", json_object_new_int((int)p->height));
		json_object_object_add(po, "image_url", json_object_new_string(img_url));
		json_object_object_add(o, "page", po);
	}
	for (i = 0; i < e->field_count; i++) {
		struct json_object *fo = json_object_new_object();
		struct json_object *bb = json_object_new_array();
		int b;
		json_object_object_add(fo, "key", json_object_new_string(e->fields[i].key));
		json_object_object_add(fo, "raw_value",
		                       e->fields[i].raw_value
		                       ? json_object_new_string(e->fields[i].raw_value)
		                       : NULL);
		if (e->fields[i].alt_value && e->fields[i].alt_value[0]) {
			json_object_object_add(fo, "alt_value",
			                       json_object_new_string(e->fields[i].alt_value));
		}
		json_object_object_add(fo, "verified_value",
		                       e->fields[i].verified_value
		                       ? json_object_new_string(e->fields[i].verified_value)
		                       : NULL);
		json_object_object_add(fo, "flags", json_object_new_int((int)e->fields[i].flags));
		for (b = 0; b < 4; b++)
			json_object_array_add(bb, json_object_new_double(e->fields[i].bbox[b]));
		json_object_object_add(fo, "bbox", bb);
		json_object_array_add(farr, fo);
	}
	json_object_object_add(o, "fields", farr);
	return o;
}

/* ── Extraction Runner (Mock or Auren) ─────────────────────────── */

static int
run_page_extraction(const scribe_page_t *page, const scribe_template_t *tpl,
                    int deep_mode,
                    scribe_entry_t **out_entries, size_t *out_count)
{
	scribe_extract_opts_t opts;
	memset(&opts, 0, sizeof(opts));
	opts.pack = g_ctx.pack;

	if (g_ctx.mock_response_count > 0) {
		scribe_mock_extractor_t mctx;
		opts.model_primary = g_ctx.model_primary[0] ? g_ctx.model_primary : "mock-pass1";
		opts.model_secondary = (g_ctx.mock_response_count >= 2)
		                       ? (g_ctx.model_secondary[0] ? g_ctx.model_secondary : "mock-pass2")
		                       : NULL;
		opts.model_escalate = (deep_mode || g_ctx.model_escalate[0])
		                      ? (g_ctx.model_escalate[0] ? g_ctx.model_escalate : "mock-escalate")
		                      : NULL;
		opts.enhance_handwriting = (deep_mode || g_ctx.enhance_handwriting) ? 1 : 0;
		mctx.responses = (const char *const *)g_ctx.mock_responses;
		mctx.response_count = g_ctx.mock_response_count;
		mctx.call_index = 0;
		return scribe_extract_page_ex(g_ctx.store, page, tpl,
		                              scribe_extractor_mock(), &mctx,
		                              &opts, out_entries, out_count);
	} else {
		scribe_auren_config_t acfg;
		memset(&acfg, 0, sizeof(acfg));
		acfg.host = g_ctx.auren_host;
		acfg.port = g_ctx.auren_port;
		acfg.endpoint = "/v1/chat/completions";
		acfg.timeout_ms = g_ctx.timeout_ms > 0 ? g_ctx.timeout_ms : 180000;

		opts.model_primary = g_ctx.model_primary;
		opts.model_secondary = g_ctx.model_secondary[0] ? g_ctx.model_secondary : NULL;
		opts.model_escalate = g_ctx.model_escalate[0] ? g_ctx.model_escalate : NULL;
		opts.enhance_handwriting = g_ctx.enhance_handwriting;

		if (deep_mode) {
			const char *strong = g_ctx.model_escalate[0] ? g_ctx.model_escalate : "smart";
			opts.enhance_handwriting = 1;
			opts.model_escalate = strong;
			if (!opts.model_secondary)
				opts.model_primary = strong;
		}

		return scribe_extract_page_ex(g_ctx.store, page, tpl,
		                              scribe_extractor_auren(), &acfg,
		                              &opts, out_entries, out_count);
	}
}

/* ── Binary-Safe Multipart Parser ──────────────────────────────── */

typedef struct {
	const uint8_t *file_data;
	size_t file_len;
	char filename[256];
	char template_id[64];
	char book_ref[64];
	uint32_t page_num;
	int extract;
} multipart_ingest_t;

static int
parse_multipart_ingest(const char *content_type, const uint8_t *body,
                       size_t body_len, multipart_ingest_t *out)
{
	const char *bpos;
	char boundary[160];
	char delim[180];
	size_t blen = 0;
	size_t dlen;
	const uint8_t *cur;
	const uint8_t *end = body + body_len;

	memset(out, 0, sizeof(*out));
	out->page_num = 1;
	out->extract = 0;

	if (!content_type || !body || body_len == 0)
		return -1;
	bpos = strcasestr(content_type, "boundary=");
	if (!bpos)
		return -1;
	bpos += 9;
	if (*bpos == '"')
		bpos++;
	while (bpos[blen] && bpos[blen] != '"' && bpos[blen] != ';' &&
	       !isspace((unsigned char)bpos[blen]) && blen + 1 < sizeof(boundary))
		blen++;
	if (blen == 0)
		return -1;
	memcpy(boundary, bpos, blen);
	boundary[blen] = '\0';

	snprintf(delim, sizeof(delim), "--%s", boundary);
	dlen = strlen(delim);

	cur = memmem(body, body_len, delim, dlen);
	while (cur && cur + dlen <= end) {
		const uint8_t *after = cur + dlen;
		const uint8_t *hdr_end;
		const uint8_t *part_data;
		const uint8_t *next_delim;
		size_t hdr_len;
		size_t part_len;
		char *hdr_str;
		char name[64];

		if (after + 2 <= end && after[0] == '-' && after[1] == '-')
			break;
		if (after + 2 <= end && after[0] == '\r' && after[1] == '\n')
			after += 2;
		else if (after + 1 <= end && after[0] == '\n')
			after += 1;

		hdr_end = memmem(after, (size_t)(end - after), "\r\n\r\n", 4);
		if (hdr_end) {
			hdr_len = (size_t)(hdr_end - after);
			part_data = hdr_end + 4;
		} else {
			hdr_end = memmem(after, (size_t)(end - after), "\n\n", 2);
			if (!hdr_end)
				break;
			hdr_len = (size_t)(hdr_end - after);
			part_data = hdr_end + 2;
		}

		next_delim = memmem(part_data, (size_t)(end - part_data), delim, dlen);
		if (!next_delim)
			break;
		part_len = (size_t)(next_delim - part_data);
		if (part_len >= 2 && part_data[part_len - 2] == '\r' &&
		    part_data[part_len - 1] == '\n')
			part_len -= 2;
		else if (part_len >= 1 && part_data[part_len - 1] == '\n')
			part_len -= 1;

		hdr_str = malloc(hdr_len + 1);
		if (!hdr_str)
			return -1;
		memcpy(hdr_str, after, hdr_len);
		hdr_str[hdr_len] = '\0';

		name[0] = '\0';
		{
			const char *np = strcasestr(hdr_str, "name=\"");
			if (np) {
				const char *q;
				np += 6;
				q = strchr(np, '"');
				if (q) {
					size_t nl = (size_t)(q - np);
					if (nl >= sizeof(name))
						nl = sizeof(name) - 1;
					memcpy(name, np, nl);
					name[nl] = '\0';
				}
			}
		}

		{
			const char *fp = strcasestr(hdr_str, "filename=\"");
			if (fp || strcasecmp(name, "file") == 0 || strcasecmp(name, "scan") == 0) {
				if (fp) {
					const char *q;
					fp += 10;
					q = strchr(fp, '"');
					if (q) {
						size_t fl = (size_t)(q - fp);
						if (fl >= sizeof(out->filename))
							fl = sizeof(out->filename) - 1;
						memcpy(out->filename, fp, fl);
						out->filename[fl] = '\0';
					}
				}
				out->file_data = part_data;
				out->file_len = part_len;
			} else if (strcasecmp(name, "template_id") == 0) {
				size_t cl = part_len < sizeof(out->template_id) - 1
				            ? part_len : sizeof(out->template_id) - 1;
				memcpy(out->template_id, part_data, cl);
				out->template_id[cl] = '\0';
			} else if (strcasecmp(name, "book_ref") == 0) {
				size_t cl = part_len < sizeof(out->book_ref) - 1
				            ? part_len : sizeof(out->book_ref) - 1;
				memcpy(out->book_ref, part_data, cl);
				out->book_ref[cl] = '\0';
			} else if (strcasecmp(name, "page_num") == 0) {
				char numbuf[32];
				size_t cl = part_len < sizeof(numbuf) - 1 ? part_len : sizeof(numbuf) - 1;
				memcpy(numbuf, part_data, cl);
				numbuf[cl] = '\0';
				out->page_num = (uint32_t)strtoul(numbuf, NULL, 10);
				if (out->page_num == 0)
					out->page_num = 1;
			} else if (strcasecmp(name, "extract") == 0) {
				if (part_len > 0 && part_data[0] != '0' && part_data[0] != 'f' &&
				    part_data[0] != 'F')
					out->extract = 1;
			}
		}

		free(hdr_str);
		cur = next_delim;
	}

	return (out->file_data && out->file_len > 0) ? 0 : -1;
}

/* ── Route Collection Helper ───────────────────────────────────── */

static void
collect_filtered_entries(const char *status_filter,
                         const char *template_filter,
                         const char *book_filter,
                         scribe_ux_state_t *ux_out,
                         struct json_object *json_arr_out)
{
	static const scribe_status_t order[] = {
		SCRIBE_STATUS_NEEDS_REVIEW,
		SCRIBE_STATUS_EXTRACTED,
		SCRIBE_STATUS_PENDING,
		SCRIBE_STATUS_VERIFIED,
	};
	size_t si;

	for (si = 0; si < sizeof(order) / sizeof(order[0]); si++) {
		scribe_id_list_t ids;
		size_t i;

		if (status_filter && status_filter[0]) {
			scribe_status_t want;
			if (scribe_status_parse(status_filter, &want) != 0 || want != order[si])
				continue;
		}
		if (scribe_entry_list_by_status(g_ctx.store, order[si], &ids) != 0)
			continue;

		for (i = 0; i < ids.count; i++) {
			scribe_entry_t e;
			scribe_page_t p;
			int have_page;

			if (scribe_entry_get(g_ctx.store, ids.ids[i], &e) != 0)
				continue;
			have_page = (scribe_page_get(g_ctx.store, e.page_id, &p) == 0);

			if (template_filter && template_filter[0] &&
			    strcmp(e.template_id, template_filter) != 0) {
				scribe_entry_free(&e);
				continue;
			}
			if (book_filter && book_filter[0] &&
			    (!have_page || strcasestr(p.book_ref, book_filter) == NULL)) {
				scribe_entry_free(&e);
				continue;
			}

			if (ux_out)
				append_queue_item(ux_out, &e, have_page ? &p : NULL);
			if (json_arr_out)
				json_object_array_add(json_arr_out, entry_to_json(&e, have_page ? &p : NULL));

			scribe_entry_free(&e);
		}
		scribe_id_list_free(&ids);
	}
}

/* ── HTTP Handlers ─────────────────────────────────────────────── */

static int
handle_get_queue(socket_t cfd, char *body)
{
	scribe_ux_state_t st;
	char status_f[32] = "";
	char tpl_f[64] = "";
	char book_f[64] = "";

	(void)body;
	axil_req_param(cfd, NULL, "status", status_f, sizeof(status_f));
	axil_req_param(cfd, NULL, "template_id", tpl_f, sizeof(tpl_f));
	axil_req_param(cfd, NULL, "book_ref", book_f, sizeof(book_f));

	if (wants_json(cfd)) {
		struct json_object *root = json_object_new_object();
		struct json_object *arr = json_object_new_array();
		collect_filtered_entries(status_f, tpl_f, book_f, NULL, arr);
		json_object_object_add(root, "ok", json_object_new_boolean(1));
		json_object_object_add(root, "entries", arr);
		axil_respond_json(cfd, 200,
		                  json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
		json_object_put(root);
		return 0;
	}

	scribe_ux_state_init(&st);
	st.view = SCRIBE_UX_VIEW_QUEUE;
	snprintf(st.filter_status, sizeof(st.filter_status), "%s", status_f);
	snprintf(st.filter_template, sizeof(st.filter_template), "%s", tpl_f);
	snprintf(st.filter_book, sizeof(st.filter_book), "%s", book_f);
	populate_template_options(&st);
	collect_filtered_entries(status_f, tpl_f, book_f, &st, NULL);

	respond_ssr_page(cfd, 200, "axil-scribe — Review Queue", &st);
	return 0;
}

static const scribe_template_t *
resolve_template(const char *id)
{
	const scribe_template_t *tpl = NULL;

	if (id && id[0])
		tpl = scribe_registry_find(g_ctx.registry, id);
	if (!tpl)
		tpl = scribe_registry_find(g_ctx.registry, "generic_record");
	if (!tpl && scribe_registry_count(g_ctx.registry) > 0)
		tpl = scribe_registry_at(g_ctx.registry, 0);
	return tpl;
}

static int
handle_post_ingest(socket_t cfd, char *body)
{
	size_t body_len = get_content_length(cfd, body);
	char ct[256] = "";
	multipart_ingest_t mp;
	const uint8_t *file_data = NULL;
	size_t file_len = 0;
	char template_id[64] = "";
	char book_ref[64] = "";
	uint32_t page_num = 1;
	int do_extract = 1;
	scribe_image_info_t info;
	scribe_page_t *pages = NULL;
	size_t page_count = 0;
	int deduped = 0;
	const scribe_template_t *tpl;
	char first_entry_id[32] = "";
	struct json_object *jpages = NULL;
	struct json_object *jentries = NULL;
	size_t pi;

	if (!body || body_len == 0) {
		respond_error(cfd, 400, "empty request body");
		return 0;
	}

	axil_header_get(cfd, "Content-Type", ct, sizeof(ct));
	if (strncasecmp(ct, "multipart/form-data", 19) == 0) {
		if (parse_multipart_ingest(ct, (const uint8_t *)body, body_len, &mp) != 0) {
			respond_error(cfd, 400, "invalid multipart/form-data or missing file part");
			return 0;
		}
		file_data = mp.file_data;
		file_len = mp.file_len;
		snprintf(template_id, sizeof(template_id), "%s", mp.template_id);
		snprintf(book_ref, sizeof(book_ref), "%s", mp.book_ref);
		page_num = mp.page_num ? mp.page_num : 1;
		do_extract = mp.extract;
	} else {
		file_data = (const uint8_t *)body;
		file_len = body_len;
		axil_req_param(cfd, NULL, "template_id", template_id, sizeof(template_id));
		axil_req_param(cfd, NULL, "book_ref", book_ref, sizeof(book_ref));
		page_num = (uint32_t)axil_req_param_int(cfd, NULL, "page_num", 1);
		if (page_num == 0)
			page_num = 1;
		do_extract = axil_req_param_bool(cfd, NULL, "extract", 1);
	}

	if (scribe_sniff_image(file_data, file_len, &info) != 0) {
		respond_error(cfd, 400, "unsupported scan format (expected PNG, JPEG, WebP, or PDF)");
		return 0;
	}

	if (info.is_pdf) {
		char tmp_dir[320];
		char tmp_pdf[384];
		FILE *fp;
		snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp", g_ctx.data_dir);
		ensure_dir(tmp_dir);
		snprintf(tmp_pdf, sizeof(tmp_pdf), "%s/upload_%ld_%d.pdf",
		         tmp_dir, (long)time(NULL), (int)getpid());
		fp = fopen(tmp_pdf, "wb");
		if (!fp || fwrite(file_data, 1, file_len, fp) != file_len) {
			if (fp) fclose(fp);
			unlink(tmp_pdf);
			respond_error(cfd, 500, "failed to stage PDF upload");
			return 0;
		}
		fclose(fp);
		if (scribe_ingest_file(g_ctx.store, tmp_pdf, book_ref, page_num, 250,
		                       &pages, &page_count) != 0) {
			unlink(tmp_pdf);
			respond_error(cfd, 500, "PDF rasterization failed (is pdftoppm installed?)");
			return 0;
		}
		unlink(tmp_pdf);
	} else {
		pages = calloc(1, sizeof(*pages));
		if (!pages) {
			respond_error(cfd, 500, "out of memory");
			return 0;
		}
		if (scribe_ingest_buffer(g_ctx.store, file_data, file_len,
		                         book_ref, page_num, &pages[0], &deduped) != 0) {
			free(pages);
			respond_error(cfd, 500, "failed to ingest scan");
			return 0;
		}
		page_count = 1;
	}

	tpl = resolve_template(template_id);
	jpages = json_object_new_array();
	jentries = json_object_new_array();

	for (pi = 0; pi < page_count; pi++) {
		struct json_object *po = json_object_new_object();
		scribe_entry_t *entries = NULL;
		size_t entry_count = 0;
		size_t ei;

		json_object_object_add(po, "id", json_object_new_string(pages[pi].id));
		json_object_object_add(po, "sha256", json_object_new_string(pages[pi].sha256));
		json_object_object_add(po, "book_ref", json_object_new_string(pages[pi].book_ref));
		json_object_object_add(po, "page_num", json_object_new_int((int)pages[pi].page_num));
		json_object_array_add(jpages, po);

		if (tpl && do_extract) {
			if (run_page_extraction(&pages[pi], tpl, 0, &entries, &entry_count) != 0) {
				entries = NULL;
				entry_count = 0;
			}
		}

		/* Ensure at least one pending entry exists even if VLM was skipped/offline */
		if (entry_count == 0 && tpl) {
			scribe_entry_t pending;
			size_t fi;
			scribe_entry_init(&pending);
			snprintf(pending.page_id, sizeof(pending.page_id), "%s", pages[pi].id);
			snprintf(pending.template_id, sizeof(pending.template_id), "%s", tpl->id);
			pending.status = SCRIBE_STATUS_PENDING;
			for (fi = 0; fi < tpl->field_count; fi++)
				scribe_entry_set_field(&pending, tpl->fields[fi].key,
				                       NULL, NULL, SCRIBE_FLAG_NONE, NULL);
			if (scribe_entry_put(g_ctx.store, &pending) == 0) {
				if (!first_entry_id[0])
					snprintf(first_entry_id, sizeof(first_entry_id), "%s", pending.id);
				json_object_array_add(jentries, entry_to_json(&pending, &pages[pi]));
			}
			scribe_entry_free(&pending);
		} else {
			for (ei = 0; ei < entry_count; ei++) {
				if (!first_entry_id[0])
					snprintf(first_entry_id, sizeof(first_entry_id), "%s", entries[ei].id);
				json_object_array_add(jentries, entry_to_json(&entries[ei], &pages[pi]));
				scribe_entry_free(&entries[ei]);
			}
			free(entries);
		}
	}

	scribe_store_save(g_ctx.store);
	free(pages);

	if (wants_json(cfd)) {
		struct json_object *resp = json_object_new_object();
		json_object_object_add(resp, "ok", json_object_new_boolean(1));
		json_object_object_add(resp, "deduped", json_object_new_boolean(deduped));
		json_object_object_add(resp, "pages", jpages);
		json_object_object_add(resp, "entries", jentries);
		axil_respond_json(cfd, 201,
		                  json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN));
		json_object_put(resp);
		return 0;
	}

	json_object_put(jpages);
	json_object_put(jentries);

	if (first_entry_id[0]) {
		char loc[96];
		snprintf(loc, sizeof(loc), "/entries/%s", first_entry_id);
		axil_redirect(cfd, loc);
	} else {
		axil_redirect(cfd, "/queue");
	}
	return 0;
}

static int
extract_entry_year(const scribe_entry_t *e, const scribe_template_t *tpl)
{
	size_t i;

	if (tpl) {
		for (i = 0; i < tpl->field_count; i++) {
			const scribe_field_val_t *fv;
			const char *v;
			int y = 0;
			if (tpl->fields[i].type != SCRIBE_FIELD_DATE)
				continue;
			fv = scribe_entry_get_field(e, tpl->fields[i].key);
			if (!fv)
				continue;
			v = fv->verified_value ? fv->verified_value : fv->raw_value;
			if (v && scribe_validate_date_range(v, 0, 0, &y, NULL, NULL) == 0)
				return y;
		}
	}
	for (i = 0; i < e->field_count; i++) {
		const char *v = e->fields[i].verified_value
		                ? e->fields[i].verified_value
		                : e->fields[i].raw_value;
		int y = 0;
		if (v && strstr(e->fields[i].key, "date") &&
		    scribe_validate_date_range(v, 0, 0, &y, NULL, NULL) == 0)
			return y;
	}
	return 0;
}

static int
handle_get_entry(socket_t cfd, char *body)
{
	char id[64] = "";
	scribe_entry_t e;
	scribe_page_t p;
	int have_page;
	const scribe_template_t *tpl;
	scribe_ux_state_t st;
	int year_hint;
	size_t i;

	(void)body;
	if (axil_param(cfd, "id", id, sizeof(id)) <= 0) {
		respond_error(cfd, 400, "missing entry id");
		return 0;
	}
	if (scribe_entry_get(g_ctx.store, id, &e) != 0) {
		respond_error(cfd, 404, "entry not found");
		return 0;
	}
	have_page = (scribe_page_get(g_ctx.store, e.page_id, &p) == 0);
	tpl = scribe_registry_find(g_ctx.registry, e.template_id);

	if (wants_json(cfd)) {
		struct json_object *root = json_object_new_object();
		json_object_object_add(root, "ok", json_object_new_boolean(1));
		json_object_object_add(root, "entry", entry_to_json(&e, have_page ? &p : NULL));
		axil_respond_json(cfd, 200,
		                  json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
		json_object_put(root);
		scribe_entry_free(&e);
		return 0;
	}

	scribe_ux_state_init(&st);
	st.view = SCRIBE_UX_VIEW_ENTRY;
	populate_template_options(&st);
	snprintf(st.entry_id, sizeof(st.entry_id), "%s", e.id);
	snprintf(st.page_id, sizeof(st.page_id), "%s", e.page_id);
	if (have_page) {
		snprintf(st.book_ref, sizeof(st.book_ref), "%s", p.book_ref);
		st.page_num = p.page_num;
	}
	snprintf(st.template_id, sizeof(st.template_id), "%s", e.template_id);
	snprintf(st.template_label, sizeof(st.template_label), "%s",
	         (tpl && tpl->label) ? tpl->label : e.template_id);
	snprintf(st.status, sizeof(st.status), "%s", scribe_status_str(e.status));
	snprintf(st.extractor_model, sizeof(st.extractor_model), "%s", e.extractor_model);
	snprintf(st.reviewed_by, sizeof(st.reviewed_by), "%s", e.reviewed_by);
	snprintf(st.image_url, sizeof(st.image_url), "/pages/%s/image", e.page_id);
	if (e.marginal_notes)
		snprintf(st.marginal_notes, sizeof(st.marginal_notes), "%s", e.marginal_notes);
	if (e.transcript)
		snprintf(st.transcript, sizeof(st.transcript), "%s", e.transcript);

	year_hint = extract_entry_year(&e, tpl);
	if (tpl) {
		for (i = 0; i < tpl->field_count && st.field_count < SCRIBE_UX_MAX_FIELDS; i++) {
			const scribe_field_spec_t *spec = &tpl->fields[i];
			const scribe_field_val_t *fv = scribe_entry_get_field(&e, spec->key);
			scribe_ux_field_t *uf = &st.fields[st.field_count++];

			memset(uf, 0, sizeof(*uf));
			snprintf(uf->key, sizeof(uf->key), "%s", spec->key);
			snprintf(uf->label, sizeof(uf->label), "%s",
			         spec->label ? spec->label : spec->key);
			snprintf(uf->type, sizeof(uf->type), "%s",
			         scribe_field_type_str(spec->type));
			if (spec->format_hint && spec->format_hint[0])
				snprintf(uf->format_hint, sizeof(uf->format_hint), "%s", spec->format_hint);
			if (spec->enum_count > 0 && spec->enum_values) {
				size_t ei, pos = 0;
				for (ei = 0; ei < spec->enum_count && pos + 2 < sizeof(uf->enum_options); ei++) {
					if (!spec->enum_values[ei] || !spec->enum_values[ei][0])
						continue;
					pos += (size_t)snprintf(uf->enum_options + pos,
					                        sizeof(uf->enum_options) - pos,
					                        "%s%s", pos ? "|" : "",
					                        spec->enum_values[ei]);
					if (pos >= sizeof(uf->enum_options))
						pos = sizeof(uf->enum_options) - 1;
				}
			}
			uf->required = spec->required;
			if (fv) {
				if (fv->raw_value)
					snprintf(uf->raw_value, sizeof(uf->raw_value), "%s", fv->raw_value);
				if (fv->alt_value)
					snprintf(uf->alt_value, sizeof(uf->alt_value), "%s", fv->alt_value);
				if (fv->verified_value)
					snprintf(uf->verified_value, sizeof(uf->verified_value), "%s",
					         fv->verified_value);
				uf->flags = fv->flags;
				memcpy(uf->bbox, fv->bbox, sizeof(uf->bbox));
			}

			/* Populate read-only advisory suggestions using dict_category or person/place */
			if (g_ctx.dict) {
				const char *cat = (spec->dict_category && spec->dict_category[0])
				                  ? spec->dict_category
				                  : (spec->type == SCRIBE_FIELD_PERSON || spec->type == SCRIBE_FIELD_PLACE)
				                    ? scribe_field_type_str(spec->type)
				                    : NULL;
				const char *q = uf->verified_value[0] ? uf->verified_value : uf->raw_value;
				if (cat && q[0]) {
					scribe_suggestion_t sugs[SCRIBE_UX_MAX_SUGGESTIONS];
					size_t ns = scribe_dict_suggest(g_ctx.dict, cat, q, year_hint,
					                                sugs, SCRIBE_UX_MAX_SUGGESTIONS);
					size_t s;
					for (s = 0; s < ns; s++) {
						snprintf(uf->suggestions[s], sizeof(uf->suggestions[s]),
						         "%.127s", sugs[s].canonical);
					}
					uf->suggestion_count = ns;
				}
			}

			if (st.active_field < 0 && (uf->flags & SCRIBE_UX_FLAG_REVIEW))
				st.active_field = (int)(st.field_count - 1);
		}
	} else {
		for (i = 0; i < e.field_count && st.field_count < SCRIBE_UX_MAX_FIELDS; i++) {
			const scribe_field_val_t *fv = &e.fields[i];
			scribe_ux_field_t *uf = &st.fields[st.field_count++];
			memset(uf, 0, sizeof(*uf));
			snprintf(uf->key, sizeof(uf->key), "%s", fv->key);
			snprintf(uf->label, sizeof(uf->label), "%s", fv->key);
			snprintf(uf->type, sizeof(uf->type), "text");
			if (fv->raw_value)
				snprintf(uf->raw_value, sizeof(uf->raw_value), "%s", fv->raw_value);
			if (fv->alt_value)
				snprintf(uf->alt_value, sizeof(uf->alt_value), "%s", fv->alt_value);
			if (fv->verified_value)
				snprintf(uf->verified_value, sizeof(uf->verified_value), "%s",
				         fv->verified_value);
			uf->flags = fv->flags;
			memcpy(uf->bbox, fv->bbox, sizeof(uf->bbox));
		}
	}

	{
		char title[128];
		snprintf(title, sizeof(title), "Verify %s — axil-scribe", e.id);
		respond_ssr_page(cfd, 200, title, &st);
	}
	scribe_entry_free(&e);
	return 0;
}

static int
handle_post_verify(socket_t cfd, char *body)
{
	char id[64] = "";
	char ct[128] = "";
	char action[32] = "verify";
	char submitted_reviewer[64] = "";
	char notes[1024] = "";
	scribe_entry_t e;
	const scribe_template_t *tpl;
	size_t i;
	int any_invalid = 0;

	if (axil_param(cfd, "id", id, sizeof(id)) <= 0) {
		respond_error(cfd, 400, "missing entry id");
		return 0;
	}
	if (scribe_entry_get(g_ctx.store, id, &e) != 0) {
		respond_error(cfd, 404, "entry not found");
		return 0;
	}
	tpl = scribe_registry_find(g_ctx.registry, e.template_id);
	axil_header_get(cfd, "Content-Type", ct, sizeof(ct));

	if (strncasecmp(ct, "application/json", 16) == 0 && body && body[0]) {
		struct json_object *root = json_tokener_parse(body);
		struct json_object *jfields = NULL;
		struct json_object *jilleg = NULL;
		struct json_object *jv = NULL;

		if (!root || !json_object_is_type(root, json_type_object)) {
			if (root) json_object_put(root);
			scribe_entry_free(&e);
			respond_error(cfd, 400, "invalid JSON payload");
			return 0;
		}
		if (json_object_object_get_ex(root, "action", &jv) && json_object_get_string(jv))
			snprintf(action, sizeof(action), "%s", json_object_get_string(jv));
		if (json_object_object_get_ex(root, "reviewed_by", &jv) && json_object_get_string(jv))
			snprintf(submitted_reviewer, sizeof(submitted_reviewer), "%s",
			         json_object_get_string(jv));
		if (json_object_object_get_ex(root, "marginal_notes", &jv) && json_object_get_string(jv))
			scribe_entry_set_marginal_notes(&e, json_object_get_string(jv));

		json_object_object_get_ex(root, "fields", &jfields);
		json_object_object_get_ex(root, "illegible", &jilleg);

		if (tpl) {
			for (i = 0; i < tpl->field_count; i++) {
				const char *key = tpl->fields[i].key;
				const scribe_field_val_t *old = scribe_entry_get_field(&e, key);
				const char *raw = old ? old->raw_value : NULL;
				const char *ver = old ? old->verified_value : NULL;
				uint32_t flags = old ? old->flags : SCRIBE_FLAG_NONE;
				float bbox[4] = {0, 0, 0, 0};
				struct json_object *fv = NULL;
				struct json_object *iv = NULL;

				if (old)
					memcpy(bbox, old->bbox, sizeof(bbox));
				flags &= ~(SCRIBE_FLAG_CONFLICT | SCRIBE_FLAG_INVALID);

				if (jfields && json_object_is_type(jfields, json_type_object) &&
				    json_object_object_get_ex(jfields, key, &fv)) {
					ver = json_object_is_type(fv, json_type_null)
					      ? NULL : json_object_get_string(fv);
				}
				if (jilleg && json_object_is_type(jilleg, json_type_object) &&
				    json_object_object_get_ex(jilleg, key, &iv)) {
					if (json_object_get_boolean(iv))
						flags |= SCRIBE_FLAG_ILLEGIBLE;
					else
						flags &= ~SCRIBE_FLAG_ILLEGIBLE;
				}
				if (ver && (!raw || strcmp(ver, raw) != 0))
					flags |= SCRIBE_FLAG_EDITED;
				scribe_entry_set_field(&e, key, raw, ver, flags, bbox);
			}
		}
		json_object_put(root);
	} else {
		if (body && body[0])
			axil_query_parse(body);

		if (axil_query_param("action", action, sizeof(action)) <= 0)
			snprintf(action, sizeof(action), "verify");
		axil_query_param("reviewed_by", submitted_reviewer, sizeof(submitted_reviewer));
		if (axil_query_param("marginal_notes", notes, sizeof(notes)) >= 0)
			scribe_entry_set_marginal_notes(&e, notes);

		if (tpl) {
			for (i = 0; i < tpl->field_count; i++) {
				const char *key = tpl->fields[i].key;
				const scribe_field_val_t *old = scribe_entry_get_field(&e, key);
				const char *raw = old ? old->raw_value : NULL;
				char *raw_dup = raw ? strdup(raw) : NULL;
				float bbox[4] = {0, 0, 0, 0};
				uint32_t flags = old ? old->flags : SCRIBE_FLAG_NONE;
				char fparam[96];
				char iparam[96];
				char val_buf[512];
				char illeg_buf[16];
				int has_val;
				int is_illeg;

				if (old)
					memcpy(bbox, old->bbox, sizeof(bbox));
				flags &= ~(SCRIBE_FLAG_CONFLICT | SCRIBE_FLAG_INVALID | SCRIBE_FLAG_ILLEGIBLE);

				snprintf(fparam, sizeof(fparam), "field_%s", key);
				snprintf(iparam, sizeof(iparam), "illegible_%s", key);

				val_buf[0] = '\0';
				has_val = (axil_query_param(fparam, val_buf, sizeof(val_buf)) >= 0);
				illeg_buf[0] = '\0';
				is_illeg = (axil_query_param(iparam, illeg_buf, sizeof(illeg_buf)) > 0 &&
				            illeg_buf[0] != '0');
				if (is_illeg)
					flags |= SCRIBE_FLAG_ILLEGIBLE;

				{
					const char *new_ver = NULL;
					if (!is_illeg && has_val && val_buf[0] != '\0')
						new_ver = val_buf;
					if (new_ver && (!raw_dup || strcmp(new_ver, raw_dup) != 0))
						flags |= SCRIBE_FLAG_EDITED;
					scribe_entry_set_field(&e, key, raw_dup, new_ver, flags, bbox);
				}
				free(raw_dup);
			}
		}
	}

	resolve_reviewer(cfd, submitted_reviewer, e.reviewed_by, sizeof(e.reviewed_by));
	e.reviewed_at = (int64_t)time(NULL);

	if (tpl)
		scribe_validate_entry_ex(g_ctx.pack, tpl, &e);
	for (i = 0; i < e.field_count; i++) {
		if (e.fields[i].flags & SCRIBE_FLAG_INVALID)
			any_invalid = 1;
	}

	if (strcasecmp(action, "verify") == 0 && !any_invalid)
		e.status = SCRIBE_STATUS_VERIFIED;
	else
		e.status = SCRIBE_STATUS_NEEDS_REVIEW;

	if (scribe_entry_put(g_ctx.store, &e) != 0) {
		scribe_entry_free(&e);
		respond_error(cfd, 500, "failed to save entry");
		return 0;
	}
	scribe_store_save(g_ctx.store);

	if (wants_json(cfd)) {
		scribe_page_t p;
		int have_page = (scribe_page_get(g_ctx.store, e.page_id, &p) == 0);
		struct json_object *resp = json_object_new_object();
		json_object_object_add(resp, "ok", json_object_new_boolean(1));
		json_object_object_add(resp, "entry", entry_to_json(&e, have_page ? &p : NULL));
		axil_respond_json(cfd, 200,
		                  json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN));
		json_object_put(resp);
		scribe_entry_free(&e);
		return 0;
	}

	if (e.status == SCRIBE_STATUS_VERIFIED) {
		axil_redirect(cfd, "/queue");
	} else {
		char loc[96];
		snprintf(loc, sizeof(loc), "/entries/%s", e.id);
		axil_redirect(cfd, loc);
	}
	scribe_entry_free(&e);
	return 0;
}

static int
handle_post_extract_entry(socket_t cfd, char *body)
{
	char id[64] = "";
	scribe_entry_t existing;
	scribe_page_t page;
	const scribe_template_t *tpl;
	scribe_entry_t *entries = NULL;
	size_t count = 0;
	char first_id[32] = "";
	size_t i;

	int deep = 0;

	deep = axil_req_param_bool(cfd, body, "deep", 0);
	if (axil_param(cfd, "id", id, sizeof(id)) <= 0) {
		respond_error(cfd, 400, "missing entry id");
		return 0;
	}
	if (scribe_entry_get(g_ctx.store, id, &existing) != 0) {
		respond_error(cfd, 404, "entry not found");
		return 0;
	}
	if (scribe_page_get(g_ctx.store, existing.page_id, &page) != 0) {
		scribe_entry_free(&existing);
		respond_error(cfd, 404, "source page not found");
		return 0;
	}
	tpl = resolve_template(existing.template_id);
	scribe_entry_free(&existing);

	if (!tpl) {
		respond_error(cfd, 500, "no template configured");
		return 0;
	}

	if (run_page_extraction(&page, tpl, deep, &entries, &count) != 0) {
		respond_error(cfd, 502, "extraction backend failed");
		return 0;
	}
	scribe_store_save(g_ctx.store);

	if (wants_json(cfd)) {
		struct json_object *resp = json_object_new_object();
		struct json_object *arr = json_object_new_array();
		for (i = 0; i < count; i++) {
			json_object_array_add(arr, entry_to_json(&entries[i], &page));
			scribe_entry_free(&entries[i]);
		}
		free(entries);
		json_object_object_add(resp, "ok", json_object_new_boolean(1));
		json_object_object_add(resp, "entries", arr);
		axil_respond_json(cfd, 200,
		                  json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN));
		json_object_put(resp);
		return 0;
	}

	if (count > 0)
		snprintf(first_id, sizeof(first_id), "%s", entries[0].id);
	for (i = 0; i < count; i++)
		scribe_entry_free(&entries[i]);
	free(entries);

	if (first_id[0]) {
		char loc[96];
		snprintf(loc, sizeof(loc), "/entries/%s", first_id);
		axil_redirect(cfd, loc);
	} else {
		axil_redirect(cfd, "/queue");
	}
	return 0;
}

static int
handle_post_page_entries(socket_t cfd, char *body)
{
	char page_id[64] = "";
	char template_id[64] = "";
	scribe_page_t page;
	const scribe_template_t *tpl;
	scribe_entry_t entry;
	size_t i;

	if (axil_param(cfd, "id", page_id, sizeof(page_id)) <= 0) {
		respond_error(cfd, 400, "missing page id");
		return 0;
	}
	if (scribe_page_get(g_ctx.store, page_id, &page) != 0) {
		respond_error(cfd, 404, "page not found");
		return 0;
	}

	axil_req_param(cfd, body, "template_id", template_id, sizeof(template_id));
	tpl = resolve_template(template_id);
	if (!tpl) {
		respond_error(cfd, 500, "no template configured");
		return 0;
	}

	scribe_entry_init(&entry);
	snprintf(entry.page_id, sizeof(entry.page_id), "%s", page.id);
	snprintf(entry.template_id, sizeof(entry.template_id), "%s", tpl->id);
	entry.status = SCRIBE_STATUS_PENDING;
	for (i = 0; i < tpl->field_count; i++)
		scribe_entry_set_field(&entry, tpl->fields[i].key,
		                       NULL, NULL, SCRIBE_FLAG_NONE, NULL);

	if (scribe_entry_put(g_ctx.store, &entry) != 0) {
		scribe_entry_free(&entry);
		respond_error(cfd, 500, "failed to create entry");
		return 0;
	}
	scribe_store_save(g_ctx.store);

	if (wants_json(cfd)) {
		struct json_object *resp = json_object_new_object();
		json_object_object_add(resp, "ok", json_object_new_boolean(1));
		json_object_object_add(resp, "entry", entry_to_json(&entry, &page));
		axil_respond_json(cfd, 201,
		                  json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN));
		json_object_put(resp);
		scribe_entry_free(&entry);
		return 0;
	}

	{
		char loc[96];
		snprintf(loc, sizeof(loc), "/entries/%s", entry.id);
		axil_redirect(cfd, loc);
	}
	scribe_entry_free(&entry);
	return 0;
}

static int
handle_get_page_image(socket_t cfd, char *body)
{
	char page_id[64] = "";
	scribe_page_t page;
	char etag[80];
	char inm[128] = "";
	uint8_t *bytes;
	size_t len = 0;

	(void)body;
	if (axil_param(cfd, "id", page_id, sizeof(page_id)) <= 0) {
		respond_error(cfd, 400, "missing page id");
		return 0;
	}
	if (scribe_page_get(g_ctx.store, page_id, &page) != 0) {
		respond_error(cfd, 404, "page not found");
		return 0;
	}

	snprintf(etag, sizeof(etag), "\"%s\"", page.sha256);
	if (axil_header_get(cfd, "If-None-Match", inm, sizeof(inm)) == 0 &&
	    strcmp(inm, etag) == 0) {
		axil_header_set(cfd, "ETag", etag);
		axil_header_set(cfd, "Cache-Control", "public, max-age=31536000, immutable");
		axil_respond(cfd, 304, "");
		return 0;
	}

	{
		char full_path[1024];
		if (page.file_path[0] == '/')
			snprintf(full_path, sizeof(full_path), "%s", page.file_path);
		else
			snprintf(full_path, sizeof(full_path), "%s/%s",
			         scribe_store_dir(g_ctx.store), page.file_path);
		bytes = read_file_all(full_path, &len);
	}
	if (!bytes) {
		respond_error(cfd, 404, "scan file missing on disk");
		return 0;
	}

	respond_binary(cfd, 200, page.mime[0] ? page.mime : "application/octet-stream",
	               bytes, len, etag, "public, max-age=31536000, immutable");
	free(bytes);
	return 0;
}

static int
handle_get_search(socket_t cfd, char *body)
{
	char q[128] = "";
	char field[64] = "";
	int folded;
	scribe_id_list_t hits = { NULL, 0 };
	scribe_ux_state_t st;
	size_t i;

	(void)body;
	axil_req_param(cfd, NULL, "q", q, sizeof(q));
	axil_req_param(cfd, NULL, "field", field, sizeof(field));
	folded = axil_req_param_bool(cfd, NULL, "folded", 0);

	if (q[0]) {
		scribe_search(g_ctx.store, field[0] ? field : NULL, q,
		              folded ? SCRIBE_SEARCH_FOLDED : SCRIBE_SEARCH_EXACT,
		              &hits);
	}

	if (wants_json(cfd)) {
		struct json_object *root = json_object_new_object();
		struct json_object *arr = json_object_new_array();
		for (i = 0; i < hits.count; i++) {
			scribe_entry_t e;
			scribe_page_t p;
			if (scribe_entry_get(g_ctx.store, hits.ids[i], &e) == 0) {
				int hp = (scribe_page_get(g_ctx.store, e.page_id, &p) == 0);
				json_object_array_add(arr, entry_to_json(&e, hp ? &p : NULL));
				scribe_entry_free(&e);
			}
		}
		scribe_id_list_free(&hits);
		json_object_object_add(root, "ok", json_object_new_boolean(1));
		json_object_object_add(root, "query", json_object_new_string(q));
		json_object_object_add(root, "folded", json_object_new_boolean(folded));
		json_object_object_add(root, "entries", arr);
		axil_respond_json(cfd, 200,
		                  json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
		json_object_put(root);
		return 0;
	}

	scribe_ux_state_init(&st);
	st.view = SCRIBE_UX_VIEW_SEARCH;
	snprintf(st.search_query, sizeof(st.search_query), "%s", q);
	st.search_folded = folded;
	populate_template_options(&st);

	for (i = 0; i < hits.count; i++) {
		scribe_entry_t e;
		scribe_page_t p;
		if (scribe_entry_get(g_ctx.store, hits.ids[i], &e) == 0) {
			int hp = (scribe_page_get(g_ctx.store, e.page_id, &p) == 0);
			append_queue_item(&st, &e, hp ? &p : NULL);
			scribe_entry_free(&e);
		}
	}
	scribe_id_list_free(&hits);

	respond_ssr_page(cfd, 200, "Search — axil-scribe", &st);
	return 0;
}

static int
handle_get_export_json(socket_t cfd, char *body)
{
	char status_f[32] = "";
	char tpl_f[64] = "";
	char book_f[64] = "";
	struct json_object *root = json_object_new_object();
	struct json_object *arr = json_object_new_array();

	(void)body;
	axil_req_param(cfd, NULL, "status", status_f, sizeof(status_f));
	axil_req_param(cfd, NULL, "template_id", tpl_f, sizeof(tpl_f));
	axil_req_param(cfd, NULL, "book_ref", book_f, sizeof(book_f));

	collect_filtered_entries(status_f, tpl_f, book_f, NULL, arr);
	json_object_object_add(root, "version", json_object_new_string(SCRIBE_VERSION));
	json_object_object_add(root, "entries", arr);

	axil_header_set(cfd, "Content-Disposition",
	                "attachment; filename=\"scribe-export.json\"");
	axil_respond_json(cfd, 200,
	                  json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
	json_object_put(root);
	return 0;
}

static int
handle_get_export_csv(socket_t cfd, char *body)
{
	char status_f[32] = "";
	char tpl_f[64] = "";
	char book_f[64] = "";
	struct json_object *arr = json_object_new_array();
	char keys[64][64];
	size_t key_count = 0;
	size_t n;
	size_t i;
	size_t k;
	strbuf_t csv = { NULL, 0, 0 };

	(void)body;
	axil_req_param(cfd, NULL, "status", status_f, sizeof(status_f));
	axil_req_param(cfd, NULL, "template_id", tpl_f, sizeof(tpl_f));
	axil_req_param(cfd, NULL, "book_ref", book_f, sizeof(book_f));

	collect_filtered_entries(status_f, tpl_f, book_f, NULL, arr);
	n = json_object_array_length(arr);

	/* Collect deterministic union of field keys across matching entries */
	for (i = 0; i < n; i++) {
		struct json_object *eo = json_object_array_get_idx(arr, i);
		struct json_object *farr = NULL;
		size_t fn, fi;
		if (!json_object_object_get_ex(eo, "fields", &farr))
			continue;
		fn = json_object_array_length(farr);
		for (fi = 0; fi < fn && key_count < 64; fi++) {
			struct json_object *fo = json_object_array_get_idx(farr, fi);
			struct json_object *jk = NULL;
			const char *ks;
			int seen = 0;
			if (!json_object_object_get_ex(fo, "key", &jk))
				continue;
			ks = json_object_get_string(jk);
			if (!ks || !ks[0])
				continue;
			for (k = 0; k < key_count; k++) {
				if (strcmp(keys[k], ks) == 0) {
					seen = 1;
					break;
				}
			}
			if (!seen)
				snprintf(keys[key_count++], sizeof(keys[0]), "%s", ks);
		}
	}

	/* CSV header */
	sb_append(&csv, "entry_id,page_id,page_sha256,book_ref,page_num,"
	                "template_id,status,reviewed_by,reviewed_at,marginal_notes,transcript");
	for (k = 0; k < key_count; k++) {
		sb_append(&csv, ",");
		sb_append_csv(&csv, keys[k]);
		sb_append(&csv, ",");
		{
			char rk[80];
			snprintf(rk, sizeof(rk), "%.63s_raw", keys[k]);
			sb_append_csv(&csv, rk);
		}
	}
	sb_append(&csv, "\r\n");

	/* CSV rows */
	for (i = 0; i < n; i++) {
		struct json_object *eo = json_object_array_get_idx(arr, i);
		struct json_object *po = NULL;
		struct json_object *farr = NULL;
		struct json_object *jv = NULL;
		char numbuf[32];

		json_object_object_get_ex(eo, "page", &po);
		json_object_object_get_ex(eo, "fields", &farr);

		sb_append_csv(&csv, json_object_object_get_ex(eo, "id", &jv) ? json_object_get_string(jv) : "");
		sb_append(&csv, ",");
		sb_append_csv(&csv, json_object_object_get_ex(eo, "page_id", &jv) ? json_object_get_string(jv) : "");
		sb_append(&csv, ",");
		sb_append_csv(&csv, (po && json_object_object_get_ex(po, "sha256", &jv)) ? json_object_get_string(jv) : "");
		sb_append(&csv, ",");
		sb_append_csv(&csv, (po && json_object_object_get_ex(po, "book_ref", &jv)) ? json_object_get_string(jv) : "");
		sb_append(&csv, ",");
		snprintf(numbuf, sizeof(numbuf), "%d",
		         (po && json_object_object_get_ex(po, "page_num", &jv)) ? json_object_get_int(jv) : 0);
		sb_append_csv(&csv, numbuf);
		sb_append(&csv, ",");
		sb_append_csv(&csv, json_object_object_get_ex(eo, "template_id", &jv) ? json_object_get_string(jv) : "");
		sb_append(&csv, ",");
		sb_append_csv(&csv, json_object_object_get_ex(eo, "status", &jv) ? json_object_get_string(jv) : "");
		sb_append(&csv, ",");
		sb_append_csv(&csv, json_object_object_get_ex(eo, "reviewed_by", &jv) ? json_object_get_string(jv) : "");
		sb_append(&csv, ",");
		snprintf(numbuf, sizeof(numbuf), "%lld",
		         (long long)(json_object_object_get_ex(eo, "reviewed_at", &jv) ? json_object_get_int64(jv) : 0));
		sb_append_csv(&csv, numbuf);
		sb_append(&csv, ",");
		sb_append_csv(&csv, json_object_object_get_ex(eo, "marginal_notes", &jv) ? json_object_get_string(jv) : "");
		sb_append(&csv, ",");
		sb_append_csv(&csv, json_object_object_get_ex(eo, "transcript", &jv) ? json_object_get_string(jv) : "");

		for (k = 0; k < key_count; k++) {
			const char *best = "";
			const char *raw = "";
			if (farr) {
				size_t fn = json_object_array_length(farr);
				size_t fi;
				for (fi = 0; fi < fn; fi++) {
					struct json_object *fo = json_object_array_get_idx(farr, fi);
					struct json_object *jk = NULL;
					if (json_object_object_get_ex(fo, "key", &jk) &&
					    strcmp(json_object_get_string(jk), keys[k]) == 0) {
						struct json_object *jr = NULL;
						struct json_object *jver = NULL;
						if (json_object_object_get_ex(fo, "raw_value", &jr) &&
						    !json_object_is_type(jr, json_type_null))
							raw = json_object_get_string(jr);
						if (json_object_object_get_ex(fo, "verified_value", &jver) &&
						    !json_object_is_type(jver, json_type_null))
							best = json_object_get_string(jver);
						else
							best = raw;
						break;
					}
				}
			}
			sb_append(&csv, ",");
			sb_append_csv(&csv, best);
			sb_append(&csv, ",");
			sb_append_csv(&csv, raw);
		}
		sb_append(&csv, "\r\n");
	}

	json_object_put(arr);
	axil_header_set(cfd, "Content-Type", "text/csv; charset=utf-8");
	axil_header_set(cfd, "Content-Disposition",
	                "attachment; filename=\"scribe-export.csv\"");
	axil_respond(cfd, 200, csv.data ? csv.data : "");
	sb_free(&csv);
	return 0;
}

static int
handle_get_transcript(socket_t cfd, char *body)
{
	char id[64] = "";
	scribe_entry_t e;
	scribe_page_t p;
	int have_page;
	const scribe_template_t *tpl;
	bud_arena *arena;
	bud_arena *prev;
	bud_node *art;
	bud_node *h1;
	bud_node *dl;
	bud_node *tbl;
	bud_node *thead;
	bud_node *trh;
	bud_node *tbody;
	char *html_body;
	strbuf_t doc = { NULL, 0, 0 };
	char ts[64] = "—";
	size_t i;

	(void)body;
	if (axil_param(cfd, "id", id, sizeof(id)) <= 0) {
		respond_error(cfd, 400, "missing entry id");
		return 0;
	}
	if (scribe_entry_get(g_ctx.store, id, &e) != 0) {
		respond_error(cfd, 404, "entry not found");
		return 0;
	}
	have_page = (scribe_page_get(g_ctx.store, e.page_id, &p) == 0);
	tpl = scribe_registry_find(g_ctx.registry, e.template_id);

	if (e.reviewed_at > 0) {
		time_t t = (time_t)e.reviewed_at;
		struct tm gmt;
		if (gmtime_r(&t, &gmt))
			strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC", &gmt);
	}

	arena = bud_arena_new(8192);
	prev = bud_arena_push(arena);

	art = bud_element_lit("article");
	bud_set_attr_lit(art, "class", "scribe-transcript");

	{
		const char *print_title = (g_ctx.pack && g_ctx.pack->ui_print_title && g_ctx.pack->ui_print_title[0])
		                          ? g_ctx.pack->ui_print_title : "Document Transcript";
		const char *source_lbl = (g_ctx.pack && g_ctx.pack->ui_source_label && g_ctx.pack->ui_source_label[0])
		                         ? g_ctx.pack->ui_source_label : "Source";
		const char *trans_lbl = (g_ctx.pack && g_ctx.pack->ui_transcript_label && g_ctx.pack->ui_transcript_label[0])
		                        ? g_ctx.pack->ui_transcript_label : "Full Transcript";
		const char *notes_lbl = (g_ctx.pack && g_ctx.pack->ui_notes_label && g_ctx.pack->ui_notes_label[0])
		                        ? g_ctx.pack->ui_notes_label : "Notes";

		h1 = bud_element_lit("h1");
		bud_append(h1, bud_textf("%s — %s (%s)",
		                         print_title,
		                         (tpl && tpl->label) ? tpl->label : e.template_id,
		                         e.id));
		bud_append(art, h1);

		dl = bud_element_lit("p");
		bud_append(dl, bud_textf("Status: %s | %s: %s p.%u | Source SHA-256: %s | "
		                         "Reviewed by: %s (%s)",
		                         scribe_status_str(e.status),
		                         source_lbl,
		                         have_page && p.book_ref[0] ? p.book_ref : "—",
		                         have_page ? p.page_num : 0u,
		                         have_page ? p.sha256 : "—",
		                         e.reviewed_by[0] ? e.reviewed_by : "unreviewed",
		                         ts));
		bud_append(art, dl);

		tbl = bud_element_lit("table");
		bud_set_attr_lit(tbl, "class", "scribe-table");
		thead = bud_element_lit("thead");
		trh = bud_element_lit("tr");
		{
			static const char *const cols[] = { "Field", "Verified Value", "Raw OCR Value", "Flags" };
			size_t c;
			for (c = 0; c < 4; c++) {
				bud_node *th = bud_element_lit("th");
				bud_append(th, bud_text_lit(cols[c]));
				bud_append(trh, th);
			}
		}
		bud_append(thead, trh);
		bud_append(tbl, thead);

		tbody = bud_element_lit("tbody");
		for (i = 0; i < e.field_count; i++) {
			const scribe_field_val_t *fv = &e.fields[i];
			bud_node *tr = bud_element_lit("tr");
			bud_node *td1 = bud_element_lit("td");
			bud_node *td2 = bud_element_lit("td");
			bud_node *td3 = bud_element_lit("td");
			bud_node *td4 = bud_element_lit("td");
			char fstr[64] = "";

			if (fv->flags & SCRIBE_FLAG_CONFLICT) strcat(fstr, "CONFLICT ");
			if (fv->flags & SCRIBE_FLAG_ILLEGIBLE) strcat(fstr, "ILLEGIBLE ");
			if (fv->flags & SCRIBE_FLAG_INVALID) strcat(fstr, "INVALID ");
			if (fv->flags & SCRIBE_FLAG_EDITED) strcat(fstr, "EDITED ");

			bud_append(td1, bud_text(fv->key));
			bud_append(td2, bud_text(fv->verified_value ? fv->verified_value : "—"));
			bud_append(td3, bud_text(fv->raw_value ? fv->raw_value : "—"));
			bud_append(td4, bud_text(fstr[0] ? fstr : "ok"));
			bud_append(tr, td1);
			bud_append(tr, td2);
			bud_append(tr, td3);
			bud_append(tr, td4);
			bud_append(tbody, tr);
		}
		bud_append(tbl, tbody);
		bud_append(art, tbl);

		if (e.transcript && e.transcript[0]) {
			bud_node *th = bud_element_lit("h2");
			bud_node *tp = bud_element_lit("pre");
			bud_set_attr_lit(tp, "style", "white-space:pre-wrap;font-family:ui-serif,Georgia,serif;");
			bud_append(th, bud_text(trans_lbl));
			bud_append(tp, bud_text(e.transcript));
			bud_append(art, th);
			bud_append(art, tp);
		}

		if (e.marginal_notes && e.marginal_notes[0]) {
			bud_node *mh = bud_element_lit("h2");
			bud_node *mp = bud_element_lit("p");
			bud_append(mh, bud_text(notes_lbl));
			bud_append(mp, bud_text(e.marginal_notes));
			bud_append(art, mh);
			bud_append(art, mp);
		}
	}

	{
		bud_node *back = bud_element_lit("p");
		char href[96];
		snprintf(href, sizeof(href), "/entries/%s", e.id);
		bud_set_attr_lit(back, "class", "scribe-no-print");
		bud_append(back, bud_link(href, "Back to Workbench", "scribe-btn"));
		bud_append(art, back);
	}

	html_body = bud_render_html(art);
	bud_arena_pop(prev);
	bud_arena_free(arena);

	sb_append(&doc, "<!doctype html>\n<html lang=\"en\">\n<head>\n"
	                "<meta charset=\"utf-8\">\n<title>Transcript ");
	sb_append(&doc, e.id);
	sb_append(&doc, "</title>\n<link rel=\"stylesheet\" href=\"/assets/scribe.css\">\n"
	                "</head>\n<body>\n");
	if (html_body)
		sb_append(&doc, html_body);
	sb_append(&doc, "\n</body>\n</html>\n");
	bud_free_string(html_body);

	axil_header_set(cfd, "Content-Type", "text/html; charset=utf-8");
	axil_respond(cfd, 200, doc.data ? doc.data : "");
	sb_free(&doc);
	scribe_entry_free(&e);
	return 0;
}

static int
handle_get_css(socket_t cfd, char *body)
{
	strbuf_t css = { NULL, 0, 0 };
	size_t i;

	(void)body;
	for (i = 0; SCRIBE_CSS_CHUNKS[i]; i++)
		sb_append(&css, SCRIBE_CSS_CHUNKS[i]);

	axil_header_set(cfd, "Content-Type", "text/css; charset=utf-8");
	axil_header_set(cfd, "Cache-Control", "public, max-age=3600");
	axil_respond(cfd, 200, css.data ? css.data : "");
	sb_free(&css);
	return 0;
}

static int
handle_get_wasm(socket_t cfd, char *body)
{
	size_t len = 0;
	uint8_t *bytes;

	(void)body;
	bytes = read_file_all(g_ctx.wasm_path, &len);
	if (!bytes) {
		respond_error(cfd, 404, "scribe_ux.wasm not found");
		return 0;
	}
	respond_binary(cfd, 200, "application/wasm", bytes, len,
	               NULL, "public, max-age=3600");
	free(bytes);
	return 0;
}

/* ── Module Lifecycle ──────────────────────────────────────────── */

static void
load_pack_dictionaries(const char *pack_dir)
{
	char ddir[512];
	DIR *dir;
	struct dirent *de;

	if (!pack_dir || !pack_dir[0])
		return;
	snprintf(ddir, sizeof(ddir), "%s/dictionaries", pack_dir);
	dir = opendir(ddir);
	if (!dir)
		return;
	while ((de = readdir(dir)) != NULL) {
		size_t nl = strlen(de->d_name);
		char path[768];
		if (nl <= 4 || strcmp(de->d_name + nl - 4, ".tsv") != 0)
			continue;
		if (!g_ctx.dict) {
			g_ctx.dict = scribe_dict_open();
			if (!g_ctx.dict)
				break;
		}
		snprintf(path, sizeof(path), "%s/%s", ddir, de->d_name);
		scribe_dict_load_tsv(g_ctx.dict, path);
	}
	closedir(dir);
}

int
axil_scribe_init(const char *data_dir, const char *templates_dir,
                 const char *dict_path)
{
	const char *env;
	const char *pack_env;
	const char *tpl_env;

	if (g_ctx.initialized)
		return 0;
	memset(&g_ctx, 0, sizeof(g_ctx));

	env = getenv("SCRIBE_DATA_DIR");
	snprintf(g_ctx.data_dir, sizeof(g_ctx.data_dir), "%s",
	         (env && env[0]) ? env : (data_dir && data_dir[0]) ? data_dir : "./var/scribe");

	pack_env = getenv("SCRIBE_PACK_DIR");
	if (pack_env && pack_env[0]) {
		g_ctx.pack = scribe_pack_load_dir(pack_env);
		if (g_ctx.pack) {
			snprintf(g_ctx.pack_dir, sizeof(g_ctx.pack_dir), "%s", pack_env);
		} else {
			fprintf(stderr, "axil-scribe: warning: failed to load pack from '%s'\n", pack_env);
		}
	}

	tpl_env = getenv("SCRIBE_TEMPLATES_DIR");
	if (tpl_env && tpl_env[0]) {
		snprintf(g_ctx.templates_dir, sizeof(g_ctx.templates_dir), "%s", tpl_env);
	} else if (templates_dir && templates_dir[0]) {
		snprintf(g_ctx.templates_dir, sizeof(g_ctx.templates_dir), "%s", templates_dir);
	} else if (g_ctx.pack_dir[0]) {
		snprintf(g_ctx.templates_dir, sizeof(g_ctx.templates_dir), "%s/templates", g_ctx.pack_dir);
	} else {
		snprintf(g_ctx.templates_dir, sizeof(g_ctx.templates_dir), "./templates");
	}

	env = getenv("SCRIBE_WASM_PATH");
	snprintf(g_ctx.wasm_path, sizeof(g_ctx.wasm_path), "%s",
	         (env && env[0]) ? env : "./htdocs/scribe_ux.wasm");

	env = getenv("SCRIBE_AUREN_HOST");
	snprintf(g_ctx.auren_host, sizeof(g_ctx.auren_host), "%s",
	         (env && env[0]) ? env : "127.0.0.1");

	env = getenv("SCRIBE_AUREN_PORT");
	g_ctx.auren_port = (env && env[0]) ? (uint16_t)strtoul(env, NULL, 10) : 4242;

	env = getenv("SCRIBE_MODEL");
	snprintf(g_ctx.model_primary, sizeof(g_ctx.model_primary), "%s",
	         (env && env[0]) ? env : "qwen2.5-vl-7b");

	env = getenv("SCRIBE_MODEL2");
	if (env && env[0])
		snprintf(g_ctx.model_secondary, sizeof(g_ctx.model_secondary), "%s", env);

	env = getenv("SCRIBE_ESCALATE_MODEL");
	if (env && env[0])
		snprintf(g_ctx.model_escalate, sizeof(g_ctx.model_escalate), "%s", env);

	env = getenv("SCRIBE_ENHANCE");
	if (env && env[0]) {
		if (env[0] != '0' && env[0] != 'f' && env[0] != 'F')
			g_ctx.enhance_handwriting = 1;
	} else if (g_ctx.pack && g_ctx.pack->preprocess_preset == SCRIBE_PREPROC_HANDWRITING) {
		g_ctx.enhance_handwriting = 1;
	}

	env = getenv("SCRIBE_TIMEOUT_MS");
	g_ctx.timeout_ms = (env && env[0]) ? atoi(env) : 180000;
	if (g_ctx.timeout_ms <= 0)
		g_ctx.timeout_ms = 180000;

	env = getenv("SCRIBE_MOCK_JSON");
	if (env && env[0])
		load_mock_json_config(env);

	if (ensure_dir(g_ctx.data_dir) != 0) {
		fprintf(stderr, "axil-scribe: cannot create data dir '%s'\n", g_ctx.data_dir);
		scribe_pack_free(g_ctx.pack);
		g_ctx.pack = NULL;
		return -1;
	}

	g_ctx.store = scribe_store_open(g_ctx.data_dir);
	if (!g_ctx.store) {
		fprintf(stderr, "axil-scribe: failed to open store at '%s'\n", g_ctx.data_dir);
		scribe_pack_free(g_ctx.pack);
		g_ctx.pack = NULL;
		return -1;
	}

	g_ctx.registry = scribe_registry_open();
	if (!g_ctx.registry) {
		scribe_store_close(g_ctx.store);
		g_ctx.store = NULL;
		scribe_pack_free(g_ctx.pack);
		g_ctx.pack = NULL;
		return -1;
	}

	if (scribe_registry_load_dir(g_ctx.registry, g_ctx.templates_dir) < 0 ||
	    scribe_registry_count(g_ctx.registry) == 0) {
		/* Fallback to installed share dir if local ./templates wasn't found */
		const char *home = getenv("HOME");
		if (home && home[0]) {
			char alt[512];
			snprintf(alt, sizeof(alt), "%s/.local/share/axil-scribe/templates", home);
			scribe_registry_load_dir(g_ctx.registry, alt);
		}
		if (scribe_registry_count(g_ctx.registry) == 0)
			scribe_registry_load_dir(g_ctx.registry, "/usr/local/share/axil-scribe/templates");
	}

	if (g_ctx.pack_dir[0])
		load_pack_dictionaries(g_ctx.pack_dir);

	env = getenv("SCRIBE_DICT_PATH");
	if (!env || !env[0])
		env = dict_path;
	if (env && env[0]) {
		if (!g_ctx.dict)
			g_ctx.dict = scribe_dict_open();
		if (g_ctx.dict)
			scribe_dict_load_tsv(g_ctx.dict, env);
	}

	g_ctx.initialized = 1;
	return 0;
}

void
axil_scribe_cleanup(void)
{
	size_t i;

	if (!g_ctx.initialized)
		return;
	if (g_ctx.store) {
		scribe_store_save(g_ctx.store);
		scribe_store_close(g_ctx.store);
		g_ctx.store = NULL;
	}
	if (g_ctx.registry) {
		scribe_registry_close(g_ctx.registry);
		g_ctx.registry = NULL;
	}
	if (g_ctx.dict) {
		scribe_dict_close(g_ctx.dict);
		g_ctx.dict = NULL;
	}
	if (g_ctx.pack) {
		scribe_pack_free(g_ctx.pack);
		g_ctx.pack = NULL;
	}
	for (i = 0; i < g_ctx.mock_response_count; i++) {
		free(g_ctx.mock_responses[i]);
		g_ctx.mock_responses[i] = NULL;
	}
	g_ctx.mock_response_count = 0;
	g_ctx.initialized = 0;
}

XY_MODULE_API void
xy_install(void)
{
	if (axil_scribe_init(NULL, NULL, NULL) != 0) {
		fprintf(stderr, "axil-scribe: xy_install initialization failed\n");
		return;
	}

	axil_register_handler("GET:/", handle_get_queue);
	axil_register_handler("GET:/queue", handle_get_queue);
	axil_register_handler("POST:/ingest", handle_post_ingest);
	axil_register_handler("GET:/entries/:id", handle_get_entry);
	axil_register_handler("POST:/entries/:id/verify", handle_post_verify);
	axil_register_handler("POST:/entries/:id/extract", handle_post_extract_entry);
	axil_register_handler("GET:/entries/:id/transcript", handle_get_transcript);
	axil_register_handler("POST:/pages/:id/entries", handle_post_page_entries);
	axil_register_handler("GET:/pages/:id/image", handle_get_page_image);
	axil_register_handler("GET:/search", handle_get_search);
	axil_register_handler("GET:/export.csv", handle_get_export_csv);
	axil_register_handler("GET:/export.json", handle_get_export_json);
	axil_register_handler("GET:/assets/scribe.css", handle_get_css);
	axil_register_handler("GET:/assets/scribe_ux.wasm", handle_get_wasm);

	fprintf(stderr,
	        "axil-scribe: mounted /queue, /ingest, /entries/:id, /pages/:id/image, "
	        "/search, /export.{csv,json} (pack=%s data=%s templates=%zu)\n",
	        (g_ctx.pack && g_ctx.pack->id) ? g_ctx.pack->id : "generic",
	        g_ctx.data_dir, scribe_registry_count(g_ctx.registry));
}

XY_MODULE_API void
xy_shutdown(void)
{
	axil_scribe_cleanup();
}
