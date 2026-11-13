/*
 * sapphire-heic2png: convert the primary image of a HEIC/HEIF file to PNG.
 *
 *   sapphire-heic2png IN.heic OUT.png   -> writes OUT.png, prints "WxH alpha|noalpha"
 *   sapphire-heic2png --info IN.heic    -> prints "WxH alpha|noalpha" plus details on stderr-free lines
 *
 * Full resolution, transformations (irot/imir/clap) applied as libheif does by
 * default, 8 bits per channel. If the image has an alpha auxiliary image the
 * PNG is RGBA (straight alpha), otherwise RGB. An embedded ICC profile
 * (prof/rICC) is copied into an iCCP chunk. nclx-only files get no colour
 * chunk (untagged = sRGB for ImageIO); the WWDC_2022 assets are nclx with
 * unspecified primaries/transfer, so that is the right result for them.
 * --info prints what the file carries ("color: ...").
 *
 * Minimal PNG writer on top of zlib (IHDR, iCCP?, IDAT*, IEND), adaptive
 * per-row filtering.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libheif/heif.h>
#include <zlib.h>

static const char *prog = "sapphire-heic2png";

static int die(const char *what, const char *detail)
{
	fprintf(stderr, "%s: %s%s%s\n", prog, what, detail ? ": " : "", detail ? detail : "");
	return 1;
}

/* ---------------------------------------------------------------- PNG ---- */
struct pngw {
	FILE *f;
	int err;
};

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void chunk(struct pngw *w, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8], crcb[4];
	uLong crc = crc32(0L, (const Bytef *)type, 4);
	if (len) crc = crc32(crc, data, len);
	put32(hdr, len);
	memcpy(hdr + 4, type, 4);
	put32(crcb, (uint32_t)crc);
	if (fwrite(hdr, 1, 8, w->f) != 8 ||
	    (len && fwrite(data, 1, len, w->f) != len) ||
	    fwrite(crcb, 1, 4, w->f) != 4)
		w->err = 1;
}

static inline uint8_t paeth(int a, int b, int c)
{
	int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	if (pa <= pb && pa <= pc) return a;
	return pb <= pc ? b : c;
}

/* Filter one row with filter type ft into out (len bytes, bpp bytes/pixel). */
static void filter_row(int ft, const uint8_t *cur, const uint8_t *prev,
		       uint8_t *out, size_t len, int bpp)
{
	for (size_t i = 0; i < len; i++) {
		int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
		int b = prev ? prev[i] : 0;
		int c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
		int v = cur[i];
		switch (ft) {
		case 0: out[i] = v; break;
		case 1: out[i] = v - a; break;
		case 2: out[i] = v - b; break;
		case 3: out[i] = v - ((a + b) >> 1); break;
		default: out[i] = v - paeth(a, b, c); break;
		}
	}
}

static int write_png(const char *path, const uint8_t *pix, int stride,
		     int w, int h, int channels,
		     const uint8_t *icc, size_t icc_len)
{
	struct pngw pw = { 0 };
	size_t rowlen = (size_t)w * channels;
	uint8_t *rows = malloc(5 * (rowlen + 1)), *ibuf = NULL;
	uint8_t obuf[65536];
	z_stream zs;
	int ret = 1;

	if (!rows) return die("out of memory", NULL);
	pw.f = fopen(path, "wb");
	if (!pw.f) { free(rows); return die(path, strerror(errno)); }

	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	if (fwrite(sig, 1, 8, pw.f) != 8) pw.err = 1;

	uint8_t ihdr[13];
	put32(ihdr, w); put32(ihdr + 4, h);
	ihdr[8] = 8;                          /* bit depth */
	ihdr[9] = channels == 4 ? 6 : 2;      /* RGBA : RGB */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;   /* deflate, adaptive filter, no interlace */
	chunk(&pw, "IHDR", ihdr, 13);

	if (icc && icc_len) {
		/* iCCP: name\0 method(0) zlib(profile) */
		uLongf clen = compressBound(icc_len);
		static const char name[] = "ICC Profile";
		ibuf = malloc(sizeof(name) + 1 + clen);
		if (ibuf) {
			memcpy(ibuf, name, sizeof(name));   /* includes NUL */
			ibuf[sizeof(name)] = 0;
			if (compress2(ibuf + sizeof(name) + 1, &clen, icc, icc_len, 9) == Z_OK)
				chunk(&pw, "iCCP", ibuf, sizeof(name) + 1 + clen);
			free(ibuf); ibuf = NULL;
		}
	}

	memset(&zs, 0, sizeof(zs));
	if (deflateInit2(&zs, 9, Z_DEFLATED, 15, 9, Z_DEFAULT_STRATEGY) != Z_OK) {
		fclose(pw.f); free(rows);
		return die("deflateInit failed", NULL);
	}
	zs.next_out = obuf; zs.avail_out = sizeof(obuf);

	for (int y = 0; y < h; y++) {
		const uint8_t *cur = pix + (size_t)y * stride;
		const uint8_t *prev = y ? pix + (size_t)(y - 1) * stride : NULL;
		/* pick the filter with the minimum sum of absolute signed residuals */
		int best = 0; unsigned long bestsum = ~0UL;
		for (int ft = 0; ft < 5; ft++) {
			uint8_t *o = rows + ft * (rowlen + 1);
			o[0] = ft;
			filter_row(ft, cur, prev, o + 1, rowlen, channels);
			unsigned long s = 0;
			for (size_t i = 1; i <= rowlen; i++) s += abs((int8_t)o[i]);
			if (s < bestsum) { bestsum = s; best = ft; }
		}
		zs.next_in = rows + best * (rowlen + 1);
		zs.avail_in = rowlen + 1;
		while (zs.avail_in) {
			if (deflate(&zs, Z_NO_FLUSH) == Z_STREAM_ERROR) goto zfail;
			if (zs.avail_out == 0) {
				chunk(&pw, "IDAT", obuf, sizeof(obuf));
				zs.next_out = obuf; zs.avail_out = sizeof(obuf);
			}
		}
	}
	for (;;) {
		int r = deflate(&zs, Z_FINISH);
		if (r == Z_STREAM_ERROR) goto zfail;
		if (zs.avail_out < sizeof(obuf))
			chunk(&pw, "IDAT", obuf, sizeof(obuf) - zs.avail_out);
		zs.next_out = obuf; zs.avail_out = sizeof(obuf);
		if (r == Z_STREAM_END) break;
	}
	chunk(&pw, "IEND", NULL, 0);
	ret = 0;
	if (0) {
zfail:
		die("deflate failed", NULL);
	}
	deflateEnd(&zs);
	free(rows);
	if (fclose(pw.f) != 0) pw.err = 1;
	if (!ret && pw.err) { ret = die(path, "write error"); }
	if (ret) remove(path);
	return ret;
}

/* --------------------------------------------------------------- HEIF ---- */
static const char *fourcc(enum heif_color_profile_type t, char *buf)
{
	if (t == heif_color_profile_type_not_present) return "none";
	buf[0] = t >> 24; buf[1] = t >> 16; buf[2] = t >> 8; buf[3] = t; buf[4] = 0;
	return buf;
}

int main(int argc, char **argv)
{
	int info = 0;
	const char *in, *out = NULL;

	if (argc == 3 && !strcmp(argv[1], "--info")) {
		info = 1; in = argv[2];
	} else if (argc == 3 && argv[1][0] != '-') {
		in = argv[1]; out = argv[2];
	} else {
		fprintf(stderr, "usage: %s IN.heic OUT.png\n       %s --info IN.heic\n", prog, prog);
		return 2;
	}

	struct heif_context *ctx = heif_context_alloc();
	if (!ctx) return die("heif_context_alloc failed", NULL);
	struct heif_error e = heif_context_read_from_file(ctx, in, NULL);
	if (e.code) return die(in, e.message);

	struct heif_image_handle *hd = NULL;
	e = heif_context_get_primary_image_handle(ctx, &hd);
	if (e.code) return die("no primary image", e.message);

	int has_alpha = heif_image_handle_has_alpha_channel(hd);
	int w = heif_image_handle_get_width(hd), h = heif_image_handle_get_height(hd);
	enum heif_color_profile_type pt = heif_image_handle_get_color_profile_type(hd);
	size_t icc_len = 0;
	uint8_t *icc = NULL;
	if (pt == heif_color_profile_type_prof || pt == heif_color_profile_type_rICC) {
		icc_len = heif_image_handle_get_raw_color_profile_size(hd);
		icc = icc_len ? malloc(icc_len) : NULL;
		if (icc && heif_image_handle_get_raw_color_profile(hd, icc).code) {
			free(icc); icc = NULL; icc_len = 0;
		}
	}

	if (info) {
		char b[5];
		printf("%dx%d %s\n", w, h, has_alpha ? "alpha" : "noalpha");
		printf("images: %d  luma_bits: %d  chroma_bits: %d  premultiplied: %d  depth: %d\n",
		       heif_context_get_number_of_top_level_images(ctx),
		       heif_image_handle_get_luma_bits_per_pixel(hd),
		       heif_image_handle_get_chroma_bits_per_pixel(hd),
		       has_alpha ? heif_image_handle_is_premultiplied_alpha(hd) : 0,
		       heif_image_handle_has_depth_image(hd));
		printf("color: %s", fourcc(pt, b));
		if (icc_len) printf(" (ICC %zu bytes)", icc_len);
		struct heif_color_profile_nclx *nclx = NULL;
		if (heif_image_handle_get_nclx_color_profile(hd, &nclx).code == 0 && nclx) {
			printf(" nclx: primaries=%d transfer=%d matrix=%d full_range=%d",
			       nclx->color_primaries, nclx->transfer_characteristics,
			       nclx->matrix_coefficients, nclx->full_range_flag);
			heif_nclx_color_profile_free(nclx);
		}
		printf("\n");
		return 0;
	}

	struct heif_decoding_options *opt = heif_decoding_options_alloc();
	opt->convert_hdr_to_8bit = 1;
	struct heif_image *img = NULL;
	e = heif_decode_image(hd, &img, heif_colorspace_RGB,
			      has_alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB,
			      opt);
	heif_decoding_options_free(opt);
	if (e.code) return die("decode failed", e.message);

	int stride = 0;
	uint8_t *pix = heif_image_get_plane(img, heif_channel_interleaved, &stride);
	if (!pix) return die("decode returned no interleaved plane", NULL);
	int iw = heif_image_get_width(img, heif_channel_interleaved);
	int ih = heif_image_get_height(img, heif_channel_interleaved);
	if (heif_image_get_bits_per_pixel_range(img, heif_channel_interleaved) != 8)
		return die("unexpected bit depth after decode", NULL);
	int ch = has_alpha ? 4 : 3;

	/* PNG wants straight alpha. */
	if (has_alpha && heif_image_is_premultiplied_alpha(img)) {
		for (int y = 0; y < ih; y++) {
			uint8_t *p = pix + (size_t)y * stride;
			for (int x = 0; x < iw; x++, p += 4) {
				unsigned a = p[3];
				if (a && a < 255)
					for (int c = 0; c < 3; c++) {
						unsigned v = (p[c] * 255u + a / 2) / a;
						p[c] = v > 255 ? 255 : v;
					}
			}
		}
	}

	if (write_png(out, pix, stride, iw, ih, ch, icc, icc_len))
		return 1;
	printf("%dx%d %s\n", iw, ih, has_alpha ? "alpha" : "noalpha");

	heif_image_release(img);
	heif_image_handle_release(hd);
	heif_context_free(ctx);
	free(icc);
	return 0;
}
