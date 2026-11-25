#define _POSIX_C_SOURCE 200809L

#include <ttypt/scribe.h>
#include <ttypt/corm.h>
#include <ttypt/lemma.h>
#include <ttypt/stele.h>

#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct scribe_store {
	char data_dir[256];
	char db_path[320];
	uint32_t pages_hd;
	uint32_t sha_hd;
	uint32_t entries_hd;
	uint32_t page_entries_hd;
	uint32_t meta_hd;
	lemma_db_t *lemma;
};

static char *dup_str(const char *s)
{
	if (!s)
		return NULL;
	size_t n = strlen(s);
	char *copy = malloc(n + 1);
	if (!copy)
		return NULL;
	memcpy(copy, s, n + 1);
	return copy;
}

static int mkdir_p(const char *path)
{
	char tmp[512];
	size_t n = stl_strlcpy(tmp, path, sizeof(tmp));
	if (n == 0 || n >= sizeof(tmp))
		return -1;

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

/* ── ID List Helper ────────────────────────────────────────────── */

static int id_list_push(scribe_id_list_t *list, const char *id)
{
	char **next = realloc(list->ids, (list->count + 1) * sizeof(*next));
	if (!next)
		return -1;
	list->ids = next;
	list->ids[list->count] = dup_str(id);
	if (!list->ids[list->count])
		return -1;
	list->count++;
	return 0;
}

void scribe_id_list_free(scribe_id_list_t *list)
{
	if (!list)
		return;
	for (size_t i = 0; i < list->count; i++)
		free(list->ids[i]);
	free(list->ids);
	list->ids = NULL;
	list->count = 0;
}

/* ── JSON Serialization (Pages & Entries) ──────────────────────── */

static char *page_to_json(const scribe_page_t *p)
{
	struct json_object *obj = json_object_new_object();
	if (!obj)
		return NULL;

	json_object_object_add(obj, "id", json_object_new_string(p->id));
	json_object_object_add(obj, "sha256", json_object_new_string(p->sha256));
	json_object_object_add(obj, "file_path", json_object_new_string(p->file_path));
	json_object_object_add(obj, "mime", json_object_new_string(p->mime));
	json_object_object_add(obj, "book_ref", json_object_new_string(p->book_ref));
	json_object_object_add(obj, "page_num", json_object_new_int64((int64_t)p->page_num));
	json_object_object_add(obj, "width", json_object_new_int64((int64_t)p->width));
	json_object_object_add(obj, "height", json_object_new_int64((int64_t)p->height));
	json_object_object_add(obj, "ingested_at", json_object_new_int64(p->ingested_at));

	const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
	char *out = dup_str(s);
	json_object_put(obj);
	return out;
}

static int page_from_json(const char *js, scribe_page_t *p)
{
	struct json_object *obj = json_tokener_parse(js);
	if (!obj || !json_object_is_type(obj, json_type_object)) {
		if (obj)
			json_object_put(obj);
		return -1;
	}
	memset(p, 0, sizeof(*p));

	struct json_object *v = NULL;
	if (json_object_object_get_ex(obj, "id", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(p->id, json_object_get_string(v), sizeof(p->id));
	if (json_object_object_get_ex(obj, "sha256", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(p->sha256, json_object_get_string(v), sizeof(p->sha256));
	if (json_object_object_get_ex(obj, "file_path", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(p->file_path, json_object_get_string(v), sizeof(p->file_path));
	if (json_object_object_get_ex(obj, "mime", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(p->mime, json_object_get_string(v), sizeof(p->mime));
	if (json_object_object_get_ex(obj, "book_ref", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(p->book_ref, json_object_get_string(v), sizeof(p->book_ref));
	if (json_object_object_get_ex(obj, "page_num", &v))
		p->page_num = (uint32_t)json_object_get_int64(v);
	if (json_object_object_get_ex(obj, "width", &v))
		p->width = (uint32_t)json_object_get_int64(v);
	if (json_object_object_get_ex(obj, "height", &v))
		p->height = (uint32_t)json_object_get_int64(v);
	if (json_object_object_get_ex(obj, "ingested_at", &v))
		p->ingested_at = json_object_get_int64(v);

	json_object_put(obj);
	return p->id[0] ? 0 : -1;
}

static char *entry_to_json(const scribe_entry_t *e)
{
	struct json_object *obj = json_object_new_object();
	if (!obj)
		return NULL;

	json_object_object_add(obj, "id", json_object_new_string(e->id));
	json_object_object_add(obj, "page_id", json_object_new_string(e->page_id));
	json_object_object_add(obj, "template_id", json_object_new_string(e->template_id));
	json_object_object_add(obj, "status", json_object_new_string(scribe_status_str(e->status)));
	json_object_object_add(obj, "extractor_model", json_object_new_string(e->extractor_model));
	json_object_object_add(obj, "reviewed_by", json_object_new_string(e->reviewed_by));
	json_object_object_add(obj, "reviewed_at", json_object_new_int64(e->reviewed_at));
	if (e->marginal_notes)
		json_object_object_add(obj, "marginal_notes", json_object_new_string(e->marginal_notes));
	else
		json_object_object_add(obj, "marginal_notes", NULL);
	if (e->transcript)
		json_object_object_add(obj, "transcript", json_object_new_string(e->transcript));
	else
		json_object_object_add(obj, "transcript", NULL);

	struct json_object *arr = json_object_new_array();
	for (size_t i = 0; i < e->field_count; i++) {
		const scribe_field_val_t *f = &e->fields[i];
		struct json_object *fo = json_object_new_object();
		json_object_object_add(fo, "key", json_object_new_string(f->key));
		json_object_object_add(fo, "raw_value",
			f->raw_value ? json_object_new_string(f->raw_value) : NULL);
		if (f->alt_value)
			json_object_object_add(fo, "alt_value", json_object_new_string(f->alt_value));
		json_object_object_add(fo, "verified_value",
			f->verified_value ? json_object_new_string(f->verified_value) : NULL);
		json_object_object_add(fo, "flags", json_object_new_int64((int64_t)f->flags));

		struct json_object *bb = json_object_new_array();
		for (int k = 0; k < 4; k++)
			json_object_array_add(bb, json_object_new_double((double)f->bbox[k]));
		json_object_object_add(fo, "bbox", bb);

		json_object_array_add(arr, fo);
	}
	json_object_object_add(obj, "fields", arr);

	const char *s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
	char *out = dup_str(s);
	json_object_put(obj);
	return out;
}

static int entry_from_json(const char *js, scribe_entry_t *e)
{
	struct json_object *obj = json_tokener_parse(js);
	if (!obj || !json_object_is_type(obj, json_type_object)) {
		if (obj)
			json_object_put(obj);
		return -1;
	}
	scribe_entry_init(e);

	struct json_object *v = NULL;
	if (json_object_object_get_ex(obj, "id", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(e->id, json_object_get_string(v), sizeof(e->id));
	if (json_object_object_get_ex(obj, "page_id", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(e->page_id, json_object_get_string(v), sizeof(e->page_id));
	if (json_object_object_get_ex(obj, "template_id", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(e->template_id, json_object_get_string(v), sizeof(e->template_id));
	if (json_object_object_get_ex(obj, "status", &v) && json_object_is_type(v, json_type_string))
		(void)scribe_status_parse(json_object_get_string(v), &e->status);
	if (json_object_object_get_ex(obj, "extractor_model", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(e->extractor_model, json_object_get_string(v), sizeof(e->extractor_model));
	if (json_object_object_get_ex(obj, "reviewed_by", &v) && json_object_is_type(v, json_type_string))
		stl_strlcpy(e->reviewed_by, json_object_get_string(v), sizeof(e->reviewed_by));
	if (json_object_object_get_ex(obj, "reviewed_at", &v))
		e->reviewed_at = json_object_get_int64(v);
	if (json_object_object_get_ex(obj, "marginal_notes", &v) && json_object_is_type(v, json_type_string))
		(void)scribe_entry_set_marginal_notes(e, json_object_get_string(v));
	if (json_object_object_get_ex(obj, "transcript", &v) && json_object_is_type(v, json_type_string))
		(void)scribe_entry_set_transcript(e, json_object_get_string(v));

	if (json_object_object_get_ex(obj, "fields", &v) && json_object_is_type(v, json_type_array)) {
		size_t nf = json_object_array_length(v);
		for (size_t i = 0; i < nf; i++) {
			struct json_object *fo = json_object_array_get_idx(v, i);
			struct json_object *jk = NULL, *jr = NULL, *ja = NULL, *jv = NULL, *jfl = NULL, *jbb = NULL;
			if (!fo || !json_object_object_get_ex(fo, "key", &jk) ||
			    !json_object_is_type(jk, json_type_string))
				continue;

			const char *raw = NULL, *alt = NULL, *ver = NULL;
			uint32_t flags = 0;
			float bbox[4] = { 0, 0, 0, 0 };

			if (json_object_object_get_ex(fo, "raw_value", &jr) &&
			    jr && json_object_is_type(jr, json_type_string))
				raw = json_object_get_string(jr);
			if (json_object_object_get_ex(fo, "alt_value", &ja) &&
			    ja && json_object_is_type(ja, json_type_string))
				alt = json_object_get_string(ja);
			if (json_object_object_get_ex(fo, "verified_value", &jv) &&
			    jv && json_object_is_type(jv, json_type_string))
				ver = json_object_get_string(jv);
			if (json_object_object_get_ex(fo, "flags", &jfl) && jfl)
				flags = (uint32_t)json_object_get_int64(jfl);
			if (json_object_object_get_ex(fo, "bbox", &jbb) &&
			    jbb && json_object_is_type(jbb, json_type_array) &&
			    json_object_array_length(jbb) == 4) {
				for (int k = 0; k < 4; k++)
					bbox[k] = (float)json_object_get_double(
						json_object_array_get_idx(jbb, k));
			}

			const char *key = json_object_get_string(jk);
			if (scribe_entry_set_field(e, key, raw, ver, flags, bbox) != 0) {
				scribe_entry_free(e);
				json_object_put(obj);
				return -1;
			}
			if (alt && scribe_entry_set_field_alt(e, key, alt) != 0) {
				scribe_entry_free(e);
				json_object_put(obj);
				return -1;
			}
		}
	}

	json_object_put(obj);
	return e->id[0] ? 0 : -1;
}

/* ── liblemma Full-Text Indexing (Exact + Folded Shadow) ───────── */

static void index_text_pair(lemma_db_t *db, const char *field,
                            const char *row_id, const char *text)
{
	if (!db || !field || !row_id || !text || !text[0])
		return;

	(void)lemma_index(db, field, row_id, text);
	(void)lemma_index(db, "all", row_id, text);

	size_t flen = strlen(text) + 1;
	char *folded = malloc(flen);
	if (folded) {
		if (scribe_ascii_fold(folded, flen, text) >= 0) {
			char ascii_field[80];
			snprintf(ascii_field, sizeof(ascii_field), "ascii:%s", field);
			(void)lemma_index(db, ascii_field, row_id, folded);
			(void)lemma_index(db, "ascii:all", row_id, folded);
		}
		free(folded);
	}
}

static void unindex_entry(scribe_store_t *store, const scribe_entry_t *e)
{
	if (!store || !store->lemma || !e || !e->id[0])
		return;

	(void)lemma_unindex(store->lemma, "all", e->id);
	(void)lemma_unindex(store->lemma, "ascii:all", e->id);
	(void)lemma_unindex(store->lemma, "marginal_notes", e->id);
	(void)lemma_unindex(store->lemma, "ascii:marginal_notes", e->id);
	(void)lemma_unindex(store->lemma, "transcript", e->id);
	(void)lemma_unindex(store->lemma, "ascii:transcript", e->id);
	(void)lemma_unindex(store->lemma, "book_ref", e->id);
	(void)lemma_unindex(store->lemma, "ascii:book_ref", e->id);

	for (size_t i = 0; i < e->field_count; i++) {
		char ascii_field[80];
		snprintf(ascii_field, sizeof(ascii_field), "ascii:%s", e->fields[i].key);
		(void)lemma_unindex(store->lemma, e->fields[i].key, e->id);
		(void)lemma_unindex(store->lemma, ascii_field, e->id);
	}
}

static void index_entry(scribe_store_t *store, const scribe_entry_t *e)
{
	if (!store || !store->lemma || !e || !e->id[0])
		return;

	for (size_t i = 0; i < e->field_count; i++) {
		const scribe_field_val_t *f = &e->fields[i];
		/* Prefer human-verified canonical value; fall back to raw VLM value. */
		const char *val = (f->verified_value && f->verified_value[0])
		                  ? f->verified_value
		                  : f->raw_value;
		if (val && val[0])
			index_text_pair(store->lemma, f->key, e->id, val);
	}

	if (e->marginal_notes && e->marginal_notes[0])
		index_text_pair(store->lemma, "marginal_notes", e->id, e->marginal_notes);

	if (e->transcript && e->transcript[0])
		index_text_pair(store->lemma, "transcript", e->id, e->transcript);

	if (e->page_id[0]) {
		scribe_page_t pg;
		if (scribe_page_get(store, e->page_id, &pg) == 0 && pg.book_ref[0])
			index_text_pair(store->lemma, "book_ref", e->id, pg.book_ref);
	}
}

static void rebuild_lemma_index(scribe_store_t *store)
{
	uint32_t cur = corm_iter(store->entries_hd, NULL, 0);
	const void *k = NULL, *v = NULL;
	while (corm_next(&k, &v, cur)) {
		scribe_entry_t e;
		if (entry_from_json((const char *)v, &e) == 0) {
			index_entry(store, &e);
			scribe_entry_free(&e);
		}
	}
	corm_fin(cur);
}

/* ── Store Lifecycle ───────────────────────────────────────────── */

scribe_store_t *scribe_store_open(const char *data_dir)
{
	if (!data_dir || !data_dir[0])
		return NULL;
	if (mkdir_p(data_dir) != 0)
		return NULL;

	scribe_store_t *store = calloc(1, sizeof(*store));
	if (!store)
		return NULL;

	stl_strlcpy(store->data_dir, data_dir, sizeof(store->data_dir));
	int n = snprintf(store->db_path, sizeof(store->db_path),
	                 "%s/scribe.corm", store->data_dir);
	if (n < 0 || (size_t)n >= sizeof(store->db_path)) {
		free(store);
		return NULL;
	}

	store->pages_hd = corm_open(store->db_path, "pages",
	                            CM_STR, CM_STR, 0xFF, CM_SORTED);
	store->sha_hd = corm_open(store->db_path, "sha_to_page",
	                          CM_STR, CM_STR, 0xFF, 0);
	store->entries_hd = corm_open(store->db_path, "entries",
	                              CM_STR, CM_STR, 0xFF, CM_SORTED);
	store->page_entries_hd = corm_open(store->db_path, "page_entries",
	                                   CM_STR, CM_STR, 0xFF,
	                                   CM_SORTED | CM_MULTIVALUE);
	store->meta_hd = corm_open(store->db_path, "meta",
	                           CM_STR, CM_STR, 0x1F, 0);

	if (store->pages_hd == CM_MISS || store->sha_hd == CM_MISS ||
	    store->entries_hd == CM_MISS || store->page_entries_hd == CM_MISS ||
	    store->meta_hd == CM_MISS) {
		scribe_store_close(store);
		return NULL;
	}

	store->lemma = lemma_open(0);
	if (!store->lemma) {
		scribe_store_close(store);
		return NULL;
	}

	rebuild_lemma_index(store);
	return store;
}

void scribe_store_close(scribe_store_t *store)
{
	if (!store)
		return;
	corm_save();
	if (store->lemma)
		lemma_close(store->lemma);
	if (store->meta_hd != CM_MISS)
		corm_close(store->meta_hd);
	if (store->page_entries_hd != CM_MISS)
		corm_close(store->page_entries_hd);
	if (store->entries_hd != CM_MISS)
		corm_close(store->entries_hd);
	if (store->sha_hd != CM_MISS)
		corm_close(store->sha_hd);
	if (store->pages_hd != CM_MISS)
		corm_close(store->pages_hd);
	free(store);
}

int scribe_store_save(scribe_store_t *store)
{
	if (!store)
		return -1;
	corm_save();
	return 0;
}

const char *scribe_store_dir(const scribe_store_t *store)
{
	return store ? store->data_dir : NULL;
}

/* ── Page CRUD ─────────────────────────────────────────────────── */

int scribe_page_put(scribe_store_t *store, const scribe_page_t *page)
{
	if (!store || !page || !page->id[0] || !page->sha256[0])
		return -1;
	char *js = page_to_json(page);
	if (!js)
		return -1;
	corm_put(store->pages_hd, page->id, js);
	corm_put(store->sha_hd, page->sha256, page->id);
	free(js);
	return 0;
}

int scribe_page_get(scribe_store_t *store, const char *page_id,
                    scribe_page_t *out_page)
{
	if (!store || !page_id || !out_page)
		return -1;
	const char *js = corm_get(store->pages_hd, page_id);
	if (!js)
		return -1;
	return page_from_json(js, out_page);
}

int scribe_page_find_by_sha256(scribe_store_t *store, const char *sha256,
                               scribe_page_t *out_page)
{
	if (!store || !sha256 || !out_page)
		return -1;
	const char *page_id = corm_get(store->sha_hd, sha256);
	if (!page_id)
		return -1;
	return scribe_page_get(store, page_id, out_page);
}

int scribe_page_list(scribe_store_t *store, scribe_id_list_t *out_ids)
{
	if (!store || !out_ids)
		return -1;
	out_ids->ids = NULL;
	out_ids->count = 0;

	uint32_t cur = corm_iter(store->pages_hd, NULL, CM_RANGE);
	const void *k = NULL, *v = NULL;
	while (corm_next(&k, &v, cur)) {
		if (id_list_push(out_ids, (const char *)k) != 0) {
			corm_fin(cur);
			scribe_id_list_free(out_ids);
			return -1;
		}
	}
	corm_fin(cur);
	return 0;
}

/* ── Entry Struct Helpers ──────────────────────────────────────── */

void scribe_entry_init(scribe_entry_t *entry)
{
	if (!entry)
		return;
	memset(entry, 0, sizeof(*entry));
}

void scribe_entry_free(scribe_entry_t *entry)
{
	if (!entry)
		return;
	free(entry->marginal_notes);
	free(entry->transcript);
	for (size_t i = 0; i < entry->field_count; i++) {
		free(entry->fields[i].raw_value);
		free(entry->fields[i].alt_value);
		free(entry->fields[i].verified_value);
	}
	free(entry->fields);
	memset(entry, 0, sizeof(*entry));
}

int scribe_entry_set_marginal_notes(scribe_entry_t *entry, const char *notes)
{
	if (!entry)
		return -1;
	char *copy = notes ? dup_str(notes) : NULL;
	if (notes && !copy)
		return -1;
	free(entry->marginal_notes);
	entry->marginal_notes = copy;
	return 0;
}

int scribe_entry_set_transcript(scribe_entry_t *entry, const char *transcript)
{
	if (!entry)
		return -1;
	char *copy = transcript ? dup_str(transcript) : NULL;
	if (transcript && !copy)
		return -1;
	free(entry->transcript);
	entry->transcript = copy;
	return 0;
}

int scribe_entry_set_field(scribe_entry_t *entry,
                           const char *key,
                           const char *raw_value,
                           const char *verified_value,
                           uint32_t flags,
                           const float bbox[4])
{
	if (!entry || !key || !key[0])
		return -1;

	scribe_field_val_t *slot = NULL;
	for (size_t i = 0; i < entry->field_count; i++) {
		if (strcmp(entry->fields[i].key, key) == 0) {
			slot = &entry->fields[i];
			break;
		}
	}

	char *raw_copy = raw_value ? dup_str(raw_value) : NULL;
	char *ver_copy = verified_value ? dup_str(verified_value) : NULL;
	if ((raw_value && !raw_copy) || (verified_value && !ver_copy)) {
		free(raw_copy);
		free(ver_copy);
		return -1;
	}

	if (!slot) {
		scribe_field_val_t *next = realloc(
			entry->fields, (entry->field_count + 1) * sizeof(*next));
		if (!next) {
			free(raw_copy);
			free(ver_copy);
			return -1;
		}
		entry->fields = next;
		slot = &entry->fields[entry->field_count++];
		memset(slot, 0, sizeof(*slot));
		stl_strlcpy(slot->key, key, sizeof(slot->key));
	} else {
		free(slot->raw_value);
		free(slot->verified_value);
	}

	slot->raw_value = raw_copy;
	slot->verified_value = ver_copy;
	slot->flags = flags;
	if (bbox)
		memcpy(slot->bbox, bbox, sizeof(slot->bbox));
	else
		memset(slot->bbox, 0, sizeof(slot->bbox));
	return 0;
}

int scribe_entry_set_field_alt(scribe_entry_t *entry,
                               const char *key,
                               const char *alt_value)
{
	if (!entry || !key || !key[0])
		return -1;

	for (size_t i = 0; i < entry->field_count; i++) {
		if (strcmp(entry->fields[i].key, key) == 0) {
			char *copy = (alt_value && alt_value[0]) ? dup_str(alt_value) : NULL;
			if (alt_value && alt_value[0] && !copy)
				return -1;
			free(entry->fields[i].alt_value);
			entry->fields[i].alt_value = copy;
			return 0;
		}
	}
	return -1;
}

const scribe_field_val_t *scribe_entry_get_field(const scribe_entry_t *entry,
                                                 const char *key)
{
	if (!entry || !key)
		return NULL;
	for (size_t i = 0; i < entry->field_count; i++) {
		if (strcmp(entry->fields[i].key, key) == 0)
			return &entry->fields[i];
	}
	return NULL;
}

/* ── Entry CRUD & 1-Page -> N-Entries Index ────────────────────── */

static void next_entry_id(scribe_store_t *store, char out_id[32])
{
	unsigned long long seq = 0;
	const char *cur = corm_get(store->meta_hd, "next_entry_seq");
	if (cur)
		seq = strtoull(cur, NULL, 10);
	seq++;

	char buf[32];
	snprintf(buf, sizeof(buf), "%llu", seq);
	corm_put(store->meta_hd, "next_entry_seq", buf);
	snprintf(out_id, 32, "en_%08llu", seq);
}

int scribe_entry_put(scribe_store_t *store, scribe_entry_t *entry)
{
	if (!store || !entry || !entry->page_id[0] || !entry->template_id[0])
		return -1;

	if (!entry->id[0])
		next_entry_id(store, entry->id);

	int existed = 0;
	int link_page = 1;
	scribe_entry_t old;
	if (scribe_entry_get(store, entry->id, &old) == 0) {
		existed = 1;
		unindex_entry(store, &old);
		if (strcmp(old.page_id, entry->page_id) == 0) {
			link_page = 0;
		} else {
			(void)corm_del_value(store->page_entries_hd,
			                     old.page_id, old.id);
		}
		scribe_entry_free(&old);
	}

	char *js = entry_to_json(entry);
	if (!js)
		return -1;
	corm_put(store->entries_hd, entry->id, js);
	free(js);

	if (!existed || link_page)
		corm_put(store->page_entries_hd, entry->page_id, entry->id);

	index_entry(store, entry);
	return 0;
}

int scribe_entry_get(scribe_store_t *store, const char *entry_id,
                     scribe_entry_t *out_entry)
{
	if (!store || !entry_id || !out_entry)
		return -1;
	const char *js = corm_get(store->entries_hd, entry_id);
	if (!js)
		return -1;
	return entry_from_json(js, out_entry);
}

int scribe_entry_del(scribe_store_t *store, const char *entry_id)
{
	if (!store || !entry_id)
		return -1;

	scribe_entry_t old;
	if (scribe_entry_get(store, entry_id, &old) != 0)
		return -1;

	unindex_entry(store, &old);
	(void)corm_del_value(store->page_entries_hd, old.page_id, old.id);
	corm_del(store->entries_hd, entry_id);
	scribe_entry_free(&old);
	return 0;
}

int scribe_entry_list_by_page(scribe_store_t *store, const char *page_id,
                              scribe_id_list_t *out_ids)
{
	if (!store || !page_id || !out_ids)
		return -1;
	out_ids->ids = NULL;
	out_ids->count = 0;

	uint32_t cur = corm_get_multi(store->page_entries_hd, page_id);
	if (cur == CM_MISS)
		return 0;

	const void *k = NULL, *v = NULL;
	while (corm_next(&k, &v, cur)) {
		if (id_list_push(out_ids, (const char *)v) != 0) {
			corm_fin(cur);
			scribe_id_list_free(out_ids);
			return -1;
		}
	}
	corm_fin(cur);
	return 0;
}

int scribe_entry_list_by_status(scribe_store_t *store, scribe_status_t status,
                                scribe_id_list_t *out_ids)
{
	if (!store || !out_ids)
		return -1;
	out_ids->ids = NULL;
	out_ids->count = 0;

	uint32_t cur = corm_iter(store->entries_hd, NULL, CM_RANGE);
	const void *k = NULL, *v = NULL;
	while (corm_next(&k, &v, cur)) {
		scribe_entry_t e;
		if (entry_from_json((const char *)v, &e) == 0) {
			int match = (e.status == status);
			scribe_entry_free(&e);
			if (match && id_list_push(out_ids, (const char *)k) != 0) {
				corm_fin(cur);
				scribe_id_list_free(out_ids);
				return -1;
			}
		}
	}
	corm_fin(cur);
	return 0;
}

/* ── Search (liblemma-backed) ──────────────────────────────────── */

int scribe_search(scribe_store_t *store,
                  const char *field,
                  const char *query,
                  scribe_search_mode_t mode,
                  scribe_id_list_t *out_ids)
{
	if (!store || !query || !out_ids)
		return -1;
	out_ids->ids = NULL;
	out_ids->count = 0;

	const char *base_field = (field && field[0]) ? field : "all";
	char target_field[80];
	char *folded_query = NULL;
	const char *q = query;

	if (mode == SCRIBE_SEARCH_FOLDED) {
		snprintf(target_field, sizeof(target_field), "ascii:%s", base_field);
		size_t qlen = strlen(query) + 1;
		folded_query = malloc(qlen);
		if (!folded_query)
			return -1;
		if (scribe_ascii_fold(folded_query, qlen, query) < 0) {
			free(folded_query);
			return -1;
		}
		q = folded_query;
	} else {
		stl_strlcpy(target_field, base_field, sizeof(target_field));
	}

	uint32_t res_hd = corm_open(NULL, NULL, CM_STR, CM_STR, 0x3F, CM_SORTED);
	if (res_hd == CM_MISS) {
		free(folded_query);
		return -1;
	}

	int handled = 0;
	(void)lemma_query(store->lemma, target_field, q, res_hd, &handled);
	free(folded_query);

	if (handled) {
		uint32_t cur = corm_iter(res_hd, NULL, CM_RANGE);
		const void *k = NULL, *v = NULL;
		while (corm_next(&k, &v, cur)) {
			if (id_list_push(out_ids, (const char *)k) != 0) {
				corm_fin(cur);
				corm_close(res_hd);
				scribe_id_list_free(out_ids);
				return -1;
			}
		}
		corm_fin(cur);
	}

	corm_close(res_hd);
	return 0;
}
