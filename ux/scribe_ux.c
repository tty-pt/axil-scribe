/*
 * Copyright (c) 2026, tty-pt
 * Pure isomorphic libbud UX for axil-scribe (compiles for both native SSR
 * and WebAssembly via clang --target=wasm32-wasi).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bud/bud.h>
#include <bud/bud_app.h>
#include <ttypt/stele_wasm.h>

#include "ttypt/scribe-ux.h"

/* ── Small Pure-C Helpers ──────────────────────────────────────── */

static void
copy_str(char *dst, size_t dstsz, const char *src)
{
	if (!dst || dstsz == 0)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	snprintf(dst, dstsz, "%s", src);
}

static bud_node *
el_class(const char *tag, const char *cls)
{
	bud_node *n = bud_element_lit(tag);
	if (n && cls && cls[0])
		bud_set_attr_name_lit(n, "class", cls);
	return n;
}

static void
append_text_el(bud_node *parent, const char *tag, const char *cls,
               const char *txt)
{
	bud_node *n = el_class(tag, cls);
	if (!n)
		return;
	if (txt)
		bud_append(n, bud_text(txt));
	bud_append(parent, n);
}

static int
parse_int_dec(const char *s, int fallback)
{
	int sign = 1;
	int val = 0;
	int any = 0;

	if (!s || !s[0])
		return fallback;
	if (*s == '-') {
		sign = -1;
		s++;
	} else if (*s == '+') {
		s++;
	}
	while (*s >= '0' && *s <= '9') {
		val = val * 10 + (*s - '0');
		any = 1;
		s++;
	}
	return any ? (val * sign) : fallback;
}

static float
parse_milli(int m)
{
	return (float)m / 1000.0f;
}

static int
first_flagged_index(const scribe_ux_state_t *st)
{
	size_t i;

	for (i = 0; i < st->field_count; i++) {
		if (st->fields[i].flags & SCRIBE_UX_FLAG_REVIEW)
			return (int)i;
	}
	return -1;
}

/* ── State Lifecycle & Action Machine ──────────────────────────── */

void
scribe_ux_state_init(scribe_ux_state_t *st)
{
	if (!st)
		return;
	memset(st, 0, sizeof(*st));
	st->view = SCRIBE_UX_VIEW_QUEUE;
	st->zoom_pct = 100;
	st->rotate_deg = 0;
	st->pan_x = 0;
	st->pan_y = 0;
	st->active_field = -1;
}

int
scribe_ux_state_from_json(scribe_ux_state_t *st, const char *json, size_t len)
{
	char view_buf[32];
	int nfields;
	int i;

	if (!st)
		return -1;
	scribe_ux_state_init(st);
	if (!json || len == 0)
		return 0;

	view_buf[0] = '\0';
	bud_json_str_len(json, len, "view", view_buf, sizeof(view_buf));
	if (strcmp(view_buf, "entry") == 0)
		st->view = SCRIBE_UX_VIEW_ENTRY;
	else if (strcmp(view_buf, "search") == 0)
		st->view = SCRIBE_UX_VIEW_SEARCH;
	else
		st->view = SCRIBE_UX_VIEW_QUEUE;

	bud_json_str_len(json, len, "flash", st->flash, sizeof(st->flash));
	bud_json_str_len(json, len, "pack_label", st->pack_label, sizeof(st->pack_label));
	bud_json_str_len(json, len, "ui_source_label", st->ui_source_label,
	                 sizeof(st->ui_source_label));
	bud_json_str_len(json, len, "ui_notes_label", st->ui_notes_label,
	                 sizeof(st->ui_notes_label));
	bud_json_str_len(json, len, "ui_transcript_label", st->ui_transcript_label,
	                 sizeof(st->ui_transcript_label));
	bud_json_str_len(json, len, "ui_deep_label", st->ui_deep_label,
	                 sizeof(st->ui_deep_label));
	bud_json_str_len(json, len, "filter_status", st->filter_status,
	                 sizeof(st->filter_status));
	bud_json_str_len(json, len, "filter_template", st->filter_template,
	                 sizeof(st->filter_template));
	bud_json_str_len(json, len, "filter_book", st->filter_book,
	                 sizeof(st->filter_book));
	bud_json_str_len(json, len, "search_query", st->search_query,
	                 sizeof(st->search_query));
	st->search_folded = bud_json_int_len(json, len, "search_folded", 0);

	bud_json_str_len(json, len, "entry_id", st->entry_id, sizeof(st->entry_id));
	bud_json_str_len(json, len, "page_id", st->page_id, sizeof(st->page_id));
	bud_json_str_len(json, len, "book_ref", st->book_ref, sizeof(st->book_ref));
	st->page_num = (uint32_t)bud_json_int_len(json, len, "page_num", 0);
	bud_json_str_len(json, len, "template_id", st->template_id,
	                 sizeof(st->template_id));
	bud_json_str_len(json, len, "template_label", st->template_label,
	                 sizeof(st->template_label));
	bud_json_str_len(json, len, "status", st->status, sizeof(st->status));
	bud_json_str_len(json, len, "extractor_model", st->extractor_model,
	                 sizeof(st->extractor_model));
	bud_json_str_len(json, len, "reviewed_by", st->reviewed_by,
	                 sizeof(st->reviewed_by));
	bud_json_str_len(json, len, "image_url", st->image_url,
	                 sizeof(st->image_url));
	bud_json_str_len(json, len, "marginal_notes", st->marginal_notes,
	                 sizeof(st->marginal_notes));
	bud_json_str_len(json, len, "transcript", st->transcript,
	                 sizeof(st->transcript));

	st->zoom_pct = bud_json_int_len(json, len, "zoom_pct", 100);
	if (st->zoom_pct < 50)
		st->zoom_pct = 50;
	if (st->zoom_pct > 400)
		st->zoom_pct = 400;
	st->rotate_deg = bud_json_int_len(json, len, "rotate_deg", 0) % 360;
	if (st->rotate_deg < 0)
		st->rotate_deg += 360;
	st->pan_x = bud_json_int_len(json, len, "pan_x", 0);
	st->pan_y = bud_json_int_len(json, len, "pan_y", 0);
	st->active_field = bud_json_int_len(json, len, "active_field", -1);
	st->high_contrast = bud_json_int_len(json, len, "high_contrast", 0) ? 1 : 0;

	nfields = bud_json_int_len(json, len, "field_count", 0);
	if (nfields < 0)
		nfields = 0;
	if (nfields > SCRIBE_UX_MAX_FIELDS)
		nfields = SCRIBE_UX_MAX_FIELDS;
	st->field_count = (size_t)nfields;

	for (i = 0; i < nfields; i++) {
		char k[32];
		scribe_ux_field_t *f = &st->fields[i];

		snprintf(k, sizeof(k), "f%d_key", i);
		bud_json_str_len(json, len, k, f->key, sizeof(f->key));
		snprintf(k, sizeof(k), "f%d_label", i);
		bud_json_str_len(json, len, k, f->label, sizeof(f->label));
		snprintf(k, sizeof(k), "f%d_type", i);
		bud_json_str_len(json, len, k, f->type, sizeof(f->type));
		snprintf(k, sizeof(k), "f%d_hint", i);
		bud_json_str_len(json, len, k, f->format_hint, sizeof(f->format_hint));
		snprintf(k, sizeof(k), "f%d_req", i);
		f->required = (uint8_t)(bud_json_int_len(json, len, k, 0) ? 1 : 0);
		snprintf(k, sizeof(k), "f%d_raw", i);
		bud_json_str_len(json, len, k, f->raw_value, sizeof(f->raw_value));
		snprintf(k, sizeof(k), "f%d_alt", i);
		bud_json_str_len(json, len, k, f->alt_value, sizeof(f->alt_value));
		snprintf(k, sizeof(k), "f%d_ver", i);
		bud_json_str_len(json, len, k, f->verified_value,
		                 sizeof(f->verified_value));
		snprintf(k, sizeof(k), "f%d_flags", i);
		f->flags = (uint32_t)bud_json_int_len(json, len, k, 0);
		snprintf(k, sizeof(k), "f%d_bx", i);
		f->bbox[0] = parse_milli(bud_json_int_len(json, len, k, 0));
		snprintf(k, sizeof(k), "f%d_by", i);
		f->bbox[1] = parse_milli(bud_json_int_len(json, len, k, 0));
		snprintf(k, sizeof(k), "f%d_bw", i);
		f->bbox[2] = parse_milli(bud_json_int_len(json, len, k, 0));
		snprintf(k, sizeof(k), "f%d_bh", i);
		f->bbox[3] = parse_milli(bud_json_int_len(json, len, k, 0));
	}

	if (st->active_field < 0 && st->field_count > 0)
		st->active_field = first_flagged_index(st);

	return 0;
}

int
scribe_ux_apply_action(scribe_ux_state_t *st, int action_id, const char *data)
{
	size_t step;
	int idx;

	if (!st)
		return -1;

	switch (action_id) {
	case SCRIBE_UX_ACT_ZOOM_IN:
		st->zoom_pct += 25;
		if (st->zoom_pct > 400)
			st->zoom_pct = 400;
		return 0;

	case SCRIBE_UX_ACT_ZOOM_OUT:
		st->zoom_pct -= 25;
		if (st->zoom_pct < 50)
			st->zoom_pct = 50;
		return 0;

	case SCRIBE_UX_ACT_ZOOM_RESET:
		st->zoom_pct = 100;
		st->rotate_deg = 0;
		st->pan_x = 0;
		st->pan_y = 0;
		return 0;

	case SCRIBE_UX_ACT_ROTATE_CW:
		st->rotate_deg = (st->rotate_deg + 90) % 360;
		return 0;

	case SCRIBE_UX_ACT_ROTATE_CCW:
		st->rotate_deg = (st->rotate_deg + 270) % 360;
		return 0;

	case SCRIBE_UX_ACT_PAN_LEFT:
		st->pan_x -= 40;
		return 0;

	case SCRIBE_UX_ACT_PAN_RIGHT:
		st->pan_x += 40;
		return 0;

	case SCRIBE_UX_ACT_PAN_UP:
		st->pan_y -= 40;
		return 0;

	case SCRIBE_UX_ACT_PAN_DOWN:
		st->pan_y += 40;
		return 0;

	case SCRIBE_UX_ACT_NEXT_FLAGGED:
		if (st->field_count == 0)
			return 0;
		for (step = 1; step <= st->field_count; step++) {
			size_t base = (st->active_field >= 0) ? (size_t)st->active_field
			                                      : st->field_count - 1;
			size_t cand = (base + step) % st->field_count;
			if (st->fields[cand].flags & SCRIBE_UX_FLAG_REVIEW) {
				st->active_field = (int)cand;
				return 0;
			}
		}
		return 0;

	case SCRIBE_UX_ACT_TOGGLE_ILLEG:
		idx = parse_int_dec(data, st->active_field);
		if (idx >= 0 && (size_t)idx < st->field_count)
			st->fields[idx].flags ^= SCRIBE_UX_FLAG_ILLEGIBLE;
		return 0;

	case SCRIBE_UX_ACT_FOCUS_FIELD:
		idx = parse_int_dec(data, -1);
		if (idx >= 0 && (size_t)idx < st->field_count)
			st->active_field = idx;
		return 0;

	case SCRIBE_UX_ACT_TOGGLE_CONTRAST:
		st->high_contrast = st->high_contrast ? 0 : 1;
		return 0;

	default:
		return -1;
	}
}

/* ── Pure libbud View Components ───────────────────────────────── */

static const char *
ux_source_label(const scribe_ux_state_t *st)
{
	return (st && st->ui_source_label[0]) ? st->ui_source_label : "Source Ref";
}

static const char *
ux_notes_label(const scribe_ux_state_t *st)
{
	return (st && st->ui_notes_label[0]) ? st->ui_notes_label : "Notes / Corrections";
}

static const char *
ux_transcript_label(const scribe_ux_state_t *st)
{
	return (st && st->ui_transcript_label[0]) ? st->ui_transcript_label : "Full Transcript";
}

static const char *
ux_deep_label(const scribe_ux_state_t *st)
{
	return (st && st->ui_deep_label[0]) ? st->ui_deep_label : "Deep Re-Extract";
}

static bud_node *
render_nav(const scribe_ux_state_t *st)
{
	bud_node *hdr = el_class("header", "scribe-header");
	bud_node *brand = bud_link("/", "axil-scribe", "scribe-brand");
	bud_node *nav = el_class("nav", "scribe-nav");
	bud_node *l_queue = bud_link("/queue", "Review Queue",
	                             st->view == SCRIBE_UX_VIEW_QUEUE ? "is-active" : NULL);
	bud_node *l_search = bud_link("/search", "Search",
	                              st->view == SCRIBE_UX_VIEW_SEARCH ? "is-active" : NULL);
	bud_node *l_csv = bud_link("/export.csv", "Export CSV", NULL);
	bud_node *l_json = bud_link("/export.json", "Export JSON", NULL);

	bud_append(nav, l_queue);
	bud_append(nav, l_search);
	bud_append(nav, l_csv);
	bud_append(nav, l_json);

	bud_append(hdr, brand);
	if (st->pack_label[0])
		append_text_el(hdr, "span", "scribe-badge scribe-pack-badge", st->pack_label);
	bud_append(hdr, nav);
	return hdr;
}

static bud_node *
render_template_select(const scribe_ux_state_t *st, const char *name,
                       const char *selected, int include_all)
{
	bud_node *sel = el_class("select", "scribe-select");
	size_t i;

	bud_set_attr_name_lit(sel, "name", name);
	if (include_all) {
		bud_node *opt = bud_element_lit("option");
		bud_set_attr_lit(opt, "value", "");
		if (!selected || !selected[0])
			bud_set_bool_attr_lit(opt, "selected");
		bud_append(opt, bud_text_lit("All templates"));
		bud_append(sel, opt);
	}
	for (i = 0; i < st->template_count; i++) {
		bud_node *opt = bud_element_lit("option");
		const char *lbl = st->templates[i].label[0] ? st->templates[i].label
		                                            : st->templates[i].id;
		bud_set_attr_name_lit(opt, "value", st->templates[i].id);
		if (selected && strcmp(selected, st->templates[i].id) == 0)
			bud_set_bool_attr_lit(opt, "selected");
		bud_append(opt, bud_text(lbl));
		bud_append(sel, opt);
	}
	return sel;
}

static bud_node *
render_queue_table(const scribe_ux_state_t *st, const char *empty_msg)
{
	bud_node *wrap = el_class("div", "scribe-table-wrap");
	bud_node *tbl;
	bud_node *thead;
	bud_node *trh;
	bud_node *tbody;
	char col_src[64];
	size_t i;

	if (st->item_count == 0) {
		append_text_el(wrap, "p", "scribe-muted", empty_msg);
		return wrap;
	}

	snprintf(col_src, sizeof(col_src), "%s / Page", ux_source_label(st));
	tbl = el_class("table", "scribe-table");
	thead = bud_element_lit("thead");
	trh = bud_element_lit("tr");
	append_text_el(trh, "th", NULL, "Entry");
	append_text_el(trh, "th", NULL, col_src);
	append_text_el(trh, "th", NULL, "Template");
	append_text_el(trh, "th", NULL, "Status");
	append_text_el(trh, "th", NULL, "Flags");
	append_text_el(trh, "th", NULL, "Summary");
	append_text_el(trh, "th", NULL, "Action");
	bud_append(thead, trh);
	bud_append(tbl, thead);

	tbody = bud_element_lit("tbody");
	for (i = 0; i < st->item_count; i++) {
		const scribe_ux_queue_item_t *it = &st->items[i];
		bud_node *tr = bud_element_lit("tr");
		bud_node *td_id = bud_element_lit("td");
		bud_node *td_bp = bud_element_lit("td");
		bud_node *td_tpl = bud_element_lit("td");
		bud_node *td_st = bud_element_lit("td");
		bud_node *td_fl = bud_element_lit("td");
		bud_node *td_sum = bud_element_lit("td");
		bud_node *td_act = bud_element_lit("td");
		char href[96];
		char status_cls[64];

		snprintf(href, sizeof(href), "/entries/%s", it->entry_id);
		snprintf(status_cls, sizeof(status_cls), "scribe-badge scribe-status-%s",
		         it->status[0] ? it->status : "pending");

		bud_append(td_id, bud_text(it->entry_id));
		if (it->book_ref[0])
			bud_append(td_bp, bud_textf("%s p.%u", it->book_ref, it->page_num));
		else
			bud_append(td_bp, bud_textf("p.%u", it->page_num));
		bud_append(td_tpl, bud_text(it->template_id));
		append_text_el(td_st, "span", status_cls, it->status);
		if (it->flagged_count > 0) {
			bud_node *fb = el_class("span", "scribe-badge scribe-flag-conflict");
			bud_append(fb, bud_textf("%u flagged", it->flagged_count));
			bud_append(td_fl, fb);
		} else {
			append_text_el(td_fl, "span", "scribe-muted", "0");
		}
		bud_append(td_sum, bud_text(it->summary[0] ? it->summary : "—"));
		bud_append(td_act, bud_link(href, "Verify", "scribe-btn scribe-btn-sm"));

		bud_append(tr, td_id);
		bud_append(tr, td_bp);
		bud_append(tr, td_tpl);
		bud_append(tr, td_st);
		bud_append(tr, td_fl);
		bud_append(tr, td_sum);
		bud_append(tr, td_act);
		bud_append(tbody, tr);
	}

	bud_append(tbl, tbody);
	bud_append(wrap, tbl);
	return wrap;
}

static bud_node *
render_queue_view(const scribe_ux_state_t *st)
{
	bud_node *sec = el_class("section", "scribe-queue-view");
	bud_node *ingest_card = el_class("div", "scribe-card");
	bud_node *form = el_class("form", "scribe-ingest-form");
	bud_node *row = el_class("div", "scribe-form-row");
	bud_node *f_file = el_class("input", "scribe-input");
	bud_node *f_book = el_class("input", "scribe-input");
	bud_node *f_page = el_class("input", "scribe-input scribe-input-sm");
	bud_node *lbl_ext = el_class("label", "scribe-check");
	bud_node *cb_ext = bud_element_lit("input");
	bud_node *filter_card = el_class("div", "scribe-card");
	bud_node *fform = el_class("form", "scribe-filter-form");
	bud_node *sel_st = el_class("select", "scribe-select");
	bud_node *in_book = el_class("input", "scribe-input");
	static const char *const statuses[] = {
		"", "needs_review", "extracted", "pending", "verified"
	};
	static const char *const status_labels[] = {
		"All statuses", "Needs review", "Extracted", "Pending", "Verified"
	};
	char src_ph[80];
	char flt_ph[80];
	size_t i;

	/* Upload & Ingest form (No-JS native multipart/form-data) */
	append_text_el(ingest_card, "h2", NULL, "Ingest Document Scan");
	bud_set_attr_lit(form, "method", "POST");
	bud_set_attr_lit(form, "action", "/ingest");
	bud_set_attr_lit(form, "enctype", "multipart/form-data");

	bud_set_attr_lit(f_file, "type", "file");
	bud_set_attr_lit(f_file, "name", "file");
	bud_set_attr_lit(f_file, "accept", "image/*,application/pdf");
	bud_set_bool_attr_lit(f_file, "required");

	snprintf(src_ph, sizeof(src_ph), "%s (optional)", ux_source_label(st));
	bud_set_attr_lit(f_book, "type", "text");
	bud_set_attr_lit(f_book, "name", "book_ref");
	bud_set_attr_name_lit(f_book, "placeholder", src_ph);

	bud_set_attr_lit(f_page, "type", "number");
	bud_set_attr_lit(f_page, "name", "page_num");
	bud_set_attr_lit(f_page, "value", "1");
	bud_set_attr_lit(f_page, "min", "1");

	bud_set_attr_lit(cb_ext, "type", "checkbox");
	bud_set_attr_lit(cb_ext, "name", "extract");
	bud_set_attr_lit(cb_ext, "value", "1");
	bud_set_bool_attr_lit(cb_ext, "checked");
	bud_append(lbl_ext, cb_ext);
	bud_append(lbl_ext, bud_text_lit(" Auto-extract"));

	bud_append(row, f_file);
	bud_append(row, render_template_select(st, "template_id",
	                                       st->filter_template, 0));
	bud_append(row, f_book);
	bud_append(row, f_page);
	bud_append(row, lbl_ext);
	bud_append(row, bud_submit_btn("Upload & Ingest", "scribe-btn scribe-btn-primary"));
	bud_append(form, row);
	bud_append(ingest_card, form);
	bud_append(sec, ingest_card);

	/* Filter bar + Queue Table */
	append_text_el(filter_card, "h2", NULL, "Verification Queue");
	bud_set_attr_lit(fform, "method", "GET");
	bud_set_attr_lit(fform, "action", "/queue");

	bud_set_attr_lit(sel_st, "name", "status");
	for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
		bud_node *opt = bud_element_lit("option");
		bud_set_attr_name_lit(opt, "value", statuses[i]);
		if (strcmp(st->filter_status, statuses[i]) == 0)
			bud_set_bool_attr_lit(opt, "selected");
		bud_append(opt, bud_text_lit(status_labels[i]));
		bud_append(sel_st, opt);
	}

	snprintf(flt_ph, sizeof(flt_ph), "Filter by %s", ux_source_label(st));
	bud_set_attr_lit(in_book, "type", "text");
	bud_set_attr_lit(in_book, "name", "book_ref");
	bud_set_attr_name_lit(in_book, "placeholder", flt_ph);
	if (st->filter_book[0])
		bud_set_attr_name_lit(in_book, "value", st->filter_book);

	bud_append(fform, sel_st);
	bud_append(fform, render_template_select(st, "template_id",
	                                         st->filter_template, 1));
	bud_append(fform, in_book);
	bud_append(fform, bud_submit_btn("Filter", "scribe-btn"));
	bud_append(filter_card, fform);
	bud_append(filter_card,
	           render_queue_table(st, "No entries in queue. Upload a scan above to begin."));
	bud_append(sec, filter_card);

	return sec;
}

static bud_node *
render_action_btn(const char *label, int action_id, const char *title)
{
	bud_node *btn = el_class("button", "scribe-btn scribe-btn-sm");
	bud_set_attr_lit(btn, "type", "button");
	bud_set_attr_fmt(btn, "data-action", "%d", action_id);
	if (title)
		bud_set_attr_name_lit(btn, "title", title);
	bud_append(btn, bud_text(label));
	return btn;
}

static bud_node *
render_entry_view(const scribe_ux_state_t *st)
{
	bud_node *grid = el_class("div", "scribe-workbench");
	bud_node *left = el_class("section", "scribe-scan-pane");
	bud_node *toolbar = el_class("div", "scribe-toolbar");
	bud_node *viewport = el_class("div", "scribe-scan-viewport");
	bud_node *stage = el_class("div", "scribe-scan-stage");
	bud_node *img = el_class("img", "scribe-scan-img");
	bud_node *page_actions = el_class("div", "scribe-page-actions");
	bud_node *add_form = el_class("form", "scribe-inline-form");
	bud_node *ext_form = el_class("form", "scribe-inline-form");
	bud_node *deep_form = el_class("form", "scribe-inline-form");
	bud_node *right = el_class("section", "scribe-form-pane");
	bud_node *meta = el_class("div", "scribe-meta-bar");
	bud_node *vform = el_class("form", "scribe-verify-form");
	bud_node *notes_wrap = el_class("div", "scribe-field");
	bud_node *notes_ta = el_class("textarea", "scribe-textarea");
	bud_node *rev_wrap = el_class("div", "scribe-field");
	bud_node *rev_in = el_class("input", "scribe-input");
	bud_node *submit_bar = el_class("div", "scribe-submit-bar");
	bud_node *btn_verify = el_class("button", "scribe-btn scribe-btn-primary");
	bud_node *btn_save = el_class("button", "scribe-btn");
	char url_buf[128];
	char cls_buf[160];
	size_t i;

	/* Left Pane: Scan Viewer + Bounding Box + Multi-Entry Controls */
	bud_append(toolbar, render_action_btn("-", SCRIBE_UX_ACT_ZOOM_OUT, "Zoom out"));
	{
		bud_node *zr = el_class("span", "scribe-badge scribe-zoom-readout");
		bud_append(zr, bud_textf("%d%% | %d deg", st->zoom_pct, st->rotate_deg));
		bud_append(toolbar, zr);
	}
	bud_append(toolbar, render_action_btn("+", SCRIBE_UX_ACT_ZOOM_IN, "Zoom in"));
	bud_append(toolbar, render_action_btn("Reset", SCRIBE_UX_ACT_ZOOM_RESET, "Reset viewport"));
	bud_append(toolbar, render_action_btn("Rot -90", SCRIBE_UX_ACT_ROTATE_CCW, "Rotate counter-clockwise"));
	bud_append(toolbar, render_action_btn("Rot +90", SCRIBE_UX_ACT_ROTATE_CW, "Rotate clockwise"));
	bud_append(toolbar, render_action_btn(
		st->high_contrast ? "Contrast: ON" : "Contrast",
		SCRIBE_UX_ACT_TOGGLE_CONTRAST,
		"Toggle non-destructive high-contrast view"));
	bud_append(toolbar, render_action_btn("Next Flagged", SCRIBE_UX_ACT_NEXT_FLAGGED, "Jump to next flagged field"));
	bud_append(left, toolbar);

	bud_set_attr_fmt(stage, "style",
	                 "transform: translate(%dpx, %dpx) scale(%d.%02d) rotate(%ddeg);",
	                 st->pan_x, st->pan_y,
	                 st->zoom_pct / 100, st->zoom_pct % 100,
	                 st->rotate_deg);
	bud_set_attr_name_lit(img, "src", st->image_url);
	bud_set_attr_fmt(img, "alt", "Document scan %s page %u",
	                 st->book_ref[0] ? st->book_ref : st->page_id, st->page_num);
	if (st->high_contrast)
		bud_set_attr_lit(img, "style", "filter: contrast(1.45) brightness(1.06);");
	bud_append(stage, img);

	/* Bounding-box overlay for active field if bbox present */
	if (st->active_field >= 0 && (size_t)st->active_field < st->field_count) {
		const scribe_ux_field_t *af = &st->fields[st->active_field];
		if (af->bbox[2] > 0.0f && af->bbox[3] > 0.0f) {
			bud_node *box = el_class("div", "scribe-bbox");
			int lx = (int)(af->bbox[0] * 1000.0f);
			int ty = (int)(af->bbox[1] * 1000.0f);
			int ww = (int)(af->bbox[2] * 1000.0f);
			int hh = (int)(af->bbox[3] * 1000.0f);
			bud_set_attr_fmt(box, "style",
			                 "left:%d.%d%%;top:%d.%d%%;width:%d.%d%%;height:%d.%d%%;",
			                 lx / 10, lx % 10, ty / 10, ty % 10,
			                 ww / 10, ww % 10, hh / 10, hh % 10);
			bud_append(stage, box);
		}
	}
	bud_append(viewport, stage);
	bud_append(left, viewport);

	/* Page-level actions: split new entry, re-extract, deep mode, printable transcript */
	snprintf(url_buf, sizeof(url_buf), "/pages/%s/entries", st->page_id);
	bud_set_attr_lit(add_form, "method", "POST");
	bud_set_attr_name_lit(add_form, "action", url_buf);
	bud_append(add_form, render_template_select(st, "template_id", st->template_id, 0));
	bud_append(add_form, bud_submit_btn("+ Add Entry on Page", "scribe-btn scribe-btn-sm"));
	bud_append(page_actions, add_form);

	snprintf(url_buf, sizeof(url_buf), "/entries/%s/extract", st->entry_id);
	bud_set_attr_lit(ext_form, "method", "POST");
	bud_set_attr_name_lit(ext_form, "action", url_buf);
	bud_append(ext_form, bud_submit_btn("Re-run Extraction", "scribe-btn scribe-btn-sm"));
	bud_append(page_actions, ext_form);

	snprintf(url_buf, sizeof(url_buf), "/entries/%s/extract?deep=1", st->entry_id);
	bud_set_attr_lit(deep_form, "method", "POST");
	bud_set_attr_name_lit(deep_form, "action", url_buf);
	bud_append(deep_form, bud_submit_btn(ux_deep_label(st), "scribe-btn scribe-btn-sm"));
	bud_append(page_actions, deep_form);

	snprintf(url_buf, sizeof(url_buf), "/entries/%s/transcript", st->entry_id);
	bud_append(page_actions, bud_link(url_buf, "Printable Transcript", "scribe-btn scribe-btn-sm"));
	bud_append(left, page_actions);

	/* Right Pane: Metadata + Transcript + Schema-Driven Verification Form */
	{
		bud_node *title = bud_element_lit("h2");
		bud_append(title, bud_textf("%s (%s)",
		                            st->template_label[0] ? st->template_label : st->template_id,
		                            st->entry_id));
		bud_append(right, title);
	}

	snprintf(cls_buf, sizeof(cls_buf), "scribe-badge scribe-status-%s",
	         st->status[0] ? st->status : "pending");
	append_text_el(meta, "span", cls_buf, st->status);
	if (st->book_ref[0]) {
		bud_node *bp = el_class("span", "scribe-meta-item");
		bud_append(bp, bud_textf("%s: %s p.%u", ux_source_label(st), st->book_ref, st->page_num));
		bud_append(meta, bp);
	}
	if (st->extractor_model[0]) {
		bud_node *em = el_class("span", "scribe-meta-item");
		bud_append(em, bud_textf("Model: %s", st->extractor_model));
		bud_append(meta, em);
	}
	if (st->reviewed_by[0]) {
		bud_node *rb = el_class("span", "scribe-meta-item");
		bud_append(rb, bud_textf("Reviewed by: %s", st->reviewed_by));
		bud_append(meta, rb);
	}
	bud_append(right, meta);

	if (st->transcript[0]) {
		bud_node *det = el_class("details", "scribe-transcript-box");
		bud_node *sum = bud_element_lit("summary");
		bud_node *pre = bud_element_lit("pre");
		bud_append(sum, bud_text(ux_transcript_label(st)));
		bud_append(pre, bud_text(st->transcript));
		bud_append(det, sum);
		bud_append(det, pre);
		bud_append(right, det);
	}

	snprintf(url_buf, sizeof(url_buf), "/entries/%s/verify", st->entry_id);
	bud_set_attr_lit(vform, "method", "POST");
	bud_set_attr_name_lit(vform, "action", url_buf);

	for (i = 0; i < st->field_count; i++) {
		const scribe_ux_field_t *f = &st->fields[i];
		bud_node *row = bud_element_lit("div");
		bud_node *head = el_class("div", "scribe-field-head");
		bud_node *lbl = el_class("label", "scribe-label");
		bud_node *flags_box = el_class("span", "scribe-flags");
		bud_node *input;
		bud_node *illeg_lbl = el_class("label", "scribe-check scribe-illeg-toggle");
		bud_node *illeg_cb = bud_element_lit("input");
		char in_name[96];
		char cb_name[96];
		char in_id[96];
		const char *display_val;
		size_t s;

		copy_str(cls_buf, sizeof(cls_buf), "scribe-field");
		if (f->flags & SCRIBE_UX_FLAG_CONFLICT)
			strncat(cls_buf, " is-conflict", sizeof(cls_buf) - strlen(cls_buf) - 1);
		if (f->flags & SCRIBE_UX_FLAG_ILLEGIBLE)
			strncat(cls_buf, " is-illegible", sizeof(cls_buf) - strlen(cls_buf) - 1);
		if (f->flags & SCRIBE_UX_FLAG_INVALID)
			strncat(cls_buf, " is-invalid", sizeof(cls_buf) - strlen(cls_buf) - 1);
		if (f->flags & SCRIBE_UX_FLAG_EDITED)
			strncat(cls_buf, " is-edited", sizeof(cls_buf) - strlen(cls_buf) - 1);
		if ((int)i == st->active_field)
			strncat(cls_buf, " is-active", sizeof(cls_buf) - strlen(cls_buf) - 1);
		bud_set_attr_name_lit(row, "class", cls_buf);
		bud_set_attr_fmt(row, "data-field-index", "%u", (unsigned)i);

		snprintf(in_id, sizeof(in_id), "field_%s", f->key);
		snprintf(in_name, sizeof(in_name), "field_%s", f->key);
		snprintf(cb_name, sizeof(cb_name), "illegible_%s", f->key);

		bud_set_attr_name_lit(lbl, "for", in_id);
		bud_append(lbl, bud_text(f->label[0] ? f->label : f->key));
		if (f->required)
			append_text_el(lbl, "span", "scribe-req", " *");
		bud_append(head, lbl);

		if (f->flags & SCRIBE_UX_FLAG_CONFLICT)
			append_text_el(flags_box, "span", "scribe-badge scribe-flag-conflict", "CONFLICT");
		if (f->flags & SCRIBE_UX_FLAG_ILLEGIBLE)
			append_text_el(flags_box, "span", "scribe-badge scribe-flag-illegible", "ILLEGIBLE");
		if (f->flags & SCRIBE_UX_FLAG_INVALID)
			append_text_el(flags_box, "span", "scribe-badge scribe-flag-invalid", "INVALID");
		if (f->flags & SCRIBE_UX_FLAG_EDITED)
			append_text_el(flags_box, "span", "scribe-badge scribe-flag-edited", "EDITED");
		bud_append(head, flags_box);
		bud_append(row, head);

		/* Verbatim raw OCR display (always preserved alongside human edit) */
		{
			bud_node *raw_el = el_class("div", "scribe-raw-value");
			bud_append(raw_el, bud_textf("Raw OCR: %s",
			                             f->raw_value[0] ? f->raw_value : "(empty / illegible)"));
			bud_append(row, raw_el);
		}
		if (f->alt_value[0]) {
			bud_node *alt_el = el_class("div", "scribe-alt-value");
			bud_append(alt_el, bud_textf("Pass 2 / Alt: %s", f->alt_value));
			bud_append(row, alt_el);
		}

		display_val = f->verified_value[0] ? f->verified_value : f->raw_value;
		if (strcmp(f->type, "notes") == 0) {
			input = el_class("textarea", "scribe-textarea");
			bud_set_attr_name_lit(input, "id", in_id);
			bud_set_attr_name_lit(input, "name", in_name);
			bud_set_attr_lit(input, "rows", "2");
			if (display_val[0])
				bud_append(input, bud_text(display_val));
		} else {
			input = el_class("input", "scribe-input");
			bud_set_attr_lit(input, "type", "text");
			bud_set_attr_name_lit(input, "id", in_id);
			bud_set_attr_name_lit(input, "name", in_name);
			bud_set_attr_name_lit(input, "value", display_val);
			if (f->format_hint[0])
				bud_set_attr_name_lit(input, "placeholder", f->format_hint);
			else if (strcmp(f->type, "date") == 0)
				bud_set_attr_lit(input, "placeholder", "YYYY-MM-DD");
		}
		bud_append(row, input);

		bud_set_attr_lit(illeg_cb, "type", "checkbox");
		bud_set_attr_name_lit(illeg_cb, "name", cb_name);
		bud_set_attr_lit(illeg_cb, "value", "1");
		if (f->flags & SCRIBE_UX_FLAG_ILLEGIBLE)
			bud_set_bool_attr_lit(illeg_cb, "checked");
		bud_append(illeg_lbl, illeg_cb);
		bud_append(illeg_lbl, bud_text_lit(" Mark illegible"));
		bud_append(row, illeg_lbl);

		/* Read-only advisory dictionary suggestions */
		if (f->suggestion_count > 0) {
			bud_node *sugs = el_class("div", "scribe-suggestions");
			append_text_el(sugs, "span", "scribe-muted", "Advisory: ");
			for (s = 0; s < f->suggestion_count; s++) {
				bud_node *chip = el_class("span", "scribe-chip");
				bud_set_attr_name_lit(chip, "data-suggest-key", f->key);
				bud_set_attr_name_lit(chip, "data-suggest-val", f->suggestions[s]);
				bud_append(chip, bud_text(f->suggestions[s]));
				bud_append(sugs, chip);
			}
			bud_append(row, sugs);
		}

		bud_append(vform, row);
	}

	/* Notes / corrections */
	append_text_el(notes_wrap, "label", "scribe-label", ux_notes_label(st));
	bud_set_attr_lit(notes_ta, "name", "marginal_notes");
	bud_set_attr_lit(notes_ta, "rows", "3");
	if (st->marginal_notes[0])
		bud_append(notes_ta, bud_text(st->marginal_notes));
	bud_append(notes_wrap, notes_ta);
	bud_append(vform, notes_wrap);

	/* Reviewer tag (auto-populated from REMOTE_USER when authenticated) */
	append_text_el(rev_wrap, "label", "scribe-label", "Reviewer");
	bud_set_attr_lit(rev_in, "type", "text");
	bud_set_attr_lit(rev_in, "name", "reviewed_by");
	bud_set_attr_lit(rev_in, "placeholder", "Reviewer name or initials");
	if (st->reviewed_by[0])
		bud_set_attr_name_lit(rev_in, "value", st->reviewed_by);
	bud_append(rev_wrap, rev_in);
	bud_append(vform, rev_wrap);

	bud_set_attr_lit(btn_verify, "type", "submit");
	bud_set_attr_lit(btn_verify, "name", "action");
	bud_set_attr_lit(btn_verify, "value", "verify");
	bud_append(btn_verify, bud_text_lit("Confirm & Verify"));

	bud_set_attr_lit(btn_save, "type", "submit");
	bud_set_attr_lit(btn_save, "name", "action");
	bud_set_attr_lit(btn_save, "value", "save");
	bud_append(btn_save, bud_text_lit("Save Draft"));

	bud_append(submit_bar, btn_verify);
	bud_append(submit_bar, btn_save);
	bud_append(submit_bar, bud_link("/queue", "Back to Queue", "scribe-btn"));
	bud_append(vform, submit_bar);

	bud_append(right, vform);
	bud_append(grid, left);
	bud_append(grid, right);
	return grid;
}

static bud_node *
render_search_view(const scribe_ux_state_t *st)
{
	bud_node *card = el_class("section", "scribe-card scribe-search-view");
	bud_node *form = el_class("form", "scribe-search-form");
	bud_node *in_q = el_class("input", "scribe-input");
	bud_node *lbl_fold = el_class("label", "scribe-check");
	bud_node *cb_fold = bud_element_lit("input");

	append_text_el(card, "h2", NULL, "Search Verified & Extracted Records");
	bud_set_attr_lit(form, "method", "GET");
	bud_set_attr_lit(form, "action", "/search");

	bud_set_attr_lit(in_q, "type", "search");
	bud_set_attr_lit(in_q, "name", "q");
	bud_set_attr_lit(in_q, "placeholder", "Search names, places, dates (accent-sensitive by default)...");
	if (st->search_query[0])
		bud_set_attr_name_lit(in_q, "value", st->search_query);

	bud_set_attr_lit(cb_fold, "type", "checkbox");
	bud_set_attr_lit(cb_fold, "name", "folded");
	bud_set_attr_lit(cb_fold, "value", "1");
	if (st->search_folded)
		bud_set_bool_attr_lit(cb_fold, "checked");
	bud_append(lbl_fold, cb_fold);
	bud_append(lbl_fold, bud_text_lit(" Fold accents (ASCII)"));

	bud_append(form, in_q);
	bud_append(form, lbl_fold);
	bud_append(form, bud_submit_btn("Search", "scribe-btn scribe-btn-primary"));
	bud_append(card, form);

	if (st->search_query[0]) {
		bud_append(card, render_queue_table(st, "No matching entries found."));
	} else {
		append_text_el(card, "p", "scribe-muted",
		               "Enter a query to search via liblemma full-text index.");
	}
	return card;
}

bud_node *
scribe_ux_render(const scribe_ux_state_t *st)
{
	bud_node *root = el_class("div", "scribe-app");
	bud_node *main_el = el_class("main", "scribe-main");

	if (!st || !root || !main_el) {
		bud_free(root);
		bud_free(main_el);
		return NULL;
	}

	bud_append(root, render_nav(st));
	if (st->flash[0])
		append_text_el(main_el, "div", "scribe-flash", st->flash);

	switch (st->view) {
	case SCRIBE_UX_VIEW_ENTRY:
		bud_append(main_el, render_entry_view(st));
		break;
	case SCRIBE_UX_VIEW_SEARCH:
		bud_append(main_el, render_search_view(st));
		break;
	case SCRIBE_UX_VIEW_QUEUE:
	default:
		bud_append(main_el, render_queue_view(st));
		break;
	}

	bud_append(root, main_el);
	return root;
}

/* ── Isomorphic WASM Module Registration ───────────────────────── */

static scribe_ux_state_t g_ux_state;

static int
scribe_stl_init(const char *json, int len)
{
	size_t jlen = (len > 0) ? (size_t)len : 0;
	return scribe_ux_state_from_json(&g_ux_state, json, jlen);
}

static bud_node *
scribe_stl_render(void)
{
	return scribe_ux_render(&g_ux_state);
}

static void
scribe_stl_action(int action_id, const char *data)
{
	if (scribe_ux_apply_action(&g_ux_state, action_id, data) == 0)
		stl_slot_invalidate(STL_SLOT_PAGE);
}

#if defined(__wasm__)
static bud_node *g_wasm_root = NULL;
static bud_arena *g_wasm_arena = NULL;

static int
scribe_noop_emit(void *user, const char *op, const char *a,
                 const char *b, const char *c)
{
	(void)user;
	(void)op;
	(void)a;
	(void)b;
	(void)c;
	return 0;
}

static int
scribe_patch_emit(void *user, const char *op, const char *a,
                  const char *b, const char *c)
{
	int slot = (int)(intptr_t)user;
	stl_host_emit_patch(slot,
	                    op, op ? strlen(op) : 0,
	                    a, a ? strlen(a) : 0,
	                    b, b ? strlen(b) : 0,
	                    c, c ? strlen(c) : 0);
	return 0;
}

static int
scribe_stl_mount(int slot)
{
	bud_arena *arena;
	bud_arena *prev;
	bud_node *root;

	(void)slot;
	if (g_wasm_root) {
		bud_free(g_wasm_root);
		g_wasm_root = NULL;
	}
	if (g_wasm_arena) {
		bud_arena_free(g_wasm_arena);
		g_wasm_arena = NULL;
	}
	arena = bud_arena_new(8192);
	if (!arena)
		return -1;
	prev = bud_arena_push(arena);
	root = scribe_stl_render();
	bud_arena_pop(prev);
	if (!root) {
		bud_arena_free(arena);
		return -1;
	}
	bud_render_ops(root, scribe_noop_emit, NULL);
	g_wasm_root = root;
	g_wasm_arena = arena;
	return 0;
}

static int
scribe_stl_update(int slot)
{
	bud_arena *arena = bud_arena_new(8192);
	bud_arena *prev;
	bud_node *next_root;

	if (!arena)
		return -1;
	prev = bud_arena_push(arena);
	next_root = scribe_stl_render();
	bud_arena_pop(prev);
	if (!next_root) {
		bud_arena_free(arena);
		return -1;
	}
	if (g_wasm_root) {
		bud_vdom_diff(g_wasm_root, next_root, scribe_patch_emit,
		              (void *)(intptr_t)slot);
		bud_free(g_wasm_root);
		bud_arena_free(g_wasm_arena);
	} else {
		bud_render_ops(next_root, scribe_patch_emit,
		               (void *)(intptr_t)slot);
	}
	g_wasm_root = next_root;
	g_wasm_arena = arena;
	return 0;
}

const stl_module_ops_t stl_mod_scribe_ux = {
	.name = "scribe_ux",
	.init = scribe_stl_init,
	.init_bin = NULL,
	.mount = scribe_stl_mount,
	.update = scribe_stl_update,
	.action = scribe_stl_action,
	.dispatch_event = NULL,
	.tick = NULL,
	.cleanup = NULL,
	.fetch_callback = NULL,
	.render = (void *(*)(void))scribe_stl_render
};

const stl_module_ops_t *g_stl_modules[] = {
	&stl_mod_scribe_ux,
	NULL
};

BUD_APP_ROUTE(scribe_stl_render)

ISO_EXPORT(int, wasm_init, const char *json, int len)
{
	stl_kernel_register_module(&stl_mod_scribe_ux);
	return stl_kernel_mount(STL_SLOT_PAGE, "scribe_ux", json, len);
}
#else
STL_WASM_MODULE(scribe_ux, scribe_stl_init, scribe_stl_render, scribe_stl_action);
#endif
