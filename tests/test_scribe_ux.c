#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bud/bud.h>
#include "ttypt/scribe-ux.h"

static void
test_queue_render_and_escape(void)
{
	scribe_ux_state_t st;
	bud_node *root;
	char *html;

	scribe_ux_state_init(&st);
	st.view = SCRIBE_UX_VIEW_QUEUE;
	snprintf(st.templates[0].id, sizeof(st.templates[0].id), "confirmation_starter");
	snprintf(st.templates[0].label, sizeof(st.templates[0].label), "Confirmation Register");
	st.template_count = 1;

	snprintf(st.items[0].entry_id, sizeof(st.items[0].entry_id), "e000001");
	snprintf(st.items[0].page_id, sizeof(st.items[0].page_id), "p000001");
	snprintf(st.items[0].book_ref, sizeof(st.items[0].book_ref), "Livro-1954");
	st.items[0].page_num = 12;
	snprintf(st.items[0].template_id, sizeof(st.items[0].template_id), "confirmation_starter");
	snprintf(st.items[0].status, sizeof(st.items[0].status), "needs_review");
	st.items[0].flagged_count = 2;
	snprintf(st.items[0].summary, sizeof(st.items[0].summary), "Joao <script>alert(1)</script>");
	st.item_count = 1;

	root = scribe_ux_render(&st);
	assert(root != NULL);
	html = bud_render_html(root);
	assert(html != NULL);
	assert(strstr(html, "action=\"/ingest\"") != NULL);
	assert(strstr(html, "enctype=\"multipart/form-data\"") != NULL);
	assert(strstr(html, "Livro-1954 p.12") != NULL);
	assert(strstr(html, "2 flagged") != NULL);
	assert(strstr(html, "/entries/e000001") != NULL);
	/* Must HTML-escape untrusted OCR summary text */
	assert(strstr(html, "<script>alert(1)</script>") == NULL);
	assert(strstr(html, "&lt;script&gt;") != NULL);

	bud_free_string(html);
	bud_free(root);
	printf("PASS: test_queue_render_and_escape\n");
}

static void
test_entry_workbench_and_actions(void)
{
	scribe_ux_state_t st;
	bud_node *root;
	char *html;

	scribe_ux_state_init(&st);
	st.view = SCRIBE_UX_VIEW_ENTRY;
	snprintf(st.entry_id, sizeof(st.entry_id), "e000007");
	snprintf(st.page_id, sizeof(st.page_id), "p000003");
	snprintf(st.book_ref, sizeof(st.book_ref), "Book-IV");
	st.page_num = 4;
	snprintf(st.template_id, sizeof(st.template_id), "confirmation_starter");
	snprintf(st.template_label, sizeof(st.template_label), "Confirmation");
	snprintf(st.status, sizeof(st.status), "needs_review");
	snprintf(st.image_url, sizeof(st.image_url), "/pages/p000003/image");

	snprintf(st.fields[0].key, sizeof(st.fields[0].key), "confirmand_name");
	snprintf(st.fields[0].label, sizeof(st.fields[0].label), "Confirmand Name");
	snprintf(st.fields[0].type, sizeof(st.fields[0].type), "person");
	snprintf(st.fields[0].format_hint, sizeof(st.fields[0].format_hint), "e.g. Maria Silva");
	st.fields[0].required = 1;
	snprintf(st.fields[0].raw_value, sizeof(st.fields[0].raw_value), "Maria Silva");
	st.fields[0].flags = SCRIBE_UX_FLAG_NONE;

	snprintf(st.fields[1].key, sizeof(st.fields[1].key), "sponsor_name");
	snprintf(st.fields[1].label, sizeof(st.fields[1].label), "Sponsor Name");
	snprintf(st.fields[1].type, sizeof(st.fields[1].type), "person");
	snprintf(st.fields[1].raw_value, sizeof(st.fields[1].raw_value), "Joao Pereyra");
	snprintf(st.fields[1].alt_value, sizeof(st.fields[1].alt_value), "João Pereira");
	st.fields[1].flags = SCRIBE_UX_FLAG_CONFLICT;
	st.fields[1].bbox[0] = 0.10f;
	st.fields[1].bbox[1] = 0.25f;
	st.fields[1].bbox[2] = 0.40f;
	st.fields[1].bbox[3] = 0.08f;
	snprintf(st.fields[1].suggestions[0], sizeof(st.fields[1].suggestions[0]), "João Pereira");
	st.fields[1].suggestion_count = 1;
	snprintf(st.transcript, sizeof(st.transcript), "Aos vinte e quatro dias...");

	snprintf(st.fields[2].key, sizeof(st.fields[2].key), "confirmation_date");
	snprintf(st.fields[2].label, sizeof(st.fields[2].label), "Confirmation Date");
	snprintf(st.fields[2].type, sizeof(st.fields[2].type), "date");
	st.fields[2].flags = SCRIBE_UX_FLAG_INVALID;
	st.field_count = 3;
	st.active_field = 1;

	/* Test viewport and field navigation actions */
	assert(scribe_ux_apply_action(&st, SCRIBE_UX_ACT_ZOOM_IN, NULL) == 0);
	assert(st.zoom_pct == 125);
	assert(scribe_ux_apply_action(&st, SCRIBE_UX_ACT_ROTATE_CW, NULL) == 0);
	assert(st.rotate_deg == 90);
	assert(scribe_ux_apply_action(&st, SCRIBE_UX_ACT_PAN_RIGHT, NULL) == 0);
	assert(st.pan_x == 40);
	assert(scribe_ux_apply_action(&st, SCRIBE_UX_ACT_TOGGLE_CONTRAST, NULL) == 0);
	assert(st.high_contrast == 1);
	assert(scribe_ux_apply_action(&st, SCRIBE_UX_ACT_NEXT_FLAGGED, NULL) == 0);
	assert(st.active_field == 2);
	assert(scribe_ux_apply_action(&st, SCRIBE_UX_ACT_NEXT_FLAGGED, NULL) == 0);
	assert(st.active_field == 1);
	assert(scribe_ux_apply_action(&st, SCRIBE_UX_ACT_TOGGLE_ILLEG, NULL) == 0);
	assert((st.fields[1].flags & SCRIBE_UX_FLAG_ILLEGIBLE) != 0);

	/* 1. Default (generic) UX vocabulary when no pack UI labels are set */
	root = scribe_ux_render(&st);
	assert(root != NULL);
	html = bud_render_html(root);
	assert(html != NULL);
	assert(strstr(html, "scribe-workbench") != NULL);
	assert(strstr(html, "action=\"/entries/e000007/verify\"") != NULL);
	assert(strstr(html, "action=\"/entries/e000007/extract?deep=1\"") != NULL);
	assert(strstr(html, "Deep Re-Extract") != NULL);
	assert(strstr(html, "Full Transcript") != NULL);
	assert(strstr(html, "Source Ref: Book-IV p.4") != NULL);
	assert(strstr(html, "Notes / Corrections") != NULL);
	assert(strstr(html, "placeholder=\"e.g. Maria Silva\"") != NULL);
	assert(strstr(html, "action=\"/pages/p000003/entries\"") != NULL);
	assert(strstr(html, "src=\"/pages/p000003/image\"") != NULL);
	assert(strstr(html, "filter: contrast(1.45)") != NULL);
	assert(strstr(html, "Contrast: ON") != NULL);
	assert(strstr(html, "scale(1.25) rotate(90deg)") != NULL);
	assert(strstr(html, "scribe-bbox") != NULL);
	assert(strstr(html, "Aos vinte e quatro dias...") != NULL);
	assert(strstr(html, "CONFLICT") != NULL);
	assert(strstr(html, "ILLEGIBLE") != NULL);
	assert(strstr(html, "INVALID") != NULL);
	assert(strstr(html, "Raw OCR: Joao Pereyra") != NULL);
	assert(strstr(html, "Pass 2 / Alt: João Pereira") != NULL);
	assert(strstr(html, "João Pereira") != NULL);
	bud_free_string(html);
	bud_free(root);

	/* 2. Pack-overridden archival vocabulary */
	snprintf(st.pack_label, sizeof(st.pack_label), "Portuguese Archival Handwriting");
	snprintf(st.ui_source_label, sizeof(st.ui_source_label), "Book / Folio");
	snprintf(st.ui_notes_label, sizeof(st.ui_notes_label), "Marginal Notes / Averbamentos");
	snprintf(st.ui_transcript_label, sizeof(st.ui_transcript_label), "Diplomatic Handwriting Transcript");
	snprintf(st.ui_deep_label, sizeof(st.ui_deep_label), "Deep Paleography");
	root = scribe_ux_render(&st);
	assert(root != NULL);
	html = bud_render_html(root);
	assert(html != NULL);
	assert(strstr(html, "Portuguese Archival Handwriting") != NULL);
	assert(strstr(html, "Deep Paleography") != NULL);
	assert(strstr(html, "Diplomatic Handwriting Transcript") != NULL);
	assert(strstr(html, "Book / Folio: Book-IV p.4") != NULL);
	assert(strstr(html, "Marginal Notes / Averbamentos") != NULL);
	bud_free_string(html);
	bud_free(root);

	assert(scribe_ux_apply_action(&st, SCRIBE_UX_ACT_ZOOM_RESET, NULL) == 0);
	assert(st.zoom_pct == 100 && st.rotate_deg == 0 && st.pan_x == 0 && st.pan_y == 0);
	printf("PASS: test_entry_workbench_and_actions\n");
}

static void
test_json_hydration_and_search_view(void)
{
	const char *json =
		"{\"view\":\"entry\",\"entry_id\":\"e000009\",\"page_id\":\"p000002\","
		"\"pack_label\":\"Portuguese Archival Handwriting\","
		"\"ui_source_label\":\"Book / Folio\",\"ui_deep_label\":\"Deep Paleography\","
		"\"ui_transcript_label\":\"Diplomatic Handwriting Transcript\","
		"\"ui_notes_label\":\"Marginal Notes / Averbamentos\","
		"\"book_ref\":\"Livro-B\",\"page_num\":7,\"template_id\":\"generic_record\","
		"\"status\":\"needs_review\",\"image_url\":\"/pages/p000002/image\","
		"\"transcript\":\"Linha 1 diplomática\",\"high_contrast\":1,"
		"\"zoom_pct\":150,\"rotate_deg\":90,\"field_count\":2,"
		"\"f0_key\":\"subject\",\"f0_label\":\"Subject\",\"f0_type\":\"person\","
		"\"f0_hint\":\"Full name\",\"f0_req\":1,\"f0_raw\":\"António\",\"f0_alt\":\"Antonio\",\"f0_ver\":\"\",\"f0_flags\":2,"
		"\"f0_bx\":100,\"f0_by\":200,\"f0_bw\":300,\"f0_bh\":50,"
		"\"f1_key\":\"date\",\"f1_label\":\"Date\",\"f1_type\":\"date\","
		"\"f1_req\":0,\"f1_raw\":\"1950-05-01\",\"f1_ver\":\"1950-05-01\",\"f1_flags\":0}";
	scribe_ux_state_t st;
	bud_node *root;
	char *html;

	assert(scribe_ux_state_from_json(&st, json, strlen(json)) == 0);
	assert(st.view == SCRIBE_UX_VIEW_ENTRY);
	assert(strcmp(st.pack_label, "Portuguese Archival Handwriting") == 0);
	assert(strcmp(st.ui_deep_label, "Deep Paleography") == 0);
	assert(strcmp(st.entry_id, "e000009") == 0);
	assert(st.page_num == 7);
	assert(st.zoom_pct == 150);
	assert(st.rotate_deg == 90);
	assert(st.high_contrast == 1);
	assert(strcmp(st.transcript, "Linha 1 diplomática") == 0);
	assert(st.field_count == 2);
	assert(strcmp(st.fields[0].format_hint, "Full name") == 0);
	assert(strcmp(st.fields[0].raw_value, "António") == 0);
	assert(strcmp(st.fields[0].alt_value, "Antonio") == 0);
	assert(st.fields[0].flags == SCRIBE_UX_FLAG_CONFLICT);
	assert(st.active_field == 0);

	/* Switch to search view and render */
	st.view = SCRIBE_UX_VIEW_SEARCH;
	snprintf(st.search_query, sizeof(st.search_query), "António");
	st.search_folded = 1;
	root = scribe_ux_render(&st);
	assert(root != NULL);
	html = bud_render_html(root);
	assert(html != NULL);
	assert(strstr(html, "action=\"/search\"") != NULL);
	assert(strstr(html, "value=\"António\"") != NULL);
	assert(strstr(html, "checked") != NULL);

	bud_free_string(html);
	bud_free(root);
	printf("PASS: test_json_hydration_and_search_view\n");
}

int
main(void)
{
	test_queue_render_and_escape();
	test_entry_workbench_and_actions();
	test_json_hydration_and_search_view();
	printf("ALL SCRIBE UX TESTS PASSED\n");
	return 0;
}
