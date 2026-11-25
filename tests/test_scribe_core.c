#define _POSIX_C_SOURCE 200809L

#include <ttypt/scribe.h>

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void rm_rf(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return;
	if (S_ISDIR(st.st_mode)) {
		DIR *dir = opendir(path);
		if (dir) {
			struct dirent *de;
			while ((de = readdir(dir)) != NULL) {
				if (strcmp(de->d_name, ".") == 0 ||
				    strcmp(de->d_name, "..") == 0)
					continue;
				char child[512];
				snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
				rm_rf(child);
			}
			closedir(dir);
		}
		rmdir(path);
	} else {
		unlink(path);
	}
}

static int id_list_contains(const scribe_id_list_t *list, const char *id)
{
	for (size_t i = 0; i < list->count; i++) {
		if (strcmp(list->ids[i], id) == 0)
			return 1;
	}
	return 0;
}

static void test_sha256_vectors(void)
{
	char hex[65];

	/* NIST FIPS 180-4 empty string */
	scribe_sha256_buf((const uint8_t *)"", 0, hex);
	assert(strcmp(hex,
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

	/* NIST FIPS 180-4 "abc" */
	scribe_sha256_buf((const uint8_t *)"abc", 3, hex);
	assert(strcmp(hex,
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

	/* Multi-block vector (56 bytes -> forces second block) */
	const char *mb = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	scribe_sha256_buf((const uint8_t *)mb, strlen(mb), hex);
	assert(strcmp(hex,
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

	printf("[PASS] test_sha256_vectors\n");
}

static void test_templates_and_hyle_defs(void)
{
	scribe_registry_t *reg = scribe_registry_open();
	assert(reg != NULL);

	int n = scribe_registry_load_dir(reg, "templates");
	assert(n == 4);
	assert(scribe_registry_count(reg) == 4);

	const scribe_template_t *conf = scribe_registry_find(reg, "confirmation_starter");
	assert(conf != NULL);
	assert(conf->version == 1);
	assert(conf->field_count == 10);
	assert(conf->hyle_defs != NULL);
	assert(strcmp(conf->hyle_defs[0].key, "person_name") == 0);
	assert(conf->hyle_defs[0].required == 1);
	assert(conf->hyle_defs[0].writable == 1);
	assert(conf->hyle_defs[0].kind == HYLE_KIND_RECORD);
	assert(conf->hyle_defs[0].search_weight == 3);
	/* Date field gets filter_style == "date" */
	assert(strcmp(conf->hyle_defs[2].key, "confirmation_date") == 0);
	assert(conf->hyle_defs[2].filter_style != NULL);
	assert(strcmp(conf->hyle_defs[2].filter_style, "date") == 0);
	/* Sentinel FIELD_END */
	assert(conf->hyle_defs[conf->field_count].key == NULL);

	const scribe_template_t *wed = scribe_registry_find(reg, "wedding_starter");
	assert(wed != NULL && wed->field_count == 9);
	assert(scribe_registry_find(reg, "baptism_starter") != NULL);
	assert(scribe_registry_find(reg, "generic_record") != NULL);

	/* Reject duplicate keys and invalid field types */
	assert(scribe_template_load_json(
		"{\"id\":\"bad\",\"fields\":[{\"key\":\"a\"},{\"key\":\"a\"}]}") == NULL);
	assert(scribe_template_load_json(
		"{\"id\":\"bad2\",\"fields\":[{\"key\":\"a\",\"type\":\"bogus\"}]}") == NULL);

	scribe_registry_close(reg);
	printf("[PASS] test_templates_and_hyle_defs\n");
}

static void test_image_sniff_and_exif(void)
{
	/* Minimal PNG header: 1200x1600 */
	uint8_t png[32] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
		0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
		0x00, 0x00, 0x04, 0xB0, /* width = 1200 */
		0x00, 0x00, 0x06, 0x40, /* height = 1600 */
		0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	scribe_image_info_t info;
	assert(scribe_sniff_image(png, sizeof(png), &info) == 0);
	assert(strcmp(info.mime, "image/png") == 0);
	assert(strcmp(info.ext, "png") == 0);
	assert(info.width == 1200 && info.height == 1600);
	assert(info.is_pdf == 0);

	/* Minimal JPEG with EXIF orientation = 6 (90 deg CW -> swaps 800x600 to 600x800) */
	uint8_t jpg[] = {
		0xFF, 0xD8,
		/* APP1 Exif */
		0xFF, 0xE1, 0x00, 0x22,
		'E', 'x', 'i', 'f', 0x00, 0x00,
		'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
		0x01, 0x00, /* 1 IFD entry */
		0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		/* SOF0: height=600 (0x0258), width=800 (0x0320) */
		0xFF, 0xC0, 0x00, 0x0B, 0x08,
		0x02, 0x58, 0x03, 0x20, 0x01, 0x01, 0x11, 0x00,
		0xFF, 0xD9
	};
	assert(scribe_sniff_image(jpg, sizeof(jpg), &info) == 0);
	assert(strcmp(info.mime, "image/jpeg") == 0);
	assert(info.exif_orientation == 6);
	assert(info.width == 600 && info.height == 800);

	/* Reject unknown bytes */
	const uint8_t junk[] = "not-an-image-at!";
	assert(scribe_sniff_image(junk, sizeof(junk) - 1, &info) != 0);

	printf("[PASS] test_image_sniff_and_exif\n");
}

static void test_ingest_crud_audit_search_and_persistence(void)
{
	char tmpdir[] = "/tmp/scribe-test-XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	scribe_store_t *store = scribe_store_open(tmpdir);
	assert(store != NULL);

	/* 1. Ingest scan + SHA-256 deduplication */
	uint8_t png[32] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
		0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
		0x00, 0x00, 0x03, 0x20, /* 800 */
		0x00, 0x00, 0x04, 0x00, /* 1024 */
		0x08, 0x02, 0x00, 0x00, 0x00, 0xDE, 0xAD, 0x01
	};
	scribe_page_t p1, p1_dup;
	int dedup = -1;
	assert(scribe_ingest_buffer(store, png, sizeof(png), "Book-1954", 42, &p1, &dedup) == 0);
	assert(dedup == 0);
	assert(p1.width == 800 && p1.height == 1024);
	assert(p1.page_num == 42);
	assert(strcmp(p1.book_ref, "Book-1954") == 0);

	/* Verify immutable scan file exists on disk */
	char abs_scan[512];
	snprintf(abs_scan, sizeof(abs_scan), "%s/%s", tmpdir, p1.file_path);
	struct stat st;
	assert(stat(abs_scan, &st) == 0 && (size_t)st.st_size == sizeof(png));

	/* Second ingest of identical bytes deduplicates */
	assert(scribe_ingest_buffer(store, png, sizeof(png), "Book-1954", 42, &p1_dup, &dedup) == 0);
	assert(dedup == 1);
	assert(strcmp(p1.id, p1_dup.id) == 0);
	assert(strcmp(p1.sha256, p1_dup.sha256) == 0);

	/* 2. 1-page -> N-entries (bound register spread with 2 entries) */
	scribe_entry_t e1;
	scribe_entry_init(&e1);
	snprintf(e1.page_id, sizeof(e1.page_id), "%s", p1.id);
	snprintf(e1.template_id, sizeof(e1.template_id), "confirmation_starter");
	e1.status = SCRIBE_STATUS_NEEDS_REVIEW;
	snprintf(e1.extractor_model, sizeof(e1.extractor_model), "qwen2.5-vl-7b");
	float bb1[4] = { 0.1f, 0.2f, 0.5f, 0.05f };
	assert(scribe_entry_set_field(&e1, "person_name",
		"Maria da Conceicao", "Maria da Conceição",
		SCRIBE_FLAG_CONFLICT | SCRIBE_FLAG_EDITED, bb1) == 0);
	assert(scribe_entry_set_field(&e1, "parish",
		"São João", "São João", SCRIBE_FLAG_NONE, NULL) == 0);
	assert(scribe_entry_set_field(&e1, "sponsor",
		NULL, NULL, SCRIBE_FLAG_ILLEGIBLE, NULL) == 0);
	assert(scribe_entry_set_marginal_notes(&e1, "Casou em 1972 em Braga") == 0);
	assert(scribe_entry_put(store, &e1) == 0);
	assert(e1.id[0] != '\0');

	scribe_entry_t e2;
	scribe_entry_init(&e2);
	snprintf(e2.page_id, sizeof(e2.page_id), "%s", p1.id);
	snprintf(e2.template_id, sizeof(e2.template_id), "confirmation_starter");
	e2.status = SCRIBE_STATUS_VERIFIED;
	snprintf(e2.reviewed_by, sizeof(e2.reviewed_by), "archivist1");
	e2.reviewed_at = 1764000000;
	assert(scribe_entry_set_field(&e2, "person_name",
		"António Ferreira", "António Ferreira", SCRIBE_FLAG_NONE, NULL) == 0);
	assert(scribe_entry_put(store, &e2) == 0);

	/* Verify 1-page -> 2 entries */
	scribe_id_list_t by_page;
	assert(scribe_entry_list_by_page(store, p1.id, &by_page) == 0);
	assert(by_page.count == 2);
	assert(id_list_contains(&by_page, e1.id));
	assert(id_list_contains(&by_page, e2.id));
	scribe_id_list_free(&by_page);

	/* Verify status queue filter */
	scribe_id_list_t need_rev;
	assert(scribe_entry_list_by_status(store, SCRIBE_STATUS_NEEDS_REVIEW, &need_rev) == 0);
	assert(need_rev.count == 1 && strcmp(need_rev.ids[0], e1.id) == 0);
	scribe_id_list_free(&need_rev);

	/* 3. Dual-value audit preservation */
	scribe_entry_t loaded;
	assert(scribe_entry_get(store, e1.id, &loaded) == 0);
	const scribe_field_val_t *fn = scribe_entry_get_field(&loaded, "person_name");
	assert(fn != NULL);
	assert(strcmp(fn->raw_value, "Maria da Conceicao") == 0);
	assert(strcmp(fn->verified_value, "Maria da Conceição") == 0);
	assert(fn->flags == (SCRIBE_FLAG_CONFLICT | SCRIBE_FLAG_EDITED));
	assert(fn->bbox[0] > 0.09f && fn->bbox[0] < 0.11f);
	const scribe_field_val_t *fs = scribe_entry_get_field(&loaded, "sponsor");
	assert(fs != NULL && fs->raw_value == NULL && (fs->flags & SCRIBE_FLAG_ILLEGIBLE));
	assert(loaded.marginal_notes && strcmp(loaded.marginal_notes, "Casou em 1972 em Braga") == 0);
	scribe_entry_free(&loaded);

	/* 4. Accent-sensitive exact search vs optional folded search */
	scribe_id_list_t hits;
	assert(scribe_search(store, "person_name", "Conceição", SCRIBE_SEARCH_EXACT, &hits) == 0);
	assert(hits.count == 1 && strcmp(hits.ids[0], e1.id) == 0);
	scribe_id_list_free(&hits);

	/* Exact mode MUST distinguish Conceição != Conceicao */
	assert(scribe_search(store, "person_name", "Conceicao", SCRIBE_SEARCH_EXACT, &hits) == 0);
	assert(hits.count == 0);
	scribe_id_list_free(&hits);

	/* Folded mode matches accent-insensitively */
	assert(scribe_search(store, "person_name", "Conceicao", SCRIBE_SEARCH_FOLDED, &hits) == 0);
	assert(hits.count == 1 && strcmp(hits.ids[0], e1.id) == 0);
	scribe_id_list_free(&hits);

	/* Search inside marginal_notes and book_ref */
	assert(scribe_search(store, "marginal_notes", "Braga", SCRIBE_SEARCH_EXACT, &hits) == 0);
	assert(hits.count == 1 && strcmp(hits.ids[0], e1.id) == 0);
	scribe_id_list_free(&hits);

	assert(scribe_search(store, "book_ref", "Book-1954", SCRIBE_SEARCH_EXACT, &hits) == 0);
	assert(hits.count == 2);
	scribe_id_list_free(&hits);

	/* 5. Persistence across close + reopen (rebuilds liblemma from libcorm) */
	char saved_e1_id[32], saved_e2_id[32];
	snprintf(saved_e1_id, sizeof(saved_e1_id), "%s", e1.id);
	snprintf(saved_e2_id, sizeof(saved_e2_id), "%s", e2.id);
	scribe_entry_free(&e1);
	scribe_entry_free(&e2);

	scribe_store_close(store);
	store = scribe_store_open(tmpdir);
	assert(store != NULL);

	assert(scribe_search(store, "all", "António", SCRIBE_SEARCH_EXACT, &hits) == 0);
	assert(hits.count == 1 && strcmp(hits.ids[0], saved_e2_id) == 0);
	scribe_id_list_free(&hits);

	/* 6. Update unindexes old token; delete removes entry + page link + tokens */
	scribe_entry_t upd;
	assert(scribe_entry_get(store, saved_e2_id, &upd) == 0);
	assert(scribe_entry_set_field(&upd, "person_name",
		"António Ferreira", "José Pereira", SCRIBE_FLAG_EDITED, NULL) == 0);
	assert(scribe_entry_put(store, &upd) == 0);
	scribe_entry_free(&upd);

	assert(scribe_search(store, "person_name", "António", SCRIBE_SEARCH_EXACT, &hits) == 0);
	assert(hits.count == 0);
	scribe_id_list_free(&hits);

	assert(scribe_search(store, "person_name", "Pereira", SCRIBE_SEARCH_EXACT, &hits) == 0);
	assert(hits.count == 1 && strcmp(hits.ids[0], saved_e2_id) == 0);
	scribe_id_list_free(&hits);

	assert(scribe_entry_del(store, saved_e2_id) == 0);
	assert(scribe_search(store, "person_name", "Pereira", SCRIBE_SEARCH_EXACT, &hits) == 0);
	assert(hits.count == 0);
	scribe_id_list_free(&hits);

	assert(scribe_entry_list_by_page(store, p1.id, &by_page) == 0);
	assert(by_page.count == 1 && strcmp(by_page.ids[0], saved_e1_id) == 0);
	scribe_id_list_free(&by_page);

	scribe_store_close(store);
	rm_rf(tmpdir);
	printf("[PASS] test_ingest_crud_audit_search_and_persistence\n");
}

static void write_minimal_two_page_pdf(const char *path)
{
	/* Minimal valid 2-page PDF with distinct page sizes so hashes differ. */
	static const char *PDF =
		"%PDF-1.4\n"
		"1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
		"2 0 obj << /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >> endobj\n"
		"3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 100 120] >> endobj\n"
		"4 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 120 140] >> endobj\n"
		"xref\n"
		"0 5\n"
		"0000000000 65535 f \n"
		"0000000009 00000 n \n"
		"0000000058 00000 n \n"
		"0000000123 00000 n \n"
		"0000000194 00000 n \n"
		"trailer << /Size 5 /Root 1 0 R >>\n"
		"startxref\n"
		"265\n"
		"%%EOF\n";
	FILE *fp = fopen(path, "wb");
	assert(fp != NULL);
	assert(fwrite(PDF, 1, strlen(PDF), fp) == strlen(PDF));
	fclose(fp);
}

static void test_pdf_multipage_ingest(void)
{
	if (access("/usr/bin/pdftoppm", X_OK) != 0 &&
	    access("/usr/local/bin/pdftoppm", X_OK) != 0) {
		printf("[SKIP] test_pdf_multipage_ingest (pdftoppm not installed)\n");
		return;
	}

	char tmpdir[] = "/tmp/scribe-pdf-test-XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	char pdf_path[320];
	snprintf(pdf_path, sizeof(pdf_path), "%s/register.pdf", tmpdir);
	write_minimal_two_page_pdf(pdf_path);

	scribe_store_t *store = scribe_store_open(tmpdir);
	assert(store != NULL);

	scribe_page_t *pages = NULL;
	size_t count = 0;
	assert(scribe_ingest_file(store, pdf_path, "Register-A", 10, 72, &pages, &count) == 0);
	assert(count == 2);
	assert(pages[0].page_num == 10 && pages[1].page_num == 11);
	assert(strcmp(pages[0].mime, "image/png") == 0);
	assert(strcmp(pages[1].mime, "image/png") == 0);
	assert(pages[0].width > 0 && pages[0].height > 0);
	assert(pages[1].width > pages[0].width);
	assert(strcmp(pages[0].id, pages[1].id) != 0);

	free(pages);
	scribe_store_close(store);
	rm_rf(tmpdir);
	printf("[PASS] test_pdf_multipage_ingest\n");
}

int main(void)
{
	assert(strcmp(scribe_version(), SCRIBE_VERSION) == 0);
	test_sha256_vectors();
	test_templates_and_hyle_defs();
	test_image_sniff_and_exif();
	test_ingest_crud_audit_search_and_persistence();
	test_pdf_multipage_ingest();
	printf("ALL SCRIBE CORE TESTS PASSED\n");
	return 0;
}
