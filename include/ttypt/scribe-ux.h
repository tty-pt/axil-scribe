#ifndef TTYPT_SCRIBE_UX_H
#define TTYPT_SCRIBE_UX_H

/**
 * @file scribe-ux.h
 * @brief Pure isomorphic UX state, action machine, and libbud renderer
 *        for axil-scribe (shared identically between native SSR and WASM).
 *
 * Strictly zero server, storage, or POSIX dependencies.
 */

#include <stddef.h>
#include <stdint.h>
#include <bud/bud.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCRIBE_UX_MAX_FIELDS      32
#define SCRIBE_UX_MAX_SUGGESTIONS 4
#define SCRIBE_UX_MAX_QUEUE       64
#define SCRIBE_UX_MAX_TEMPLATES   16

/* Mirror of scribe_field_flags_t without pulling in native store headers */
#define SCRIBE_UX_FLAG_NONE      0u
#define SCRIBE_UX_FLAG_ILLEGIBLE (1u << 0)
#define SCRIBE_UX_FLAG_CONFLICT  (1u << 1)
#define SCRIBE_UX_FLAG_EDITED    (1u << 2)
#define SCRIBE_UX_FLAG_INVALID   (1u << 3)
#define SCRIBE_UX_FLAG_REVIEW    (SCRIBE_UX_FLAG_ILLEGIBLE | SCRIBE_UX_FLAG_CONFLICT | SCRIBE_UX_FLAG_INVALID)

typedef enum {
	SCRIBE_UX_VIEW_QUEUE = 0,
	SCRIBE_UX_VIEW_ENTRY,
	SCRIBE_UX_VIEW_SEARCH,
} scribe_ux_view_t;

typedef enum {
	SCRIBE_UX_ACT_ZOOM_IN      = 1,
	SCRIBE_UX_ACT_ZOOM_OUT     = 2,
	SCRIBE_UX_ACT_ZOOM_RESET   = 3,
	SCRIBE_UX_ACT_ROTATE_CW    = 4,
	SCRIBE_UX_ACT_ROTATE_CCW   = 5,
	SCRIBE_UX_ACT_PAN_LEFT     = 6,
	SCRIBE_UX_ACT_PAN_RIGHT    = 7,
	SCRIBE_UX_ACT_PAN_UP       = 8,
	SCRIBE_UX_ACT_PAN_DOWN     = 9,
	SCRIBE_UX_ACT_NEXT_FLAGGED = 10,
	SCRIBE_UX_ACT_TOGGLE_ILLEG = 11,
	SCRIBE_UX_ACT_FOCUS_FIELD  = 12,
	SCRIBE_UX_ACT_TOGGLE_CONTRAST = 13,
} scribe_ux_action_t;

typedef struct {
	char id[64];
	char label[96];
} scribe_ux_template_opt_t;

typedef struct {
	char entry_id[32];
	char page_id[32];
	char book_ref[64];
	uint32_t page_num;
	char template_id[64];
	char status[32];
	uint32_t flagged_count;
	char summary[128];
} scribe_ux_queue_item_t;

typedef struct {
	char key[64];
	char label[96];
	char type[16];
	char format_hint[64];
	char enum_options[256]; /* pipe-delimited allowed values for enum fields */
	uint8_t required;
	char raw_value[256];
	char alt_value[256];
	char verified_value[256];
	uint32_t flags;
	float bbox[4]; /* normalized [x, y, w, h], all 0 if absent */
	char suggestions[SCRIBE_UX_MAX_SUGGESTIONS][128];
	size_t suggestion_count;
} scribe_ux_field_t;

typedef struct {
	scribe_ux_view_t view;
	char flash[256];

	/* Optional domain-pack UI vocabulary overrides (empty => generic defaults) */
	char pack_label[96];
	char ui_source_label[48];
	char ui_notes_label[64];
	char ui_transcript_label[64];
	char ui_deep_label[48];

	/* Filters & search */
	char filter_status[32];
	char filter_template[64];
	char filter_book[64];
	char search_query[128];
	int search_folded;

	/* Template selector options */
	scribe_ux_template_opt_t templates[SCRIBE_UX_MAX_TEMPLATES];
	size_t template_count;

	/* Queue / search hit rows */
	scribe_ux_queue_item_t items[SCRIBE_UX_MAX_QUEUE];
	size_t item_count;

	/* Active entry workbench */
	char entry_id[32];
	char page_id[32];
	char book_ref[64];
	uint32_t page_num;
	char template_id[64];
	char template_label[96];
	char status[32];
	char extractor_model[128];
	char reviewed_by[64];
	char image_url[128];
	char marginal_notes[512];
	char transcript[4096];

	scribe_ux_field_t fields[SCRIBE_UX_MAX_FIELDS];
	size_t field_count;

	/* Interactive viewport & focus state */
	int zoom_pct;      /* 50 .. 400, default 100 */
	int rotate_deg;    /* 0, 90, 180, 270 */
	int pan_x;         /* px offset */
	int pan_y;         /* px offset */
	int active_field;  /* index into fields[], or -1 */
	int high_contrast; /* 1 = apply non-destructive high-contrast scan filter */
} scribe_ux_state_t;

/** Initialize state with safe viewport defaults (zoom=100, active_field=-1). */
void scribe_ux_state_init(scribe_ux_state_t *st);

/** Hydrate interactive viewport/entry state from SSR bud-state JSON. */
int scribe_ux_state_from_json(scribe_ux_state_t *st, const char *json, size_t len);

/** Apply a pure UI action (zoom, rotate, pan, next-flagged, toggle illegible). */
int scribe_ux_apply_action(scribe_ux_state_t *st, int action_id, const char *data);

/** Render the complete application view tree (identical on SSR and WASM). */
bud_node *scribe_ux_render(const scribe_ux_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* TTYPT_SCRIBE_UX_H */
