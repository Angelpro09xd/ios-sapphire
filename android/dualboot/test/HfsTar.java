package com.sapphire.dualboot;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * HfsTar: parts of iOS's root as a tar, read from Android through Hfs.java (the installer's overlay).
 *   CLASSPATH=tar.dex app_process / com.sapphire.dualboot.HfsTar OUT.tar EXCLUDES.txt /path [/path ...]
 * ustar with GNU long names, numeric owners (no names), modes, mtimes, symlinks; a file with several names
 * becomes one file and tar hard links. EXCLUDES: path prefixes left out (one per line, '-' for none).
 * Files iOS stores compressed (decmpfs) cannot be read here: listed on stderr as "SKIP compressed".
 */
public class HfsTar {
	static OutputStream out;
	static long written;

	static void octal(byte[] h, int off, int len, long v) {
		String s = Long.toOctalString(v);
		while (s.length() < len - 1) {
			s = "0" + s;
		}
		byte[] b = s.getBytes(StandardCharsets.US_ASCII);
		System.arraycopy(b, b.length - (len - 1), h, off, len - 1);
		h[off + len - 1] = 0;
	}

	static void write(byte[] b, int off, int len) throws IOException {
		out.write(b, off, len);
		written += len;
	}

	static void pad() throws IOException {
		int r = (int) (written % 512);
		if (r != 0) {
			write(new byte[512 - r], 0, 512 - r);
		}
	}

	static void header(String name, char type, int mode, int uid, int gid, long size, long mtime, String link) throws IOException {
		byte[] nb = name.getBytes(StandardCharsets.UTF_8);
		if (nb.length > 99) {
			/* GNU long name: a ././@LongLink entry holding the name */
			header("././@LongLink", 'L', 0, 0, 0, nb.length + 1, 0, null);
			write(nb, 0, nb.length);
			write(new byte[1], 0, 1);
			pad();
		}
		byte[] lb = link != null ? link.getBytes(StandardCharsets.UTF_8) : null;
		if (lb != null && lb.length > 99) {
			header("././@LongLink", 'K', 0, 0, 0, lb.length + 1, 0, null);
			write(lb, 0, lb.length);
			write(new byte[1], 0, 1);
			pad();
		}
		byte[] h = new byte[512];
		System.arraycopy(nb, 0, h, 0, Math.min(nb.length, 99));
		octal(h, 100, 8, mode & 07777);
		octal(h, 108, 8, uid);
		octal(h, 116, 8, gid);
		octal(h, 124, 12, size);
		octal(h, 136, 12, mtime);
		for (int i = 148; i < 156; i++) {
			h[i] = ' ';
		}
		h[156] = (byte) type;
		if (lb != null) {
			System.arraycopy(lb, 0, h, 157, Math.min(lb.length, 99));
		}
		System.arraycopy("ustar  \0".getBytes(StandardCharsets.US_ASCII), 0, h, 257, 8);   /* GNU magic */
		long sum = 0;
		for (byte x : h) {
			sum += x & 0xff;
		}
		octal(h, 148, 7, sum);
		h[155] = ' ';
		write(h, 0, 512);
	}

	public static void main(String[] a) throws Exception {
		List<String> excl = new ArrayList<>();
		if (!a[1].equals("-")) {
			try (BufferedReader r = new BufferedReader(new FileReader(a[1]))) {
				for (String l; (l = r.readLine()) != null; ) {
					l = l.trim();
					if (l.startsWith("/")) {
						excl.add(l);
					}
				}
			}
		}
		Hfs h = Hfs.open(Ios.ROOT);
		out = new BufferedOutputStream(new FileOutputStream(a[0]), 1 << 20);
		Map<Integer, String> linked = new HashMap<>();   /* inode id -> first name written */
		int files = 0, skipped = 0;
		long t = System.currentTimeMillis();
		for (int i = 2; i < a.length; i++) {
			String top = a[i];
			Hfs.Entry te = h.lookup(top);
			if (te == null) {
				System.err.println("MISSING " + top);
				continue;
			}
			/* the folders on the way: their own metadata */
			StringBuilder way = new StringBuilder();
			String[] parts = top.substring(1).split("/");
			for (int k = 0; k < parts.length - 1; k++) {
				way.append('/').append(parts[k]);
				Hfs.Entry d = h.lookup(way.toString());
				header(way.substring(1) + "/", '5', d.mode, d.uid, d.gid, 0, d.modified / 1000, null);
			}
			Deque<Object[]> todo = new ArrayDeque<>();
			todo.push(new Object[]{te, top});
			while (!todo.isEmpty()) {
				Object[] item = todo.pop();
				Hfs.Entry e = (Hfs.Entry) item[0];
				String p = (String) item[1];
				boolean ex = false;
				for (String x : excl) {
					if (p.equals(x) || p.startsWith(x.endsWith("/") ? x : x + "/")) {
						ex = true;
						break;
					}
				}
				if (ex) {
					continue;
				}
				String name = p.substring(1);
				long mt = e.modified / 1000;
				if (e.folder) {
					header(name + "/", '5', e.mode, e.uid, e.gid, 0, mt, null);
					List<Hfs.Entry> kids = h.list(e.id);
					for (int k = kids.size() - 1; k >= 0; k--) {
						todo.push(new Object[]{kids.get(k), p + "/" + kids.get(k).name});
					}
				} else if (e.symlink) {
					String target = new String(h.head(e, 4096), StandardCharsets.UTF_8);
					header(name, '2', e.mode == 0 ? 0755 : e.mode, e.uid, e.gid, 0, mt, target);
				} else {
					Hfs.Entry f = e.hardLink ? h.resolve(e) : e;
					if (f.compressed) {
						System.err.println("SKIP compressed " + p);
						skipped++;
						continue;
					}
					if (e.hardLink) {
						String first = linked.get(f.id);
						if (first != null) {
							header(name, '1', f.mode, f.uid, f.gid, 0, f.modified / 1000, first);
							continue;
						}
						linked.put(f.id, name);
					}
					header(name, '0', f.mode, f.uid, f.gid, f.size, f.modified / 1000, null);
					final long[] got = {0};
					h.copy(f, new OutputStream() {
						@Override
						public void write(int b) throws IOException {
							HfsTar.write(new byte[]{(byte) b}, 0, 1);
							got[0]++;
						}

						@Override
						public void write(byte[] b, int off, int len) throws IOException {
							HfsTar.write(b, off, len);
							got[0] += len;
						}
					}, null);
					if (got[0] != f.size) {
						throw new IOException(p + ": " + got[0] + " bytes of " + f.size);
					}
					pad();
					files++;
				}
			}
		}
		write(new byte[1024], 0, 1024);
		out.close();
		System.err.println("DONE " + files + " files, " + skipped + " skipped, " + written / 1000000 + " MB, " + (System.currentTimeMillis() - t) / 1000 + " s");
		RootShell.close();
	}
}
