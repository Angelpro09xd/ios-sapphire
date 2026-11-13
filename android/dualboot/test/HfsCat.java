package com.sapphire.dualboot;

import java.io.FileOutputStream;

/**
 * HfsCat: files out of iOS's root, read from Android through Hfs.java (installer inventory).
 *   CLASSPATH=cat.dex app_process / com.sapphire.dualboot.HfsCat OUTDIR /path/in/ios [/another ...]
 * Each file is written to OUTDIR with its path's slashes turned into '__'.
 */
public class HfsCat {
	public static void main(String[] a) throws Exception {
		Hfs h = Hfs.open(Ios.ROOT);
		for (int i = 1; i < a.length; i++) {
			Hfs.Entry e = h.lookup(a[i]);
			if (e == null || e.folder) {
				System.out.println("missing: " + a[i]);
				continue;
			}
			String out = a[0] + "/" + a[i].replaceAll("^/", "").replace("/", "__");
			try (FileOutputStream o = new FileOutputStream(out)) {
				h.copy(e, o, null);
			}
			System.out.println(a[i] + " -> " + out + " (" + h.resolve(e).size + " bytes)");
		}
		RootShell.close();
	}
}
