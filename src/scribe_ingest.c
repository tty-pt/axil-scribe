#define _POSIX_C_SOURCE 200809L

#include <ttypt/scribe.h>
#include <ttypt/stele.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ── Pure-C FIPS-180-4 SHA-256 (zero OpenSSL dependency) ───────── */

typedef struct {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t data[64];
	size_t datalen;
} sha256_ctx_t;

static const uint32_t SHA256_K[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static inline uint32_t rotr32(uint32_t x, uint32_t n)
{
	return (x >> n) | (x << (32u - n));
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t block[64])
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4] << 24) |
		       ((uint32_t)block[i * 4 + 1] << 16) |
		       ((uint32_t)block[i * 4 + 2] << 8) |
		       ((uint32_t)block[i * 4 + 3]);
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = ctx->state[0], b = ctx->state[1];
	uint32_t c = ctx->state[2], d = ctx->state[3];
	uint32_t e = ctx->state[4], f = ctx->state[5];
	uint32_t g = ctx->state[6], h = ctx->state[7];

	for (int i = 0; i < 64; i++) {
		uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
		uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + maj;

		h = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}

	ctx->state[0] += a; ctx->state[1] += b;
	ctx->state[2] += c; ctx->state[3] += d;
	ctx->state[4] += e; ctx->state[5] += f;
	ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
	ctx->datalen = 0;
	ctx->bitlen = 0;
	ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
	ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
	ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
	ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		ctx->data[ctx->datalen++] = data[i];
		if (ctx->datalen == 64) {
			sha256_transform(ctx, ctx->data);
			ctx->bitlen += 512;
			ctx->datalen = 0;
		}
	}
}

static void sha256_final_hex(sha256_ctx_t *ctx, char out_hex[65])
{
	static const char HEX[] = "0123456789abcdef";
	size_t i = ctx->datalen;

	ctx->bitlen += (uint64_t)ctx->datalen * 8u;
	ctx->data[i++] = 0x80;
	if (i > 56) {
		while (i < 64)
			ctx->data[i++] = 0;
		sha256_transform(ctx, ctx->data);
		i = 0;
	}
	while (i < 56)
		ctx->data[i++] = 0;

	for (int shift = 56, idx = 56; shift >= 0; shift -= 8, idx++)
		ctx->data[idx] = (uint8_t)((ctx->bitlen >> shift) & 0xffu);
	sha256_transform(ctx, ctx->data);

	for (int s = 0; s < 8; s++) {
		uint32_t v = ctx->state[s];
		for (int b = 3; b >= 0; b--) {
			uint8_t byte = (uint8_t)((v >> (b * 8)) & 0xffu);
			size_t pos = (size_t)s * 8u + (size_t)(3 - b) * 2u;
			out_hex[pos]     = HEX[byte >> 4];
			out_hex[pos + 1] = HEX[byte & 0x0fu];
		}
	}
	out_hex[64] = '\0';
}

void scribe_sha256_buf(const uint8_t *data, size_t len, char out_hex[65])
{
	sha256_ctx_t ctx;
	sha256_init(&ctx);
	if (data && len > 0)
		sha256_update(&ctx, data, len);
	sha256_final_hex(&ctx, out_hex);
}

int scribe_sha256_file(const char *path, char out_hex[65])
{
	if (!path || !out_hex)
		return -1;
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;

	sha256_ctx_t ctx;
	sha256_init(&ctx);
	uint8_t buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
		sha256_update(&ctx, buf, n);
	int err = ferror(fp);
	fclose(fp);
	if (err)
		return -1;

	sha256_final_hex(&ctx, out_hex);
	return 0;
}

/* ── Pure-C Image Magic, Dimensions & EXIF Orientation Sniffer ─── */

static uint16_t rd_u16_be(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint16_t rd_u16_le(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static uint32_t rd_u32_be(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static uint32_t rd_u32_le(const uint8_t *p)
{
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[1] << 8)  | (uint32_t)p[0];
}

static uint8_t parse_exif_orientation(const uint8_t *seg, size_t len)
{
	/* seg starts at APP1 payload: "Exif\0\0" + TIFF header */
	if (len < 14 || memcmp(seg, "Exif\0\0", 6) != 0)
		return 0;

	const uint8_t *tiff = seg + 6;
	size_t tlen = len - 6;
	int le = 0;

	if (tiff[0] == 'I' && tiff[1] == 'I')
		le = 1;
	else if (tiff[0] == 'M' && tiff[1] == 'M')
		le = 0;
	else
		return 0;

	uint16_t magic = le ? rd_u16_le(tiff + 2) : rd_u16_be(tiff + 2);
	if (magic != 42)
		return 0;

	uint32_t ifd_off = le ? rd_u32_le(tiff + 4) : rd_u32_be(tiff + 4);
	if (ifd_off + 2 > tlen)
		return 0;

	uint16_t n_entries = le ? rd_u16_le(tiff + ifd_off) : rd_u16_be(tiff + ifd_off);
	size_t pos = ifd_off + 2;

	for (uint16_t i = 0; i < n_entries; i++, pos += 12) {
		if (pos + 12 > tlen)
			break;
		uint16_t tag = le ? rd_u16_le(tiff + pos) : rd_u16_be(tiff + pos);
		if (tag == 0x0112u) {
			uint16_t val = le ? rd_u16_le(tiff + pos + 8)
			                  : rd_u16_be(tiff + pos + 8);
			if (val >= 1 && val <= 8)
				return (uint8_t)val;
			return 0;
		}
	}
	return 0;
}

static int sniff_jpeg(const uint8_t *data, size_t len, scribe_image_info_t *out)
{
	stl_strlcpy(out->mime, "image/jpeg", sizeof(out->mime));
	stl_strlcpy(out->ext, "jpg", sizeof(out->ext));

	size_t pos = 2;
	while (pos + 4 <= len) {
		if (data[pos] != 0xFF)
			break;
		while (pos < len && data[pos] == 0xFF)
			pos++;
		if (pos >= len)
			break;
		uint8_t marker = data[pos++];

		if (marker == 0xD9 || marker == 0xDA) /* EOI or SOS */
			break;
		if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
			continue;

		if (pos + 2 > len)
			break;
		uint16_t seg_len = rd_u16_be(data + pos);
		if (seg_len < 2 || pos + seg_len > len)
			break;

		if (marker == 0xE1 && out->exif_orientation == 0) {
			out->exif_orientation = parse_exif_orientation(
				data + pos + 2, (size_t)seg_len - 2u);
		} else if ((marker >= 0xC0 && marker <= 0xC3) ||
		           (marker >= 0xC5 && marker <= 0xC7) ||
		           (marker >= 0xC9 && marker <= 0xCB) ||
		           (marker >= 0xCD && marker <= 0xCF)) {
			if (seg_len >= 7) {
				out->height = rd_u16_be(data + pos + 3);
				out->width  = rd_u16_be(data + pos + 5);
			}
		}

		pos += seg_len;
	}

	/* EXIF orientations 5..8 rotate 90/270 degrees: swap logical W/H. */
	if (out->exif_orientation >= 5 && out->exif_orientation <= 8) {
		uint32_t tmp = out->width;
		out->width = out->height;
		out->height = tmp;
	}
	return 0;
}

static int sniff_webp(const uint8_t *data, size_t len, scribe_image_info_t *out)
{
	stl_strlcpy(out->mime, "image/webp", sizeof(out->mime));
	stl_strlcpy(out->ext, "webp", sizeof(out->ext));

	if (len >= 30 && memcmp(data + 12, "VP8X", 4) == 0) {
		out->width  = 1u + ((uint32_t)data[24] | ((uint32_t)data[25] << 8) |
		                    ((uint32_t)data[26] << 16));
		out->height = 1u + ((uint32_t)data[27] | ((uint32_t)data[28] << 8) |
		                    ((uint32_t)data[29] << 16));
	} else if (len >= 30 && memcmp(data + 12, "VP8 ", 4) == 0) {
		out->width  = rd_u16_le(data + 26) & 0x3fffu;
		out->height = rd_u16_le(data + 28) & 0x3fffu;
	} else if (len >= 25 && memcmp(data + 12, "VP8L", 4) == 0) {
		uint32_t bits = rd_u32_le(data + 21);
		out->width  = (bits & 0x3fffu) + 1u;
		out->height = ((bits >> 14) & 0x3fffu) + 1u;
	}
	return 0;
}

int scribe_sniff_image(const uint8_t *data, size_t len,
                       scribe_image_info_t *out)
{
	if (!data || !out || len < 5)
		return -1;
	memset(out, 0, sizeof(*out));

	/* PDF */
	if (memcmp(data, "%PDF-", 5) == 0) {
		stl_strlcpy(out->mime, "application/pdf", sizeof(out->mime));
		stl_strlcpy(out->ext, "pdf", sizeof(out->ext));
		out->is_pdf = 1;
		return 0;
	}

	/* PNG */
	static const uint8_t PNG_SIG[8] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
	};
	if (len >= 24 && memcmp(data, PNG_SIG, 8) == 0) {
		stl_strlcpy(out->mime, "image/png", sizeof(out->mime));
		stl_strlcpy(out->ext, "png", sizeof(out->ext));
		out->width  = rd_u32_be(data + 16);
		out->height = rd_u32_be(data + 20);
		return 0;
	}

	/* JPEG */
	if (len >= 3 && data[0] == 0xFF && data[1] == 0xD8)
		return sniff_jpeg(data, len, out);

	/* WebP */
	if (len >= 12 && memcmp(data, "RIFF", 4) == 0 &&
	    memcmp(data + 8, "WEBP", 4) == 0)
		return sniff_webp(data, len, out);

	return -1;
}

/* ── Immutable Content-Addressed Intake ────────────────────────── */

static int mkdir_p(const char *path)
{
	char tmp[512];
	size_t n = stl_strlcpy(tmp, path, sizeof(tmp));
	if (n == 0 || n >= sizeof(tmp))
		return -1;

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int write_atomic(const char *dst_path, const uint8_t *data, size_t len)
{
	char tmp_path[512];
	int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld",
	                 dst_path, (long)getpid());
	if (n < 0 || (size_t)n >= sizeof(tmp_path))
		return -1;

	FILE *fp = fopen(tmp_path, "wb");
	if (!fp)
		return -1;
	size_t written = fwrite(data, 1, len, fp);
	int flush_err = fflush(fp);
	int fd = fileno(fp);
	if (fd >= 0)
		(void)stl_fsync(fd);
	int close_err = fclose(fp);

	if (written != len || flush_err != 0 || close_err != 0) {
		unlink(tmp_path);
		return -1;
	}
	if (rename(tmp_path, dst_path) != 0) {
		unlink(tmp_path);
		return -1;
	}
	return 0;
}

int scribe_ingest_buffer(scribe_store_t *store,
                         const uint8_t *data, size_t len,
                         const char *book_ref, uint32_t page_num,
                         scribe_page_t *out_page, int *out_deduped)
{
	if (!store || !data || len == 0 || !out_page)
		return -1;
	if (out_deduped)
		*out_deduped = 0;

	scribe_image_info_t info;
	if (scribe_sniff_image(data, len, &info) != 0 || info.is_pdf)
		return -1;

	char sha[65];
	scribe_sha256_buf(data, len, sha);

	/* Deduplicate on SHA-256: never create duplicate immutable scans. */
	if (scribe_page_find_by_sha256(store, sha, out_page) == 0) {
		if (out_deduped)
			*out_deduped = 1;
		return 0;
	}

	const char *base_dir = scribe_store_dir(store);
	char bucket_dir[512];
	int bn = snprintf(bucket_dir, sizeof(bucket_dir),
	                  "%s/scans/%.2s", base_dir, sha);
	if (bn < 0 || (size_t)bn >= sizeof(bucket_dir))
		return -1;
	if (mkdir_p(bucket_dir) != 0)
		return -1;

	char rel_path[256];
	int rn = snprintf(rel_path, sizeof(rel_path),
	                  "scans/%.2s/%s.%s", sha, sha, info.ext);
	if (rn < 0 || (size_t)rn >= sizeof(rel_path))
		return -1;

	char abs_path[512];
	int an = snprintf(abs_path, sizeof(abs_path),
	                  "%s/%s", base_dir, rel_path);
	if (an < 0 || (size_t)an >= sizeof(abs_path))
		return -1;

	if (write_atomic(abs_path, data, len) != 0)
		return -1;

	scribe_page_t page;
	memset(&page, 0, sizeof(page));
	snprintf(page.id, sizeof(page.id), "pg_%.16s", sha);
	stl_strlcpy(page.sha256, sha, sizeof(page.sha256));
	stl_strlcpy(page.file_path, rel_path, sizeof(page.file_path));
	stl_strlcpy(page.mime, info.mime, sizeof(page.mime));
	if (book_ref)
		stl_strlcpy(page.book_ref, book_ref, sizeof(page.book_ref));
	page.page_num = page_num ? page_num : 1u;
	page.width = info.width;
	page.height = info.height;
	page.ingested_at = (int64_t)time(NULL);

	if (scribe_page_put(store, &page) != 0)
		return -1;

	*out_page = page;
	return 0;
}

static uint8_t *read_file_bytes(const char *path, size_t *out_len)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	long sz = ftell(fp);
	if (sz <= 0 || sz > 128L * 1024L * 1024L) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	uint8_t *buf = malloc((size_t)sz);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	size_t n = fread(buf, 1, (size_t)sz, fp);
	fclose(fp);
	if (n != (size_t)sz) {
		free(buf);
		return NULL;
	}
	*out_len = (size_t)sz;
	return buf;
}

static int cmp_str_ptr(const void *a, const void *b)
{
	const char *sa = *(const char *const *)a;
	const char *sb = *(const char *const *)b;
	return strcmp(sa, sb);
}

static void rm_rf_tmp(const char *dir_path)
{
	DIR *dir = opendir(dir_path);
	if (dir) {
		struct dirent *de;
		while ((de = readdir(dir)) != NULL) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;
			char p[512];
			int n = snprintf(p, sizeof(p), "%s/%s", dir_path, de->d_name);
			if (n > 0 && (size_t)n < sizeof(p))
				unlink(p);
		}
		closedir(dir);
	}
	rmdir(dir_path);
}

static int ingest_pdf_pages(scribe_store_t *store,
                            const char *src_path,
                            const char *book_ref,
                            uint32_t start_page_num,
                            uint32_t dpi,
                            scribe_page_t **out_pages,
                            size_t *out_count)
{
	char tmpdir[256];
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/scribe-pdf-XXXXXX");
	if (!mkdtemp(tmpdir))
		return -1;

	char prefix[300];
	snprintf(prefix, sizeof(prefix), "%s/page", tmpdir);

	char dpi_str[16];
	snprintf(dpi_str, sizeof(dpi_str), "%u", dpi ? dpi : 250u);

	pid_t pid = fork();
	if (pid < 0) {
		rm_rf_tmp(tmpdir);
		return -1;
	}
	if (pid == 0) {
		char *argv[] = {
			"pdftoppm", "-png", "-r", dpi_str,
			(char *)src_path, prefix, NULL
		};
		execvp("pdftoppm", argv);
		_exit(127);
	}

	int status = 0;
	if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		rm_rf_tmp(tmpdir);
		return -1;
	}

	DIR *dir = opendir(tmpdir);
	if (!dir) {
		rm_rf_tmp(tmpdir);
		return -1;
	}

	char **names = NULL;
	size_t n_names = 0, cap = 0;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		size_t nl = strlen(de->d_name);
		if (nl <= 4 || strcmp(de->d_name + nl - 4, ".png") != 0)
			continue;
		if (n_names == cap) {
			size_t ncap = cap ? cap * 2 : 8;
			char **next = realloc(names, ncap * sizeof(*next));
			if (!next)
				break;
			names = next;
			cap = ncap;
		}
		names[n_names] = malloc(nl + 1);
		if (!names[n_names])
			break;
		memcpy(names[n_names], de->d_name, nl + 1);
		n_names++;
	}
	closedir(dir);

	if (n_names == 0) {
		free(names);
		rm_rf_tmp(tmpdir);
		return -1;
	}

	qsort(names, n_names, sizeof(*names), cmp_str_ptr);

	scribe_page_t *pages = calloc(n_names, sizeof(*pages));
	if (!pages) {
		for (size_t i = 0; i < n_names; i++)
			free(names[i]);
		free(names);
		rm_rf_tmp(tmpdir);
		return -1;
	}

	uint32_t base_pnum = start_page_num ? start_page_num : 1u;
	size_t ingested = 0;
	int rc = 0;

	for (size_t i = 0; i < n_names; i++) {
		char img_path[512];
		snprintf(img_path, sizeof(img_path), "%s/%s", tmpdir, names[i]);

		size_t blen = 0;
		uint8_t *buf = read_file_bytes(img_path, &blen);
		if (!buf) {
			rc = -1;
			break;
		}
		int dedup = 0;
		int ir = scribe_ingest_buffer(store, buf, blen, book_ref,
		                              base_pnum + (uint32_t)i,
		                              &pages[ingested], &dedup);
		free(buf);
		if (ir != 0) {
			rc = -1;
			break;
		}
		ingested++;
	}

	for (size_t i = 0; i < n_names; i++)
		free(names[i]);
	free(names);
	rm_rf_tmp(tmpdir);

	if (rc != 0) {
		free(pages);
		return -1;
	}

	*out_pages = pages;
	*out_count = ingested;
	return 0;
}

int scribe_ingest_file(scribe_store_t *store,
                       const char *src_path,
                       const char *book_ref,
                       uint32_t start_page_num,
                       uint32_t dpi,
                       scribe_page_t **out_pages,
                       size_t *out_count)
{
	if (!store || !src_path || !out_pages || !out_count)
		return -1;
	*out_pages = NULL;
	*out_count = 0;

	size_t blen = 0;
	uint8_t *buf = read_file_bytes(src_path, &blen);
	if (!buf)
		return -1;

	scribe_image_info_t info;
	if (scribe_sniff_image(buf, blen, &info) != 0) {
		free(buf);
		return -1;
	}

	if (info.is_pdf) {
		free(buf);
		return ingest_pdf_pages(store, src_path, book_ref,
		                        start_page_num, dpi,
		                        out_pages, out_count);
	}

	scribe_page_t *page = calloc(1, sizeof(*page));
	if (!page) {
		free(buf);
		return -1;
	}
	int dedup = 0;
	int rc = scribe_ingest_buffer(store, buf, blen, book_ref,
	                              start_page_num ? start_page_num : 1u,
	                              page, &dedup);
	free(buf);
	if (rc != 0) {
		free(page);
		return -1;
	}

	*out_pages = page;
	*out_count = 1;
	return 0;
}
