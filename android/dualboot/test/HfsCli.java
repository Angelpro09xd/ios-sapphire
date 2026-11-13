package com.sapphire.dualboot;

import java.util.List;

/**
 * HfsCli: Hfs.java run on the phone without the UI (app_process), against iOS's root partition.
 *   CLASSPATH=/data/local/tmp/hfscli.dex app_process / com.sapphire.dualboot.HfsCli [/path/in/ios] [file-to-head]
 */
public class HfsCli {
	public static void main(String[] a) throws Exception {
		Hfs h = Hfs.open(Ios.ROOT);
		System.out.println(h.signature + " block " + h.blockSize + " total " + Ui.size(h.totalBytes()) + " used " + Ui.size(h.usedBytes()));
		int dir = Hfs.ROOT_ID;
		String path = a.length > 0 ? a[0] : "/";
		for (String part : path.split("/")) {
			if (part.isEmpty()) {
				continue;
			}
			Hfs.Entry found = null;
			for (Hfs.Entry e : h.list(dir)) {
				if (e.name.equals(part)) {
					found = e;
				}
			}
			if (found == null || !found.folder) {
				throw new Exception("no folder " + part);
			}
			dir = found.id;
		}
		long t = System.currentTimeMillis();
		List<Hfs.Entry> l = h.list(dir);
		System.out.println(path + ": " + l.size() + " entries in " + (System.currentTimeMillis() - t) + " ms");
		for (int i = 0; i < Math.min(l.size(), 25); i++) {
			Hfs.Entry e = l.get(i);
			System.out.println((e.folder ? "d " : e.symlink ? "l " : "- ") + e.name + "  " + e.size + (e.compressed ? " [compressed]" : "")
			    + (e.hardLink ? " [hardlink]" : ""));
		}
		if (a.length > 1) {
			for (Hfs.Entry e : l) {
				if (e.name.equals(a[1])) {
					byte[] b = h.head(e, 400);
					System.out.println("--- head of " + e.name + " (" + e.size + " bytes):\n" + new String(b, "UTF-8"));
				}
			}
		}
		RootShell.close();
	}
}
