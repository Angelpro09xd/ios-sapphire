package com.sapphire.dualboot;

import java.io.BufferedWriter;
import java.io.FileWriter;
import java.util.ArrayDeque;
import java.util.Deque;

/**
 * HfsWalk: every entry of iOS's root, read from Android through Hfs.java, one line each:
 *   TYPE MODE UID GID SIZE MTIME PATH [-> TARGET]
 * (TYPE d/f/l/h for folder, file, symlink, hard link; MODE octal; MTIME seconds since 1970). For the dual-boot
 * installer's inventory: compared with the same listing of the image first flashed (tools/inventory).
 *   CLASSPATH=walk.dex app_process / com.sapphire.dualboot.HfsWalk OUT.txt
 */
public class HfsWalk {
	public static void main(String[] a) throws Exception {
		Hfs h = Hfs.open(Ios.ROOT);
		long t = System.currentTimeMillis();
		int n = 0;
		Deque<Object[]> todo = new ArrayDeque<>();
		todo.push(new Object[]{Hfs.ROOT_ID, ""});
		try (BufferedWriter w = new BufferedWriter(new FileWriter(a[0]), 1 << 20)) {
			while (!todo.isEmpty()) {
				Object[] d = todo.pop();
				for (Hfs.Entry e : h.list((Integer) d[0])) {
					String p = d[1] + "/" + e.name.replace("\n", "\\n");
					String type = e.folder ? "d" : e.symlink ? "l" : e.hardLink ? "h" : "f";
					long size = e.folder ? 0 : e.size;
					if (e.hardLink) {
						try {
							size = h.resolve(e).size;
						} catch (Exception x) {
							size = -1;
						}
					}
					StringBuilder s = new StringBuilder();
					s.append(type).append(' ').append(Integer.toOctalString(e.mode & 07777)).append(' ').append(e.uid).append(' ')
					    .append(e.gid).append(' ').append(size).append(' ').append(e.modified / 1000).append(' ').append(p);
					if (e.symlink && e.size > 0 && e.size < 4096) {
						s.append(" -> ").append(new String(h.head(e, 4096), "UTF-8"));
					}
					w.write(s.toString());
					w.write('\n');
					n++;
					if (e.folder) {
						todo.push(new Object[]{e.id, p});
					}
				}
			}
		}
		System.out.println(n + " entries in " + (System.currentTimeMillis() - t) / 1000 + " s");
		RootShell.close();
	}
}
