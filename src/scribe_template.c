#define _POSIX_C_SOURCE 200809L

#include <ttypt/scribe.h>

#include <dirent.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scribe_registry {
	scribe_template_t **items;
	size_t count;
	size_t cap;
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

static char *read_entire_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	long sz = ftell(fp);
	if (sz < 0 || sz > 4 * 1024 * 1024) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	char *buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	size_t nread = fread(buf, 1, (size_t)sz, fp);
	fclose(fp);
	if (nread != (size_t)sz) {
		free(buf);
		return NULL;
	}
	buf[sz] = '\0';
	return buf;
}

static int compile_hyle_defs(scribe_template_t *tpl)
{
	hyle_schema_desc_t *defs = calloc(tpl->field_count + 1, sizeof(*defs));
	if (!defs)
		return -1;

	for (size_t i = 0; i < tpl->field_count; i++) {
		const scribe_field_spec_t *f = &tpl->fields[i];
		hyle_schema_desc_t *d = &defs[i];

		d->key = f->key;
		d->offset = 0;
		d->size = 0;
		d->is_int = 0;
		d->kind = HYLE_KIND_RECORD;
		d->is_array = 0;
		d->qm_type = (f->type == SCRIBE_FIELD_NOTES) ? HYLE_CM_VSTR : HYLE_CM_STR;
		d->type = HYLE_FIELD_STRING;
		d->writable = 1;
		d->required = f->required ? 1 : 0;
		d->min_length = f->required ? 1 : 0;
		d->filter_style = (f->type == SCRIBE_FIELD_DATE) ? "date" : NULL;
		d->search_weight = (f->type == SCRIBE_FIELD_PERSON) ? 3 : 1;
	}
	/* Sentinel FIELD_END (key == NULL) already zeroed by calloc. */
	tpl->hyle_defs = defs;
	return 0;
}

scribe_template_t *scribe_template_load_json(const char *json_str)
{
	if (!json_str)
		return NULL;

	struct json_object *root = json_tokener_parse(json_str);
	if (!root || !json_object_is_type(root, json_type_object)) {
		if (root)
			json_object_put(root);
		return NULL;
	}

	struct json_object *jid = NULL, *jlabel = NULL, *jver = NULL;
	struct json_object *jhint = NULL, *jfields = NULL;

	if (!json_object_object_get_ex(root, "id", &jid) ||
	    !json_object_is_type(jid, json_type_string) ||
	    !json_object_object_get_ex(root, "fields", &jfields) ||
	    !json_object_is_type(jfields, json_type_array)) {
		json_object_put(root);
		return NULL;
	}

	const char *id_str = json_object_get_string(jid);
	if (!id_str || !id_str[0]) {
		json_object_put(root);
		return NULL;
	}

	scribe_template_t *tpl = calloc(1, sizeof(*tpl));
	if (!tpl) {
		json_object_put(root);
		return NULL;
	}

	tpl->id = dup_str(id_str);
	if (json_object_object_get_ex(root, "label", &jlabel) &&
	    json_object_is_type(jlabel, json_type_string))
		tpl->label = dup_str(json_object_get_string(jlabel));
	else
		tpl->label = dup_str(id_str);

	if (json_object_object_get_ex(root, "version", &jver) &&
	    json_object_is_type(jver, json_type_int))
		tpl->version = (uint32_t)json_object_get_int(jver);
	else
		tpl->version = 1;

	if (json_object_object_get_ex(root, "prompt_hint", &jhint) &&
	    json_object_is_type(jhint, json_type_string))
		tpl->prompt_hint = dup_str(json_object_get_string(jhint));
	else
		tpl->prompt_hint = dup_str("");

	size_t nfields = json_object_array_length(jfields);
	if (!tpl->id || !tpl->label || !tpl->prompt_hint || nfields == 0) {
		scribe_template_free(tpl);
		json_object_put(root);
		return NULL;
	}

	tpl->fields = calloc(nfields, sizeof(*tpl->fields));
	if (!tpl->fields) {
		scribe_template_free(tpl);
		json_object_put(root);
		return NULL;
	}
	tpl->field_count = nfields;

	for (size_t i = 0; i < nfields; i++) {
		struct json_object *jf = json_object_array_get_idx(jfields, i);
		struct json_object *jk = NULL, *jl = NULL, *jt = NULL, *jr = NULL;

		if (!jf || !json_object_is_type(jf, json_type_object) ||
		    !json_object_object_get_ex(jf, "key", &jk) ||
		    !json_object_is_type(jk, json_type_string)) {
			scribe_template_free(tpl);
			json_object_put(root);
			return NULL;
		}

		const char *kstr = json_object_get_string(jk);
		if (!kstr || !kstr[0]) {
			scribe_template_free(tpl);
			json_object_put(root);
			return NULL;
		}

		/* Reject duplicate field keys inside a template. */
		for (size_t prev = 0; prev < i; prev++) {
			if (strcmp(tpl->fields[prev].key, kstr) == 0) {
				scribe_template_free(tpl);
				json_object_put(root);
				return NULL;
			}
		}

		tpl->fields[i].key = dup_str(kstr);
		if (json_object_object_get_ex(jf, "label", &jl) &&
		    json_object_is_type(jl, json_type_string))
			tpl->fields[i].label = dup_str(json_object_get_string(jl));
		else
			tpl->fields[i].label = dup_str(kstr);

		tpl->fields[i].type = SCRIBE_FIELD_TEXT;
		if (json_object_object_get_ex(jf, "type", &jt) &&
		    json_object_is_type(jt, json_type_string)) {
			if (scribe_field_type_parse(json_object_get_string(jt),
			                            &tpl->fields[i].type) != 0) {
				scribe_template_free(tpl);
				json_object_put(root);
				return NULL;
			}
		}

		if (json_object_object_get_ex(jf, "required", &jr) &&
		    json_object_is_type(jr, json_type_boolean))
			tpl->fields[i].required = json_object_get_boolean(jr) ? 1 : 0;

		if (!tpl->fields[i].key || !tpl->fields[i].label) {
			scribe_template_free(tpl);
			json_object_put(root);
			return NULL;
		}
	}

	json_object_put(root);

	if (compile_hyle_defs(tpl) != 0) {
		scribe_template_free(tpl);
		return NULL;
	}
	return tpl;
}

scribe_template_t *scribe_template_load_file(const char *path)
{
	if (!path)
		return NULL;
	char *text = read_entire_file(path);
	if (!text)
		return NULL;
	scribe_template_t *tpl = scribe_template_load_json(text);
	free(text);
	return tpl;
}

void scribe_template_free(scribe_template_t *tpl)
{
	if (!tpl)
		return;
	free(tpl->id);
	free(tpl->label);
	free(tpl->prompt_hint);
	if (tpl->fields) {
		for (size_t i = 0; i < tpl->field_count; i++) {
			free(tpl->fields[i].key);
			free(tpl->fields[i].label);
		}
		free(tpl->fields);
	}
	free(tpl->hyle_defs);
	free(tpl);
}

scribe_registry_t *scribe_registry_open(void)
{
	return calloc(1, sizeof(scribe_registry_t));
}

void scribe_registry_close(scribe_registry_t *reg)
{
	if (!reg)
		return;
	for (size_t i = 0; i < reg->count; i++)
		scribe_template_free(reg->items[i]);
	free(reg->items);
	free(reg);
}

int scribe_registry_add(scribe_registry_t *reg, scribe_template_t *tpl)
{
	if (!reg || !tpl || !tpl->id)
		return -1;

	/* Replace existing template with same id if present. */
	for (size_t i = 0; i < reg->count; i++) {
		if (strcmp(reg->items[i]->id, tpl->id) == 0) {
			scribe_template_free(reg->items[i]);
			reg->items[i] = tpl;
			return 0;
		}
	}

	if (reg->count == reg->cap) {
		size_t ncap = reg->cap ? reg->cap * 2 : 8;
		scribe_template_t **next = realloc(reg->items, ncap * sizeof(*next));
		if (!next)
			return -1;
		reg->items = next;
		reg->cap = ncap;
	}

	reg->items[reg->count++] = tpl;
	return 0;
}

static int has_json_ext(const char *name)
{
	size_t n = strlen(name);
	return n > 5 && strcmp(name + n - 5, ".json") == 0;
}

int scribe_registry_load_dir(scribe_registry_t *reg, const char *dir_path)
{
	if (!reg || !dir_path)
		return -1;

	DIR *dir = opendir(dir_path);
	if (!dir)
		return -1;

	int loaded = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || !has_json_ext(de->d_name))
			continue;

		char path[512];
		int n = snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(path))
			continue;

		scribe_template_t *tpl = scribe_template_load_file(path);
		if (!tpl) {
			closedir(dir);
			return -1;
		}
		if (scribe_registry_add(reg, tpl) != 0) {
			scribe_template_free(tpl);
			closedir(dir);
			return -1;
		}
		loaded++;
	}

	closedir(dir);
	return loaded;
}

const scribe_template_t *scribe_registry_find(const scribe_registry_t *reg,
                                              const char *id)
{
	if (!reg || !id)
		return NULL;
	for (size_t i = 0; i < reg->count; i++) {
		if (strcmp(reg->items[i]->id, id) == 0)
			return reg->items[i];
	}
	return NULL;
}

size_t scribe_registry_count(const scribe_registry_t *reg)
{
	return reg ? reg->count : 0;
}

const scribe_template_t *scribe_registry_at(const scribe_registry_t *reg,
                                            size_t index)
{
	if (!reg || index >= reg->count)
		return NULL;
	return reg->items[index];
}
