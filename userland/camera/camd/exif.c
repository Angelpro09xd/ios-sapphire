/* Exif APP1 builder (little-endian TIFF), see exif.h */
#include "exif.h"
#include <string.h>

enum { T_BYTE = 1, T_ASCII = 2, T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5, T_UNDEF = 7 };

typedef struct {
	uint16_t tag, type;
	uint32_t count;
	const void *data; /* count items of the type */
	uint32_t inl;     /* SHORT/LONG value when data is NULL */
} Ent;

typedef struct {
	uint8_t *t; /* TIFF start */
	size_t n, cap;
	int bad;
} W;

static void put16(W *w, size_t at, unsigned v)
{
	if (at + 2 > w->cap) { w->bad = 1; return; }
	w->t[at] = v & 0xff;
	w->t[at + 1] = (v >> 8) & 0xff;
}

static void put32(W *w, size_t at, uint32_t v)
{
	if (at + 4 > w->cap) { w->bad = 1; return; }
	for (int i = 0; i < 4; i++) w->t[at + i] = (v >> (8 * i)) & 0xff;
}

static size_t tsize(int type) { return type == T_SHORT ? 2 : type == T_LONG ? 4 : type == T_RATIONAL ? 8 : 1; }

/* an IFD at the current end, its out-of-line data after it; returns the IFD's offset. *next_at
 * receives where the "next IFD" offset lives (0 written). */
static size_t ifd(W *w, const Ent *e, int n)
{
	size_t at = w->n, data = at + 2 + 12 * (size_t)n + 4;
	put16(w, at, (unsigned)n);
	for (int i = 0; i < n; i++) {
		size_t p = at + 2 + 12 * (size_t)i, len = tsize(e[i].type) * e[i].count;
		put16(w, p, e[i].tag);
		put16(w, p + 2, e[i].type);
		put32(w, p + 4, e[i].count);
		put32(w, p + 8, 0);
		if (!e[i].data) {
			if (e[i].type == T_SHORT) put16(w, p + 8, e[i].inl);
			else put32(w, p + 8, e[i].inl);
			continue;
		}
		uint8_t *dst;
		if (len <= 4) {
			if (p + 8 + 4 > w->cap) { w->bad = 1; continue; }
			dst = w->t + p + 8;
		} else {
			put32(w, p + 8, (uint32_t)data);
			if (data + len + 1 > w->cap) { w->bad = 1; continue; }
			dst = w->t + data;
			data += len + (len & 1);
		}
		if (e[i].type == T_RATIONAL) {
			const uint32_t *r = e[i].data;
			for (uint32_t k = 0; k < 2 * e[i].count; k++) put32(w, (size_t)(dst - w->t) + 4 * k, r[k]);
		} else if (e[i].type == T_SHORT) {
			const uint16_t *r = e[i].data;
			for (uint32_t k = 0; k < e[i].count; k++) put16(w, (size_t)(dst - w->t) + 2 * k, r[k]);
		} else {
			memcpy(dst, e[i].data, len);
		}
	}
	put32(w, at + 2 + 12 * (size_t)n, 0);
	w->n = data;
	return at;
}

size_t exif_build(uint8_t *buf, size_t cap, const ExifInfo *e)
{
	if (cap < 64) return 0;
	memcpy(buf, "\xff\xe1\0\0Exif\0\0", 10);
	W w = { buf + 10, 8, cap - 10 > 65000 ? 65000 : cap - 10, 0 };
	memcpy(w.t, "II*\0", 4);
	put32(&w, 4, 8);
	const char *make = e->make ? e->make : "Xiaomi", *model = e->model ? e->model : "Redmi Note 13";
	const char *sw = e->software ? e->software : "camd", *dt = e->datetime ? e->datetime : "2000:01:01 00:00:00";
	static const uint32_t res[2] = { 72, 1 };
	uint32_t exp[2] = { e->exposure_us, 1000000 }, fnum[2] = { 175, 100 };
	/* placeholder for the Exif IFD offset, patched below */
	Ent i0[] = {
		{ 0x010F, T_ASCII, (uint32_t)strlen(make) + 1, make, 0 },
		{ 0x0110, T_ASCII, (uint32_t)strlen(model) + 1, model, 0 },
		{ 0x0112, T_SHORT, 1, NULL, (uint32_t)(e->orientation ? e->orientation : 1) },
		{ 0x011A, T_RATIONAL, 1, res, 0 },
		{ 0x011B, T_RATIONAL, 1, res, 0 },
		{ 0x0128, T_SHORT, 1, NULL, 2 },
		{ 0x0131, T_ASCII, (uint32_t)strlen(sw) + 1, sw, 0 },
		{ 0x0132, T_ASCII, (uint32_t)strlen(dt) + 1, dt, 0 },
		{ 0x8769, T_LONG, 1, NULL, 0 },
	};
	int n0 = (int)(sizeof i0 / sizeof i0[0]);
	size_t at0 = ifd(&w, i0, n0);
	size_t exif_at = w.n;
	put32(&w, at0 + 2 + 12 * (size_t)(n0 - 1) + 8, (uint32_t)exif_at);
	Ent ex[10];
	int m = 0;
	if (e->exposure_us) ex[m++] = (Ent){ 0x829A, T_RATIONAL, 1, exp, 0 };
	ex[m++] = (Ent){ 0x829D, T_RATIONAL, 1, fnum, 0 };
	if (e->iso) ex[m++] = (Ent){ 0x8827, T_SHORT, 1, NULL, e->iso > 65535 ? 65535 : e->iso };
	ex[m++] = (Ent){ 0x9000, T_UNDEF, 4, "0232", 0 };
	ex[m++] = (Ent){ 0x9003, T_ASCII, (uint32_t)strlen(dt) + 1, dt, 0 };
	ex[m++] = (Ent){ 0x9004, T_ASCII, (uint32_t)strlen(dt) + 1, dt, 0 };
	ex[m++] = (Ent){ 0xA002, T_LONG, 1, NULL, e->width };
	ex[m++] = (Ent){ 0xA003, T_LONG, 1, NULL, e->height };
	ifd(&w, ex, m);
	if (w.bad) return 0;
	size_t seg = 2 + 6 + w.n; /* length field counts itself */
	buf[2] = (uint8_t)(seg >> 8);
	buf[3] = (uint8_t)(seg & 0xff);
	return 2 + seg;
}
