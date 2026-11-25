#define _POSIX_C_SOURCE 200809L

#include <ttypt/scribe.h>
#include <ttypt/stele.h>

#include <dirent.h>
#include <json-c/json.h>
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

static char *read_text_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	long sz = ftell(fp);
	if (sz < 0 || sz > 4L * 1024L * 1024L) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	char *buf = malloc((size_t)sz + 1u);
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
	buf[sz] = '\0';
	return buf;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s --fixtures <dir> [--templates <dir>] [--backend mock|auren]\n"
		"          [--model <name>] [--model2 <name>] [--host <host>] [--port <port>]\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *fixtures_dir = NULL;
	const char *templates_dir = "templates";
	const char *backend = "mock";
	const char *model = "chat";
	const char *model2 = NULL;
	const char *host = "127.0.0.1";
	uint16_t port = 4242;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fixtures") == 0 && i + 1 < argc)
			fixtures_dir = argv[++i];
		else if (strcmp(argv[i], "--templates") == 0 && i + 1 < argc)
			templates_dir = argv[++i];
		else if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc)
			backend = argv[++i];
		else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
			model = argv[++i];
		else if (strcmp(argv[i], "--model2") == 0 && i + 1 < argc)
			model2 = argv[++i];
		else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
			host = argv[++i];
		else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
			port = (uint16_t)atoi(argv[++i]);
		else {
			usage(argv[0]);
			return 2;
		}
	}

	if (!fixtures_dir) {
		usage(argv[0]);
		return 2;
	}

	scribe_registry_t *reg = scribe_registry_open();
	if (!reg || scribe_registry_load_dir(reg, templates_dir) <= 0) {
		fprintf(stderr, "scribe-bench: failed to load templates from %s\n", templates_dir);
		scribe_registry_close(reg);
		return 1;
	}

	DIR *dir = opendir(fixtures_dir);
	if (!dir) {
		fprintf(stderr, "scribe-bench: cannot open fixtures dir %s\n", fixtures_dir);
		scribe_registry_close(reg);
		return 1;
	}

	char **truth_files = NULL;
	size_t n_truth = 0, cap = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		size_t nl = strlen(de->d_name);
		if (nl <= 11 || strcmp(de->d_name + nl - 11, ".truth.json") != 0)
			continue;
		if (n_truth == cap) {
			size_t ncap = cap ? cap * 2 : 8;
			char **next = realloc(truth_files, ncap * sizeof(*next));
			if (!next)
				break;
			truth_files = next;
			cap = ncap;
		}
		truth_files[n_truth] = malloc(nl + 1u);
		if (!truth_files[n_truth])
			break;
		memcpy(truth_files[n_truth], de->d_name, nl + 1u);
		n_truth++;
	}
	closedir(dir);

	if (n_truth == 0) {
		fprintf(stderr, "scribe-bench: no *.truth.json files found in %s\n", fixtures_dir);
		free(truth_files);
		scribe_registry_close(reg);
		return 1;
	}
	qsort(truth_files, n_truth, sizeof(*truth_files), cmp_str);

	char tmpdir[] = "/tmp/scribe-bench-XXXXXX";
	if (!mkdtemp(tmpdir)) {
		fprintf(stderr, "scribe-bench: mkdtemp failed\n");
		return 1;
	}
	scribe_store_t *store = scribe_store_open(tmpdir);
	if (!store) {
		rm_rf(tmpdir);
		return 1;
	}

	scribe_auren_config_t acfg = {
		.host = host,
		.port = port,
		.endpoint = "/v1/chat/completions",
		.timeout_ms = 120000,
	};

	scribe_bench_metrics_t total = { 0 };
	int failures = 0;

	printf("scribe-bench | backend=%s | model=%s%s%s | docs=%zu\n",
	       backend, model, model2 ? " + " : "", model2 ? model2 : "", n_truth);
	printf("------------------------------------------------------------------------\n");

	for (size_t i = 0; i < n_truth; i++) {
		char truth_path[512];
		snprintf(truth_path, sizeof(truth_path), "%s/%s", fixtures_dir, truth_files[i]);
		char *truth_text = read_text_file(truth_path);
		if (!truth_text) {
			failures++;
			continue;
		}

		struct json_object *jtruth = json_tokener_parse(truth_text);
		struct json_object *jimg = NULL, *jtpl = NULL;
		if (!jtruth ||
		    !json_object_object_get_ex(jtruth, "image", &jimg) ||
		    !json_object_object_get_ex(jtruth, "template_id", &jtpl)) {
			fprintf(stderr, "  [ERR] invalid truth file: %s\n", truth_files[i]);
			if (jtruth)
				json_object_put(jtruth);
			free(truth_text);
			failures++;
			continue;
		}

		const char *img_name = json_object_get_string(jimg);
		const char *tpl_id = json_object_get_string(jtpl);
		const scribe_template_t *tpl = scribe_registry_find(reg, tpl_id);
		if (!tpl) {
			fprintf(stderr, "  [ERR] unknown template %s in %s\n", tpl_id, truth_files[i]);
			json_object_put(jtruth);
			free(truth_text);
			failures++;
			continue;
		}

		char img_path[512];
		snprintf(img_path, sizeof(img_path), "%s/%s", fixtures_dir, img_name);
		scribe_page_t *pages = NULL;
		size_t npages = 0;
		if (scribe_ingest_file(store, img_path, "bench", 1, 200, &pages, &npages) != 0 ||
		    npages == 0) {
			fprintf(stderr, "  [ERR] failed to ingest %s\n", img_path);
			json_object_put(jtruth);
			free(truth_text);
			failures++;
			continue;
		}

		scribe_entry_t *truth_entries = NULL;
		size_t n_truth_entries = 0;
		if (scribe_parse_extraction_json(truth_text, tpl, pages[0].id,
		                                 "truth", &truth_entries,
		                                 &n_truth_entries) != 0) {
			fprintf(stderr, "  [ERR] failed to parse truth entries in %s\n", truth_files[i]);
			free(pages);
			json_object_put(jtruth);
			free(truth_text);
			failures++;
			continue;
		}

		const scribe_extractor_ops_t *ops = NULL;
		void *ectx = NULL;
		const char *mock_resps[2] = { truth_text, truth_text };
		scribe_mock_extractor_t mock = {
			.responses = mock_resps,
			.response_count = 2,
			.call_index = 0,
		};

		if (strcmp(backend, "auren") == 0) {
			ops = scribe_extractor_auren();
			ectx = &acfg;
		} else {
			ops = scribe_extractor_mock();
			ectx = &mock;
		}

		scribe_entry_t *pred_entries = NULL;
		size_t n_pred = 0;
		scribe_bench_metrics_t doc_m = { 0 };
		if (scribe_extract_page(store, &pages[0], tpl, ops, ectx,
		                        model, model2, &pred_entries, &n_pred) == 0) {
			scribe_bench_score_entries(tpl, pred_entries, n_pred,
			                           truth_entries, n_truth_entries, &doc_m);
			scribe_bench_score_entries(tpl, pred_entries, n_pred,
			                           truth_entries, n_truth_entries, &total);
			printf("  %-30s tpl=%-20s exact=%zu/%zu flagged=%zu silent_err=%zu\n",
			       img_name, tpl_id, doc_m.exact_matches, doc_m.fields_total,
			       doc_m.flagged_fields, doc_m.silent_errors);
			for (size_t k = 0; k < n_pred; k++)
				scribe_entry_free(&pred_entries[k]);
			free(pred_entries);
		} else {
			fprintf(stderr, "  [FAIL] extraction failed for %s\n", img_name);
			failures++;
		}

		for (size_t k = 0; k < n_truth_entries; k++)
			scribe_entry_free(&truth_entries[k]);
		free(truth_entries);
		free(pages);
		json_object_put(jtruth);
		free(truth_text);
	}

	double exact_pct = total.fields_total
	                   ? (100.0 * (double)total.exact_matches / (double)total.fields_total)
	                   : 0.0;
	double silent_pct = total.fields_total
	                    ? (100.0 * (double)total.silent_errors / (double)total.fields_total)
	                    : 0.0;

	printf("------------------------------------------------------------------------\n");
	printf("SUMMARY: docs=%zu entries=%zu fields=%zu exact=%.1f%% (%zu/%zu) "
	       "flagged=%zu silent_err=%.1f%% (%zu/%zu)\n",
	       total.docs_total, total.entries_total, total.fields_total,
	       exact_pct, total.exact_matches, total.fields_total,
	       total.flagged_fields, silent_pct, total.silent_errors, total.fields_total);

	for (size_t i = 0; i < n_truth; i++)
		free(truth_files[i]);
	free(truth_files);
	scribe_store_close(store);
	rm_rf(tmpdir);
	scribe_registry_close(reg);
	return failures ? 1 : 0;
}
