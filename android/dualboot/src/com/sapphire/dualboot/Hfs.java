package com.sapphire.dualboot;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A read-only HFS+ / HFSX reader (Apple TN1150) over iOS's root partition, through RootShell: the volume
 * header, the catalog B-tree (directory listings, lookups), the extents overflow B-tree (files in more than
 * eight pieces) and hard links (iNode files in the private metadata folder). Nothing is ever written: the
 * volume is journaled and mounted by iOS, and a write from here could corrupt it.
 */
final class Hfs {
	static final int ROOT_ID = 2;
	static final long EPOCH_OFFSET = 2082844800L;     /* seconds from 1904-01-01 to 1970-01-01 */

	final String device;
	final int blockSize;
	final long totalBlocks, freeBlocks;
	final boolean caseSensitive;
	final String signature;
	private final Fork catalog, extents;
	private final BTree cat, ext;
	private int privateDirId = -1;

	/** One entry of a folder. */
	static final class Entry {
		String name;
		boolean folder;
		int id, parent;
		long size;                      /* data fork bytes (files), item count (folders) */
		long modified;                  /* ms since 1970 */
		int mode, uid, gid;
		boolean compressed, hardLink, symlink;
		Fork data;

		boolean isDirectory() {
			return folder;
		}
	}

	/** A fork: its byte size and where its blocks are. */
	static final class Fork {
		long size;
		int fileId, forkType;
		final List<long[]> runs = new ArrayList<>();   /* {start block, block count} */

		long blocks() {
			long n = 0;
			for (long[] r : runs) {
				n += r[1];
			}
			return n;
		}
	}

	private Hfs(String device, byte[] vh) throws IOException {
		this.device = device;
		signature = new String(vh, 1024, 2, StandardCharsets.US_ASCII);
		if (!signature.equals("H+") && !signature.equals("HX")) {
			throw new IOException(L.t("no hay un volumen HFS+ en ", "no HFS+ volume on ") + device);
		}
		caseSensitive = signature.equals("HX");
		blockSize = be32(vh, 1024 + 40);
		totalBlocks = be32(vh, 1024 + 44) & 0xffffffffL;
		freeBlocks = be32(vh, 1024 + 48) & 0xffffffffL;
		extents = fork(vh, 1024 + 192, 3, 0);
		catalog = fork(vh, 1024 + 272, 4, 0);
		ext = new BTree(extents, false);
		cat = new BTree(catalog, true);
	}

	static Hfs open(String device) throws IOException {
		return new Hfs(device, RootShell.read(device, 0, 4096));
	}

	long usedBytes() {
		return (totalBlocks - freeBlocks) * blockSize;
	}

	long totalBytes() {
		return totalBlocks * blockSize;
	}

	/* ---------- forks and extents ---------- */

	private static Fork fork(byte[] b, int off, int fileId, int type) {
		Fork f = new Fork();
		f.size = be64(b, off);
		f.fileId = fileId;
		f.forkType = type;
		for (int i = 0; i < 8; i++) {
			long start = be32(b, off + 16 + i * 8) & 0xffffffffL, count = be32(b, off + 20 + i * 8) & 0xffffffffL;
			if (count == 0) {
				break;
			}
			f.runs.add(new long[]{start, count});
		}
		return f;
	}

	/** The fork's runs completed from the extents overflow file when the first eight do not cover it. */
	private synchronized void complete(Fork f) throws IOException {
		long need = (f.size + blockSize - 1) / blockSize;
		while (f.blocks() < need && ext != null && f.fileId != 3) {
			long have = f.blocks();
			byte[] key = new byte[12];
			put16(key, 0, 10);
			key[2] = (byte) f.forkType;
			put32(key, 4, f.fileId);
			put32(key, 8, (int) have);
			Record r = ext.find(key);
			if (r == null || be32(r.node, r.key + 4) != f.fileId || (r.node[r.key + 2] & 0xff) != f.forkType
			    || (be32(r.node, r.key + 8) & 0xffffffffL) != have) {
				throw new IOException(L.t("extents perdidos del fichero ", "lost extents of file ") + f.fileId);
			}
			for (int i = 0; i < 8; i++) {
				long start = be32(r.node, r.data + i * 8) & 0xffffffffL, count = be32(r.node, r.data + 4 + i * 8) & 0xffffffffL;
				if (count == 0) {
					break;
				}
				f.runs.add(new long[]{start, count});
			}
		}
	}

	/** LEN bytes of fork F from byte POS (inside the fork). */
	byte[] readFork(Fork f, long pos, int len) throws IOException {
		complete(f);
		byte[] out = new byte[len];
		int done = 0;
		while (done < len) {
			long p = pos + done;
			long blk = p / blockSize;
			int inBlock = (int) (p % blockSize);
			long dev = -1, avail = 0;
			for (long[] r : f.runs) {
				if (blk < r[1]) {
					dev = (r[0] + blk) * blockSize + inBlock;
					avail = (r[1] - blk) * blockSize - inBlock;
					break;
				}
				blk -= r[1];
			}
			if (dev < 0) {
				throw new IOException(L.t("lectura fuera del fichero", "read beyond the end of the file"));
			}
			int want = (int) Math.min(Math.min(avail, len - done), 4 << 20);
			long a = dev & ~4095L;
			int n = (int) (((dev + want + 4095) & ~4095L) - a);
			byte[] chunk = RootShell.read(device, a, n);
			System.arraycopy(chunk, (int) (dev - a), out, done, want);
			done += want;
		}
		return out;
	}

	/* ---------- B-trees ---------- */

	private static final class Record {
		byte[] node;
		int key, data;          /* offsets of the key and of what follows it */
	}

	private final class BTree {
		final Fork fork;
		final boolean catalog;
		int nodeSize, root, firstLeaf;
		boolean binaryCompare;
		final Map<Integer, byte[]> cache = new LinkedHashMap<Integer, byte[]>(64, 0.75f, true) {
			@Override
			protected boolean removeEldestEntry(Map.Entry<Integer, byte[]> e) {
				return size() > 512;
			}
		};

		BTree(Fork fork, boolean catalog) throws IOException {
			this.fork = fork;
			this.catalog = catalog;
			nodeSize = 512;
			byte[] h = readFork(fork, 0, 512);
			nodeSize = be16(h, 14 + 18);
			root = be32(h, 14 + 2);
			firstLeaf = be32(h, 14 + 10);
			binaryCompare = (h[14 + 37] & 0xff) == 0xBC;
		}

		/* synchronized: the app browses and searches from different threads, and the cache is a LinkedHashMap */
		synchronized byte[] node(int n) throws IOException {
			byte[] b = cache.get(n);
			if (b == null) {
				b = readFork(fork, (long) n * nodeSize, nodeSize);
				cache.put(n, b);
			}
			return b;
		}

		int records(byte[] node) {
			return be16(node, 10);
		}

		int recordOffset(byte[] node, int i) {
			return be16(node, nodeSize - 2 * (i + 1));
		}

		int compare(byte[] a, int ao, byte[] b, int bo) {
			if (catalog) {
				long pa = be32(a, ao + 2) & 0xffffffffL, pb = be32(b, bo + 2) & 0xffffffffL;
				if (pa != pb) {
					return Long.compare(pa, pb);
				}
				int la = be16(a, ao + 6), lb = be16(b, bo + 6);
				for (int i = 0; i < Math.min(la, lb); i++) {
					int ca = be16(a, ao + 8 + 2 * i), cb = be16(b, bo + 8 + 2 * i);
					if (!binaryCompare) {
						ca = fold(ca);
						cb = fold(cb);
					}
					if (ca != cb) {
						return Integer.compare(ca, cb);
					}
				}
				return Integer.compare(la, lb);
			}
			/* extents: fork type, file id, start block */
			int ta = a[ao + 2] & 0xff, tb = b[bo + 2] & 0xff;
			long fa = be32(a, ao + 4) & 0xffffffffL, fb = be32(b, bo + 4) & 0xffffffffL;
			if (fa != fb) {
				return Long.compare(fa, fb);
			}
			if (ta != tb) {
				return Integer.compare(ta, tb);
			}
			return Long.compare(be32(a, ao + 8) & 0xffffffffL, be32(b, bo + 8) & 0xffffffffL);
		}

		/** The last record whose key is <= KEY (in a leaf), or null. */
		Record find(byte[] key) throws IOException {
			int n = root;
			for (int depth = 0; depth < 32 && n != 0; depth++) {
				byte[] node = node(n);
				int kind = node[8];
				int count = records(node), best = -1;
				for (int i = 0; i < count; i++) {
					if (compare(node, recordOffset(node, i), key, 0) <= 0) {
						best = i;
					} else {
						break;
					}
				}
				if (kind == -1) {               /* leaf */
					if (best < 0) {
						return null;
					}
					Record r = new Record();
					r.node = node;
					r.key = recordOffset(node, best);
					r.data = r.key + 2 + be16(node, r.key);
					if ((r.data & 1) != 0) {
						r.data++;
					}
					return r;
				}
				if (best < 0) {
					best = 0;
				}
				int k = recordOffset(node, best);
				int ptr = k + 2 + be16(node, k);
				n = be32(node, ptr);
			}
			return null;
		}
	}

	/* a simplified HFS+ case fold (ASCII and Latin-1): only used on volumes that are not HFSX */
	private static int fold(int c) {
		if (c >= 'A' && c <= 'Z') {
			return c + 32;
		}
		if (c >= 0xC0 && c <= 0xDE && c != 0xD7) {
			return c + 32;
		}
		return c;
	}

	/* ---------- the catalog ---------- */

	private static byte[] catalogKey(int parent, String name) {
		char[] cs = name.toCharArray();
		byte[] k = new byte[8 + 2 * cs.length];
		put16(k, 0, 6 + 2 * cs.length);
		put32(k, 2, parent);
		put16(k, 6, cs.length);
		for (int i = 0; i < cs.length; i++) {
			put16(k, 8 + 2 * i, cs[i]);
		}
		return k;
	}

	/** The contents of folder ID, folders first, then by name. */
	List<Entry> list(int id) throws IOException {
		List<Entry> out = new ArrayList<>();
		byte[] start = catalogKey(id, "");
		Record r = cat.find(start);
		/* walk the leaves from there while the parent stays ID */
		byte[] node = r != null ? r.node : cat.node(cat.firstLeaf);
		int index = 0;
		if (r != null) {
			for (int i = 0; i < cat.records(node); i++) {
				if (cat.recordOffset(node, i) == r.key) {
					index = i;
				}
			}
		}
		for (int guard = 0; guard < 100000 && node != null; guard++) {
			int count = cat.records(node);
			for (int i = index; i < count; i++) {
				int k = cat.recordOffset(node, i);
				int parent = be32(node, k + 2);
				if (parent < id) {
					continue;
				}
				if (parent > id) {
					sort(out);
					return out;
				}
				int d = k + 2 + be16(node, k);
				if ((d & 1) != 0) {
					d++;
				}
				Entry e = entry(node, k, d);
				if (e != null) {
					out.add(e);
				}
			}
			int next = be32(node, 0);
			if (next == 0) {
				break;
			}
			node = cat.node(next);
			index = 0;
		}
		sort(out);
		return out;
	}

	private static void sort(List<Entry> l) {
		Collections.sort(l, (a, b) -> a.folder != b.folder ? (a.folder ? -1 : 1) : a.name.compareToIgnoreCase(b.name));
	}

	private Entry entry(byte[] node, int k, int d) {
		int type = be16(node, d);
		if (type != 1 && type != 2) {
			return null;                    /* thread records */
		}
		Entry e = new Entry();
		int len = be16(node, k + 6);
		char[] cs = new char[len];
		for (int i = 0; i < len; i++) {
			cs[i] = (char) be16(node, k + 8 + 2 * i);
		}
		e.name = new String(cs);
		e.parent = be32(node, k + 2);
		e.folder = type == 1;
		e.id = be32(node, d + 8);
		e.modified = ((be32(node, d + 16) & 0xffffffffL) - EPOCH_OFFSET) * 1000L;
		e.uid = be32(node, d + 32);
		e.gid = be32(node, d + 36);
		int ownerFlags = node[d + 41] & 0xff;
		e.mode = be16(node, d + 42);
		e.compressed = (ownerFlags & 0x20) != 0;
		if (e.folder) {
			e.size = be32(node, d + 4) & 0xffffffffL;
		} else {
			e.data = fork(node, d + 88, e.id, 0);
			e.size = e.data.size;
			e.symlink = (e.mode & 0xf000) == 0xa000;
			String fileType = new String(node, d + 48, 4, StandardCharsets.ISO_8859_1);
			String creator = new String(node, d + 52, 4, StandardCharsets.ISO_8859_1);
			if (fileType.equals("hlnk") && creator.equals("hfs+")) {
				e.hardLink = true;
				e.id = be32(node, d + 44);  /* permissions.special: the iNode number */
			}
			if (e.name.startsWith("\u0000\u0000\u0000\u0000HFS+ Private Data")) {
				return null;
			}
		}
		if (e.folder && e.name.equals("\u0000\u0000\u0000\u0000HFS+ Private Data")) {
			privateDirId = e.id;
			return null;
		}
		if (e.parent == ROOT_ID && (e.name.equals(".HFS+ Private Directory Data\r") || e.name.equals(".journal")
		    || e.name.equals(".journal_info_block"))) {
			return null;
		}
		return e;
	}

	/** The entry at PATH ("/System/Library/..."), following no symlinks, or null. */
	Entry lookup(String path) throws IOException {
		int dir = ROOT_ID;
		Entry found = null;
		for (String part : path.split("/")) {
			if (part.isEmpty()) {
				continue;
			}
			if (found != null && !found.folder) {
				return null;
			}
			Record r = cat.find(catalogKey(dir, part));
			if (r == null || be32(r.node, r.key + 2) != dir || be16(r.node, r.key + 6) != part.length()) {
				return null;
			}
			found = entry(r.node, r.key, r.data);
			if (found == null || !found.name.equals(part)) {
				return null;
			}
			dir = found.id;
		}
		return found;
	}

	/** iOS's ProductVersion and ProductBuildVersion, from SystemVersion.plist ("16.7.16 (20H392)"). */
	String iosVersion() throws IOException {
		Entry e = lookup("/System/Library/CoreServices/SystemVersion.plist");
		if (e == null) {
			return null;
		}
		String s = new String(head(e, 8192), StandardCharsets.UTF_8);
		return plistString(s, "ProductVersion") + " (" + plistString(s, "ProductBuildVersion") + ")";
	}

	private static String plistString(String xml, String key) {
		int k = xml.indexOf("<key>" + key + "</key>");
		int a = k < 0 ? -1 : xml.indexOf("<string>", k), b = a < 0 ? -1 : xml.indexOf("</string>", a);
		return a < 0 || b < 0 ? "?" : xml.substring(a + 8, b);
	}

	/** A hard link's real file (the iNode in the private folder), or the entry itself. */
	Entry resolve(Entry e) throws IOException {
		if (!e.hardLink) {
			return e;
		}
		if (privateDirId < 0) {
			list(ROOT_ID);
		}
		Record r = cat.find(catalogKey(privateDirId, "iNode" + e.id));
		if (r == null) {
			throw new IOException(L.t("enlace duro sin destino (iNode", "hard link without target (iNode") + e.id + ")");
		}
		Entry t = entry(r.node, r.key, r.data);
		if (t == null) {
			throw new IOException(L.t("enlace duro sin destino", "hard link without target"));
		}
		t.name = e.name;
		return t;
	}

	/** The first LIMIT bytes of a file (text and image previews). */
	byte[] head(Entry e, int limit) throws IOException {
		Entry t = resolve(e);
		if (t.compressed) {
			throw new IOException(L.t("fichero comprimido (decmpfs): todavía no se puede leer desde Android", "compressed file (decmpfs): cannot be read from Android yet"));
		}
		int n = (int) Math.min(t.size, limit);
		return n == 0 ? new byte[0] : readFork(t.data, 0, n);
	}

	/** The whole file into OUT; PROGRESS gets bytes done. */
	void copy(Entry e, OutputStream out, java.util.function.LongConsumer progress) throws IOException {
		Entry t = resolve(e);
		if (t.compressed) {
			throw new IOException(L.t("fichero comprimido (decmpfs): todavía no se puede exportar", "compressed file (decmpfs): cannot be exported yet"));
		}
		for (long done = 0; done < t.size; ) {
			int n = (int) Math.min(4L << 20, t.size - done);
			out.write(readFork(t.data, done, n));
			done += n;
			if (progress != null) {
				progress.accept(done);
			}
		}
	}

	/* ---------- big-endian helpers ---------- */

	static int be16(byte[] b, int o) {
		return (b[o] & 0xff) << 8 | (b[o + 1] & 0xff);
	}

	static int be32(byte[] b, int o) {
		return (b[o] & 0xff) << 24 | (b[o + 1] & 0xff) << 16 | (b[o + 2] & 0xff) << 8 | (b[o + 3] & 0xff);
	}

	static long be64(byte[] b, int o) {
		return (be32(b, o) & 0xffffffffL) << 32 | (be32(b, o + 4) & 0xffffffffL);
	}

	static void put16(byte[] b, int o, int v) {
		b[o] = (byte) (v >> 8);
		b[o + 1] = (byte) v;
	}

	static void put32(byte[] b, int o, int v) {
		b[o] = (byte) (v >> 24);
		b[o + 1] = (byte) (v >> 16);
		b[o + 2] = (byte) (v >> 8);
		b[o + 3] = (byte) v;
	}
}
