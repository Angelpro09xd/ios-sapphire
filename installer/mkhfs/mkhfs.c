/*
 * mkhfs: fill a freshly formatted HFSX volume with a directory tree, without any HFS+ driver.
 *
 *   mkfs.hfsplus -s -J -v iOSRoot TARGET      (hfsprogs: case-sensitive, journaled, empty)
 *   mkhfs TARGET SRCDIR                       (TARGET: a block device or an image file)
 *
 * Neither Windows nor the phone's Android/recovery kernel has an HFS+ driver, and iOS's root on sapphire is
 * HFSX (tools/mkiosroot.sh built the first one on Linux by mounting it). mkhfs writes what that mount wrote:
 * every folder, file (data fork), symlink and hard link of SRCDIR with owner, group, mode and times, into a
 * new catalog B-tree, the allocation bitmap and the data blocks; what mkfs made (the empty journal and its
 * info block, the private folders, the extents file) is kept. Names are stored decomposed (TN1150, Unicode
 * 3.2, decomp.h), ':' as '/'. Hard links are written the way Linux's hfsplus driver writes them (an "iNodeN"
 * file in the private metadata folder, link records of type 'hlnk'/'hfs+' carrying N), which is the format of
 * the image XNU has booted since October. Extended attributes and resource forks are not written (rsync -aH
 * did not copy them either). Devices, fifos and sockets are skipped with a warning.
 *
 * The volume must be fresh (one file system, nothing in it but what mkfs made): run it right after mkfs.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "decomp.h"

#define HFS_EPOCH       2082844800u     /* 1904-01-01 to 1970-01-01, seconds */
#define FIRST_USER_CNID 16

static int dev_fd;
static uint32_t bsize, total_blocks;
static uint8_t vh[512];
static uint8_t *bitmap;
static uint32_t node_size, next_cnid;
static int folder_count_mask;           /* folders keep a subfolder count (kHFSHasFolderCountMask) */
static int verbose, progress;

static void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "mkhfs: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static void *
xalloc(size_t n)
{
	void *p = calloc(1, n ? n : 1);
	if (!p) {
		die("out of memory (%zu bytes)", n);
	}
	return p;
}

/* ---------------- big-endian helpers ---------------- */

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) << 32 | g32(p + 4); }
static void p16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)(v >> 32)); p32(p + 4, (uint32_t)v); }

/* ---------------- device I/O ---------------- */

static void
dev_read(uint64_t off, void *buf, size_t len)
{
	for (size_t done = 0; done < len;) {
		ssize_t n = pread(dev_fd, (uint8_t *)buf + done, len - done, (off_t)(off + done));
		if (n <= 0) {
			die("read at %llu: %s", (unsigned long long)(off + done), n ? strerror(errno) : "end of device");
		}
		done += (size_t)n;
	}
}

static void
dev_write(uint64_t off, const void *buf, size_t len)
{
	for (size_t done = 0; done < len;) {
		ssize_t n = pwrite(dev_fd, (const uint8_t *)buf + done, len - done, (off_t)(off + done));
		if (n <= 0) {
			die("write at %llu: %s", (unsigned long long)(off + done), strerror(errno));
		}
		done += (size_t)n;
	}
}

/* ---------------- forks ---------------- */

struct fork {
	uint64_t size;
	uint32_t blocks;
	uint32_t ext[8][2];             /* start block, block count */
};

static void
fork_get(struct fork *f, const uint8_t *p)
{
	f->size = g64(p);
	f->blocks = g32(p + 12);
	for (int i = 0; i < 8; i++) {
		f->ext[i][0] = g32(p + 16 + 8 * i);
		f->ext[i][1] = g32(p + 20 + 8 * i);
	}
}

static void
fork_put(uint8_t *p, const struct fork *f)
{
	memset(p, 0, 80);
	p64(p, f->size);
	p32(p + 12, f->blocks);
	for (int i = 0; i < 8; i++) {
		p32(p + 16 + 8 * i, f->ext[i][0]);
		p32(p + 20 + 8 * i, f->ext[i][1]);
	}
}

/* the device byte offset of fork byte OFF (forks fully described by their 8 extents) */
static uint64_t
fork_offset(const struct fork *f, uint64_t off)
{
	uint64_t blk = off / bsize;
	for (int i = 0; i < 8 && f->ext[i][1]; i++) {
		if (blk < f->ext[i][1]) {
			return ((uint64_t)f->ext[i][0] + blk) * bsize + off % bsize;
		}
		blk -= f->ext[i][1];
	}
	die("fork offset %llu outside its extents", (unsigned long long)off);
	return 0;
}

static void
fork_io(const struct fork *f, uint64_t off, void *buf, size_t len, int write)
{
	while (len) {
		uint64_t dev = fork_offset(f, off);
		size_t n = bsize - (size_t)(off % bsize);
		n = n < len ? n : len;
		/* extend over physically contiguous blocks */
		while (n < len && fork_offset(f, off + n) == dev + n) {
			size_t more = len - n < bsize ? len - n : bsize;
			n += more;
		}
		if (write) {
			dev_write(dev, buf, n);
		} else {
			dev_read(dev, buf, n);
		}
		off += n;
		buf = (uint8_t *)buf + n;
		len -= n;
	}
}

/* ---------------- allocation ---------------- */

static int bit_get(uint32_t b) { return (bitmap[b >> 3] >> (7 - (b & 7))) & 1; }
static void bit_set(uint32_t b, int v)
{
	if (v) {
		bitmap[b >> 3] |= (uint8_t)(0x80 >> (b & 7));
	} else {
		bitmap[b >> 3] &= (uint8_t)~(0x80 >> (b & 7));
	}
}

static uint32_t alloc_cursor;
static uint32_t alloc_limit;            /* blocks from here on are not used (-r: the installer keeps data there) */

/* COUNT blocks for F: contiguous runs from the cursor, at most 8 of them */
static void
alloc_fork(struct fork *f, uint32_t count)
{
	memset(f->ext, 0, sizeof(f->ext));
	f->blocks = count;
	int e = 0;
	uint32_t b = alloc_cursor;
	while (count) {
		while (b < alloc_limit && bit_get(b)) {
			b++;
		}
		if (b >= alloc_limit) {
			die("volume full (%u more blocks needed)", count);
		}
		uint32_t start = b, n = 0;
		while (b < alloc_limit && !bit_get(b) && n < count) {
			bit_set(b++, 1);
			n++;
		}
		if (e == 8) {
			die("no room in eight pieces for a file of %u blocks", f->blocks);
		}
		f->ext[e][0] = start;
		f->ext[e][1] = n;
		e++;
		count -= n;
	}
	alloc_cursor = b;
}

/* ---------------- names: UTF-8 -> UTF-16, decomposed (TN1150) ---------------- */

static int
decompose(uint16_t c, uint16_t *out)
{
	if (c >= 0xAC00 && c <= 0xD7A3) {              /* Hangul syllable */
		unsigned s = c - 0xAC00, l = 0x1100 + s / 588, v = 0x1161 + (s % 588) / 28, t = 0x11A7 + s % 28;
		out[0] = (uint16_t)l;
		out[1] = (uint16_t)v;
		if (t != 0x11A7) {
			out[2] = (uint16_t)t;
			return 3;
		}
		return 2;
	}
	int lo = 0, hi = (int)(sizeof(decomp_table) / sizeof(decomp_table[0])) - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (decomp_table[mid].cp == c) {
			for (int i = 0; i < decomp_table[mid].n; i++) {
				out[i] = decomp_table[mid].d[i];
			}
			return decomp_table[mid].n;
		}
		if (decomp_table[mid].cp < c) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	out[0] = c;
	return 1;
}

/* returns the number of UTF-16 units written to OUT (max 255) */
static int
hfs_name(const char *s, uint16_t *out)
{
	int n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		uint32_t c;
		if (*p < 0x80) {
			c = *p++;
		} else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
			c = (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
			p += 2;
		} else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
			c = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
			p += 3;
		} else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
			c = (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
			p += 4;
		} else {
			die("name is not UTF-8: %s", s);
		}
		if (c == ':') {
			c = '/';
		}
		uint16_t tmp[4];
		int k;
		if (c >= 0x10000) {
			c -= 0x10000;
			tmp[0] = (uint16_t)(0xD800 | (c >> 10));
			tmp[1] = (uint16_t)(0xDC00 | (c & 0x3FF));
			k = 2;
		} else {
			k = decompose((uint16_t)c, tmp);
		}
		if (n + k > 255) {
			die("name too long: %s", s);
		}
		for (int i = 0; i < k; i++) {
			out[n++] = tmp[i];
		}
	}
	return n;
}

/* ---------------- catalog records ---------------- */

struct rec {
	uint32_t parent;
	uint16_t nlen;
	uint16_t *name;
	uint8_t *data;
	uint16_t dlen;
	uint8_t type;                   /* 1 folder, 2 file, 3/4 thread */
	uint32_t cnid;                  /* folders/files: their CNID */
};

static struct rec **recs;              /* pointers: records stay put when the array grows or is sorted */
static size_t nrecs, caprecs;

static struct rec *
rec_add(uint32_t parent, const uint16_t *name, int nlen, uint16_t dlen)
{
	if (nrecs == caprecs) {
		caprecs = caprecs ? caprecs * 2 : 65536;
		recs = realloc(recs, caprecs * sizeof(*recs));
		if (!recs) {
			die("out of memory (records)");
		}
	}
	struct rec *r = xalloc(sizeof(*r));
	recs[nrecs++] = r;
	r->parent = parent;
	r->nlen = (uint16_t)nlen;
	r->name = xalloc((size_t)(nlen ? nlen : 1) * 2);
	memcpy(r->name, name, (size_t)nlen * 2);
	r->data = xalloc(dlen);
	r->dlen = dlen;
	return r;
}

static int
rec_cmp(const void *a, const void *b)
{
	const struct rec *x = *(const struct rec *const *)a, *y = *(const struct rec *const *)b;
	if (x->parent != y->parent) {
		return x->parent < y->parent ? -1 : 1;
	}
	int n = x->nlen < y->nlen ? x->nlen : y->nlen;
	for (int i = 0; i < n; i++) {
		if (x->name[i] != y->name[i]) {
			return x->name[i] < y->name[i] ? -1 : 1;
		}
	}
	return (int)x->nlen - (int)y->nlen;
}

static void
thread_add(uint32_t cnid, uint32_t parent, const uint16_t *name, int nlen, int folder)
{
	struct rec *r = rec_add(cnid, NULL, 0, (uint16_t)(10 + 2 * nlen));
	r->type = folder ? 3 : 4;
	p16(r->data, folder ? 3 : 4);
	p32(r->data + 4, parent);
	p16(r->data + 8, (uint32_t)nlen);
	for (int i = 0; i < nlen; i++) {
		p16(r->data + 10 + 2 * i, name[i]);
	}
}

static void
bsd_put(uint8_t *p, const struct stat *st, uint32_t special)
{
	p32(p, st->st_uid);
	p32(p + 4, st->st_gid);
	p[8] = 0;                       /* adminFlags */
	p[9] = 0;                       /* ownerFlags */
	p16(p + 10, st->st_mode);
	p32(p + 12, special);
}

static void
dates_put(uint8_t *p, const struct stat *st)
{
	uint32_t m = (uint32_t)st->st_mtime + HFS_EPOCH, a = (uint32_t)st->st_atime + HFS_EPOCH;
	uint32_t c = (uint32_t)st->st_ctime + HFS_EPOCH;
	p32(p, m);                      /* createDate (no birth time from lstat) */
	p32(p + 4, m);                  /* contentModDate */
	p32(p + 8, c);                  /* attributeModDate */
	p32(p + 12, a);                 /* accessDate */
	p32(p + 16, 0);                 /* backupDate */
}

/* ---------------- the source tree ---------------- */

/*
 * The tree written is virtual: layers of real directories.
 *   base      SRCDIR, at /
 *   graft     -g VPATH=DIR: DIR appears at VPATH (above the base there)
 *   union     -u VPATH=DIR: a graft whose contents also show under / wherever the base has nothing --
 *             the same file twice, so a hard link (mkiosroot.sh's cryptex links)
 *   overlay   -o DIR: DIR over everything at / (later ones on top)
 *   exclude   -x FILE: virtual paths left out (one per line)
 * Priority, high to low: overlays (last first), grafts, base, unions.
 */
enum { L_UNION, L_BASE, L_GRAFT, L_OVERLAY };
struct layer {
	int kind;
	const char *at;                 /* graft/union: the virtual path ("/private/preboot/Cryptexes/OS") */
	const char *dir;
};
#define MAX_LAYERS 512                  /* prio = kind * MAX_LAYERS + index: kinds never overlap */
static struct layer layers[MAX_LAYERS];
static int nlayers;
static char **excl;
static size_t nexcl;

static int
cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int
excluded(const char *v)
{
	return nexcl && bsearch(&v, excl, nexcl, sizeof(*excl), cmp_str) != NULL;
}

struct link {
	dev_t dev;
	ino_t ino;
	uint32_t inode_cnid;            /* the iNode file's CNID (= the link id), 0 until made */
	int count;                      /* names met in the tree */
	int parts;                      /* union layers: 1 met in its graft, 2 met under / */
	char *first;                    /* real path of the first one (to copy the data from) */
	struct stat st;
};

static struct link *links;              /* open hash on (dev, ino): every regular file */
static size_t caplinks, nlinks;
static uint32_t private_dir;            /* "\0\0\0\0HFS+ Private Data" */
static uint32_t private_date;
static uint64_t files, folders, bytes, skipped;

static size_t
link_hash(dev_t d, ino_t i)
{
	uint64_t h = (uint64_t)i * 0x9e3779b97f4a7c15ull ^ (uint64_t)d * 0xc2b2ae3d27d4eb4full;
	return (size_t)(h ^ h >> 29);
}

static struct link *
link_get(const struct stat *st, int create)
{
	if (create && (nlinks + 1) * 2 > caplinks) {
		size_t oc = caplinks;
		struct link *old = links;
		caplinks = caplinks ? caplinks * 2 : 1 << 16;
		links = calloc(caplinks, sizeof(*links));
		if (!links) {
			die("out of memory (links)");
		}
		for (size_t i = 0; i < oc; i++) {
			if (old[i].count) {
				size_t j = link_hash(old[i].dev, old[i].ino) & (caplinks - 1);
				while (links[j].count) {
					j = (j + 1) & (caplinks - 1);
				}
				links[j] = old[i];
			}
		}
		free(old);
	}
	if (!caplinks) {
		return NULL;
	}
	size_t j = link_hash(st->st_dev, st->st_ino) & (caplinks - 1);
	while (links[j].count) {
		if (links[j].dev == st->st_dev && links[j].ino == st->st_ino) {
			return &links[j];
		}
		j = (j + 1) & (caplinks - 1);
	}
	if (!create) {
		return NULL;
	}
	nlinks++;
	return &links[j];
}

struct pending {                        /* file data to copy once space is allocated */
	char *path;
	char *target;                   /* symlinks: the target, read while walking */
	struct rec *r;                  /* its file record (data fork at +88) */
	uint64_t size;
};
static struct pending *pend;
static size_t npend, cappend;

static void
pend_add(const char *path, struct rec *r, uint64_t size)
{
	if (npend == cappend) {
		cappend = cappend ? cappend * 2 : 65536;
		pend = realloc(pend, cappend * sizeof(*pend));
		if (!pend) {
			die("out of memory (files)");
		}
	}
	pend[npend].path = strdup(path);
	pend[npend].r = r;
	pend[npend].size = size;
	pend[npend].target = NULL;
	npend++;
}

/* metadata of an HFS+ source volume at its root (a copy of an image): the new volume has its own */
static int
volume_meta(const char *name)
{
	static const char *const skip[] = {".journal", ".journal_info_block", ".hotfiles.btree", ".HFS+ Private Directory Data\r", NULL};
	for (int i = 0; skip[i]; i++) {
		if (!strcmp(name, skip[i])) {
			return 1;
		}
	}
	return 0;
}

struct ent {
	char *name;
	char *real;                     /* NULL: a folder made up (the way to a graft) */
	int prio;
	int adopted;                    /* an overlay folder that took the metadata of the one below */
	int part;                       /* from a union layer: 1 its graft, 2 its contents under / */
	struct stat st;
};

static int
ent_cmp(const void *a, const void *b)
{
	const struct ent *x = a, *y = b;
	int c = strcmp(x->name, y->name);
	return c ? c : y->prio - x->prio;
}

static void
ent_add(struct ent **v, size_t *n, size_t *cap, const char *name, const char *real, int prio, const struct stat *st)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 64;
		*v = realloc(*v, *cap * sizeof(**v));
		if (!*v) {
			die("out of memory (listing)");
		}
	}
	struct ent *e = &(*v)[(*n)++];
	e->name = strdup(name);
	e->real = real ? strdup(real) : NULL;
	e->prio = prio;
	e->adopted = 0;
	e->part = 0;
	e->st = *st;
}

static int
prio_of(int i)
{
	/* overlays above grafts above the base above unions; later layers of a kind above earlier ones */
	/* a union layer's graft ranks as a graft (it is the cryptex at its own path); its copy under / ranks lowest */
	return (layers[i].kind == L_UNION ? L_GRAFT : layers[i].kind) * MAX_LAYERS + i;
}

/* the real directory behind virtual folder V in layer I (not the union part): 1, or 0 if none */
static int
real_dir(int i, const char *v, char *out, size_t sz)
{
	const struct layer *l = &layers[i];
	if (l->kind == L_BASE || l->kind == L_OVERLAY) {
		snprintf(out, sz, "%s%s", l->dir, v);
		return 1;
	}
	size_t al = strlen(l->at);
	if (!strncmp(v, l->at, al) && (v[al] == 0 || v[al] == '/')) {
		snprintf(out, sz, "%s%s", l->dir, v + al);
		return 1;
	}
	return 0;
}

static void
list_real(struct ent **e, size_t *n, size_t *cap, const char *real, int i, int prio, int top)
{
	char sub[4096];
	DIR *d = opendir(real);
	if (!d) {
		if (errno != ENOENT && errno != ENOTDIR) {
			die("%s: %s", real, strerror(errno));
		}
		return;
	}
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || (layers[i].kind == L_BASE && top && volume_meta(de->d_name))) {
			continue;
		}
		snprintf(sub, sizeof(sub), "%s/%s", real, de->d_name);
		struct stat st;
		if (lstat(sub, &st)) {
			/* listed but not there (names a Linux HFS+ mount normalises differently): skipped */
			fprintf(stderr, "mkhfs: skipped (%s): %s\n", strerror(errno), sub);
			skipped++;
			continue;
		}
		ent_add(e, n, cap, de->d_name, sub, prio, &st);
	}
	closedir(d);
}

/* the entries of virtual folder V, merged over the layers: one per name, the winner's (sorted) */
static size_t
list_dir(const char *v, struct ent **out)
{
	struct ent *e = NULL;
	size_t n = 0, cap = 0;
	char real[4096];
	for (int i = 0; i < nlayers; i++) {
		if (real_dir(i, v, real, sizeof(real))) {
			size_t n0 = n;
			list_real(&e, &n, &cap, real, i, prio_of(i), !*v);
			for (size_t j = n0; j < n && layers[i].kind == L_UNION; j++) {
				e[j].part = 1;
			}
		}
		if (layers[i].kind == L_UNION) {
			/* the union part: the graft's contents at the same path under /, below everything */
			snprintf(real, sizeof(real), "%s%s", layers[i].dir, v);
			size_t n0 = n;
			list_real(&e, &n, &cap, real, i, i, !*v);
			for (size_t j = n0; j < n; j++) {
				e[j].part = 2;
			}
		}
		/* a graft below V: its first component must show here even if no layer has it */
		if (layers[i].kind == L_GRAFT || layers[i].kind == L_UNION) {
			size_t vl = strlen(v);
			const char *at = layers[i].at;
			if (!strncmp(at, v, vl) && at[vl] == '/') {
				const char *c = at + vl + 1, *slash = strchr(c, '/');
				char name[256];
				size_t cl = slash ? (size_t)(slash - c) : strlen(c);
				memcpy(name, c, cl);
				name[cl] = 0;
				struct stat st;
				if (!slash) {
					if (stat(layers[i].dir, &st)) {
						die("%s: %s", layers[i].dir, strerror(errno));
					}
					ent_add(&e, &n, &cap, name, layers[i].dir, prio_of(i), &st);
				} else {
					memset(&st, 0, sizeof(st));
					st.st_mode = S_IFDIR | 0755;
					st.st_mtime = st.st_atime = st.st_ctime = time(NULL);
					ent_add(&e, &n, &cap, name, NULL, -1, &st);     /* lowest: a real folder wins */
				}
			}
		}
	}
	qsort(e, n, sizeof(*e), ent_cmp);
	size_t m = 0;
	char vp[4096];
	for (size_t i = 0; i < n; i++) {
		snprintf(vp, sizeof(vp), "%s/%s", v, e[i].name);
		if (m && !strcmp(e[m - 1].name, e[i].name)) {
			/*
			 * a folder an overlay only adds to (made with mkdir for the files in it) keeps the
			 * metadata of the folder underneath: owner, mode, dates come from the system's own
			 */
			struct ent *w = &e[m - 1];
			if (S_ISDIR(w->st.st_mode) && S_ISDIR(e[i].st.st_mode) && w->prio >= L_OVERLAY * MAX_LAYERS &&
			    e[i].prio >= 0 && !w->adopted) {
				w->st = e[i].st;
				w->adopted = 1;
			}
			free(e[i].name);
			free(e[i].real);
			continue;
		}
		if (excluded(vp)) {
			free(e[i].name);
			free(e[i].real);
			continue;
		}
		e[m++] = e[i];
	}
	*out = e;
	return m;
}

static void
ents_free(struct ent *e, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		free(e[i].name);
		free(e[i].real);
	}
	free(e);
}

/* first pass: how many names each regular file has in the tree */
static void
scan_links(const char *v)
{
	struct ent *e;
	size_t n = list_dir(v, &e);
	char vp[4096];
	for (size_t i = 0; i < n; i++) {
		if (S_ISDIR(e[i].st.st_mode)) {
			snprintf(vp, sizeof(vp), "%s/%s", v, e[i].name);
			scan_links(vp);
		} else if (S_ISREG(e[i].st.st_mode) && (e[i].st.st_nlink > 1 || e[i].part)) {
			/*
			 * a file with other names (st_nlink), or one met through a union layer (its graft and
			 * the root): only these become hard links. apfs-fuse shows APFS's own sibling links with
			 * one inode but st_nlink 1; rsync -aH copied those apart in the images that boot, so here too.
			 */
			struct link *l = link_get(&e[i].st, 1);
			l->parts |= e[i].part;
			if (!l->count) {
				l->dev = e[i].st.st_dev;
				l->ino = e[i].st.st_ino;
				l->first = strdup(e[i].real);
				l->st = e[i].st;
			}
			l->count++;
		}
	}
	ents_free(e, n);
}

static void
file_record(struct rec *r, uint32_t cnid, const struct stat *st, uint32_t special)
{
	uint8_t *d = r->data;
	r->type = 2;
	r->cnid = cnid;
	p16(d, 2);
	p16(d + 2, 0x0002);             /* kHFSThreadExistsMask */
	p32(d + 8, cnid);
	dates_put(d + 12, st);
	bsd_put(d + 32, st, special);
}

/* second pass: records for everything in virtual folder V (CNID PARENT) */
static void
walk(const char *v, uint32_t parent)
{
	struct ent *e;
	size_t n = list_dir(v, &e);
	char vp[4096];
	uint16_t name[255];
	for (size_t i = 0; i < n; i++) {
		struct stat st = e[i].st;
		const char *sub = e[i].real;
		int nlen = hfs_name(e[i].name, name);
		if (S_ISDIR(st.st_mode)) {
			uint32_t cnid = next_cnid++;
			struct rec *r = rec_add(parent, name, nlen, 88);
			r->type = 1;
			r->cnid = cnid;
			p16(r->data, 1);
			p32(r->data + 8, cnid);
			dates_put(r->data + 12, &st);
			bsd_put(r->data + 32, &st, 0);
			thread_add(cnid, parent, name, nlen, 1);
			folders++;
			snprintf(vp, sizeof(vp), "%s/%s", v, e[i].name);
			walk(vp, cnid);
		} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
			char target[4096];
			ssize_t tlen = 0;
			if (S_ISLNK(st.st_mode)) {
				/* read now: a link whose target cannot be read is left out, not written empty */
				tlen = readlink(sub, target, sizeof(target));
				if (tlen <= 0 || tlen >= (ssize_t)sizeof(target)) {
					fprintf(stderr, "mkhfs: skipped (readlink: %s): %s\n", tlen < 0 ? strerror(errno) : "empty or too long", sub);
					skipped++;
					continue;
				}
				st.st_size = tlen;
			}
			uint32_t cnid = next_cnid++;
			struct rec *r = rec_add(parent, name, nlen, 248);
			thread_add(cnid, parent, name, nlen, 0);
			files++;
			struct link *l = S_ISREG(st.st_mode) ? link_get(&st, 0) : NULL;
			/* union files: a link only when met both in the cryptex and under / (mkiosroot.sh's ln) */
			if (l && l->count > 1 && (l->st.st_nlink > 1 || l->parts == 3)) {
				/* a hard link: the data lives once, in the private folder's iNode file */
				if (!l->inode_cnid) {
					l->inode_cnid = next_cnid++;
					char iname[32];
					uint16_t iw[32];
					snprintf(iname, sizeof(iname), "iNode%u", l->inode_cnid);
					int in = hfs_name(iname, iw);
					struct rec *ir = rec_add(private_dir, iw, in, 248);
					file_record(ir, l->inode_cnid, &l->st, (uint32_t)l->count);
					thread_add(l->inode_cnid, private_dir, iw, in, 0);
					files++;
					p64(ir->data + 88, (uint64_t)l->st.st_size);
					pend_add(l->first, ir, (uint64_t)l->st.st_size);
					bytes += (uint64_t)l->st.st_size;
				}
				file_record(r, cnid, &st, l->inode_cnid);
				uint8_t *dd = r->data;
				memset(dd + 32, 0, 16);                 /* Linux leaves the link's permissions empty */
				p32(dd + 32 + 12, l->inode_cnid);       /* permissions.special: the link id */
				memcpy(dd + 48, "hlnkhfs+", 8);         /* fdType, fdCreator */
				p16(dd + 56, 0x0100);                   /* fdFlags: kHasBeenInited */
				p32(dd + 12, private_date);             /* createDate: the private folder's */
			} else if (S_ISLNK(st.st_mode)) {
				file_record(r, cnid, &st, 0);
				memcpy(r->data + 48, "slnkrhap", 8);
				p64(r->data + 88, (uint64_t)st.st_size);
				pend_add(sub, r, (uint64_t)st.st_size);
				pend[npend - 1].target = xalloc((size_t)tlen + 1);
				memcpy(pend[npend - 1].target, target, (size_t)tlen);
			} else {
				file_record(r, cnid, &st, 1);
				p64(r->data + 88, (uint64_t)st.st_size);
				if (st.st_size) {
					pend_add(sub, r, (uint64_t)st.st_size);
				}
				bytes += (uint64_t)st.st_size;
			}
		} else {
			fprintf(stderr, "mkhfs: skipped (not a file, folder or symlink): %s\n", sub ? sub : e[i].name);
			skipped++;
		}
	}
	ents_free(e, n);
}

/* ---------------- B-tree ---------------- */

static uint32_t
rec_bytes(const struct rec *r)
{
	return 2 + 6 + 2u * r->nlen + r->dlen;   /* keyLength + parentID + name length + name, data */
}

static uint8_t *
node_new(uint8_t *tree, uint32_t n, int8_t kind, uint8_t height)
{
	uint8_t *p = tree + (size_t)n * node_size;
	memset(p, 0, node_size);
	p[8] = (uint8_t)kind;
	p[9] = height;
	p16(p + node_size - 2, 14);             /* offset of record 0 */
	return p;
}

/* appends a record (KEY of KLEN bytes incl. its length field, then DATA) to node P; 0 if full */
static int
node_put(uint8_t *p, const uint8_t *key, uint32_t klen, const uint8_t *data, uint32_t dlen)
{
	uint16_t n = g16(p + 10);
	uint32_t free_off = g16(p + node_size - 2 * (n + 1));
	uint32_t need = klen + dlen;
	if (free_off + need + 2 * (uint32_t)(n + 2) > node_size) {
		return 0;
	}
	memcpy(p + free_off, key, klen);
	memcpy(p + free_off + klen, data, dlen);
	p16(p + 10, n + 1u);
	p16(p + node_size - 2 * (n + 2), free_off + need);
	return 1;
}

static uint32_t
key_of(const struct rec *r, uint8_t *k)
{
	p16(k, 6 + 2u * r->nlen);
	p32(k + 2, r->parent);
	p16(k + 6, r->nlen);
	for (int i = 0; i < r->nlen; i++) {
		p16(k + 8 + 2 * i, r->name[i]);
	}
	return 8 + 2u * r->nlen;
}

/* builds the catalog in TREE (ALLOC nodes); returns nodes used, fills the header's numbers */
static uint32_t
build_catalog(uint8_t *tree, uint32_t alloc, uint32_t *root, uint32_t *depth, uint32_t *leaves_first,
    uint32_t *leaves_last)
{
	uint8_t key[520];
	uint32_t next = 1, first = 1;
	/* leaves */
	uint8_t *cur = node_new(tree, next, -1, 1);
	uint32_t *level = xalloc((nrecs + 2) * sizeof(uint32_t)), nlevel = 0;  /* first node of each */
	level[nlevel++] = next;
	for (size_t i = 0; i < nrecs; i++) {
		uint32_t kl = key_of(recs[i], key);
		if (!node_put(cur, key, kl, recs[i]->data, recs[i]->dlen)) {
			uint32_t prev = next++;
			if (next >= alloc) {
				die("catalog larger than planned");
			}
			p32(cur, next);                         /* fLink */
			cur = node_new(tree, next, -1, 1);
			p32(cur + 4, prev);                     /* bLink */
			level[nlevel++] = next;
			if (!node_put(cur, key, kl, recs[i]->data, recs[i]->dlen)) {
				die("a record does not fit in a node");
			}
		}
	}
	*leaves_first = first;
	*leaves_last = next;
	next++;
	/* index levels until one node */
	uint32_t height = 1;
	while (nlevel > 1) {
		height++;
		uint32_t *up = xalloc((nlevel + 2) * sizeof(uint32_t)), nup = 0;
		uint32_t start = next;
		cur = node_new(tree, next, 0, (uint8_t)height);
		up[nup++] = next;
		for (uint32_t i = 0; i < nlevel; i++) {
			uint8_t *child = tree + (size_t)level[i] * node_size;
			uint16_t off = g16(child + node_size - 2);   /* first record of the child */
			uint32_t kl = 2u + g16(child + off);
			uint8_t ptr[4];
			p32(ptr, level[i]);
			if (!node_put(cur, child + off, kl, ptr, 4)) {
				uint32_t prev = next++;
				if (next >= alloc) {
					die("catalog larger than planned");
				}
				p32(cur, next);
				cur = node_new(tree, next, 0, (uint8_t)height);
				p32(cur + 4, prev);
				up[nup++] = next;
				node_put(cur, child + off, kl, ptr, 4);
			}
		}
		(void)start;
		next++;
		free(level);
		level = up;
		nlevel = nup;
	}
	*root = level[0];
	*depth = height;
	free(level);
	return next;
}

/* ---------------- main ---------------- */

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: mkhfs TARGET SRCDIR [options]\n"
		    "  TARGET: freshly formatted with mkfs.hfsplus -s -J\n"
		    "  -g VPATH=DIR  DIR shows at VPATH\n"
		    "  -u VPATH=DIR  as -g, and its files also show under / where nothing else is (hard links)\n"
		    "  -o DIR        DIR over everything at / (later -o on top)\n"
		    "  -x FILE       paths (one per line, from /) left out\n"
		    "  -r BYTES      leave the volume's last BYTES alone (they stay free, their contents untouched)\n"
		    "  -v            verbose\n");
		return 2;
	}
	uint64_t reserve = 0;
	layers[nlayers].kind = L_BASE;
	layers[nlayers].at = "";
	layers[nlayers++].dir = argv[2];
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "-p")) {
			progress = 1;
		} else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
			reserve = strtoull(argv[++i], NULL, 0);
		} else if ((!strcmp(argv[i], "-g") || !strcmp(argv[i], "-u") || !strcmp(argv[i], "-o")) && i + 1 < argc) {
			if (nlayers == MAX_LAYERS) {
				die("too many layers");
			}
			struct layer *l = &layers[nlayers++];
			char *arg = argv[i + 1];
			if (argv[i][1] == 'o') {
				l->kind = L_OVERLAY;
				l->at = "";
				l->dir = arg;
			} else {
				char *eq = strchr(arg, '=');
				if (!eq || arg[0] != '/' || eq[-1] == '/') {
					die("%s %s: VPATH=DIR, VPATH from / without a trailing /", argv[i], arg);
				}
				*eq = 0;
				l->kind = argv[i][1] == 'u' ? L_UNION : L_GRAFT;
				l->at = arg;
				l->dir = eq + 1;
			}
			i++;
		} else if (!strcmp(argv[i], "-x") && i + 1 < argc) {
			FILE *f = fopen(argv[++i], "r");
			if (!f) {
				die("%s: %s", argv[i], strerror(errno));
			}
			char line[4096];
			size_t cap = 0;
			while (fgets(line, sizeof(line), f)) {
				line[strcspn(line, "\r\n")] = 0;
				size_t ll = strlen(line);
				while (ll > 1 && line[ll - 1] == '/') {
					line[--ll] = 0;
				}
				if (line[0] != '/') {
					continue;
				}
				if (nexcl == cap) {
					cap = cap ? cap * 2 : 256;
					excl = realloc(excl, cap * sizeof(*excl));
				}
				excl[nexcl++] = strdup(line);
			}
			fclose(f);
			qsort(excl, nexcl, sizeof(*excl), cmp_str);
		} else {
			die("unknown option %s", argv[i]);
		}
	}
	dev_fd = open(argv[1], O_RDWR);
	if (dev_fd < 0) {
		die("%s: %s", argv[1], strerror(errno));
	}
	dev_read(1024, vh, 512);
	if (g16(vh) != 0x4858 || g16(vh + 2) != 5) {
		die("%s is not an HFSX volume (signature %04x version %u): format it with mkfs.hfsplus -s -J", argv[1], g16(vh), g16(vh + 2));
	}
	if (!(g32(vh + 4) & (1u << 13))) {
		die("the volume is not journaled (mkfs.hfsplus -J)");
	}
	bsize = g32(vh + 40);
	total_blocks = g32(vh + 44);
	{
		uint64_t rb = (reserve + bsize - 1) / bsize + 1;     /* + the block of the alternate volume header */
		if (reserve && rb >= total_blocks) {
			die("-r %llu: more than the volume", (unsigned long long)reserve);
		}
		alloc_limit = reserve ? total_blocks - (uint32_t)rb : total_blocks;
	}
	next_cnid = g32(vh + 64);
	if (g32(vh + 32) > 8 || g32(vh + 36) > 8) {
		die("the volume is not fresh (%u files, %u folders): run mkhfs right after mkfs", g32(vh + 32), g32(vh + 36));
	}
	struct fork alloc_f, ext_f, cat_f, attr_f;
	fork_get(&alloc_f, vh + 112);
	fork_get(&ext_f, vh + 192);
	fork_get(&cat_f, vh + 272);
	fork_get(&attr_f, vh + 352);

	/* the allocation bitmap */
	bitmap = xalloc((size_t)alloc_f.size);
	fork_io(&alloc_f, 0, bitmap, (size_t)alloc_f.size, 0);

	/* the fresh catalog: keep its records (root, private folders, journal files) */
	uint8_t *old = xalloc((size_t)cat_f.size);
	fork_io(&cat_f, 0, old, (size_t)cat_f.size, 0);
	node_size = g16(old + 14 + 18);
	uint32_t old_leaf = g32(old + 14 + 10);
	uint8_t cmp_type = old[14 + 37];
	if (cmp_type != 0xBC) {
		die("the catalog is not case-sensitive (mkfs.hfsplus -s)");
	}
	for (uint32_t n = old_leaf; n;) {
		uint8_t *p = old + (size_t)n * node_size;
		uint16_t cnt = g16(p + 10);
		for (uint16_t i = 0; i < cnt; i++) {
			uint16_t off = g16(p + node_size - 2 * (i + 1)), end = g16(p + node_size - 2 * (i + 2));
			uint16_t kl = g16(p + off);
			uint32_t parent = g32(p + off + 2);
			uint16_t nl = g16(p + off + 6);
			uint16_t nm[255];
			for (int k = 0; k < nl; k++) {
				nm[k] = g16(p + off + 8 + 2 * k);
			}
			uint16_t dlen = (uint16_t)(end - off - 2 - kl);
			struct rec *r = rec_add(parent, nm, nl, dlen);
			memcpy(r->data, p + off + 2 + kl, dlen);
			r->type = (uint8_t)g16(r->data);
			if (r->type == 1 || r->type == 2) {
				r->cnid = g32(r->data + 8);
			}
			if (r->type == 1 && r->cnid == 2) {
				folder_count_mask = (g16(r->data + 2) & 0x0010) != 0;
			}
			static const uint16_t priv[] = {0, 0, 0, 0, 'H', 'F', 'S', '+', ' ', 'P', 'r', 'i', 'v', 'a', 't', 'e', ' ', 'D', 'a', 't', 'a'};
			if (r->type == 1 && parent == 2 && nl == 21 && !memcmp(nm, priv, sizeof(priv))) {
				private_dir = r->cnid;
				private_date = g32(r->data + 12);
			}
		}
		n = g32(p);
	}
	if (!private_dir) {
		/*
		 * hfsprogs mkfs does not make it (XNU and Linux make it at the first mount): made here as XNU's
		 * hfs_privatedir_init does -- root-owned, mode 0, invisible and name-locked in the Finder flags.
		 */
		static const uint16_t priv[] = {0, 0, 0, 0, 'H', 'F', 'S', '+', ' ', 'P', 'r', 'i', 'v', 'a', 't', 'e', ' ', 'D', 'a', 't', 'a'};
		struct stat st;
		memset(&st, 0, sizeof(st));
		st.st_mode = S_IFDIR;
		st.st_mtime = st.st_atime = st.st_ctime = time(NULL);
		private_dir = next_cnid++;
		private_date = (uint32_t)st.st_mtime + HFS_EPOCH;
		struct rec *r = rec_add(2, priv, 21, 88);
		r->type = 1;
		r->cnid = private_dir;
		p16(r->data, 1);
		p32(r->data + 8, private_dir);
		dates_put(r->data + 12, &st);
		bsd_put(r->data + 32, &st, 0);
		p16(r->data + 48 + 8, 0x5000);          /* frFlags: kIsInvisible | kNameLocked */
		thread_add(private_dir, 2, priv, 21, 1);
	}
	size_t kept = nrecs;
	/* the old catalog's blocks go back to the pool: the new one is written elsewhere */
	for (int i = 0; i < 8 && cat_f.ext[i][1]; i++) {
		for (uint32_t b = 0; b < cat_f.ext[i][1]; b++) {
			bit_set(cat_f.ext[i][0] + b, 0);
		}
	}

	/* the tree */
	fprintf(stderr, "mkhfs: scanning %s\n", argv[2]);
	scan_links("");
	walk("", 2);
	fprintf(stderr, "mkhfs: %llu folders, %llu files, %.1f GB, %zu hard-linked inodes; %llu skipped\n",
	    (unsigned long long)folders, (unsigned long long)files, bytes / 1e9, nlinks, (unsigned long long)skipped);
	(void)kept;

	/* folder valences (and subfolder counts) from the final records */
	qsort(recs, nrecs, sizeof(*recs), rec_cmp);
	for (size_t i = 0; i < nrecs; i++) {
		if (recs[i]->type == 1) {
			uint32_t val = 0, sub = 0;
			uint32_t id = recs[i]->cnid;
			/* children: records with parent == id, not threads (threads are keyed by their own CNID) */
			struct rec probe = {.parent = id, .nlen = 0};
			struct rec *pp = &probe;
			size_t lo = 0, hi = nrecs;
			while (lo < hi) {
				size_t mid = (lo + hi) / 2;
				if (rec_cmp(&recs[mid], &pp) < 0) {
					lo = mid + 1;
				} else {
					hi = mid;
				}
			}
			for (size_t j = lo; j < nrecs && recs[j]->parent == id; j++) {
				if (recs[j]->type == 1 || recs[j]->type == 2) {
					val++;
					sub += recs[j]->type == 1;
				}
			}
			p32(recs[i]->data + 4, val);
			if (folder_count_mask) {
				p16(recs[i]->data + 2, g16(recs[i]->data + 2) | 0x0010);
				p32(recs[i]->data + 84, sub);
			}
		}
	}

	/* the new catalog: plan its size, allocate it first (near the volume's start), build it */
	uint64_t payload = 0;
	for (size_t i = 0; i < nrecs; i++) {
		payload += rec_bytes(recs[i]) + 2;
	}
	uint32_t leaf_est = (uint32_t)(payload / (node_size - 64) + 2);
	uint32_t nodes = leaf_est + leaf_est / 8 + 64;          /* leaves, index, slack */
	nodes += nodes / 2 + 1024;                              /* room for iOS to grow it */
	uint32_t map_cap = (node_size - 256) * 8;
	if (nodes > map_cap) {
		nodes = map_cap;
	}
	alloc_cursor = 0;
	struct fork new_cat;
	alloc_fork(&new_cat, (uint32_t)(((uint64_t)nodes * node_size + bsize - 1) / bsize));
	new_cat.size = (uint64_t)nodes * node_size;
	uint8_t *tree = xalloc((size_t)new_cat.size);
	uint32_t root, depth, lf, ll;
	uint32_t used = build_catalog(tree, nodes, &root, &depth, &lf, &ll);
	/* header node */
	uint8_t *h = node_new(tree, 0, 1, 0);
	p32(h, 0);
	p16(h + 10, 3);
	uint8_t *hr = h + 14;
	memcpy(hr, old + 14, 106);                              /* the fresh header record: keep clump etc. */
	p16(hr, depth);
	p32(hr + 2, root);
	p32(hr + 6, (uint32_t)nrecs);
	p32(hr + 10, lf);
	p32(hr + 14, ll);
	p16(hr + 18, node_size);
	p32(hr + 22, nodes);
	p32(hr + 26, nodes - used);
	p16(h + node_size - 4, 14 + 106);                       /* user data record */
	p16(h + node_size - 6, 14 + 106 + 128);                 /* map record */
	p16(h + node_size - 8, node_size - 8);                  /* free space */
	uint8_t *map = h + 14 + 106 + 128;
	for (uint32_t n = 0; n < used; n++) {
		map[n >> 3] |= (uint8_t)(0x80 >> (n & 7));
	}

	/* file data: allocate and copy */
	uint8_t *buf = xalloc(8u << 20);
	uint64_t done = 0;
	time_t t0 = time(NULL);
	for (size_t i = 0; i < npend; i++) {
		struct pending *pd = &pend[i];
		struct fork f;
		uint32_t blocks = (uint32_t)((pd->size + bsize - 1) / bsize);
		alloc_fork(&f, blocks);
		f.size = pd->size;
		fork_put(pd->r->data + 88, &f);
		if (pd->target) {
			memset(buf, 0, bsize);
			memcpy(buf, pd->target, (size_t)pd->size);
			fork_io(&f, 0, buf, bsize, 1);
			continue;
		}
		int fd = open(pd->path, O_RDONLY);
		if (fd < 0) {
			die("%s: %s", pd->path, strerror(errno));
		}
		for (uint64_t off = 0; off < pd->size;) {
			size_t want = pd->size - off < (8u << 20) ? (size_t)(pd->size - off) : (8u << 20);
			ssize_t n = pread(fd, buf, want, (off_t)off);
			if (n <= 0) {
				die("%s: read: %s", pd->path, n ? strerror(errno) : "file shrank");
			}
			size_t padded = ((size_t)n + bsize - 1) / bsize * bsize;
			memset(buf + n, 0, padded - (size_t)n);
			fork_io(&f, off, buf, padded, 1);
			off += (uint64_t)n;
			done += (uint64_t)n;
		}
		close(fd);
		if (verbose || (i % 20000) == 0) {
			fprintf(stderr, "mkhfs: %zu/%zu files, %.1f/%.1f GB, %lds\n", i, npend, done / 1e9, bytes / 1e9, (long)(time(NULL) - t0));
		}
		if (progress && (i % 500) == 0) {
			/* for the installer's PC side: "PROGRESS done total" */
			printf("PROGRESS %llu %llu\n", (unsigned long long)done, (unsigned long long)bytes);
			fflush(stdout);
		}
	}
	/* the data forks are now in the records: build the catalog again with them */
	used = build_catalog(tree, nodes, &root, &depth, &lf, &ll);
	p16(hr, depth);
	p32(hr + 2, root);
	p32(hr + 10, lf);
	p32(hr + 14, ll);
	p32(hr + 26, nodes - used);
	fork_io(&new_cat, 0, tree, (size_t)new_cat.size, 1);

	/* bitmap, volume header (and its copy at the end) */
	fork_io(&alloc_f, 0, bitmap, (size_t)alloc_f.size, 1);
	uint32_t free_blocks = 0;
	for (uint32_t b = 0; b < total_blocks; b++) {
		free_blocks += !bit_get(b);
	}
	uint32_t nfiles = 0, nfolders = 0;
	for (size_t i = 0; i < nrecs; i++) {
		nfiles += recs[i]->type == 2;
		nfolders += recs[i]->type == 1 && recs[i]->cnid != 2;
	}
	p32(vh + 32, nfiles);
	p32(vh + 36, nfolders);
	p32(vh + 48, free_blocks);
	p32(vh + 52, alloc_cursor);                             /* nextAllocation */
	p32(vh + 64, next_cnid);
	p32(vh + 68, g32(vh + 68) + 1);                         /* writeCount */
	p32(vh + 20, (uint32_t)time(NULL) + HFS_EPOCH);         /* modifyDate */
	fork_put(vh + 272, &new_cat);
	(void)ext_f;
	(void)attr_f;
	dev_write(1024, vh, 512);
	dev_write((uint64_t)total_blocks * bsize - 1024, vh, 512);
	if (fsync(dev_fd)) {
		die("fsync: %s", strerror(errno));
	}
	fprintf(stderr, "mkhfs: done: %u files, %u folders, catalog %u/%u nodes, %.1f GB free\n", nfiles, nfolders, used, nodes,
	    (double)free_blocks * bsize / 1e9);
	return 0;
}
