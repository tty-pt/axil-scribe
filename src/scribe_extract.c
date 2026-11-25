#define _POSIX_C_SOURCE 200809L

#include <ttypt/scribe.h>
#include <ttypt/stele.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
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

/* ── Paleographic Uncertainty Marker Detector ──────────────────── */

static int contains_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !needle[0])
		return 0;
	size_t nlen = strlen(needle);
	for (const char *p = haystack; *p; p++) {
		size_t i = 0;
		while (i < nlen && p[i] &&
		       tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == nlen)
			return 1;
	}
	return 0;
}

int scribe_has_uncertain_marker_ex(const char *val, const scribe_pack_t *pack)
{
	if (!val || !val[0])
		return 0;
	if (contains_ci(val, "[?]"))
		return 1;

	if (pack && pack->uncertainty_markers && pack->uncertainty_markers[0]) {
		const char *p = pack->uncertainty_markers;
		while (*p) {
			while (*p == ',' || isspace((unsigned char)*p))
				p++;
			if (!*p)
				break;
			const char *end = p;
			while (*end && *end != ',')
				end++;
			while (end > p && isspace((unsigned char)end[-1]))
				end--;
			size_t tlen = (size_t)(end - p);
			if (tlen > 0 && tlen < 64) {
				char tok[64];
				memcpy(tok, p, tlen);
				tok[tlen] = '\0';
				if (contains_ci(val, tok))
					return 1;
			}
			p = *end ? end + 1 : end;
		}
		return 0;
	}

	return contains_ci(val, "[ileg") || contains_ci(val, "[illeg");
}

int scribe_has_uncertain_marker(const char *val)
{
	return scribe_has_uncertain_marker_ex(val, NULL);
}

/* ── Deterministic Schema + Pack-Derived VLM Prompt Builder ────── */

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

int scribe_build_extraction_prompt_ex(const scribe_pack_t *pack,
                                      const scribe_template_t *tpl,
                                      char **out_prompt)
{
	if (!tpl || !out_prompt)
		return -1;
	*out_prompt = NULL;

	const char *persona = (pack && pack->system_persona && pack->system_persona[0])
	                      ? pack->system_persona
	                      : "You are a precise document transcription and structured data extraction specialist.";
	int transcript_first = (pack && pack->transcript_first) ? 1 : 0;

	strbuf_t sb = { 0 };
	if (sb_puts(&sb, persona) != 0 ||
	    sb_puts(&sb, "\nDocument template: ") != 0 ||
	    sb_puts(&sb, tpl->label ? tpl->label : tpl->id) != 0 ||
	    sb_puts(&sb, "\n") != 0)
		goto fail;

	if (tpl->prompt_hint && tpl->prompt_hint[0]) {
		if (sb_puts(&sb, "Template instructions: ") != 0 ||
		    sb_puts(&sb, tpl->prompt_hint) != 0 ||
		    sb_puts(&sb, "\n") != 0)
			goto fail;
	}

	if (pack && pack->extraction_guide && pack->extraction_guide[0]) {
		if (sb_puts(&sb, "Domain methodology:\n") != 0 ||
		    sb_puts(&sb, pack->extraction_guide) != 0 ||
		    sb_puts(&sb, "\n") != 0)
			goto fail;
	}

	if (transcript_first) {
		if (sb_puts(&sb,
			"Extraction order:\n"
			"1. First read the ENTIRE document line by line and emit a complete verbatim transcription in \"transcript\".\n"
			"2. Second map each requested field in \"fields\" from your transcript and visual inspection.\n"
			"3. Third provide normalized bounding boxes [x, y, w, h] (0.0..1.0) in \"bbox\" for each extracted field where visible.\n") != 0)
			goto fail;
	} else {
		if (sb_puts(&sb,
			"Extraction steps:\n"
			"1. Extract each requested field verbatim into \"fields\" from visual inspection.\n"
			"2. Provide normalized bounding boxes [x, y, w, h] (0.0..1.0) in \"bbox\" where visible, and optionally full text in \"transcript\".\n") != 0)
			goto fail;
	}

	if (sb_puts(&sb,
		"Rules:\n"
		"- Return ONLY valid JSON matching {\"entries\":[{\"transcript\":\"...\",\"fields\":{...},\"bbox\":{\"key\":[x,y,w,h]},\"marginal_notes\":null}]}.\n"
		"- If the page contains multiple distinct records, include one object per record in \"entries\".\n"
		"- Preserve exact spellings and diacritics as written on the document.\n"
		"- Format date fields as ISO YYYY-MM-DD when full date is visible (or YYYY-MM / YYYY if partial).\n"
		"- Output null for any missing or completely unreadable field, or mark unreadable tokens with [?]; never fabricate text.\n"
		"- Place side annotations, stamps, or extra comments in \"marginal_notes\".\n"
		"Fields:\n") != 0)
		goto fail;

	for (size_t i = 0; i < tpl->field_count; i++) {
		const scribe_field_spec_t *f = &tpl->fields[i];
		char line[512];
		snprintf(line, sizeof(line), "- \"%s\" (%s, %s): %s",
		         f->key,
		         scribe_field_type_str(f->type),
		         f->required ? "required" : "optional",
		         f->label ? f->label : f->key);
		if (sb_puts(&sb, line) != 0)
			goto fail;
		if (f->format_hint && f->format_hint[0]) {
			if (sb_puts(&sb, " [format: ") != 0 ||
			    sb_puts(&sb, f->format_hint) != 0 ||
			    sb_puts(&sb, "]") != 0)
				goto fail;
		}
		if (f->enum_count > 0 && f->enum_values) {
			if (sb_puts(&sb, " [allowed: ") != 0)
				goto fail;
			for (size_t e = 0; e < f->enum_count; e++) {
				if (e > 0 && sb_puts(&sb, " | ") != 0)
					goto fail;
				if (f->enum_values[e] && sb_puts(&sb, f->enum_values[e]) != 0)
					goto fail;
			}
			if (sb_puts(&sb, "]") != 0)
				goto fail;
		}
		if (sb_puts(&sb, "\n") != 0)
			goto fail;
	}

	*out_prompt = sb.buf;
	return 0;

fail:
	free(sb.buf);
	return -1;
}

int scribe_build_extraction_prompt(const scribe_template_t *tpl,
                                   char **out_prompt)
{
	return scribe_build_extraction_prompt_ex(NULL, tpl, out_prompt);
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

static float clamp01(float v)
{
	if (v < 0.0f)
		return 0.0f;
	if (v > 1.0f)
		return 1.0f;
	return v;
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

	struct json_object *jtr = NULL;
	if (json_object_object_get_ex(eobj, "transcript", &jtr) &&
	    jtr && json_object_is_type(jtr, json_type_string)) {
		const char *ts = json_object_get_string(jtr);
		if (ts && ts[0])
			(void)scribe_entry_set_transcript(out, ts);
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

		if (val && scribe_has_uncertain_marker(val))
			flags |= SCRIBE_FLAG_ILLEGIBLE;

		if (jbboxes) {
			struct json_object *jb = NULL;
			if (json_object_object_get_ex(jbboxes, spec->key, &jb) &&
			    jb && json_object_is_type(jb, json_type_array) &&
			    json_object_array_length(jb) == 4) {
				for (int b = 0; b < 4; b++) {
					struct json_object *el = json_object_array_get_idx(jb, b);
					bbox[b] = el ? (float)json_object_get_double(el) : 0.0f;
				}
				bbox[0] = clamp01(bbox[0]);
				bbox[1] = clamp01(bbox[1]);
				bbox[2] = clamp01(bbox[2]);
				bbox[3] = clamp01(bbox[3]);
				if (bbox[0] + bbox[2] > 1.0f)
					bbox[2] = 1.0f - bbox[0];
				if (bbox[1] + bbox[3] > 1.0f)
					bbox[3] = 1.0f - bbox[1];
				if (bbox[2] > 0.001f && bbox[3] > 0.001f)
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

		if ((!e1->transcript || !e1->transcript[0]) &&
		    e2->transcript && e2->transcript[0]) {
			(void)scribe_entry_set_transcript(e1, e2->transcript);
		}

		for (size_t f = 0; f < e1->field_count; f++) {
			scribe_field_val_t *f1 = &e1->fields[f];
			const scribe_field_val_t *f2 = scribe_entry_get_field(e2, f1->key);
			const char *v1 = f1->raw_value;
			const char *v2 = f2 ? f2->raw_value : NULL;

			/* Adopt Pass-2 bbox when Pass 1 omitted it. */
			if (f2 && (f1->bbox[2] <= 0.0f || f1->bbox[3] <= 0.0f) &&
			    f2->bbox[2] > 0.0f && f2->bbox[3] > 0.0f) {
				memcpy(f1->bbox, f2->bbox, sizeof(f1->bbox));
			}

			if (!str_equal_trimmed(v1, v2)) {
				f1->flags |= SCRIBE_FLAG_CONFLICT;
				if ((!v1 || !v1[0]) && v2 && v2[0]) {
					/* Pass 1 returned NULL, Pass 2 read text -> surface Pass 2. */
					(void)scribe_entry_set_field(
						e1, f1->key, v2, f1->verified_value,
						f1->flags, f1->bbox);
				} else if (v2 && v2[0]) {
					/* Preserve Pass 2 alternative reading for reviewer & arbitration. */
					(void)scribe_entry_set_field_alt(e1, f1->key, v2);
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

/* ── Ephemeral Non-Destructive Image Preprocessing & Cropping ──── */

static uint8_t *read_file_bytes(const char *path, size_t *out_len)
{
	if (!path || !out_len)
		return NULL;
	*out_len = 0;

	FILE *fp = fopen(path, "rb");
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

static int run_magick_argv(char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			(void)dup2(devnull, STDOUT_FILENO);
			(void)dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		execvp("magick", argv);
		execvp("convert", argv);
		_exit(127);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int scribe_prepare_ocr_image_preset(const char *src_path,
                                    scribe_preproc_preset_t preset,
                                    uint8_t **out_bytes,
                                    size_t *out_len,
                                    char out_mime[64])
{
	if (!src_path || !src_path[0] || !out_bytes || !out_len || !out_mime)
		return -1;
	*out_bytes = NULL;
	*out_len = 0;

	char tmp_path[] = "/tmp/scribe-ocr-XXXXXX";
	int fd = mkstemp(tmp_path);
	if (fd < 0)
		return -1;
	close(fd);

	char out_spec[128];
	snprintf(out_spec, sizeof(out_spec), "png:%s", tmp_path);

	int rc;
	if (preset == SCRIBE_PREPROC_HANDWRITING) {
		/*
		 * Gentle non-destructive handwriting enhancement:
		 * EXIF auto-orient + luminance grayscale + soft 0.5% contrast stretch
		 * + mild unsharp mask. Deliberately avoids hard binarization, which
		 * destroys faded hairlines and bleed-through discrimination.
		 */
		char *argv[] = {
			"magick",
			(char *)src_path,
			"-auto-orient",
			"-colorspace", "Gray",
			"-contrast-stretch", "0.5%x0.5%",
			"-unsharp", "0x1.0+0.8+0.02",
			out_spec,
			NULL
		};
		rc = run_magick_argv(argv);
	} else if (preset == SCRIBE_PREPROC_DOCUMENT) {
		/*
		 * Clean photographed/scanned document preset:
		 * EXIF auto-orient + mild deskew + subtle contrast normalization.
		 */
		char *argv[] = {
			"magick",
			(char *)src_path,
			"-auto-orient",
			"-deskew", "40%",
			"-contrast-stretch", "0.2%x0.2%",
			out_spec,
			NULL
		};
		rc = run_magick_argv(argv);
	} else {
		char *argv[] = {
			"magick",
			(char *)src_path,
			"-auto-orient",
			out_spec,
			NULL
		};
		rc = run_magick_argv(argv);
	}

	if (rc != 0) {
		unlink(tmp_path);
		return -1;
	}

	size_t len = 0;
	uint8_t *buf = read_file_bytes(tmp_path, &len);
	unlink(tmp_path);
	if (!buf || len == 0) {
		free(buf);
		return -1;
	}

	*out_bytes = buf;
	*out_len = len;
	snprintf(out_mime, 64, "image/png");
	return 0;
}

int scribe_prepare_ocr_image(const char *src_path,
                             int enhance_handwriting,
                             uint8_t **out_bytes,
                             size_t *out_len,
                             char out_mime[64])
{
	return scribe_prepare_ocr_image_preset(
		src_path,
		enhance_handwriting ? SCRIBE_PREPROC_HANDWRITING : SCRIBE_PREPROC_NONE,
		out_bytes, out_len, out_mime);
}

int scribe_crop_ocr_region(const char *src_path,
                           const float bbox[4],
                           uint8_t **out_bytes,
                           size_t *out_len,
                           char out_mime[64])
{
	if (!src_path || !src_path[0] || !bbox || !out_bytes || !out_len || !out_mime)
		return -1;
	if (bbox[2] <= 0.0f || bbox[3] <= 0.0f)
		return -1;
	*out_bytes = NULL;
	*out_len = 0;

	size_t raw_len = 0;
	uint8_t *raw = read_file_bytes(src_path, &raw_len);
	if (!raw)
		return -1;

	scribe_image_info_t info;
	int sniff_rc = scribe_sniff_image(raw, raw_len, &info);
	free(raw);
	if (sniff_rc != 0 || info.width == 0 || info.height == 0)
		return -1;

	uint32_t eff_w = info.width;
	uint32_t eff_h = info.height;
	if (info.exif_orientation >= 5u && info.exif_orientation <= 8u) {
		eff_w = info.height;
		eff_h = info.width;
	}

	/* Generous padding around normalized bbox so cursive loops & line context remain intact. */
	float x0 = bbox[0] - 0.12f;
	float y0 = bbox[1] - 0.08f;
	float x1 = bbox[0] + bbox[2] + 0.12f;
	float y1 = bbox[1] + bbox[3] + 0.08f;
	if (x0 < 0.0f) x0 = 0.0f;
	if (y0 < 0.0f) y0 = 0.0f;
	if (x1 > 1.0f) x1 = 1.0f;
	if (y1 > 1.0f) y1 = 1.0f;
	if (x1 <= x0 || y1 <= y0)
		return -1;

	uint32_t px = (uint32_t)(x0 * (float)eff_w);
	uint32_t py = (uint32_t)(y0 * (float)eff_h);
	uint32_t pw = (uint32_t)((x1 - x0) * (float)eff_w);
	uint32_t ph = (uint32_t)((y1 - y0) * (float)eff_h);
	if (pw < 16u) pw = (eff_w < 16u) ? eff_w : 16u;
	if (ph < 16u) ph = (eff_h < 16u) ? eff_h : 16u;
	if (px + pw > eff_w) pw = eff_w - px;
	if (py + ph > eff_h) ph = eff_h - py;

	char geom[64];
	snprintf(geom, sizeof(geom), "%ux%u+%u+%u", pw, ph, px, py);

	char tmp_path[] = "/tmp/scribe-crop-XXXXXX";
	int fd = mkstemp(tmp_path);
	if (fd < 0)
		return -1;
	close(fd);

	char out_spec[128];
	snprintf(out_spec, sizeof(out_spec), "png:%s", tmp_path);

	char *argv[] = {
		"magick",
		(char *)src_path,
		"-auto-orient",
		"-crop", geom,
		"+repage",
		"-resize", "150%",
		"-colorspace", "Gray",
		"-contrast-stretch", "0.5%x0.5%",
		"-unsharp", "0x1.0+0.8+0.02",
		out_spec,
		NULL
	};
	int rc = run_magick_argv(argv);
	if (rc != 0) {
		unlink(tmp_path);
		return -1;
	}

	size_t len = 0;
	uint8_t *buf = read_file_bytes(tmp_path, &len);
	unlink(tmp_path);
	if (!buf || len == 0) {
		free(buf);
		return -1;
	}

	*out_bytes = buf;
	*out_len = len;
	snprintf(out_mime, 64, "image/png");
	return 0;
}

static int auren_extract_json_multi(void *ctx,
                                    const uint8_t *const *images,
                                    const size_t *image_lens,
                                    const char *const *mimes,
                                    size_t image_count,
                                    const scribe_template_t *tpl,
                                    const char *model,
                                    const char *prompt,
                                    float temperature,
                                    char **out_json)
{
	(void)tpl;
	if (!images || !image_lens || !mimes || image_count == 0 || !prompt || !out_json)
		return -1;
	*out_json = NULL;

	const scribe_auren_config_t *cfg = (const scribe_auren_config_t *)ctx;
	const char *host = (cfg && cfg->host && cfg->host[0]) ? cfg->host : "127.0.0.1";
	uint16_t port = (cfg && cfg->port) ? cfg->port : 4242u;
	const char *endpoint = (cfg && cfg->endpoint && cfg->endpoint[0])
	                       ? cfg->endpoint : "/v1/chat/completions";
	int timeout_ms = (cfg && cfg->timeout_ms > 0) ? cfg->timeout_ms : 180000;

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

	for (size_t i = 0; i < image_count; i++) {
		if (!images[i] || image_lens[i] == 0 || !mimes[i]) {
			json_object_put(req);
			return -1;
		}
		char *b64 = scribe_base64_encode(images[i], image_lens[i]);
		if (!b64) {
			json_object_put(req);
			return -1;
		}
		size_t url_len = strlen("data:") + strlen(mimes[i]) + strlen(";base64,") + strlen(b64) + 1u;
		char *data_url = malloc(url_len);
		if (!data_url) {
			free(b64);
			json_object_put(req);
			return -1;
		}
		snprintf(data_url, url_len, "data:%s;base64,%s", mimes[i], b64);
		free(b64);

		struct json_object *pimg = json_object_new_object();
		json_object_object_add(pimg, "type", json_object_new_string("image_url"));
		struct json_object *iurl = json_object_new_object();
		json_object_object_add(iurl, "url", json_object_new_string(data_url));
		json_object_object_add(pimg, "image_url", iurl);
		json_object_array_add(parts, pimg);
		free(data_url);
	}

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

static int auren_extract_json(void *ctx,
                              const uint8_t *image_bytes, size_t image_len,
                              const char *mime,
                              const scribe_template_t *tpl,
                              const char *model,
                              const char *prompt,
                              float temperature,
                              char **out_json)
{
	const uint8_t *imgs[1] = { image_bytes };
	size_t lens[1] = { image_len };
	const char *mimes[1] = { mime };
	return auren_extract_json_multi(ctx, imgs, lens, mimes, 1u,
	                                tpl, model, prompt, temperature, out_json);
}

static const scribe_extractor_ops_t AUREN_OPS = {
	.name = "auren",
	.extract_json = auren_extract_json,
	.extract_json_multi = auren_extract_json_multi,
};

const scribe_extractor_ops_t *scribe_extractor_auren(void)
{
	return &AUREN_OPS;
}

/* ── End-to-End Page Extraction Orchestrator ───────────────────── */

static int call_extractor(const scribe_extractor_ops_t *ops,
                          void *extractor_ctx,
                          const uint8_t *const *images,
                          const size_t *image_lens,
                          const char *const *mimes,
                          size_t image_count,
                          const scribe_template_t *tpl,
                          const char *model,
                          const char *prompt,
                          float temperature,
                          char **out_json)
{
	if (!ops || image_count == 0)
		return -1;
	if (image_count > 1 && ops->extract_json_multi) {
		return ops->extract_json_multi(extractor_ctx, images, image_lens,
		                               mimes, image_count, tpl, model,
		                               prompt, temperature, out_json);
	}
	if (!ops->extract_json)
		return -1;
	return ops->extract_json(extractor_ctx, images[0], image_lens[0],
	                         mimes[0], tpl, model, prompt, temperature, out_json);
}

static int build_arbitration_prompt(const scribe_pack_t *pack,
                                    const scribe_template_t *tpl,
                                    const scribe_entry_t *entry,
                                    int has_crop,
                                    char **out_prompt)
{
	if (!tpl || !entry || !out_prompt)
		return -1;
	*out_prompt = NULL;

	const char *persona = (pack && pack->system_persona && pack->system_persona[0])
	                      ? pack->system_persona
	                      : "You are a precise document transcription specialist arbitrating disputed or hard-to-read fields.";

	const uint32_t dispute_mask = SCRIBE_FLAG_CONFLICT | SCRIBE_FLAG_ILLEGIBLE;
	strbuf_t sb = { 0 };
	if (sb_puts(&sb, persona) != 0 ||
	    sb_puts(&sb, "\nTask: Arbitrate disputed or hard-to-read fields in this \"") != 0 ||
	    sb_puts(&sb, tpl->label ? tpl->label : tpl->id) != 0 ||
	    sb_puts(&sb, "\" document.\nImage 1 is the full document scan.") != 0 ||
	    (has_crop && sb_puts(&sb, " Image 2 is a 150% zoomed, contrast-enhanced crop of the disputed region.") != 0) ||
	    sb_puts(&sb,
		"\nCarefully inspect the actual visible characters/strokes.\n"
		"CRITICAL RULES:\n"
		"1. NEVER guess or invent plausible values when text is obscured or unreadable.\n"
		"2. If a word or token is genuinely unreadable, mark ONLY that token with \"[?]\"; "
		"if a field is completely absent from the document, return null.\n"
		"3. For DATE fields, normalize full dates to ISO YYYY-MM-DD (or YYYY-MM / YYYY). "
		"Preserve exact written spelling for names/places/text.\n"
		"4. Return STRICT JSON ONLY (no markdown fences) with shape:\n"
		"{\"fields\": {\"<key>\": \"<resolved string or null>\"}}\n\n") != 0) {
		free(sb.buf);
		return -1;
	}

	if (pack && pack->extraction_guide && pack->extraction_guide[0]) {
		if (sb_puts(&sb, "Domain methodology:\n") != 0 ||
		    sb_puts(&sb, pack->extraction_guide) != 0 ||
		    sb_puts(&sb, "\n\n") != 0) {
			free(sb.buf);
			return -1;
		}
	}

	if (entry->transcript && entry->transcript[0]) {
		if (sb_puts(&sb, "Draft transcript for context:\n") != 0 ||
		    sb_puts(&sb, entry->transcript) != 0 ||
		    sb_puts(&sb, "\n\n") != 0) {
			free(sb.buf);
			return -1;
		}
	}

	if (sb_puts(&sb, "Disputed or uncertain fields to arbitrate:\n") != 0) {
		free(sb.buf);
		return -1;
	}

	size_t count = 0;
	for (size_t i = 0; i < entry->field_count; i++) {
		const scribe_field_val_t *f = &entry->fields[i];
		if (!(f->flags & dispute_mask))
			continue;

		const scribe_field_spec_t *fd = NULL;
		for (size_t k = 0; k < tpl->field_count; k++) {
			if (strcmp(tpl->fields[k].key, f->key) == 0) {
				fd = &tpl->fields[k];
				break;
			}
		}

		const char *type_str = fd ? scribe_field_type_str(fd->type) : "text";
		char line[1024];
		snprintf(line, sizeof(line), "- \"%s\" (%s, %s): pass1=%s%s%s%s\n",
			f->key,
			(fd && fd->label) ? fd->label : f->key,
			type_str,
			(f->raw_value && f->raw_value[0]) ? f->raw_value : "<null>",
			(f->alt_value && f->alt_value[0]) ? " | pass2=" : "",
			(f->alt_value && f->alt_value[0]) ? f->alt_value : "",
			(f->flags & SCRIBE_FLAG_ILLEGIBLE) ? " [flagged illegible]" : "");
		if (sb_puts(&sb, line) != 0) {
			free(sb.buf);
			return -1;
		}
		count++;
	}

	if (count == 0) {
		free(sb.buf);
		return -1;
	}

	*out_prompt = sb.buf;
	return 0;
}

static int apply_arbitration_json(const scribe_pack_t *pack,
                                  const char *raw_response,
                                  scribe_entry_t *entry,
                                  const char *model_escalate)
{
	if (!raw_response || !entry)
		return -1;

	char *jbuf = extract_json_slice(raw_response);
	if (!jbuf)
		return -1;

	struct json_object *root = json_tokener_parse(jbuf);
	free(jbuf);
	if (!root || !json_object_is_type(root, json_type_object)) {
		if (root)
			json_object_put(root);
		return -1;
	}

	struct json_object *fields_obj = NULL;
	if (!json_object_object_get_ex(root, "fields", &fields_obj) ||
	    !fields_obj || !json_object_is_type(fields_obj, json_type_object)) {
		fields_obj = root;
	}

	const uint32_t dispute_mask = SCRIBE_FLAG_CONFLICT | SCRIBE_FLAG_ILLEGIBLE;
	size_t applied = 0;

	for (size_t i = 0; i < entry->field_count; i++) {
		scribe_field_val_t *f = &entry->fields[i];
		if (!(f->flags & dispute_mask))
			continue;

		struct json_object *jv = NULL;
		if (!json_object_object_get_ex(fields_obj, f->key, &jv))
			continue;

		const char *resolved = NULL;
		if (jv && json_object_is_type(jv, json_type_string)) {
			const char *s = json_object_get_string(jv);
			if (s && s[0])
				resolved = s;
		} else if (jv && (json_object_is_type(jv, json_type_int) ||
		                  json_object_is_type(jv, json_type_double))) {
			resolved = json_object_get_string(jv);
		}

		/* Preserve prior reading in alt_value if the arbitrator changed it. */
		if (resolved && f->raw_value && f->raw_value[0] &&
		    !str_equal_trimmed(f->raw_value, resolved) &&
		    (!f->alt_value || !f->alt_value[0])) {
			(void)scribe_entry_set_field_alt(entry, f->key, f->raw_value);
		}

		uint32_t new_flags = f->flags;
		if (!resolved || scribe_has_uncertain_marker_ex(resolved, pack))
			new_flags |= SCRIBE_FLAG_ILLEGIBLE;
		else
			new_flags &= ~SCRIBE_FLAG_ILLEGIBLE;

		/* Always retain SCRIBE_FLAG_CONFLICT if Pass 1 & Pass 2 disagreed. */
		(void)scribe_entry_set_field(entry, f->key, resolved,
		                             f->verified_value, new_flags, f->bbox);
		applied++;
	}

	if (applied > 0 && model_escalate && model_escalate[0] &&
	    !strstr(entry->extractor_model, model_escalate)) {
		char merged[64];
		snprintf(merged, sizeof(merged), "%.40s+%.20s",
		         entry->extractor_model, model_escalate);
		snprintf(entry->extractor_model, sizeof(entry->extractor_model), "%s", merged);
	}

	json_object_put(root);
	return (applied > 0) ? 0 : -1;
}

static void apply_pack_uncertainty_markers(const scribe_pack_t *pack,
                                           scribe_entry_t *entries,
                                           size_t count)
{
	if (!pack || !pack->uncertainty_markers || !pack->uncertainty_markers[0])
		return;
	for (size_t i = 0; i < count; i++) {
		for (size_t f = 0; f < entries[i].field_count; f++) {
			scribe_field_val_t *fv = &entries[i].fields[f];
			if (fv->raw_value && scribe_has_uncertain_marker_ex(fv->raw_value, pack))
				fv->flags |= SCRIBE_FLAG_ILLEGIBLE;
		}
	}
}

int scribe_extract_page_ex(scribe_store_t *store,
                           const scribe_page_t *page,
                           const scribe_template_t *tpl,
                           const scribe_extractor_ops_t *ops,
                           void *extractor_ctx,
                           const scribe_extract_opts_t *opts,
                           scribe_entry_t **out_entries,
                           size_t *out_count)
{
	if (!store || !page || !tpl || !ops || !ops->extract_json ||
	    !out_entries || !out_count)
		return -1;
	*out_entries = NULL;
	*out_count = 0;

	char abs_path[512];
	int pn = snprintf(abs_path, sizeof(abs_path), "%s/%s",
	                  scribe_store_dir(store), page->file_path);
	if (pn <= 0 || (size_t)pn >= sizeof(abs_path))
		return -1;

	size_t raw_len = 0;
	uint8_t *raw_bytes = read_file_bytes(abs_path, &raw_len);
	if (!raw_bytes)
		return -1;

	scribe_image_info_t img_info;
	memset(&img_info, 0, sizeof(img_info));
	(void)scribe_sniff_image(raw_bytes, raw_len, &img_info);

	const scribe_pack_t *pack = opts ? opts->pack : NULL;
	const char *m1 = (opts && opts->model_primary && opts->model_primary[0])
	                 ? opts->model_primary : "chat";
	const char *m2 = (opts && opts->model_secondary && opts->model_secondary[0])
	                 ? opts->model_secondary : NULL;
	const char *mesc = (opts && opts->model_escalate && opts->model_escalate[0])
	                   ? opts->model_escalate : NULL;
	scribe_preproc_preset_t preset = pack ? pack->preprocess_preset : SCRIBE_PREPROC_NONE;
	if (opts && opts->enhance_handwriting)
		preset = SCRIBE_PREPROC_HANDWRITING;

	/*
	 * Non-destructive OCR view: auto-orient if EXIF orientation > 1, and
	 * optionally build an ephemeral preprocessed copy per pack preset.
	 */
	uint8_t *oriented_bytes = NULL;
	size_t oriented_len = 0;
	char oriented_mime[64] = { 0 };
	const uint8_t *base_bytes = raw_bytes;
	size_t base_len = raw_len;
	const char *base_mime = page->mime;

	if (img_info.exif_orientation > 1u &&
	    scribe_prepare_ocr_image_preset(abs_path, SCRIBE_PREPROC_NONE,
	                                    &oriented_bytes, &oriented_len,
	                                    oriented_mime) == 0) {
		base_bytes = oriented_bytes;
		base_len = oriented_len;
		base_mime = oriented_mime;
	}

	uint8_t *enh_bytes = NULL;
	size_t enh_len = 0;
	char enh_mime[64] = { 0 };
	if (preset != SCRIBE_PREPROC_NONE) {
		(void)scribe_prepare_ocr_image_preset(abs_path, preset,
		                                      &enh_bytes, &enh_len, enh_mime);
	}

	const uint8_t *pass_imgs[2] = { base_bytes, enh_bytes };
	size_t pass_lens[2] = { base_len, enh_len };
	const char *pass_mimes[2] = { base_mime, enh_mime };
	size_t pass_img_count = (enh_bytes && enh_len > 0) ? 2u : 1u;

	char *prompt = NULL;
	if (scribe_build_extraction_prompt_ex(pack, tpl, &prompt) != 0) {
		free(enh_bytes);
		free(oriented_bytes);
		free(raw_bytes);
		return -1;
	}

	char *raw1 = NULL;
	if (call_extractor(ops, extractor_ctx, pass_imgs, pass_lens, pass_mimes,
	                   pass_img_count, tpl, m1, prompt, 0.0f, &raw1) != 0) {
		free(prompt);
		free(enh_bytes);
		free(oriented_bytes);
		free(raw_bytes);
		return -1;
	}

	scribe_entry_t *e1 = NULL;
	size_t n1 = 0;
	if (scribe_parse_extraction_json(raw1, tpl, page->id, m1, &e1, &n1) != 0) {
		free(raw1);
		free(prompt);
		free(enh_bytes);
		free(oriented_bytes);
		free(raw_bytes);
		return -1;
	}
	free(raw1);
	apply_pack_uncertainty_markers(pack, e1, n1);

	/* Optional 2-pass conflict check */
	if (m2) {
		float temp2 = (strcmp(m1, m2) == 0) ? 0.25f : 0.0f;
		char *raw2 = NULL;
		if (call_extractor(ops, extractor_ctx, pass_imgs, pass_lens, pass_mimes,
		                   pass_img_count, tpl, m2, prompt, temp2, &raw2) == 0) {
			scribe_entry_t *e2 = NULL;
			size_t n2 = 0;
			if (scribe_parse_extraction_json(raw2, tpl, page->id, m2, &e2, &n2) == 0) {
				apply_pack_uncertainty_markers(pack, e2, n2);
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

	/*
	 * Optional zoomed-crop arbitration for entries with CONFLICT or ILLEGIBLE fields.
	 */
	if (mesc) {
		const uint32_t dispute_mask = SCRIBE_FLAG_CONFLICT | SCRIBE_FLAG_ILLEGIBLE;
		for (size_t i = 0; i < n1; i++) {
			int has_dispute = 0;
			int has_bbox = 0;
			float ux0 = 1.0f, uy0 = 1.0f, ux1 = 0.0f, uy1 = 0.0f;

			for (size_t f = 0; f < e1[i].field_count; f++) {
				const scribe_field_val_t *fv = &e1[i].fields[f];
				if (!(fv->flags & dispute_mask))
					continue;
				has_dispute = 1;
				if (fv->bbox[2] > 0.0f && fv->bbox[3] > 0.0f) {
					float bx0 = fv->bbox[0];
					float by0 = fv->bbox[1];
					float bx1 = fv->bbox[0] + fv->bbox[2];
					float by1 = fv->bbox[1] + fv->bbox[3];
					if (bx0 < ux0) ux0 = bx0;
					if (by0 < uy0) uy0 = by0;
					if (bx1 > ux1) ux1 = bx1;
					if (by1 > uy1) uy1 = by1;
					has_bbox = 1;
				}
			}

			if (!has_dispute)
				continue;

			uint8_t *crop_bytes = NULL;
			size_t crop_len = 0;
			char crop_mime[64] = { 0 };
			if (has_bbox && ux1 > ux0 && uy1 > uy0) {
				float union_bbox[4] = { ux0, uy0, ux1 - ux0, uy1 - uy0 };
				(void)scribe_crop_ocr_region(abs_path, union_bbox,
				                             &crop_bytes, &crop_len, crop_mime);
			}

			char *arb_prompt = NULL;
			if (build_arbitration_prompt(pack, tpl, &e1[i],
			                             (crop_bytes && crop_len > 0) ? 1 : 0,
			                             &arb_prompt) == 0) {
				const uint8_t *arb_imgs[2] = { base_bytes, NULL };
				size_t arb_lens[2] = { base_len, 0 };
				const char *arb_mimes[2] = { base_mime, NULL };
				size_t arb_count = 1u;

				if (crop_bytes && crop_len > 0) {
					arb_imgs[1] = crop_bytes;
					arb_lens[1] = crop_len;
					arb_mimes[1] = crop_mime;
					arb_count = 2u;
				} else if (enh_bytes && enh_len > 0) {
					arb_imgs[1] = enh_bytes;
					arb_lens[1] = enh_len;
					arb_mimes[1] = enh_mime;
					arb_count = 2u;
				}

				char *raw_arb = NULL;
				if (call_extractor(ops, extractor_ctx, arb_imgs, arb_lens, arb_mimes,
				                   arb_count, tpl, mesc, arb_prompt, 0.0f, &raw_arb) == 0) {
					(void)apply_arbitration_json(pack, raw_arb, &e1[i], mesc);
					free(raw_arb);
				}
				free(arb_prompt);
			}
			free(crop_bytes);
		}
	}

	free(enh_bytes);
	free(oriented_bytes);
	free(raw_bytes);

	/* Validate, route status, and persist each extracted entry. */
	for (size_t i = 0; i < n1; i++) {
		size_t flagged = scribe_validate_entry_ex(pack, tpl, &e1[i]);
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
	scribe_extract_opts_t opts;
	memset(&opts, 0, sizeof(opts));
	opts.model_primary = model_primary;
	opts.model_secondary = model_secondary;
	return scribe_extract_page_ex(store, page, tpl, ops, extractor_ctx,
	                              &opts, out_entries, out_count);
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
