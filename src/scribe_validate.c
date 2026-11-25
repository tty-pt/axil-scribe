#define _POSIX_C_SOURCE 200809L

#include <ttypt/scribe.h>
#include <ttypt/stele.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── ISO Date & Chronological Sanity Validation ────────────────── */

static int is_leap_year(int y)
{
	return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
	static const int DIM[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};
	if (m < 1 || m > 12)
		return 0;
	if (m == 2 && is_leap_year(y))
		return 29;
	return DIM[m - 1];
}

static int parse_digits(const char *p, size_t n, int *out)
{
	int v = 0;
	for (size_t i = 0; i < n; i++) {
		if (!isdigit((unsigned char)p[i]))
			return -1;
		v = v * 10 + (p[i] - '0');
	}
	*out = v;
	return 0;
}

int scribe_validate_date_range(const char *s, int min_year, int max_year,
                               int *out_year, int *out_month, int *out_day)
{
	if (!s || !s[0])
		return -1;

	size_t len = strlen(s);
	int y = 0, m = 0, d = 0;

	if (len == 4) {
		if (parse_digits(s, 4, &y) != 0)
			return -1;
	} else if (len == 7 && s[4] == '-') {
		if (parse_digits(s, 4, &y) != 0 ||
		    parse_digits(s + 5, 2, &m) != 0)
			return -1;
		if (m < 1 || m > 12)
			return -1;
	} else if (len == 10 && s[4] == '-' && s[7] == '-') {
		if (parse_digits(s, 4, &y) != 0 ||
		    parse_digits(s + 5, 2, &m) != 0 ||
		    parse_digits(s + 8, 2, &d) != 0)
			return -1;
		int max_d = days_in_month(y, m);
		if (max_d == 0 || d < 1 || d > max_d)
			return -1;
	} else {
		return -1;
	}

	if (min_year > 0 && y < min_year)
		return -1;
	if (max_year > 0 && y > max_year)
		return -1;

	if (out_year)
		*out_year = y;
	if (out_month)
		*out_month = m;
	if (out_day)
		*out_day = d;
	return 0;
}

int scribe_validate_date(const char *s, int *out_year,
                         int *out_month, int *out_day)
{
	return scribe_validate_date_range(s, 1500, 2100, out_year, out_month, out_day);
}

/* Parse a generic number or currency string (e.g. "123.45", "-42", "EUR 1,234.50", "$19.99"). */
static int parse_number_val(const char *s, int allow_currency, double *out)
{
	if (!s || !s[0])
		return -1;

	const char *p = s;
	while (*p && isspace((unsigned char)*p))
		p++;

	if (allow_currency) {
		/* Skip optional leading currency code/symbol (e.g. "EUR ", "USD ", "$", "€", "£") */
		while (*p && !isdigit((unsigned char)*p) && *p != '-' && *p != '+' && *p != '.')
			p++;
	}

	if (!*p)
		return -1;

	/* Normalize optional thousands commas/spaces when a decimal point exists */
	char norm[64];
	size_t ni = 0;
	int digits = 0;
	int dots = 0;
	if (*p == '-' || *p == '+') {
		norm[ni++] = *p++;
	}
	while (*p && ni + 1 < sizeof(norm)) {
		if (isdigit((unsigned char)*p)) {
			norm[ni++] = *p++;
			digits++;
		} else if (*p == '.' || *p == ',') {
			/* Treat comma as decimal separator if <=2 digits follow at end */
			if (*p == ',' && p[1] && isdigit((unsigned char)p[1]) &&
			    (!p[2] || (isdigit((unsigned char)p[2]) && !isdigit((unsigned char)p[3])))) {
				if (dots > 0)
					return -1;
				norm[ni++] = '.';
				dots++;
				p++;
			} else if (*p == ',') {
				p++; /* thousands separator */
			} else {
				if (dots > 0)
					return -1;
				norm[ni++] = '.';
				dots++;
				p++;
			}
		} else if (isspace((unsigned char)*p)) {
			break;
		} else if (allow_currency) {
			break; /* trailing currency symbol/code like "12.50 EUR" */
		} else {
			return -1;
		}
	}
	norm[ni] = '\0';
	if (digits == 0)
		return -1;

	if (!allow_currency) {
		while (*p && isspace((unsigned char)*p))
			p++;
		if (*p != '\0')
			return -1;
	}

	char *endp = NULL;
	double v = strtod(norm, &endp);
	if (!endp || endp == norm)
		return -1;
	if (out)
		*out = v;
	return 0;
}

static const char *effective_field_val(const scribe_field_val_t *f)
{
	if (!f)
		return NULL;
	if (f->verified_value && f->verified_value[0])
		return f->verified_value;
	if (f->raw_value && f->raw_value[0])
		return f->raw_value;
	return NULL;
}

static scribe_field_val_t *find_field_mut(scribe_entry_t *entry,
                                          const char *key)
{
	for (size_t i = 0; i < entry->field_count; i++) {
		if (strcmp(entry->fields[i].key, key) == 0)
			return &entry->fields[i];
	}
	return NULL;
}

/* Compare two parsed dates; returns <0 if a < b, 0 if comparable-equal, >0 if a > b. */
static int cmp_parsed_dates(int y1, int m1, int d1, int y2, int m2, int d2)
{
	if (y1 != y2)
		return y1 < y2 ? -1 : 1;
	if (m1 > 0 && m2 > 0 && m1 != m2)
		return m1 < m2 ? -1 : 1;
	if (d1 > 0 && d2 > 0 && d1 != d2)
		return d1 < d2 ? -1 : 1;
	return 0;
}

static void check_date_order(scribe_entry_t *entry,
                             const char *earlier_key,
                             const char *later_key,
                             int min_year, int max_year)
{
	scribe_field_val_t *fa = find_field_mut(entry, earlier_key);
	scribe_field_val_t *fb = find_field_mut(entry, later_key);
	const char *va = effective_field_val(fa);
	const char *vb = effective_field_val(fb);
	if (!va || !vb)
		return;

	int y1, m1, d1, y2, m2, d2;
	if (scribe_validate_date_range(va, min_year, max_year, &y1, &m1, &d1) != 0)
		return;
	if (scribe_validate_date_range(vb, min_year, max_year, &y2, &m2, &d2) != 0)
		return;

	if (cmp_parsed_dates(y1, m1, d1, y2, m2, d2) > 0)
		fb->flags |= SCRIBE_FLAG_INVALID;
}

static void check_num_order(scribe_entry_t *entry,
                            const char *smaller_key,
                            const char *larger_key)
{
	scribe_field_val_t *fa = find_field_mut(entry, smaller_key);
	scribe_field_val_t *fb = find_field_mut(entry, larger_key);
	const char *va = effective_field_val(fa);
	const char *vb = effective_field_val(fb);
	if (!va || !vb)
		return;

	double a = 0.0, b = 0.0;
	if (parse_number_val(va, 1, &a) != 0 || parse_number_val(vb, 1, &b) != 0)
		return;

	if (a > b + 1e-9)
		fb->flags |= SCRIBE_FLAG_INVALID;
}

size_t scribe_validate_entry_ex(const scribe_pack_t *pack,
                                const scribe_template_t *tpl,
                                scribe_entry_t *entry)
{
	if (!tpl || !entry)
		return 0;

	int min_year = (pack && pack->min_year > 0) ? pack->min_year : 1500;
	int max_year = (pack && pack->max_year > 0) ? pack->max_year : 2100;

	/* Clear prior INVALID bits before re-evaluating deterministically. */
	for (size_t i = 0; i < entry->field_count; i++)
		entry->fields[i].flags &= ~SCRIBE_FLAG_INVALID;

	for (size_t i = 0; i < tpl->field_count; i++) {
		const scribe_field_spec_t *spec = &tpl->fields[i];
		scribe_field_val_t *f = find_field_mut(entry, spec->key);
		const char *val = effective_field_val(f);

		if (!val) {
			if (spec->required) {
				if (!f) {
					(void)scribe_entry_set_field(
						entry, spec->key, NULL, NULL,
						SCRIBE_FLAG_INVALID, NULL);
				} else if (!(f->flags & SCRIBE_FLAG_ILLEGIBLE)) {
					f->flags |= SCRIBE_FLAG_INVALID;
				}
			}
			continue;
		}

		if (spec->type == SCRIBE_FIELD_DATE) {
			if (scribe_validate_date_range(val, min_year, max_year,
			                               NULL, NULL, NULL) != 0)
				f->flags |= SCRIBE_FLAG_INVALID;
		} else if (spec->type == SCRIBE_FIELD_NUMBER) {
			if (parse_number_val(val, 0, NULL) != 0)
				f->flags |= SCRIBE_FLAG_INVALID;
		} else if (spec->type == SCRIBE_FIELD_CURRENCY) {
			if (parse_number_val(val, 1, NULL) != 0)
				f->flags |= SCRIBE_FLAG_INVALID;
		} else if (spec->type == SCRIBE_FIELD_ENUM && spec->enum_count > 0) {
			int matched = 0;
			for (size_t e = 0; e < spec->enum_count; e++) {
				if (spec->enum_values[e] &&
				    strcmp(spec->enum_values[e], val) == 0) {
					matched = 1;
					break;
				}
			}
			if (!matched)
				f->flags |= SCRIBE_FLAG_INVALID;
		}
	}

	/* Evaluate declarative template constraints (zero hardcoded domain keys). */
	for (size_t i = 0; i < tpl->constraint_count; i++) {
		const scribe_constraint_t *c = &tpl->constraints[i];
		if (c->op == SCRIBE_CONSTRAINT_DATE_LE) {
			check_date_order(entry, c->lhs, c->rhs, min_year, max_year);
		} else if (c->op == SCRIBE_CONSTRAINT_NUM_LE) {
			check_num_order(entry, c->lhs, c->rhs);
		} else if (c->op == SCRIBE_CONSTRAINT_DATE_RANGE) {
			scribe_field_val_t *f = find_field_mut(entry, c->lhs);
			const char *val = effective_field_val(f);
			if (val && scribe_validate_date_range(val, c->min_year, c->max_year,
			                                      NULL, NULL, NULL) != 0)
				f->flags |= SCRIBE_FLAG_INVALID;
		}
	}

	size_t flagged = 0;
	const uint32_t review_mask =
		SCRIBE_FLAG_ILLEGIBLE | SCRIBE_FLAG_CONFLICT | SCRIBE_FLAG_INVALID;
	for (size_t i = 0; i < entry->field_count; i++) {
		if (entry->fields[i].flags & review_mask)
			flagged++;
	}
	return flagged;
}

size_t scribe_validate_entry(const scribe_template_t *tpl,
                             scribe_entry_t *entry)
{
	return scribe_validate_entry_ex(NULL, tpl, entry);
}

/* ── Non-Destructive Advisory Dictionary ───────────────────────── */

typedef struct {
	char category[32];
	char canonical[128];
	char folded[128];
	int year_from;
	int year_to;
} dict_item_t;

struct scribe_dict {
	dict_item_t *items;
	size_t count;
	size_t cap;
};

scribe_dict_t *scribe_dict_open(void)
{
	return calloc(1, sizeof(scribe_dict_t));
}

void scribe_dict_close(scribe_dict_t *dict)
{
	if (!dict)
		return;
	free(dict->items);
	free(dict);
}

int scribe_dict_add(scribe_dict_t *dict, const char *category,
                    const char *canonical, int year_from, int year_to)
{
	if (!dict || !category || !category[0] || !canonical || !canonical[0])
		return -1;

	if (dict->count == dict->cap) {
		size_t ncap = dict->cap ? dict->cap * 2 : 16;
		dict_item_t *next = realloc(dict->items, ncap * sizeof(*next));
		if (!next)
			return -1;
		dict->items = next;
		dict->cap = ncap;
	}

	dict_item_t *it = &dict->items[dict->count];
	memset(it, 0, sizeof(*it));
	stl_strlcpy(it->category, category, sizeof(it->category));
	stl_strlcpy(it->canonical, canonical, sizeof(it->canonical));
	if (scribe_ascii_fold(it->folded, sizeof(it->folded), canonical) < 0)
		stl_strlcpy(it->folded, canonical, sizeof(it->folded));
	it->year_from = year_from;
	it->year_to = year_to;
	dict->count++;
	return 0;
}

int scribe_dict_load_tsv(scribe_dict_t *dict, const char *path)
{
	if (!dict || !path)
		return -1;

	FILE *fp = fopen(path, "r");
	if (!fp)
		return -1;

	char line[512];
	int loaded = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';
		if (n == 0 || line[0] == '#')
			continue;

		char *t1 = strchr(line, '\t');
		if (!t1)
			continue;
		*t1 = '\0';
		char *cat = line;
		char *name = t1 + 1;

		int yfrom = 0, yto = 0;
		char *t2 = strchr(name, '\t');
		if (t2) {
			*t2 = '\0';
			char *sfrom = t2 + 1;
			char *t3 = strchr(sfrom, '\t');
			if (t3) {
				*t3 = '\0';
				yto = atoi(t3 + 1);
			}
			yfrom = atoi(sfrom);
		}

		if (scribe_dict_add(dict, cat, name, yfrom, yto) == 0)
			loaded++;
	}

	fclose(fp);
	return loaded;
}

static size_t levenshtein_bounded(const char *a, const char *b)
{
	size_t la = strlen(a);
	size_t lb = strlen(b);
	if (la > 127)
		la = 127;
	if (lb > 127)
		lb = 127;

	size_t prev[128], curr[128];
	for (size_t j = 0; j <= lb; j++)
		prev[j] = j;

	for (size_t i = 1; i <= la; i++) {
		curr[0] = i;
		for (size_t j = 1; j <= lb; j++) {
			size_t cost = (a[i - 1] == b[j - 1]) ? 0u : 1u;
			size_t del = prev[j] + 1u;
			size_t ins = curr[j - 1] + 1u;
			size_t sub = prev[j - 1] + cost;
			size_t best = del < ins ? del : ins;
			if (sub < best)
				best = sub;
			curr[j] = best;
		}
		memcpy(prev, curr, (lb + 1) * sizeof(size_t));
	}
	return prev[lb];
}

size_t scribe_dict_suggest(const scribe_dict_t *dict,
                           const char *category,
                           const char *raw_value,
                           int year,
                           scribe_suggestion_t *out,
                           size_t max_out)
{
	if (!dict || !category || !raw_value || !raw_value[0] || !out || max_out == 0)
		return 0;

	char qfold[128];
	if (scribe_ascii_fold(qfold, sizeof(qfold), raw_value) < 0)
		stl_strlcpy(qfold, raw_value, sizeof(qfold));
	size_t qlen = strlen(qfold);
	if (qlen == 0)
		return 0;

	size_t n_out = 0;
	for (size_t i = 0; i < dict->count; i++) {
		const dict_item_t *it = &dict->items[i];
		if (strcmp(it->category, category) != 0)
			continue;
		if (year > 0) {
			if (it->year_from > 0 && year < it->year_from)
				continue;
			if (it->year_to > 0 && year > it->year_to)
				continue;
		}

		size_t ilen = strlen(it->folded);
		size_t denom = qlen > ilen ? qlen : ilen;
		if (denom == 0)
			continue;
		size_t dist = levenshtein_bounded(qfold, it->folded);
		float score = 1.0f - ((float)dist / (float)denom);
		if (score < 0.45f)
			continue;

		/* Insert sorted descending by score into bounded output array. */
		size_t pos = n_out;
		while (pos > 0 && out[pos - 1].score < score)
			pos--;
		if (pos >= max_out)
			continue;

		size_t move_end = (n_out < max_out) ? n_out : (max_out - 1);
		for (size_t k = move_end; k > pos; k--)
			out[k] = out[k - 1];

		memset(&out[pos], 0, sizeof(out[pos]));
		stl_strlcpy(out[pos].canonical, it->canonical, sizeof(out[pos].canonical));
		out[pos].year_from = it->year_from;
		out[pos].year_to = it->year_to;
		out[pos].score = score;
		if (n_out < max_out)
			n_out++;
	}

	return n_out;
}
