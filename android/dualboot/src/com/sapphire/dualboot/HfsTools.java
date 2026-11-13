package com.sapphire.dualboot;

import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Whole-tree jobs over iOS's volume (read-only): search by name, a folder streamed out as a zip. */
final class HfsTools {
	private HfsTools() {
	}

	/** Set from the UI to stop a running job. */
	static final class Cancel {
		volatile boolean cancelled;
	}

	interface Found {
		void on(Hfs.Entry e, String path);
	}

	interface Status {
		void on(long seen, String where);
	}

	/**
	 * Every entry under folder FROM (path FROMPATH) whose name contains QUERY (any case), breadth first so
	 * shallow matches come early. Stops at MAX matches or when cancelled; returns the entries seen.
	 */
	static long search(Hfs h, int from, String fromPath, String query, int max, Cancel c, Found found, Status status) throws IOException {
		String q = query.toLowerCase(Locale.ROOT);
		Deque<Object[]> todo = new ArrayDeque<>();
		todo.add(new Object[]{from, fromPath.equals("/") ? "" : fromPath});
		long seen = 0;
		int matches = 0;
		while (!todo.isEmpty() && !c.cancelled) {
			Object[] d = todo.poll();
			List<Hfs.Entry> l;
			try {
				l = h.list((Integer) d[0]);
			} catch (IOException e) {
				continue;
			}
			for (Hfs.Entry e : l) {
				String p = d[1] + "/" + e.name;
				seen++;
				if (e.name.toLowerCase(Locale.ROOT).contains(q)) {
					found.on(e, p);
					if (++matches >= max) {
						return seen;
					}
				}
				if (e.folder) {
					todo.add(new Object[]{e.id, p});
				}
			}
			if (status != null) {
				status.on(seen, (String) d[1]);
			}
		}
		return seen;
	}

	/** Totals of folder ID: {files, bytes}. */
	static long[] measure(Hfs h, int id, Cancel c) throws IOException {
		long[] t = new long[2];
		Deque<Integer> todo = new ArrayDeque<>();
		todo.add(id);
		while (!todo.isEmpty() && !c.cancelled) {
			for (Hfs.Entry e : h.list(todo.poll())) {
				if (e.folder) {
					todo.add(e.id);
				} else if (!e.symlink) {
					t[0]++;
					t[1] += e.size;
				}
			}
		}
		return t;
	}

	/**
	 * Folder ID as a zip into OUT, its contents under PREFIX ("Carpeta/" or, for an .ipa, "Payload/X.app/").
	 * Files iOS keeps compressed (decmpfs) cannot be read from here yet: they are left out and listed in
	 * an OMITIDOS.txt inside the zip. Returns the number of files left out.
	 */
	static int zip(Hfs h, int id, String prefix, OutputStream out, Cancel c, Status status) throws IOException {
		List<String> skipped = new ArrayList<>();
		long[] done = {0};
		try (ZipOutputStream z = new ZipOutputStream(out)) {
			z.setLevel(1);              /* speed over size: much of iOS is already compressed */
			Deque<Object[]> todo = new ArrayDeque<>();
			todo.add(new Object[]{id, prefix});
			while (!todo.isEmpty()) {
				if (c.cancelled) {
					throw new IOException(L.t("cancelado", "cancelled"));
				}
				Object[] d = todo.poll();
				String dir = (String) d[1];
				if (!dir.isEmpty()) {
					z.putNextEntry(new ZipEntry(dir));
					z.closeEntry();
				}
				for (Hfs.Entry e : h.list((Integer) d[0])) {
					String p = dir + e.name;
					if (e.folder) {
						todo.add(new Object[]{e.id, p + "/"});
						continue;
					}
					Hfs.Entry t;
					try {
						t = h.resolve(e);
					} catch (IOException x) {
						skipped.add(p + L.t("  (enlace roto)", "  (broken link)"));
						continue;
					}
					if (t.compressed) {
						skipped.add(p + L.t("  (comprimido por iOS)", "  (compressed by iOS)"));
						continue;
					}
					if (e.symlink) {
						skipped.add(p + "  -> " + new String(h.head(e, 4096), java.nio.charset.StandardCharsets.UTF_8));
						continue;
					}
					ZipEntry ze = new ZipEntry(p);
					ze.setTime(e.modified);
					z.putNextEntry(ze);
					long base = done[0];
					h.copy(t, z, n -> {
						done[0] = base + n;
						if (status != null) {
							status.on(done[0], p);
						}
						if (c.cancelled) {
							throw new java.io.UncheckedIOException(new IOException(L.t("cancelado", "cancelled")));
						}
					});
					z.closeEntry();
				}
			}
			if (!skipped.isEmpty()) {
				z.putNextEntry(new ZipEntry(prefix + L.t("OMITIDOS.txt", "SKIPPED.txt")));
				z.write((L.t("Ficheros que no se pudieron copiar desde Android (comprimidos por iOS con decmpfs) y enlaces ", "Files that could not be copied from Android (compressed by iOS with decmpfs) and symbolic ")
				    + L.t("simbólicos:\n\n", "links:\n\n") + String.join("\n", skipped) + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
				z.closeEntry();
			}
		} catch (java.io.UncheckedIOException e) {
			throw e.getCause();
		}
		return skipped.size();
	}
}
