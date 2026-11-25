#ifndef TTYPT_SCRIBE_H
#define TTYPT_SCRIBE_H

/**
 * @file scribe.h
 * @brief Public C API for libscribe — handwritten & archival document
 *        template schemas, immutable scan intake, dual-value audit storage,
 *        and full-text search.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <hyle/schema.h>

#define SCRIBE_VERSION "0.1.0"

/* ── Template Schema ───────────────────────────────────────────── */

typedef enum {
	SCRIBE_FIELD_TEXT = 0,
	SCRIBE_FIELD_DATE,
	SCRIBE_FIELD_PERSON,
	SCRIBE_FIELD_PLACE,
	SCRIBE_FIELD_NOTES,
} scribe_field_type_t;

typedef struct {
	char *key;
	char *label;
	scribe_field_type_t type;
	uint8_t required;
} scribe_field_spec_t;

typedef struct {
	char *id;
	char *label;
	uint32_t version;
	char *prompt_hint;
	scribe_field_spec_t *fields;
	size_t field_count;
	/** Compiled framework-neutral hyle descriptors (field_count + 1, FIELD_END). */
	hyle_schema_desc_t *hyle_defs;
} scribe_template_t;

typedef struct scribe_registry scribe_registry_t;

/* ── Immutable Source Page ─────────────────────────────────────── */

typedef struct {
	char id[32];
	char sha256[65];
	char file_path[256];
	char mime[64];
	char book_ref[64];
	uint32_t page_num;
	uint32_t width;
	uint32_t height;
	int64_t ingested_at;
} scribe_page_t;

/* ── Record Entry (1 page -> N entries) & Dual-Value Audit ─────── */

typedef enum {
	SCRIBE_STATUS_PENDING = 0,
	SCRIBE_STATUS_EXTRACTED,
	SCRIBE_STATUS_NEEDS_REVIEW,
	SCRIBE_STATUS_VERIFIED,
} scribe_status_t;

typedef enum {
	SCRIBE_FLAG_NONE      = 0,
	SCRIBE_FLAG_ILLEGIBLE = 1u << 0,
	SCRIBE_FLAG_CONFLICT  = 1u << 1,
	SCRIBE_FLAG_EDITED    = 1u << 2,
	SCRIBE_FLAG_INVALID   = 1u << 3,
} scribe_field_flags_t;

typedef struct {
	char key[64];
	char *raw_value;       /* verbatim VLM transcription (NULL if illegible) */
	char *verified_value;  /* human-verified canonical value (NULL if unverified) */
	uint32_t flags;        /* bitmask of scribe_field_flags_t */
	float bbox[4];         /* optional normalized [x, y, w, h], all 0 if absent */
} scribe_field_val_t;

typedef struct {
	char id[32];
	char page_id[32];
	char template_id[64];
	scribe_status_t status;
	char extractor_model[128];
	char reviewed_by[64];
	int64_t reviewed_at;
	char *marginal_notes;
	scribe_field_val_t *fields;
	size_t field_count;
} scribe_entry_t;

/* ── Image Sniff Metadata ──────────────────────────────────────── */

typedef struct {
	char mime[64];
	char ext[16];
	uint32_t width;
	uint32_t height;
	uint8_t exif_orientation; /* 1..8 (1 = normal, 0 = absent) */
	uint8_t is_pdf;
} scribe_image_info_t;

/* ── Search Options ────────────────────────────────────────────── */

typedef enum {
	SCRIBE_SEARCH_EXACT  = 0, /* accent-sensitive via liblemma */
	SCRIBE_SEARCH_FOLDED = 1, /* accent-stripped ASCII shadow index */
} scribe_search_mode_t;

typedef struct {
	char **ids;
	size_t count;
} scribe_id_list_t;

/* ── Opaque Store Handle ───────────────────────────────────────── */

typedef struct scribe_store scribe_store_t;

/* ── Version & Helpers ─────────────────────────────────────────── */

const char *scribe_version(void);
const char *scribe_status_str(scribe_status_t status);
int scribe_status_parse(const char *s, scribe_status_t *out);
const char *scribe_field_type_str(scribe_field_type_t type);
int scribe_field_type_parse(const char *s, scribe_field_type_t *out);

/**
 * @brief Locale-independent UTF-8 Latin diacritic fold to lowercase ASCII.
 * @return Bytes written (excluding NUL), or -1 if outsz is insufficient.
 */
int scribe_ascii_fold(char *out, size_t outsz, const char *in);

/* ── Template Loading & Registry ───────────────────────────────── */

scribe_template_t *scribe_template_load_json(const char *json_str);
scribe_template_t *scribe_template_load_file(const char *path);
void scribe_template_free(scribe_template_t *tpl);

scribe_registry_t *scribe_registry_open(void);
void scribe_registry_close(scribe_registry_t *reg);
int scribe_registry_add(scribe_registry_t *reg, scribe_template_t *tpl);
int scribe_registry_load_dir(scribe_registry_t *reg, const char *dir_path);
const scribe_template_t *scribe_registry_find(const scribe_registry_t *reg,
                                              const char *id);
size_t scribe_registry_count(const scribe_registry_t *reg);
const scribe_template_t *scribe_registry_at(const scribe_registry_t *reg,
                                            size_t index);

/* ── SHA-256, Image Sniffing & Immutable Intake ────────────────── */

void scribe_sha256_buf(const uint8_t *data, size_t len, char out_hex[65]);
int scribe_sha256_file(const char *path, char out_hex[65]);
int scribe_sniff_image(const uint8_t *data, size_t len,
                       scribe_image_info_t *out);

/**
 * @brief Ingest raw image bytes into the immutable content-addressed store.
 *
 * Deduplicates by SHA-256. If an identical scan already exists, populates
 * @p out_page with the existing record and sets *@p out_deduped = 1.
 */
int scribe_ingest_buffer(scribe_store_t *store,
                         const uint8_t *data, size_t len,
                         const char *book_ref, uint32_t page_num,
                         scribe_page_t *out_page, int *out_deduped);

/**
 * @brief Ingest an image file or multi-page PDF into the store.
 *
 * For single images, creates (or deduplicates) 1 page. For PDFs, rasterizes
 * pages at @p dpi (default 250 when 0) via pdftoppm and ingests each page.
 * Caller releases @p out_pages with free().
 */
int scribe_ingest_file(scribe_store_t *store,
                       const char *src_path,
                       const char *book_ref,
                       uint32_t start_page_num,
                       uint32_t dpi,
                       scribe_page_t **out_pages,
                       size_t *out_count);

/* ── Store Lifecycle & CRUD ────────────────────────────────────── */

scribe_store_t *scribe_store_open(const char *data_dir);
void scribe_store_close(scribe_store_t *store);
int scribe_store_save(scribe_store_t *store);
const char *scribe_store_dir(const scribe_store_t *store);

/* Pages */
int scribe_page_put(scribe_store_t *store, const scribe_page_t *page);
int scribe_page_get(scribe_store_t *store, const char *page_id,
                    scribe_page_t *out_page);
int scribe_page_find_by_sha256(scribe_store_t *store, const char *sha256,
                               scribe_page_t *out_page);
int scribe_page_list(scribe_store_t *store, scribe_id_list_t *out_ids);

/* Entries (1 page -> N entries) */
void scribe_entry_init(scribe_entry_t *entry);
void scribe_entry_free(scribe_entry_t *entry);
int scribe_entry_set_field(scribe_entry_t *entry,
                           const char *key,
                           const char *raw_value,
                           const char *verified_value,
                           uint32_t flags,
                           const float bbox[4]);
const scribe_field_val_t *scribe_entry_get_field(const scribe_entry_t *entry,
                                                 const char *key);
int scribe_entry_set_marginal_notes(scribe_entry_t *entry, const char *notes);

int scribe_entry_put(scribe_store_t *store, scribe_entry_t *entry);
int scribe_entry_get(scribe_store_t *store, const char *entry_id,
                     scribe_entry_t *out_entry);
int scribe_entry_del(scribe_store_t *store, const char *entry_id);
int scribe_entry_list_by_page(scribe_store_t *store, const char *page_id,
                              scribe_id_list_t *out_ids);
int scribe_entry_list_by_status(scribe_store_t *store, scribe_status_t status,
                                scribe_id_list_t *out_ids);

/* Search (liblemma-backed) */
int scribe_search(scribe_store_t *store,
                  const char *field, /* NULL or "all" for full-record search */
                  const char *query,
                  scribe_search_mode_t mode,
                  scribe_id_list_t *out_ids);

void scribe_id_list_free(scribe_id_list_t *list);

#ifdef __cplusplus
}
#endif

#endif /* TTYPT_SCRIBE_H */
