/*
 * pwd.db and spwd.db in the format libiosexec reads (OpenBSD getpwent.c, db 1.85 hash, host order):
 * keys '1'+name, '2'+entry number (int, from 1), '3'+uid (int, first entry wins); record
 * name\0 passwd\0 uid gid (int) change (time_t) class\0 gecos\0 dir\0 shell\0 expire (time_t) flags (int).
 * pwd.db carries "*" for every non-empty password.
 *   mkpwdb master.passwd outdir
 * Built for Linux with FreeBSD's lib/libc/db (hash, db, mpool) and a few libc shims.
 */
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <db.h>

static size_t field(char *buf, size_t n, const char *s)
{
	size_t l = strlen(s) + 1;
	memcpy(buf + n, s, l);
	return n + l;
}

static void put(DB *db, char tag, const void *k, size_t kl, DBT *d, int flags)
{
	char kb[256];
	kb[0] = tag;
	memcpy(kb + 1, k, kl);
	DBT key = { kb, kl + 1 };
	if (db->put(db, &key, d, flags) == -1)
		err(1, "put");
}

int main(int argc, char **argv)
{
	if (argc != 3)
		errx(1, "usage: mkpwdb master.passwd outdir");
	FILE *f = fopen(argv[1], "r");
	if (!f)
		err(1, "%s", argv[1]);
	char p1[4096], p2[4096];
	snprintf(p1, sizeof p1, "%s/pwd.db", argv[2]);
	snprintf(p2, sizeof p2, "%s/spwd.db", argv[2]);
	HASHINFO hi = { .bsize = 4096, .ffactor = 32, .nelem = 256, .cachesize = 2048 * 1024 };
	DB *pdb = dbopen(p1, O_RDWR | O_CREAT | O_TRUNC | O_EXCL, 0644, DB_HASH, &hi);
	DB *sdb = dbopen(p2, O_RDWR | O_CREAT | O_TRUNC | O_EXCL, 0600, DB_HASH, &hi);
	if (!pdb || !sdb)
		err(1, "dbopen");
	char line[4096];
	int num = 0;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		if (!line[0] || line[0] == '#')
			continue;
		char *v[10], *s = line;
		int i;
		for (i = 0; i < 10 && s; i++)
			v[i] = strsep(&s, ":");
		if (i != 10 || s)
			errx(1, "bad line: %s", line);
		int uid = (int)strtol(v[2], NULL, 10), gid = (int)strtol(v[3], NULL, 10), fl = 0;
		time_t change = strtoll(v[5], NULL, 10), expire = strtoll(v[6], NULL, 10);
		for (int shadow = 0; shadow < 2; shadow++) {
			char buf[2048];
			size_t n = field(buf, 0, v[0]);
			n = field(buf, n, shadow || !v[1][0] ? v[1] : "*");
			memcpy(buf + n, &uid, sizeof uid); n += sizeof uid;
			memcpy(buf + n, &gid, sizeof gid); n += sizeof gid;
			memcpy(buf + n, &change, sizeof change); n += sizeof change;
			n = field(buf, n, v[4]);
			n = field(buf, n, v[7]);
			n = field(buf, n, v[8]);
			n = field(buf, n, v[9]);
			memcpy(buf + n, &expire, sizeof expire); n += sizeof expire;
			memcpy(buf + n, &fl, sizeof fl); n += sizeof fl;
			DBT d = { buf, n };
			DB *db = shadow ? sdb : pdb;
			int k = num + 1;
			put(db, '1', v[0], strlen(v[0]), &d, 0);
			put(db, '2', &k, sizeof k, &d, 0);
			put(db, '3', &uid, sizeof uid, &d, R_NOOVERWRITE);
		}
		num++;
	}
	if (pdb->close(pdb) || sdb->close(sdb))
		err(1, "close");
	printf("%d entries\n", num);
	return 0;
}
