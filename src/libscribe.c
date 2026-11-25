#include <ttypt/scribe.h>

#include <string.h>

const char *scribe_version(void)
{
	return SCRIBE_VERSION;
}

const char *scribe_status_str(scribe_status_t status)
{
	switch (status) {
	case SCRIBE_STATUS_PENDING:      return "pending";
	case SCRIBE_STATUS_EXTRACTED:    return "extracted";
	case SCRIBE_STATUS_NEEDS_REVIEW: return "needs_review";
	case SCRIBE_STATUS_VERIFIED:     return "verified";
	}
	return "pending";
}

int scribe_status_parse(const char *s, scribe_status_t *out)
{
	if (!s || !out)
		return -1;
	if (strcmp(s, "pending") == 0) {
		*out = SCRIBE_STATUS_PENDING;
		return 0;
	}
	if (strcmp(s, "extracted") == 0) {
		*out = SCRIBE_STATUS_EXTRACTED;
		return 0;
	}
	if (strcmp(s, "needs_review") == 0) {
		*out = SCRIBE_STATUS_NEEDS_REVIEW;
		return 0;
	}
	if (strcmp(s, "verified") == 0) {
		*out = SCRIBE_STATUS_VERIFIED;
		return 0;
	}
	return -1;
}

const char *scribe_field_type_str(scribe_field_type_t type)
{
	switch (type) {
	case SCRIBE_FIELD_TEXT:   return "text";
	case SCRIBE_FIELD_DATE:   return "date";
	case SCRIBE_FIELD_PERSON: return "person";
	case SCRIBE_FIELD_PLACE:  return "place";
	case SCRIBE_FIELD_NOTES:  return "notes";
	}
	return "text";
}

int scribe_field_type_parse(const char *s, scribe_field_type_t *out)
{
	if (!s || !out)
		return -1;
	if (strcmp(s, "text") == 0) {
		*out = SCRIBE_FIELD_TEXT;
		return 0;
	}
	if (strcmp(s, "date") == 0) {
		*out = SCRIBE_FIELD_DATE;
		return 0;
	}
	if (strcmp(s, "person") == 0) {
		*out = SCRIBE_FIELD_PERSON;
		return 0;
	}
	if (strcmp(s, "place") == 0) {
		*out = SCRIBE_FIELD_PLACE;
		return 0;
	}
	if (strcmp(s, "notes") == 0) {
		*out = SCRIBE_FIELD_NOTES;
		return 0;
	}
	return -1;
}

/*
 * Fold Latin-1 Supplement codepoints (U+00C0..U+00FF) to lowercase ASCII
 * for optional accent-insensitive lookup without locale or iconv.
 */
static char fold_latin1(uint8_t cp)
{
	switch (cp) {
	case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
	case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
		return 'a';
	case 0xC7: case 0xE7:
		return 'c';
	case 0xC8: case 0xC9: case 0xCA: case 0xCB:
	case 0xE8: case 0xE9: case 0xEA: case 0xEB:
		return 'e';
	case 0xCC: case 0xCD: case 0xCE: case 0xCF:
	case 0xEC: case 0xED: case 0xEE: case 0xEF:
		return 'i';
	case 0xD1: case 0xF1:
		return 'n';
	case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
	case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8:
		return 'o';
	case 0xD9: case 0xDA: case 0xDB: case 0xDC:
	case 0xF9: case 0xFA: case 0xFB: case 0xFC:
		return 'u';
	case 0xDD: case 0xFD: case 0xFF:
		return 'y';
	default:
		return 0;
	}
}

int scribe_ascii_fold(char *out, size_t outsz, const char *in)
{
	size_t oi = 0;

	if (!out || outsz == 0 || !in)
		return -1;

	for (const unsigned char *p = (const unsigned char *)in; *p; ) {
		unsigned char c = *p;

		if (c < 0x80) {
			if (oi + 1 >= outsz)
				return -1;
			if (c >= 'A' && c <= 'Z')
				c = (unsigned char)(c + ('a' - 'A'));
			out[oi++] = (char)c;
			p++;
			continue;
		}

		/* UTF-8 2-byte Latin-1 Supplement: U+00C0..U+00FF => 0xC3 0x80..0xBF */
		if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
			uint8_t cp = (uint8_t)(0xC0u + (p[1] - 0x80u));
			char folded = fold_latin1(cp);
			if (folded) {
				if (oi + 1 >= outsz)
					return -1;
				out[oi++] = folded;
				p += 2;
				continue;
			}
		}

		/* Preserve any other byte verbatim. */
		if (oi + 1 >= outsz)
			return -1;
		out[oi++] = (char)c;
		p++;
	}

	out[oi] = '\0';
	return (int)oi;
}
