#define _POSIX_C_SOURCE 200809L

#include <ttypt/scribe.h>
#include <ttypt/stele.h>

#include <ctype.h>
#include <errno.h>
#include <json-c/json.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ── Base64 Encoder (pure C) ───────────────────────────────────── */

char *scribe_base64_encode(const uint8_t *data, size_t len)
{
	static const char B64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	if (!data && len > 0)
		return NULL;

	size_t out_len = ((len + 2u) / 3u) * 4u;
	char *out = malloc(out_len + 1u);
	if (!out)
		return NULL;

	size_t oi = 0;
	for (size_t i = 0; i < len; i += 3u) {
		uint32_t v = (uint32_t)data[i] << 16;
		if (i + 1u < len)
			v |= (uint32_t)data[i + 1u] << 8;
		if (i + 2u < len)
			v |= (uint32_t)data[i + 2u];

		out[oi++] = B64[(v >> 18) & 0x3fu];
		out[oi++] = B64[(v >> 12) & 0x3fu];
		out[oi++] = (i + 1u < len) ? B64[(v >> 6) & 0x3fu] : '=';
		out[oi++] = (i + 2u < len) ? B64[v & 0x3fu] : '=';
	}
	out[oi] = '\0';
	return out;
}

/* ── Deterministic Schema-Derived VLM Prompt Builder ───────────── */

typedef struct {
	char *buf;
	size_t len;
	size_t cap;
} strbuf_t;

static int sb_append(strbuf_t *sb, const char *s, size_t n)
{
	if (sb->len + n + 1u > sb->cap) {
		size_t ncap = sb->cap ? sb->cap * 2u : 512u;
		while (ncap < sb->len + n + 1u)
			ncap *= 2u;
		char *next = realloc(sb->buf, ncap);
		if (!next)
			return -1;
		sb->buf = next;
		sb->cap = ncap;
	}
	memcpy(sb->buf + sb->len, s, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
	return 0;
}

static int sb_puts(strbuf_t *sb, const char *s)
{
	return sb_append(sb, s, strlen(s));
}

int scribe_build_extraction_prompt(const scribe_template_t *tpl,
                                   char **out_prompt)
{
	if (!tpl || !out_prompt)
		return -1;
	*out_prompt = NULL;

	strbuf_t sb = { 0 };
	if (sb_puts(&sb, "You are an archival document transcription assistant.\n") != 0 ||
	    sb_puts(&sb, "Document template: ") != 0 ||
	    sb_puts(&sb, tpl->label ? tpl->label : tpl->id) != 0 ||
	    sb_puts(&sb, "\n") != 0)
		goto fail;

	if (tpl->prompt_hint && tpl->prompt_hint[0]) {
		if (sb_puts(&sb, "Instructions: ") != 0 ||
		    sb_puts(&sb, tpl->prompt_hint) != 0 ||
		    sb_puts(&sb, "\n") != 0)
			goto fail;
	}

	if (sb_puts(&sb,
		"Rules:\n"
		"- Return ONLY valid JSON matching {\"entries\":[{\"fields\":{...},\"marginal_notes\":null}]}.\n"
		"- If the page is a register with multiple entries, include one object per entry in \"entries\".\n"
		"- Transcribe text verbatim, preserving historical spellings and diacritics.\n"
		"- Format dates as YYYY-MM-DD when full date is visible (or YYYY-MM / YYYY if partial).\n"
		"- Output null for any missing or illegible field; never invent or guess unreadable text.\n"
		"- Place handwritten margin annotations in \"marginal_notes\".\n"
		"Fields:\n") != 0)
		goto fail;

	for (size_t i = 0; i < tpl->field_count; i++) {
		const scribe_field_spec_t *f = &tpl->fields[i];
		char line[256];
		snprintf(line, sizeof(line), "- \"%s\" (%s, %s): %s\n",
		         f->key,
		         scribe_field_type_str(f->type),
		         f->required ? "required" : "optional",
		         f->label ? f->label : f->key);
		if (sb_puts(&sb, line) != 0)
			goto fail;
	}

	*out_prompt = sb.buf;
	return 0;

fail:
	free(sb.buf);
	return -1;
}

/* ── Strip Optional Markdown Fences & Parse VLM JSON ───────────── */

static const char *skip_ws(const char *p)
{
	while (*p && isspace((unsigned char)*p))
		p++;
	return p;
}

static char *extract_json_slice(const char *raw)
{
	if (!raw)
		return NULL;
	const char *p = skip_ws(raw);

	/* Strip leading ```json or ``` fence if present */
	if (strncmp(p, "```", 3) == 0) {
		p += 3;
		while (*p && *p != '\n')
			p++;
		if (*p == '\n')
			p++;
		p = skip_ws(p);
	}

	const char *start = strchr(p, '{');
	if (!start)
		return NULL;
	const char *end = strrchr(start, '}');
	if (!end || end < start)
		return NULL;

	size_t n = (size_t)(end - start) + 1u;
	char *copy = malloc(n + 1u);
	if (!copy)
		return NULL;
	memcpy(copy, start, n);
	copy[n] = '\0';
	return copy;
}

static int parse_single_entry_obj(struct json_object *eobj,
                                  const scribe_template_t *tpl,
                                  const char *page_id,
                                  const char *model,
                                  scribe_entry_t *out)
{
	scribe_entry_init(out);
	if (page_id)
		stl_strlcpy(out->page_id, page_id, sizeof(out->page_id));
	stl_strlcpy(out->template_id, tpl->id, sizeof(out->template_id));
	if (model)
		stl_strlcpy(out->extractor_model, model, sizeof(out->extractor_model));
	out->status = SCRIBE_STATUS_EXTRACTED;

	struct json_object *jfields = NULL;
	if (!json_object_object_get_ex(eobj, "fields", &jfields) ||
	    !json_object_is_type(jfields, json_type_object)) {
		/* Allow flat object where field keys sit directly at entry root. */
		jfields = eobj;
	}

	struct json_object *jmarg = NULL;
	if (json_object_object_get_ex(eobj, "marginal_notes", &jmarg) &&
	    jmarg && json_object_is_type(jmarg, json_type_string)) {
		const char *ms = json_object_get_string(jmarg);
		if (ms && ms[0])
			(void)scribe_entry_set_marginal_notes(out, ms);
	}

	struct json_object *jbboxes = NULL;
	if (!json_object_object_get_ex(eobj, "bbox", &jbboxes) ||
	    !jbboxes || !json_object_is_type(jbboxes, json_type_object)) {
		jbboxes = NULL;
	}

	for (size_t i = 0; i < tpl->field_count; i++) {
		const scribe_field_spec_t *spec = &tpl->fields[i];
		struct json_object *jv = NULL;
		const char *val = NULL;
		uint32_t flags = SCRIBE_FLAG_NONE;
		float bbox[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		int has_bbox = 0;

		if (json_object_object_get_ex(jfields, spec->key, &jv) && jv) {
			if (json_object_is_type(jv, json_type_string)) {
				const char *s = json_object_get_string(jv);
				if (s && s[0])
					val = s;
			} else if (json_object_is_type(jv, json_type_null)) {
				if (spec->required)
					flags |= SCRIBE_FLAG_ILLEGIBLE;
			} else {
				const char *s = json_object_get_string(jv);
				if (s && s[0])
					val = s;
			}
		} else if (spec->required) {
			flags |= SCRIBE_FLAG_ILLEGIBLE;
		}

		if (jbboxes) {
			struct json_object *jb = NULL;
			if (json_object_object_get_ex(jbboxes, spec->key, &jb) &&
			    jb && json_object_is_type(jb, json_type_array) &&
			    json_object_array_length(jb) == 4) {
				for (int b = 0; b < 4; b++) {
					struct json_object *el = json_object_array_get_idx(jb, b);
					bbox[b] = el ? (float)json_object_get_double(el) : 0.0f;
				}
				has_bbox = 1;
			}
		}

		if (scribe_entry_set_field(out, spec->key, val, NULL, flags,
		                           has_bbox ? bbox : NULL) != 0) {
			scribe_entry_free(out);
			return -1;
		}
	}

	return 0;
}

int scribe_parse_extraction_json(const char *raw_response,
                                 const scribe_template_t *tpl,
                                 const char *page_id,
                                 const char *model,
                                 scribe_entry_t **out_entries,
                                 size_t *out_count)
{
	if (!raw_response || !tpl || !out_entries || !out_count)
		return -1;
	*out_entries = NULL;
	*out_count = 0;

	char *slice = extract_json_slice(raw_response);
	if (!slice)
		return -1;

	struct json_object *root = json_tokener_parse(slice);
	free(slice);
	if (!root || !json_object_is_type(root, json_type_object)) {
		if (root)
			json_object_put(root);
		return -1;
	}

	struct json_object *jentries = NULL;
	if (json_object_object_get_ex(root, "entries", &jentries) &&
	    json_object_is_type(jentries, json_type_array)) {
		size_t n = json_object_array_length(jentries);
		if (n == 0) {
			json_object_put(root);
			return -1;
		}
		scribe_entry_t *arr = calloc(n, sizeof(*arr));
		if (!arr) {
			json_object_put(root);
			return -1;
		}
		for (size_t i = 0; i < n; i++) {
			struct json_object *eo = json_object_array_get_idx(jentries, i);
			if (!eo || !json_object_is_type(eo, json_type_object) ||
			    parse_single_entry_obj(eo, tpl, page_id, model, &arr[i]) != 0) {
				for (size_t k = 0; k < i; k++)
					scribe_entry_free(&arr[k]);
				free(arr);
				json_object_put(root);
				return -1;
			}
		}
		json_object_put(root);
		*out_entries = arr;
		*out_count = n;
		return 0;
	}

	/* Fallback: single-entry JSON object */
	scribe_entry_t *one = calloc(1, sizeof(*one));
	if (!one) {
		json_object_put(root);
		return -1;
	}
	if (parse_single_entry_obj(root, tpl, page_id, model, one) != 0) {
		free(one);
		json_object_put(root);
		return -1;
	}
	json_object_put(root);
	*out_entries = one;
	*out_count = 1;
	return 0;
}

/* ── 2-Pass Disagreement / Conflict Merger ─────────────────────── */

static int str_equal_trimmed(const char *a, const char *b)
{
	if (!a)
		a = "";
	if (!b)
		b = "";
	while (*a && isspace((unsigned char)*a))
		a++;
	while (*b && isspace((unsigned char)*b))
		b++;
	size_t la = strlen(a);
	size_t lb = strlen(b);
	while (la > 0 && isspace((unsigned char)a[la - 1]))
		la--;
	while (lb > 0 && isspace((unsigned char)b[lb - 1]))
		lb--;
	return la == lb && (la == 0 || memcmp(a, b, la) == 0);
}

size_t scribe_merge_two_pass(scribe_entry_t *pass1, size_t count1,
                             const scribe_entry_t *pass2, size_t count2)
{
	if (!pass1 || count1 == 0)
		return 0;

	size_t conflicts = 0;

	/* Entry count mismatch: flag every entry so a human inspects split boundaries. */
	if (!pass2 || count1 != count2) {
		for (size_t i = 0; i < count1; i++) {
			pass1[i].status = SCRIBE_STATUS_NEEDS_REVIEW;
			for (size_t f = 0; f < pass1[i].field_count; f++) {
				pass1[i].fields[f].flags |= SCRIBE_FLAG_CONFLICT;
				conflicts++;
			}
		}
		return conflicts;
	}

	for (size_t i = 0; i < count1; i++) {
		scribe_entry_t *e1 = &pass1[i];
		const scribe_entry_t *e2 = &pass2[i];

		for (size_t f = 0; f < e1->field_count; f++) {
			scribe_field_val_t *f1 = &e1->fields[f];
			const scribe_field_val_t *f2 = scribe_entry_get_field(e2, f1->key);
			const char *v1 = f1->raw_value;
			const char *v2 = f2 ? f2->raw_value : NULL;

			if (!str_equal_trimmed(v1, v2)) {
				f1->flags |= SCRIBE_FLAG_CONFLICT;
				/* If Pass 1 returned NULL but Pass 2 read text, surface Pass 2 for review. */
				if ((!v1 || !v1[0]) && v2 && v2[0]) {
					(void)scribe_entry_set_field(
						e1, f1->key, v2, f1->verified_value,
						f1->flags, f1->bbox);
				}
				conflicts++;
			}
		}
	}

	return conflicts;
}

/* ── Built-In Mock Backend ─────────────────────────────────────── */

static int mock_extract_json(void *ctx,
                             const uint8_t *image_bytes, size_t image_len,
                             const char *mime,
                             const scribe_template_t *tpl,
                             const char *model,
                             const char *prompt,
                             float temperature,
                             char **out_json)
{
	(void)image_bytes;
	(void)image_len;
	(void)mime;
	(void)tpl;
	(void)model;
	(void)prompt;
	(void)temperature;

	scribe_mock_extractor_t *m = (scribe_mock_extractor_t *)ctx;
	if (!m || !m->responses || m->response_count == 0 || !out_json)
		return -1;

	size_t idx = m->call_index % m->response_count;
	m->call_index++;

	const char *resp = m->responses[idx];
	if (!resp)
		return -1;
	size_t n = strlen(resp);
	char *copy = malloc(n + 1u);
	if (!copy)
		return -1;
	memcpy(copy, resp, n + 1u);
	*out_json = copy;
	return 0;
}

static const scribe_extractor_ops_t MOCK_OPS = {
	.name = "mock",
	.extract_json = mock_extract_json,
};

const scribe_extractor_ops_t *scribe_extractor_mock(void)
{
	return &MOCK_OPS;
}

/* ── Built-In Pure-Socket axil-auren HTTP Backend ──────────────── */

static int connect_tcp(const char *host, uint16_t port, int timeout_ms)
{
	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo *res = NULL;
	if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
		return -1;

	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;

		struct timeval tv;
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static int send_all(int fd, const char *buf, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = send(fd, buf + sent, len - sent, 0);
		if (n <= 0)
			return -1;
		sent += (size_t)n;
	}
	return 0;
}

static char *decode_chunked_body(const char *body)
{
	strbuf_t sb = { 0 };
	const char *p = body;
	while (*p) {
		char *endp = NULL;
		unsigned long chunk_len = strtoul(p, &endp, 16);
		if (endp == p)
			break;
		const char *crlf = strstr(endp, "\r\n");
		if (!crlf)
			break;
		p = crlf + 2;
		if (chunk_len == 0)
			break;
		if (sb_append(&sb, p, (size_t)chunk_len) != 0) {
			free(sb.buf);
			return NULL;
		}
		p += chunk_len;
		if (p[0] == '\r' && p[1] == '\n')
			p += 2;
	}
	return sb.buf;
}

static int auren_extract_json(void *ctx,
                              const uint8_t *image_bytes, size_t image_len,
                              const char *mime,
                              const scribe_template_t *tpl,
                              const char *model,
                              const char *prompt,
                              float temperature,
                              char **out_json)
{
	(void)tpl;
	if (!image_bytes || image_len == 0 || !mime || !prompt || !out_json)
		return -1;
	*out_json = NULL;

	const scribe_auren_config_t *cfg = (const scribe_auren_config_t *)ctx;
	const char *host = (cfg && cfg->host && cfg->host[0]) ? cfg->host : "127.0.0.1";
	uint16_t port = (cfg && cfg->port) ? cfg->port : 4242u;
	const char *endpoint = (cfg && cfg->endpoint && cfg->endpoint[0])
	                       ? cfg->endpoint : "/v1/chat/completions";
	int timeout_ms = (cfg && cfg->timeout_ms > 0) ? cfg->timeout_ms : 60000;

	char *b64 = scribe_base64_encode(image_bytes, image_len);
	if (!b64)
		return -1;

	size_t url_len = strlen("data:") + strlen(mime) + strlen(";base64,") + strlen(b64) + 1u;
	char *data_url = malloc(url_len);
	if (!data_url) {
		free(b64);
		return -1;
	}
	snprintf(data_url, url_len, "data:%s;base64,%s", mime, b64);
	free(b64);

	/* Build OpenAI-compatible multimodal chat payload */
	struct json_object *req = json_object_new_object();
	json_object_object_add(req, "model",
		json_object_new_string((model && model[0]) ? model : "chat"));
	json_object_object_add(req, "stream", json_object_new_boolean(0));
	json_object_object_add(req, "temperature", json_object_new_double((double)temperature));

	struct json_object *msgs = json_object_new_array();
	struct json_object *msg = json_object_new_object();
	json_object_object_add(msg, "role", json_object_new_string("user"));

	struct json_object *parts = json_object_new_array();
	struct json_object *ptext = json_object_new_object();
	json_object_object_add(ptext, "type", json_object_new_string("text"));
	json_object_object_add(ptext, "text", json_object_new_string(prompt));
	json_object_array_add(parts, ptext);

	struct json_object *pimg = json_object_new_object();
	json_object_object_add(pimg, "type", json_object_new_string("image_url"));
	struct json_object *iurl = json_object_new_object();
	json_object_object_add(iurl, "url", json_object_new_string(data_url));
	json_object_object_add(pimg, "image_url", iurl);
	json_object_array_add(parts, pimg);
	free(data_url);

	json_object_object_add(msg, "content", parts);
	json_object_array_add(msgs, msg);
	json_object_object_add(req, "messages", msgs);

	const char *body_str = json_object_to_json_string_ext(
		req, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
	size_t body_len = strlen(body_str);

	char hdr[512];
	int hlen = snprintf(hdr, sizeof(hdr),
		"POST %s HTTP/1.1\r\n"
		"Host: %s:%u\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n\r\n",
		endpoint, host, (unsigned)port, body_len);
	if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) {
		json_object_put(req);
		return -1;
	}

	int fd = connect_tcp(host, port, timeout_ms);
	if (fd < 0) {
		json_object_put(req);
		return -1;
	}

	int rc = -1;
	if (send_all(fd, hdr, (size_t)hlen) != 0 ||
	    send_all(fd, body_str, body_len) != 0) {
		close(fd);
		json_object_put(req);
		return -1;
	}
	json_object_put(req);

	strbuf_t resp = { 0 };
	char chunk[4096];
	ssize_t n;
	while ((n = recv(fd, chunk, sizeof(chunk), 0)) > 0) {
		if (sb_append(&resp, chunk, (size_t)n) != 0) {
			free(resp.buf);
			close(fd);
			return -1;
		}
	}
	close(fd);

	if (!resp.buf || resp.len < 12) {
		free(resp.buf);
		return -1;
	}

	/* Verify 2xx HTTP status */
	int http_status = 0;
	if (sscanf(resp.buf, "HTTP/%*d.%*d %d", &http_status) != 1 ||
	    http_status < 200 || http_status >= 300) {
		free(resp.buf);
		return -1;
	}

	char *sep = strstr(resp.buf, "\r\n\r\n");
	if (!sep) {
		free(resp.buf);
		return -1;
	}
	*sep = '\0';
	const char *raw_body = sep + 4;

	char *decoded = NULL;
	const char *json_body = raw_body;
	if (strstr(resp.buf, "Transfer-Encoding: chunked") ||
	    strstr(resp.buf, "transfer-encoding: chunked")) {
		decoded = decode_chunked_body(raw_body);
		if (!decoded) {
			free(resp.buf);
			return -1;
		}
		json_body = decoded;
	}

	struct json_object *jresp = json_tokener_parse(json_body);
	free(decoded);
	free(resp.buf);
	if (!jresp || !json_object_is_type(jresp, json_type_object)) {
		if (jresp)
			json_object_put(jresp);
		return -1;
	}

	struct json_object *jchoices = NULL;
	if (json_object_object_get_ex(jresp, "choices", &jchoices) &&
	    json_object_is_type(jchoices, json_type_array) &&
	    json_object_array_length(jchoices) > 0) {
		struct json_object *c0 = json_object_array_get_idx(jchoices, 0);
		struct json_object *jmsg = NULL, *jcontent = NULL;
		if (c0 && json_object_object_get_ex(c0, "message", &jmsg) &&
		    jmsg && json_object_object_get_ex(jmsg, "content", &jcontent) &&
		    jcontent && json_object_is_type(jcontent, json_type_string)) {
			const char *content = json_object_get_string(jcontent);
			if (content) {
				size_t clen = strlen(content);
				*out_json = malloc(clen + 1u);
				if (*out_json) {
					memcpy(*out_json, content, clen + 1u);
					rc = 0;
				}
			}
		}
	}

	json_object_put(jresp);
	return rc;
}

static const scribe_extractor_ops_t AUREN_OPS = {
	.name = "auren",
	.extract_json = auren_extract_json,
};

const scribe_extractor_ops_t *scribe_extractor_auren(void)
{
	return &AUREN_OPS;
}

/* ── End-to-End Page Extraction Orchestrator ───────────────────── */

static uint8_t *load_page_bytes(const scribe_store_t *store,
                                const scribe_page_t *page,
                                size_t *out_len)
{
	char abs_path[512];
	int n = snprintf(abs_path, sizeof(abs_path), "%s/%s",
	                 scribe_store_dir(store), page->file_path);
	if (n <= 0 || (size_t)n >= sizeof(abs_path))
		return NULL;

	FILE *fp = fopen(abs_path, "rb");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	long sz = ftell(fp);
	if (sz <= 0 || sz > 64L * 1024L * 1024L) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	uint8_t *buf = malloc((size_t)sz);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	size_t nr = fread(buf, 1, (size_t)sz, fp);
	fclose(fp);
	if (nr != (size_t)sz) {
		free(buf);
		return NULL;
	}
	*out_len = (size_t)sz;
	return buf;
}

int scribe_extract_page(scribe_store_t *store,
                        const scribe_page_t *page,
                        const scribe_template_t *tpl,
                        const scribe_extractor_ops_t *ops,
                        void *extractor_ctx,
                        const char *model_primary,
                        const char *model_secondary,
                        scribe_entry_t **out_entries,
                        size_t *out_count)
{
	if (!store || !page || !tpl || !ops || !ops->extract_json ||
	    !out_entries || !out_count)
		return -1;
	*out_entries = NULL;
	*out_count = 0;

	size_t img_len = 0;
	uint8_t *img_bytes = load_page_bytes(store, page, &img_len);
	if (!img_bytes)
		return -1;

	char *prompt = NULL;
	if (scribe_build_extraction_prompt(tpl, &prompt) != 0) {
		free(img_bytes);
		return -1;
	}

	const char *m1 = (model_primary && model_primary[0]) ? model_primary : "chat";
	char *raw1 = NULL;
	if (ops->extract_json(extractor_ctx, img_bytes, img_len, page->mime,
	                      tpl, m1, prompt, 0.0f, &raw1) != 0) {
		free(prompt);
		free(img_bytes);
		return -1;
	}

	scribe_entry_t *e1 = NULL;
	size_t n1 = 0;
	if (scribe_parse_extraction_json(raw1, tpl, page->id, m1, &e1, &n1) != 0) {
		free(raw1);
		free(prompt);
		free(img_bytes);
		return -1;
	}
	free(raw1);

	/* Optional 2-pass conflict check */
	if (model_secondary && model_secondary[0]) {
		float temp2 = (strcmp(m1, model_secondary) == 0) ? 0.25f : 0.0f;
		char *raw2 = NULL;
		if (ops->extract_json(extractor_ctx, img_bytes, img_len, page->mime,
		                      tpl, model_secondary, prompt, temp2, &raw2) == 0) {
			scribe_entry_t *e2 = NULL;
			size_t n2 = 0;
			if (scribe_parse_extraction_json(raw2, tpl, page->id,
			                                 model_secondary, &e2, &n2) == 0) {
				(void)scribe_merge_two_pass(e1, n1, e2, n2);
				for (size_t i = 0; i < n2; i++)
					scribe_entry_free(&e2[i]);
				free(e2);
			} else {
				/* Pass 2 returned malformed JSON: flag entries for review. */
				(void)scribe_merge_two_pass(e1, n1, NULL, 0);
			}
			free(raw2);
		} else {
			(void)scribe_merge_two_pass(e1, n1, NULL, 0);
		}
	}

	free(prompt);
	free(img_bytes);

	/* Validate, route status, and persist each extracted entry. */
	for (size_t i = 0; i < n1; i++) {
		size_t flagged = scribe_validate_entry(tpl, &e1[i]);
		if (flagged > 0)
			e1[i].status = SCRIBE_STATUS_NEEDS_REVIEW;
		else if (e1[i].status != SCRIBE_STATUS_NEEDS_REVIEW)
			e1[i].status = SCRIBE_STATUS_EXTRACTED;

		if (scribe_entry_put(store, &e1[i]) != 0) {
			for (size_t k = 0; k < n1; k++)
				scribe_entry_free(&e1[k]);
			free(e1);
			return -1;
		}
	}

	*out_entries = e1;
	*out_count = n1;
	return 0;
}

/* ── Benchmark Scoring (Exact Match & Silent Error Rate) ───────── */

void scribe_bench_score_entries(const scribe_template_t *tpl,
                                const scribe_entry_t *predicted,
                                size_t pred_count,
                                const scribe_entry_t *truth,
                                size_t truth_count,
                                scribe_bench_metrics_t *metrics)
{
	if (!tpl || !truth || !metrics)
		return;

	const uint32_t review_mask =
		SCRIBE_FLAG_ILLEGIBLE | SCRIBE_FLAG_CONFLICT | SCRIBE_FLAG_INVALID;

	metrics->docs_total++;
	metrics->entries_total += truth_count;

	for (size_t i = 0; i < truth_count; i++) {
		const scribe_entry_t *te = &truth[i];
		const scribe_entry_t *pe = (predicted && i < pred_count) ? &predicted[i] : NULL;

		for (size_t k = 0; k < tpl->field_count; k++) {
			const char *key = tpl->fields[k].key;
			const scribe_field_val_t *tf = scribe_entry_get_field(te, key);
			const scribe_field_val_t *pf = pe ? scribe_entry_get_field(pe, key) : NULL;

			const char *tv = tf ? (tf->verified_value ? tf->verified_value : tf->raw_value) : NULL;
			const char *pv = pf ? (pf->verified_value ? pf->verified_value : pf->raw_value) : NULL;
			uint32_t pflags = pf ? pf->flags : 0u;

			metrics->fields_total++;
			if (pflags & review_mask)
				metrics->flagged_fields++;

			if (str_equal_trimmed(tv, pv)) {
				metrics->exact_matches++;
			} else if (!(pflags & review_mask)) {
				/* Wrong prediction AND unflagged => silent hallucination! */
				metrics->silent_errors++;
			}
		}
	}
}
