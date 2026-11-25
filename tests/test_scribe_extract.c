#define _POSIX_C_SOURCE 200809L

#include <ttypt/scribe.h>

#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

static void test_base64_and_prompt(void)
{
	char *b0 = scribe_base64_encode((const uint8_t *)"", 0);
	assert(b0 && strcmp(b0, "") == 0);
	free(b0);

	char *b1 = scribe_base64_encode((const uint8_t *)"foobar", 6);
	assert(b1 && strcmp(b1, "Zm9vYmFy") == 0);
	free(b1);

	char *b2 = scribe_base64_encode((const uint8_t *)"foo", 3);
	assert(b2 && strcmp(b2, "Zm9v") == 0);
	free(b2);

	char *b3 = scribe_base64_encode((const uint8_t *)"f", 1);
	assert(b3 && strcmp(b3, "Zg==") == 0);
	free(b3);

	scribe_template_t *tpl = scribe_template_load_file("templates/confirmation_starter.json");
	assert(tpl != NULL);
	char *prompt = NULL;
	assert(scribe_build_extraction_prompt(tpl, &prompt) == 0 && prompt != NULL);
	assert(strstr(prompt, "\"person_name\"") != NULL);
	assert(strstr(prompt, "\"confirmation_date\"") != NULL);
	assert(strstr(prompt, "marginal_notes") != NULL);
	free(prompt);
	scribe_template_free(tpl);

	printf("[PASS] test_base64_and_prompt\n");
}

static void test_date_and_entry_validation(void)
{
	int y, m, d;
	assert(scribe_validate_date("1958-06-14", &y, &m, &d) == 0);
	assert(y == 1958 && m == 6 && d == 14);
	assert(scribe_validate_date("2000-02-29", NULL, NULL, NULL) == 0);
	assert(scribe_validate_date("1958-06", &y, &m, &d) == 0 && d == 0);
	assert(scribe_validate_date("1958", &y, &m, &d) == 0 && m == 0);

	/* Invalid dates */
	assert(scribe_validate_date("2025-02-29", NULL, NULL, NULL) != 0);
	assert(scribe_validate_date("1958-13-01", NULL, NULL, NULL) != 0);
	assert(scribe_validate_date("1958-04-31", NULL, NULL, NULL) != 0);
	assert(scribe_validate_date("1200-01-01", NULL, NULL, NULL) != 0);
	assert(scribe_validate_date("not-a-date", NULL, NULL, NULL) != 0);

	scribe_template_t *tpl = scribe_template_load_file("templates/confirmation_starter.json");
	assert(tpl != NULL);

	/* Chronology violation: baptism after confirmation */
	scribe_entry_t e;
	scribe_entry_init(&e);
	assert(scribe_entry_set_field(&e, "person_name", "Ana Silva", NULL, 0, NULL) == 0);
	assert(scribe_entry_set_field(&e, "confirmation_date", "1950-05-01", NULL, 0, NULL) == 0);
	assert(scribe_entry_set_field(&e, "baptism_date", "1955-05-01", NULL, 0, NULL) == 0);
	assert(scribe_entry_set_field(&e, "parish", "São João", NULL, 0, NULL) == 0);

	size_t flagged = scribe_validate_entry(tpl, &e);
	assert(flagged == 1);
	const scribe_field_val_t *fc = scribe_entry_get_field(&e, "confirmation_date");
	assert(fc && (fc->flags & SCRIBE_FLAG_INVALID));
	/* Non-destructive: raw_value untouched */
	assert(strcmp(fc->raw_value, "1950-05-01") == 0);

	/* Fix date order -> 0 flagged */
	assert(scribe_entry_set_field(&e, "baptism_date", "1940-05-01", NULL, 0, NULL) == 0);
	assert(scribe_validate_entry(tpl, &e) == 0);

	scribe_entry_free(&e);
	scribe_template_free(tpl);
	printf("[PASS] test_date_and_entry_validation\n");
}

static void test_advisory_dictionary(void)
{
	scribe_dict_t *dict = scribe_dict_open();
	assert(dict != NULL);

	assert(scribe_dict_add(dict, "officiant", "Pe. Manuel da Silva", 1940, 1970) == 0);
	assert(scribe_dict_add(dict, "officiant", "Pe. Manuel de Sousa", 1975, 2005) == 0);
	assert(scribe_dict_add(dict, "parish", "Paróquia de São João", 0, 0) == 0);

	scribe_suggestion_t out[4];
	/* Era filter 1955 picks Pe. Manuel da Silva and excludes 1975..2005 */
	size_t n = scribe_dict_suggest(dict, "officiant", "Pe Manel da Silba", 1955, out, 4);
	assert(n == 1);
	assert(strcmp(out[0].canonical, "Pe. Manuel da Silva") == 0);
	assert(out[0].score > 0.75f);

	/* Accent-folded parish suggestion */
	n = scribe_dict_suggest(dict, "parish", "Paroquia de Sao Joao", 1960, out, 4);
	assert(n == 1);
	assert(strcmp(out[0].canonical, "Paróquia de São João") == 0);
	assert(out[0].score > 0.99f);

	scribe_dict_close(dict);
	printf("[PASS] test_advisory_dictionary\n");
}

static void test_mock_extraction_and_two_pass_conflicts(void)
{
	char tmpdir[] = "/tmp/scribe-ext-XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);

	scribe_store_t *store = scribe_store_open(tmpdir);
	assert(store != NULL);

	scribe_template_t *tpl = scribe_template_load_file("templates/confirmation_starter.json");
	assert(tpl != NULL);

	uint8_t png[32] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
		0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
		0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00,
		0x08, 0x02, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC
	};
	scribe_page_t page;
	int dedup = 0;
	assert(scribe_ingest_buffer(store, png, sizeof(png), "Book-7", 1, &page, &dedup) == 0);

	/* 2-pass responses wrapped in markdown fences with surname conflict */
	const char *resps[2] = {
		"```json\n"
		"{\"entries\":[{\"fields\":{"
		"\"person_name\":\"Maria da Conceição\","
		"\"confirmation_date\":\"1958-06-14\","
		"\"parish\":\"Paróquia de São João\","
		"\"sponsor\":null"
		"},\"marginal_notes\":\"Casou em 1971\"}]}\n"
		"```",
		"{\"entries\":[{\"fields\":{"
		"\"person_name\":\"Maria da Consolação\","
		"\"confirmation_date\":\"1958-06-14\","
		"\"parish\":\"Paróquia de São João\","
		"\"sponsor\":\"Helena Duarte\""
		"},\"marginal_notes\":\"Casou em 1971\"}]}"
	};
	scribe_mock_extractor_t mock = { .responses = resps, .response_count = 2, .call_index = 0 };

	scribe_entry_t *entries = NULL;
	size_t n = 0;
	assert(scribe_extract_page(store, &page, tpl, scribe_extractor_mock(), &mock,
	                           "model-a", "model-b", &entries, &n) == 0);
	assert(n == 1);
	assert(entries[0].status == SCRIBE_STATUS_NEEDS_REVIEW);

	const scribe_field_val_t *fn = scribe_entry_get_field(&entries[0], "person_name");
	assert(fn && (fn->flags & SCRIBE_FLAG_CONFLICT));
	assert(strcmp(fn->raw_value, "Maria da Conceição") == 0);

	/* Pass 1 had NULL sponsor, Pass 2 read Helena Duarte -> surfaced with CONFLICT */
	const scribe_field_val_t *fs = scribe_entry_get_field(&entries[0], "sponsor");
	assert(fs && (fs->flags & SCRIBE_FLAG_CONFLICT));
	assert(fs->raw_value && strcmp(fs->raw_value, "Helena Duarte") == 0);

	/* Date agreed across both passes -> no conflict */
	const scribe_field_val_t *fd = scribe_entry_get_field(&entries[0], "confirmation_date");
	assert(fd && fd->flags == SCRIBE_FLAG_NONE);

	/* Verify persisted & searchable via liblemma */
	scribe_id_list_t hits;
	assert(scribe_search(store, "person_name", "Conceição", SCRIBE_SEARCH_EXACT, &hits) == 0);
	assert(hits.count == 1 && strcmp(hits.ids[0], entries[0].id) == 0);
	scribe_id_list_free(&hits);

	scribe_entry_free(&entries[0]);
	free(entries);
	scribe_template_free(tpl);
	scribe_store_close(store);
	rm_rf(tmpdir);
	printf("[PASS] test_mock_extraction_and_two_pass_conflicts\n");
}

static void test_auren_socket_client_chunked(void)
{
	/* Spin up a tiny loopback TCP server simulating axil-auren /v1/chat/completions */
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	assert(srv >= 0);
	int one = 1;
	(void)setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0; /* ephemeral port */
	assert(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	assert(listen(srv, 4) == 0);

	socklen_t alen = sizeof(addr);
	assert(getsockname(srv, (struct sockaddr *)&addr, &alen) == 0);
	uint16_t port = ntohs(addr.sin_port);

	pid_t pid = fork();
	assert(pid >= 0);
	if (pid == 0) {
		int cli = accept(srv, NULL, NULL);
		if (cli >= 0) {
			char reqbuf[16384];
			size_t total = 0;
			size_t expected = 0;
			while (total < sizeof(reqbuf) - 1) {
				ssize_t rn = recv(cli, reqbuf + total,
				                  sizeof(reqbuf) - 1 - total, 0);
				if (rn <= 0)
					break;
				total += (size_t)rn;
				reqbuf[total] = '\0';
				if (expected == 0) {
					char *sep = strstr(reqbuf, "\r\n\r\n");
					if (sep) {
						char *cl = strstr(reqbuf, "Content-Length:");
						size_t blen = cl ? (size_t)strtoul(cl + 15, NULL, 10) : 0;
						expected = (size_t)(sep + 4 - reqbuf) + blen;
					}
				}
				if (expected > 0 && total >= expected)
					break;
			}
			int ok = (total > 0) &&
			         strstr(reqbuf, "POST /v1/chat/completions") != NULL &&
			         strstr(reqbuf, "data:image/png;base64,") != NULL;
			const char *json_body =
				"{\"choices\":[{\"message\":{\"content\":"
				"\"{\\\"entries\\\":[{\\\"fields\\\":{"
				"\\\"person_name\\\":\\\"João Pedro\\\","
				"\\\"confirmation_date\\\":\\\"1962-04-15\\\","
				"\\\"parish\\\":\\\"Sé Nova\\\"},"
				"\\\"marginal_notes\\\":null}]}\"}}]}";
			char resp[2048];
			if (ok) {
				snprintf(resp, sizeof(resp),
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: application/json\r\n"
					"Transfer-Encoding: chunked\r\n"
					"Connection: close\r\n\r\n"
					"%zx\r\n%s\r\n0\r\n\r\n",
					strlen(json_body), json_body);
			} else {
				snprintf(resp, sizeof(resp),
					"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
			}
			(void)send(cli, resp, strlen(resp), 0);
			close(cli);
		}
		close(srv);
		_exit(0);
	}
	close(srv);

	char tmpdir[] = "/tmp/scribe-auren-XXXXXX";
	assert(mkdtemp(tmpdir) != NULL);
	scribe_store_t *store = scribe_store_open(tmpdir);
	assert(store != NULL);
	scribe_template_t *tpl = scribe_template_load_file("templates/confirmation_starter.json");
	assert(tpl != NULL);

	uint8_t png[32] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
		0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R',
		0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x08, 0x02, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33
	};
	scribe_page_t page;
	int dedup = 0;
	assert(scribe_ingest_buffer(store, png, sizeof(png), "Book-Live", 3, &page, &dedup) == 0);

	scribe_auren_config_t cfg = {
		.host = "127.0.0.1",
		.port = port,
		.endpoint = "/v1/chat/completions",
		.timeout_ms = 5000,
	};

	scribe_entry_t *entries = NULL;
	size_t n = 0;
	assert(scribe_extract_page(store, &page, tpl, scribe_extractor_auren(), &cfg,
	                           "qwen2.5-vl-7b", NULL, &entries, &n) == 0);
	assert(n == 1);
	assert(entries[0].status == SCRIBE_STATUS_EXTRACTED);
	const scribe_field_val_t *fn = scribe_entry_get_field(&entries[0], "person_name");
	assert(fn && fn->raw_value && strcmp(fn->raw_value, "João Pedro") == 0);

	int wst = 0;
	waitpid(pid, &wst, 0);

	scribe_entry_free(&entries[0]);
	free(entries);
	scribe_template_free(tpl);
	scribe_store_close(store);
	rm_rf(tmpdir);
	printf("[PASS] test_auren_socket_client_chunked\n");
}

static void test_benchmark_scoring(void)
{
	scribe_template_t *tpl = scribe_template_load_file("templates/generic_record.json");
	assert(tpl != NULL);

	scribe_entry_t truth, pred;
	scribe_entry_init(&truth);
	scribe_entry_init(&pred);

	assert(scribe_entry_set_field(&truth, "person_name", "Maria Silva", NULL, 0, NULL) == 0);
	assert(scribe_entry_set_field(&truth, "date", "1955-01-10", NULL, 0, NULL) == 0);
	assert(scribe_entry_set_field(&truth, "place", "Braga", NULL, 0, NULL) == 0);

	/* person_name exact, date wrong+FLAGGED (not silent), place wrong+UNFLAGGED (silent error!) */
	assert(scribe_entry_set_field(&pred, "person_name", "Maria Silva", NULL, 0, NULL) == 0);
	assert(scribe_entry_set_field(&pred, "date", "1955-01-19", NULL, SCRIBE_FLAG_CONFLICT, NULL) == 0);
	assert(scribe_entry_set_field(&pred, "place", "Porto", NULL, SCRIBE_FLAG_NONE, NULL) == 0);

	scribe_bench_metrics_t m = { 0 };
	scribe_bench_score_entries(tpl, &pred, 1, &truth, 1, &m);
	assert(m.docs_total == 1);
	assert(m.entries_total == 1);
	assert(m.fields_total == tpl->field_count);
	/* title, book_page_entry, notes all NULL==NULL (3) + person_name exact (1) = 4 */
	assert(m.exact_matches == 4);
	assert(m.flagged_fields == 1);
	assert(m.silent_errors == 1);

	scribe_entry_free(&truth);
	scribe_entry_free(&pred);
	scribe_template_free(tpl);
	printf("[PASS] test_benchmark_scoring\n");
}

int main(void)
{
	test_base64_and_prompt();
	test_date_and_entry_validation();
	test_advisory_dictionary();
	test_mock_extraction_and_two_pass_conflicts();
	test_auren_socket_client_chunked();
	test_benchmark_scoring();
	printf("ALL SCRIBE EXTRACT TESTS PASSED\n");
	return 0;
}
