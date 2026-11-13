/*
 * plistop: property lists changed in place, for the installer's recipes (in OrangeFox: no Python there).
 * Reads binary (bplist00) or XML plists and writes them back in the same format (or -b / -x).
 *
 *   plistop [-b|-x] FILE OP ARGS... [OP ARGS...]
 *     get KEYPATH                 print a string/number/bool value
 *     set KEYPATH TYPE VALUE      TYPE: bool string int real date(seconds since 2001) data(base64) dict array
 *     merge PLIST                 the root dict of PLIST merged into FILE's (dicts deeply)
 *     put KEYPATH PLIST [KEY]     KEYPATH set to PLIST's root (or to its KEY)
 *     del KEYPATH                 removed if present
 *     rename-each KEYPATH OLD NEW in every dict inside KEYPATH: key OLD renamed NEW
 *     del-each KEYPATH KEY VALUE  dicts inside KEYPATH whose KEY is the string VALUE: removed
 * KEYPATH: dict keys joined by ':' ("" is the root); missing dicts are made by set/put. FILE missing:
 * starts as an empty dict (written as binary unless -x). Exit 1 and nothing written on an error.
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { T_DICT, T_ARRAY, T_STRING, T_INT, T_REAL, T_BOOL, T_DATE, T_DATA, T_UID };

struct node {
	int type;
	char *s;                        /* string (UTF-8), data bytes */
	size_t len;                     /* data length */
	int64_t i;                      /* int, bool, uid */
	double r;                       /* real, date */
	size_t n, cap;                  /* dict: keys[i] -> v[i]; array: v[i] */
	char **keys;
	struct node **v;
};

static void
die(const char *fmt, const char *a)
{
	fprintf(stderr, "plistop: ");
	fprintf(stderr, fmt, a);
	fputc('\n', stderr);
	exit(1);
}

static void *
xalloc(size_t n)
{
	void *p = calloc(1, n ? n : 1);
	if (!p) {
		die("out of memory%s", "");
	}
	return p;
}

static struct node *
mk(int type)
{
	struct node *n = xalloc(sizeof(*n));
	n->type = type;
	return n;
}

static void
push(struct node *c, char *key, struct node *v)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 8;
		c->v = realloc(c->v, c->cap * sizeof(*c->v));
		c->keys = realloc(c->keys, c->cap * sizeof(*c->keys));
		if (!c->v || !c->keys) {
			die("out of memory%s", "");
		}
	}
	c->keys[c->n] = key;
	c->v[c->n++] = v;
}

static int
dict_find(const struct node *d, const char *k)
{
	for (size_t i = 0; i < d->n; i++) {
		if (!strcmp(d->keys[i], k)) {
			return (int)i;
		}
	}
	return -1;
}

static void
dict_set(struct node *d, const char *k, struct node *v)
{
	int i = dict_find(d, k);
	if (i >= 0) {
		d->v[i] = v;
	} else {
		push(d, strdup(k), v);
	}
}

static void
dict_del(struct node *d, size_t i)
{
	memmove(d->keys + i, d->keys + i + 1, (d->n - i - 1) * sizeof(*d->keys));
	memmove(d->v + i, d->v + i + 1, (d->n - i - 1) * sizeof(*d->v));
	d->n--;
}

/* ---------------- binary ---------------- */

static const uint8_t *B;
static size_t BL;
static int osz, rsz;
static uint64_t nobj, otab;

static uint64_t
be(const uint8_t *p, int n)
{
	uint64_t v = 0;
	for (int i = 0; i < n; i++) {
		v = v << 8 | p[i];
	}
	return v;
}

static struct node *bparse(uint64_t ref, int depth);

static uint64_t
bcount(uint8_t marker, size_t *off)
{
	uint64_t c = marker & 0xf;
	if (c == 0xf) {
		if (*off >= BL || (B[*off] & 0xf0) != 0x10) {
			die("bad binary plist (count)%s", "");
		}
		int n = 1 << (B[*off] & 0xf);
		if (*off + 1 + n > BL) {
			die("bad binary plist%s", "");
		}
		c = be(B + *off + 1, n);
		*off += 1 + n;
	}
	return c;
}

static char *
utf16_to_utf8(const uint8_t *p, uint64_t n)
{
	char *o = xalloc(n * 3 + 1), *q = o;
	for (uint64_t i = 0; i < n; i++) {
		uint32_t c = (uint32_t)(p[2 * i] << 8 | p[2 * i + 1]);
		if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) {
			uint32_t d = (uint32_t)(p[2 * i + 2] << 8 | p[2 * i + 3]);
			if (d >= 0xdc00 && d < 0xe000) {
				c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
				i++;
			}
		}
		if (c < 0x80) {
			*q++ = (char)c;
		} else if (c < 0x800) {
			*q++ = (char)(0xc0 | c >> 6);
			*q++ = (char)(0x80 | (c & 0x3f));
		} else if (c < 0x10000) {
			*q++ = (char)(0xe0 | c >> 12);
			*q++ = (char)(0x80 | (c >> 6 & 0x3f));
			*q++ = (char)(0x80 | (c & 0x3f));
		} else {
			*q++ = (char)(0xf0 | c >> 18);
			*q++ = (char)(0x80 | (c >> 12 & 0x3f));
			*q++ = (char)(0x80 | (c >> 6 & 0x3f));
			*q++ = (char)(0x80 | (c & 0x3f));
		}
	}
	*q = 0;
	return o;
}

static struct node *
bparse(uint64_t ref, int depth)
{
	if (ref >= nobj || depth > 64) {
		die("bad binary plist (reference)%s", "");
	}
	size_t off = (size_t)be(B + otab + ref * osz, osz);
	if (off >= BL) {
		die("bad binary plist (offset)%s", "");
	}
	uint8_t m = B[off++];
	struct node *n;
	uint64_t c;
	switch (m >> 4) {
	case 0x0:
		n = mk(T_BOOL);
		n->i = m == 0x09;
		return n;
	case 0x1: {
		int len = 1 << (m & 0xf);
		if (off + len > BL) {
			die("bad binary plist%s", "");
		}
		n = mk(T_INT);
		n->i = (int64_t)be(B + off + (len > 8 ? len - 8 : 0), len > 8 ? 8 : len);
		return n;
	}
	case 0x2: {
		int len = 1 << (m & 0xf);
		n = mk(T_REAL);
		uint64_t v = be(B + off, len);
		if (len == 4) {
			float f;
			uint32_t w = (uint32_t)v;
			memcpy(&f, &w, 4);
			n->r = f;
		} else {
			memcpy(&n->r, &v, 8);
		}
		return n;
	}
	case 0x3: {
		n = mk(T_DATE);
		uint64_t v = be(B + off, 8);
		memcpy(&n->r, &v, 8);
		return n;
	}
	case 0x4:
		c = bcount(m, &off);
		if (off + c > BL) {
			die("bad binary plist (data)%s", "");
		}
		n = mk(T_DATA);
		n->s = xalloc(c);
		memcpy(n->s, B + off, c);
		n->len = c;
		return n;
	case 0x5:
		c = bcount(m, &off);
		if (off + c > BL) {
			die("bad binary plist (string)%s", "");
		}
		n = mk(T_STRING);
		n->s = xalloc(c + 1);
		memcpy(n->s, B + off, c);
		return n;
	case 0x6:
		c = bcount(m, &off);
		if (off + 2 * c > BL) {
			die("bad binary plist (string)%s", "");
		}
		n = mk(T_STRING);
		n->s = utf16_to_utf8(B + off, c);
		return n;
	case 0x8:
		n = mk(T_UID);
		n->i = (int64_t)be(B + off, (m & 0xf) + 1);
		return n;
	case 0xa:
		c = bcount(m, &off);
		n = mk(T_ARRAY);
		for (uint64_t i = 0; i < c; i++) {
			push(n, NULL, bparse(be(B + off + i * rsz, rsz), depth + 1));
		}
		return n;
	case 0xd:
		c = bcount(m, &off);
		n = mk(T_DICT);
		for (uint64_t i = 0; i < c; i++) {
			struct node *k = bparse(be(B + off + i * rsz, rsz), depth + 1);
			if (k->type != T_STRING) {
				die("bad binary plist (dict key)%s", "");
			}
			push(n, k->s, bparse(be(B + off + (c + i) * rsz, rsz), depth + 1));
		}
		return n;
	}
	die("bad binary plist (object type)%s", "");
	return NULL;
}

static struct node *
parse_binary(const uint8_t *d, size_t len)
{
	if (len < 40) {
		die("bad binary plist%s", "");
	}
	B = d;
	BL = len;
	const uint8_t *t = d + len - 32;
	osz = t[6];
	rsz = t[7];
	nobj = be(t + 8, 8);
	uint64_t top = be(t + 16, 8);
	otab = be(t + 24, 8);
	if (osz < 1 || osz > 8 || rsz < 1 || rsz > 8 || otab + nobj * osz > len - 32) {
		die("bad binary plist (trailer)%s", "");
	}
	return bparse(top, 0);
}

/* writer: objects numbered depth-first, no sharing (plists here are small) */
struct out {
	uint8_t *p;
	size_t n, cap;
};

static void
emit(struct out *o, const void *d, size_t n)
{
	if (o->n + n > o->cap) {
		o->cap = (o->n + n) * 2;
		o->p = realloc(o->p, o->cap);
		if (!o->p) {
			die("out of memory%s", "");
		}
	}
	memcpy(o->p + o->n, d, n);
	o->n += n;
}

static void
emit_be(struct out *o, uint64_t v, int n)
{
	uint8_t b[8];
	for (int i = 0; i < n; i++) {
		b[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
	}
	emit(o, b, n);
}

static uint64_t nobjs;
static uint64_t *offs;
static int ref_size;

static uint64_t
count_objs(const struct node *n)
{
	uint64_t c = 1;
	for (size_t i = 0; i < n->n && (n->type == T_DICT || n->type == T_ARRAY); i++) {
		c += count_objs(n->v[i]) + (n->type == T_DICT);
	}
	return c;
}

static void
emit_marker(struct out *o, uint8_t type, uint64_t count)
{
	if (count < 15) {
		uint8_t m = (uint8_t)(type << 4 | count);
		emit(o, &m, 1);
	} else {
		uint8_t m = (uint8_t)(type << 4 | 0xf);
		emit(o, &m, 1);
		int n = count < 256 ? 0 : count < 65536 ? 1 : count < 4294967296ull ? 2 : 3;
		uint8_t im = (uint8_t)(0x10 | n);
		emit(o, &im, 1);
		emit_be(o, count, 1 << n);
	}
}

static int
is_ascii(const char *s)
{
	for (; *s; s++) {
		if ((uint8_t)*s >= 0x80) {
			return 0;
		}
	}
	return 1;
}

static void
emit_string(struct out *o, const char *s)
{
	if (is_ascii(s)) {
		emit_marker(o, 0x5, strlen(s));
		emit(o, s, strlen(s));
		return;
	}
	/* UTF-16BE */
	size_t cap = strlen(s) * 2 + 2, n = 0;
	uint16_t *u = xalloc(cap * sizeof(*u));
	const uint8_t *p = (const uint8_t *)s;
	while (*p) {
		uint32_t c;
		if (*p < 0x80) {
			c = *p++;
		} else if ((*p & 0xe0) == 0xc0 && p[1]) {
			c = (uint32_t)(*p & 0x1f) << 6 | (p[1] & 0x3f);
			p += 2;
		} else if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) {
			c = (uint32_t)(*p & 0x0f) << 12 | (uint32_t)(p[1] & 0x3f) << 6 | (p[2] & 0x3f);
			p += 3;
		} else if (p[1] && p[2] && p[3]) {
			c = (uint32_t)(*p & 0x07) << 18 | (uint32_t)(p[1] & 0x3f) << 12 | (uint32_t)(p[2] & 0x3f) << 6 | (p[3] & 0x3f);
			p += 4;
		} else {
			die("bad UTF-8 in a string%s", "");
			return;
		}
		if (c >= 0x10000) {
			c -= 0x10000;
			u[n++] = (uint16_t)(0xd800 + (c >> 10));
			u[n++] = (uint16_t)(0xdc00 + (c & 0x3ff));
		} else {
			u[n++] = (uint16_t)c;
		}
	}
	emit_marker(o, 0x6, n);
	for (size_t i = 0; i < n; i++) {
		emit_be(o, u[i], 2);
	}
	free(u);
}

/* writes N (object number *next) and its children; returns its number */
static uint64_t
bwrite(struct out *o, const struct node *n, uint64_t *next)
{
	uint64_t me = (*next)++;
	uint64_t *kids = NULL, *keys = NULL;
	if (n->type == T_DICT || n->type == T_ARRAY) {
		kids = xalloc(n->n * sizeof(*kids));
		keys = xalloc(n->n * sizeof(*keys));
		/* key strings first, then values (numbers only; written after this object) */
	}
	/* the children are written after this object, so their numbers are known only then: number them now */
	uint64_t mark = *next;
	if (n->type == T_DICT) {
		for (size_t i = 0; i < n->n; i++) {
			keys[i] = mark++;
		}
	}
	uint64_t base = mark;
	(void)base;
	offs[me] = o->n;
	switch (n->type) {
	case T_BOOL: {
		uint8_t m = n->i ? 0x09 : 0x08;
		emit(o, &m, 1);
		break;
	}
	case T_INT: {
		int64_t v = n->i;
		if (v >= 0 && v < 256) {
			emit_marker(o, 0x1, 0);
			emit_be(o, (uint64_t)v, 1);
		} else if (v >= 0 && v < 65536) {
			emit_marker(o, 0x1, 1);
			emit_be(o, (uint64_t)v, 2);
		} else if (v >= 0 && v < 4294967296ll) {
			emit_marker(o, 0x1, 2);
			emit_be(o, (uint64_t)v, 4);
		} else {
			emit_marker(o, 0x1, 3);
			emit_be(o, (uint64_t)v, 8);
		}
		break;
	}
	case T_REAL: {
		uint64_t v;
		memcpy(&v, &n->r, 8);
		emit_marker(o, 0x2, 3);
		emit_be(o, v, 8);
		break;
	}
	case T_DATE: {
		uint64_t v;
		memcpy(&v, &n->r, 8);
		uint8_t m = 0x33;
		emit(o, &m, 1);
		emit_be(o, v, 8);
		break;
	}
	case T_DATA:
		emit_marker(o, 0x4, n->len);
		emit(o, n->s, n->len);
		break;
	case T_STRING:
		emit_string(o, n->s);
		break;
	case T_UID: {
		int len = n->i < 256 ? 1 : n->i < 65536 ? 2 : 4;
		uint8_t m = (uint8_t)(0x80 | (len - 1));
		emit(o, &m, 1);
		emit_be(o, (uint64_t)n->i, len);
		break;
	}
	case T_ARRAY:
	case T_DICT: {
		/* the child numbers: depth-first after the keys, so each child's subtree size decides the next */
		uint64_t c = mark;
		for (size_t i = 0; i < n->n; i++) {
			kids[i] = c;
			c += count_objs(n->v[i]);
		}
		emit_marker(o, n->type == T_DICT ? 0xd : 0xa, n->n);
		if (n->type == T_DICT) {
			for (size_t i = 0; i < n->n; i++) {
				emit_be(o, keys[i], ref_size);
			}
		}
		for (size_t i = 0; i < n->n; i++) {
			emit_be(o, kids[i], ref_size);
		}
		*next = mark;
		if (n->type == T_DICT) {
			for (size_t i = 0; i < n->n; i++) {
				offs[keys[i]] = o->n;
				emit_string(o, n->keys[i]);
			}
		}
		for (size_t i = 0; i < n->n; i++) {
			uint64_t got = bwrite(o, n->v[i], next);
			if (got != kids[i]) {
				die("internal numbering error%s", "");
			}
		}
		break;
	}
	}
	free(kids);
	free(keys);
	return me;
}

static void
write_binary(struct out *o, const struct node *root)
{
	nobjs = count_objs(root);
	ref_size = nobjs < 256 ? 1 : nobjs < 65536 ? 2 : 4;
	offs = xalloc(nobjs * sizeof(*offs));
	emit(o, "bplist00", 8);
	uint64_t next = 0;
	bwrite(o, root, &next);
	if (next != nobjs) {
		die("internal count error%s", "");
	}
	uint64_t ot = o->n;
	int os = ot < 256 ? 1 : ot < 65536 ? 2 : ot < 4294967296ull ? 4 : 8;
	for (uint64_t i = 0; i < nobjs; i++) {
		emit_be(o, offs[i], os);
	}
	uint8_t tr[32] = {0};
	tr[6] = (uint8_t)os;
	tr[7] = (uint8_t)ref_size;
	for (int i = 0; i < 8; i++) {
		tr[8 + i] = (uint8_t)(nobjs >> (8 * (7 - i)));
		tr[24 + i] = (uint8_t)(ot >> (8 * (7 - i)));
	}
	emit(o, tr, 32);
}

/* ---------------- XML ---------------- */

static const char *X;

static void
skip_ws(void)
{
	for (;;) {
		while (*X == ' ' || *X == '\t' || *X == '\n' || *X == '\r') {
			X++;
		}
		if (!strncmp(X, "<!--", 4)) {
			const char *e = strstr(X, "-->");
			X = e ? e + 3 : X + strlen(X);
		} else if (!strncmp(X, "<?", 2) || !strncmp(X, "<!", 2)) {
			const char *e = strchr(X, '>');
			X = e ? e + 1 : X + strlen(X);
		} else {
			return;
		}
	}
}

/* the next tag's name into T; returns 1 open, 2 close, 3 empty (<x/>) */
static int
tag(char *t, size_t tsz)
{
	skip_ws();
	if (*X != '<') {
		die("bad XML plist near: %.20s", X);
	}
	X++;
	int close = *X == '/';
	if (close) {
		X++;
	}
	size_t n = 0;
	while (*X && *X != '>' && *X != ' ' && *X != '/' && n + 1 < tsz) {
		t[n++] = *X++;
	}
	t[n] = 0;
	while (*X && *X != '>' && *X != '/') {
		X++;                    /* attributes (plist version="1.0") */
	}
	int empty = *X == '/';
	while (*X && *X != '>') {
		X++;
	}
	if (*X) {
		X++;
	}
	return close ? 2 : empty ? 3 : 1;
}

static char *
text_until_close(const char *name)
{
	char end[32];
	snprintf(end, sizeof(end), "</%s>", name);
	const char *e = strstr(X, end);
	if (!e) {
		die("bad XML plist: no %s", end);
	}
	char *o = xalloc((size_t)(e - X) + 1), *q = o;
	for (const char *p = X; p < e;) {
		if (*p == '&') {
			if (!strncmp(p, "&amp;", 5)) { *q++ = '&'; p += 5; }
			else if (!strncmp(p, "&lt;", 4)) { *q++ = '<'; p += 4; }
			else if (!strncmp(p, "&gt;", 4)) { *q++ = '>'; p += 4; }
			else if (!strncmp(p, "&quot;", 6)) { *q++ = '"'; p += 6; }
			else if (!strncmp(p, "&apos;", 6)) { *q++ = '\''; p += 6; }
			else if (p[1] == '#') {
				unsigned long c = p[2] == 'x' ? strtoul(p + 3, NULL, 16) : strtoul(p + 2, NULL, 10);
				const char *sc = strchr(p, ';');
				p = sc ? sc + 1 : p + 2;
				if (c < 0x80) { *q++ = (char)c; }
				else if (c < 0x800) { *q++ = (char)(0xc0 | c >> 6); *q++ = (char)(0x80 | (c & 0x3f)); }
				else { *q++ = (char)(0xe0 | c >> 12); *q++ = (char)(0x80 | (c >> 6 & 0x3f)); *q++ = (char)(0x80 | (c & 0x3f)); }
			} else {
				*q++ = *p++;
			}
		} else {
			*q++ = *p++;
		}
	}
	*q = 0;
	X = e + strlen(end);
	return o;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *
b64_decode(const char *s, size_t *len)
{
	char *o = xalloc(strlen(s) * 3 / 4 + 4);
	uint32_t acc = 0;
	int bits = 0;
	size_t n = 0;
	for (; *s; s++) {
		const char *p = strchr(b64, *s);
		if (!p || !*s) {
			continue;       /* whitespace, '=' */
		}
		acc = acc << 6 | (uint32_t)(p - b64);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			o[n++] = (char)(acc >> bits & 0xff);
		}
	}
	*len = n;
	return o;
}

/* days from 1970-01-01 to Y-M-D (proleptic Gregorian) */
static int64_t
days_civil(int64_t y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

#define EPOCH_2001 978307200.0

static struct node *
xparse(int depth)
{
	char t[32];
	int k = tag(t, sizeof(t));
	if (depth > 64 || k == 2) {
		die("bad XML plist at <%s>", t);
	}
	struct node *n;
	if (!strcmp(t, "dict")) {
		n = mk(T_DICT);
		if (k == 3) {
			return n;
		}
		for (;;) {
			const char *save = X;
			char kt[32];
			int kk = tag(kt, sizeof(kt));
			if (kk == 2 && !strcmp(kt, "dict")) {
				return n;
			}
			if (strcmp(kt, "key")) {
				X = save;
				die("bad XML plist: <%s> where a key goes", kt);
			}
			char *key = kk == 3 ? strdup("") : text_until_close("key");
			push(n, key, xparse(depth + 1));
		}
	}
	if (!strcmp(t, "array")) {
		n = mk(T_ARRAY);
		if (k == 3) {
			return n;
		}
		for (;;) {
			const char *save = X;
			char at[32];
			if (tag(at, sizeof(at)) == 2 && !strcmp(at, "array")) {
				return n;
			}
			X = save;
			push(n, NULL, xparse(depth + 1));
		}
	}
	if (!strcmp(t, "true") || !strcmp(t, "false")) {
		n = mk(T_BOOL);
		n->i = t[0] == 't';
		if (k == 1) {
			free(text_until_close(t));
		}
		return n;
	}
	char *txt = k == 3 ? strdup("") : text_until_close(t);
	if (!strcmp(t, "string")) {
		n = mk(T_STRING);
		n->s = txt;
	} else if (!strcmp(t, "integer")) {
		n = mk(T_INT);
		n->i = strtoll(txt, NULL, 0);
	} else if (!strcmp(t, "real")) {
		n = mk(T_REAL);
		n->r = strtod(txt, NULL);
	} else if (!strcmp(t, "data")) {
		n = mk(T_DATA);
		n->s = b64_decode(txt, &n->len);
	} else if (!strcmp(t, "date")) {
		int y = 2001, mo = 1, d = 1, h = 0, mi = 0, s = 0;
		sscanf(txt, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s);
		n = mk(T_DATE);
		n->r = (double)(days_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s) - EPOCH_2001;
	} else {
		die("unknown XML plist element <%s>", t);
		return NULL;
	}
	return n;
}

static struct node *
parse_xml(const char *d)
{
	X = d;
	char t[32];
	skip_ws();
	const char *save = X;
	if (tag(t, sizeof(t)) != 1 || strcmp(t, "plist")) {
		X = save;               /* a bare value */
	}
	return xparse(0);
}

static void
xesc(struct out *o, const char *s)
{
	for (; *s; s++) {
		if (*s == '&') emit(o, "&amp;", 5);
		else if (*s == '<') emit(o, "&lt;", 4);
		else if (*s == '>') emit(o, "&gt;", 4);
		else emit(o, s, 1);
	}
}

static void
indent(struct out *o, int d)
{
	for (int i = 0; i < d; i++) {
		emit(o, "\t", 1);
	}
}

static void
xwrite(struct out *o, const struct node *n, int d)
{
	char buf[64];
	indent(o, d);
	switch (n->type) {
	case T_DICT:
		if (!n->n) {
			emit(o, "<dict/>\n", 8);
			return;
		}
		emit(o, "<dict>\n", 7);
		for (size_t i = 0; i < n->n; i++) {
			indent(o, d + 1);
			emit(o, "<key>", 5);
			xesc(o, n->keys[i]);
			emit(o, "</key>\n", 7);
			xwrite(o, n->v[i], d + 1);
		}
		indent(o, d);
		emit(o, "</dict>\n", 8);
		return;
	case T_ARRAY:
		if (!n->n) {
			emit(o, "<array/>\n", 9);
			return;
		}
		emit(o, "<array>\n", 8);
		for (size_t i = 0; i < n->n; i++) {
			xwrite(o, n->v[i], d + 1);
		}
		indent(o, d);
		emit(o, "</array>\n", 9);
		return;
	case T_STRING:
		emit(o, "<string>", 8);
		xesc(o, n->s);
		emit(o, "</string>\n", 10);
		return;
	case T_INT:
	case T_UID:
		snprintf(buf, sizeof(buf), "<integer>%lld</integer>\n", (long long)n->i);
		emit(o, buf, strlen(buf));
		return;
	case T_REAL:
		snprintf(buf, sizeof(buf), "<real>%.17g</real>\n", n->r);
		emit(o, buf, strlen(buf));
		return;
	case T_BOOL:
		emit(o, n->i ? "<true/>\n" : "<false/>\n", n->i ? 8 : 9);
		return;
	case T_DATE: {
		time_t t = (time_t)llround(n->r + EPOCH_2001);
		struct tm tm;
		gmtime_r(&t, &tm);
		strftime(buf, sizeof(buf), "<date>%Y-%m-%dT%H:%M:%SZ</date>\n", &tm);
		emit(o, buf, strlen(buf));
		return;
	}
	case T_DATA: {
		emit(o, "<data>", 6);
		const uint8_t *p = (const uint8_t *)n->s;
		for (size_t i = 0; i < n->len; i += 3) {
			uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n->len ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n->len ? p[i + 2] : 0);
			char c[4] = {b64[v >> 18], b64[v >> 12 & 63], i + 1 < n->len ? b64[v >> 6 & 63] : '=', i + 2 < n->len ? b64[v & 63] : '='};
			emit(o, c, 4);
		}
		emit(o, "</data>\n", 8);
		return;
	}
	}
}

static void
write_xml(struct out *o, const struct node *root)
{
	const char *h = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\">\n";
	emit(o, h, strlen(h));
	xwrite(o, root, 0);
	emit(o, "</plist>\n", 9);
}

/* ---------------- files and key paths ---------------- */

static struct node *
load(const char *path, int *binary, int must)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		if (must || errno != ENOENT) {
			die("%s: cannot read", path);
		}
		*binary = 1;
		return mk(T_DICT);
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = xalloc((size_t)len + 1);
	if (fread(d, 1, (size_t)len, f) != (size_t)len) {
		die("%s: cannot read", path);
	}
	fclose(f);
	if (len >= 8 && !memcmp(d, "bplist00", 8)) {
		*binary = 1;
		return parse_binary(d, (size_t)len);
	}
	*binary = 0;
	if (len >= 3 && d[0] == 0xef && d[1] == 0xbb && d[2] == 0xbf) {
		return parse_xml((char *)d + 3);
	}
	return parse_xml((char *)d);
}

/* the node at KEYPATH; MAKE: missing dicts made on the way. NULL if absent. */
static struct node *
at(struct node *root, const char *keypath, int make)
{
	struct node *n = root;
	if (!*keypath) {
		return n;
	}
	char *kp = strdup(keypath), *save = NULL;
	for (char *k = strtok_r(kp, ":", &save); k; k = strtok_r(NULL, ":", &save)) {
		if (n->type != T_DICT) {
			die("%s: not a dict on the way", keypath);
		}
		int i = dict_find(n, k);
		if (i < 0) {
			if (!make) {
				free(kp);
				return NULL;
			}
			struct node *d = mk(T_DICT);
			push(n, strdup(k), d);
			n = d;
		} else {
			n = n->v[i];
		}
	}
	free(kp);
	return n;
}

/* the parent dict of KEYPATH (made if MAKE) and its last key in *last */
static struct node *
parent_of(struct node *root, const char *keypath, char **last, int make)
{
	char *kp = strdup(keypath);
	char *c = strrchr(kp, ':');
	struct node *p;
	if (c) {
		*c = 0;
		p = at(root, kp, make);
		*last = strdup(c + 1);
	} else {
		p = root;
		*last = strdup(kp);
	}
	free(kp);
	if (p && p->type != T_DICT) {
		die("%s: parent is not a dict", keypath);
	}
	return p;
}

static void
merge(struct node *dst, const struct node *src)
{
	for (size_t i = 0; i < src->n; i++) {
		int j = dict_find(dst, src->keys[i]);
		if (j >= 0 && dst->v[j]->type == T_DICT && src->v[i]->type == T_DICT) {
			merge(dst->v[j], src->v[i]);
		} else {
			dict_set(dst, src->keys[i], src->v[i]);
		}
	}
}

int
main(int argc, char **argv)
{
	int force = 0;                  /* 'b' or 'x' */
	int a = 1;
	if (a < argc && (!strcmp(argv[a], "-b") || !strcmp(argv[a], "-x"))) {
		force = argv[a][1];
		a++;
	}
	if (argc - a < 2) {
		fprintf(stderr, "usage: plistop [-b|-x] FILE OP ARGS... (see the source's header)\n");
		return 2;
	}
	const char *file = argv[a++];
	int binary;
	struct node *root = load(file, &binary, 0);
	int changed = 0;
	while (a < argc) {
		const char *op = argv[a++];
#define NEED(k) if (argc - a < (k)) die("%s: missing arguments", op)
		if (!strcmp(op, "get")) {
			NEED(1);
			struct node *n = at(root, argv[a++], 0);
			if (!n) {
				return 1;
			}
			if (n->type == T_STRING) printf("%s\n", n->s);
			else if (n->type == T_INT) printf("%lld\n", (long long)n->i);
			else if (n->type == T_BOOL) printf("%s\n", n->i ? "true" : "false");
			else if (n->type == T_REAL) printf("%.17g\n", n->r);
			else printf("(%s)\n", n->type == T_DICT ? "dict" : n->type == T_ARRAY ? "array" : "data");
		} else if (!strcmp(op, "set")) {
			NEED(3);
			const char *kp = argv[a], *ty = argv[a + 1], *val = argv[a + 2];
			a += 3;
			struct node *v;
			if (!strcmp(ty, "bool")) { v = mk(T_BOOL); v->i = !strcmp(val, "true") || !strcmp(val, "1") || !strcmp(val, "yes"); }
			else if (!strcmp(ty, "string")) { v = mk(T_STRING); v->s = strdup(val); }
			else if (!strcmp(ty, "int")) { v = mk(T_INT); v->i = strtoll(val, NULL, 0); }
			else if (!strcmp(ty, "real")) { v = mk(T_REAL); v->r = strtod(val, NULL); }
			else if (!strcmp(ty, "date")) { v = mk(T_DATE); v->r = strtod(val, NULL); }
			else if (!strcmp(ty, "data")) { v = mk(T_DATA); v->s = b64_decode(val, &v->len); }
			else if (!strcmp(ty, "dict")) { v = mk(T_DICT); }
			else if (!strcmp(ty, "array")) { v = mk(T_ARRAY); }
			else { die("unknown type %s", ty); return 1; }
			char *last;
			struct node *p = parent_of(root, kp, &last, 1);
			dict_set(p, last, v);
			changed = 1;
		} else if (!strcmp(op, "merge")) {
			NEED(1);
			int b2;
			struct node *src = load(argv[a++], &b2, 1);
			if (src->type != T_DICT || root->type != T_DICT) {
				die("merge: both roots must be dicts%s", "");
			}
			merge(root, src);
			changed = 1;
		} else if (!strcmp(op, "put")) {
			NEED(2);
			const char *kp = argv[a];
			int b2;
			struct node *src = load(argv[a + 1], &b2, 1);
			a += 2;
			if (a < argc && strcmp(argv[a], "get") && strcmp(argv[a], "set") && strcmp(argv[a], "merge") && strcmp(argv[a], "put") &&
			    strcmp(argv[a], "del") && strcmp(argv[a], "rename-each") && strcmp(argv[a], "del-each")) {
				src = at(src, argv[a++], 0);
				if (!src) {
					die("put: no such key in the source%s", "");
				}
			}
			char *last;
			struct node *p = parent_of(root, kp, &last, 1);
			dict_set(p, last, src);
			changed = 1;
		} else if (!strcmp(op, "del")) {
			NEED(1);
			char *last;
			struct node *p = parent_of(root, argv[a++], &last, 0);
			int i = p ? dict_find(p, last) : -1;
			if (i >= 0) {
				dict_del(p, (size_t)i);
				changed = 1;
			}
		} else if (!strcmp(op, "rename-each")) {
			NEED(3);
			struct node *c = at(root, argv[a], 0);
			const char *old = argv[a + 1], *nw = argv[a + 2];
			a += 3;
			int k = 0;
			for (size_t i = 0; c && i < c->n; i++) {
				struct node *d = c->v[i];
				int j = d->type == T_DICT ? dict_find(d, old) : -1;
				if (j >= 0 && dict_find(d, nw) < 0) {
					d->keys[j] = strdup(nw);
					k++;
				}
			}
			fprintf(stderr, "plistop: %s renamed in %d\n", old, k);
			changed = 1;
		} else if (!strcmp(op, "del-each")) {
			NEED(3);
			struct node *c = at(root, argv[a], 0);
			const char *key = argv[a + 1], *val = argv[a + 2];
			a += 3;
			int k = 0;
			for (size_t i = 0; c && i < c->n;) {
				struct node *d = c->v[i];
				int j = d->type == T_DICT ? dict_find(d, key) : -1;
				if (j >= 0 && d->v[j]->type == T_STRING && !strcmp(d->v[j]->s, val)) {
					if (c->type == T_DICT) {
						dict_del(c, i);
					} else {
						memmove(c->v + i, c->v + i + 1, (c->n - i - 1) * sizeof(*c->v));
						c->n--;
					}
					k++;
				} else {
					i++;
				}
			}
			fprintf(stderr, "plistop: %d removed (%s = %s)\n", k, key, val);
			changed = 1;
		} else {
			die("unknown operation %s", op);
		}
	}
	if (!changed) {
		return 0;
	}
	struct out o = {0};
	if (force == 'b' || (force != 'x' && binary)) {
		write_binary(&o, root);
	} else {
		write_xml(&o, root);
	}
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.plistop", file);
	FILE *f = fopen(tmp, "wb");
	if (!f || fwrite(o.p, 1, o.n, f) != o.n || fclose(f)) {
		remove(tmp);
		die("%s: cannot write", tmp);
	}
	if (rename(tmp, file)) {
		remove(tmp);
		die("%s: cannot replace", file);
	}
	return 0;
}
