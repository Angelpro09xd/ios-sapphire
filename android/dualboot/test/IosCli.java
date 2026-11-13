package com.sapphire.dualboot;

import java.util.List;

/**
 * IosCli: the new read-only pieces run on the phone without the UI (app_process): the IPA inbox's geometry
 * and header as they are on iosroot (only read: open() never writes), and the iOS apps with their icons.
 *   CLASSPATH=/data/local/tmp/ioscli.dex app_process / com.sapphire.dualboot.IosCli [inbox|apps|search QUERY]
 */
public class IosCli {
	public static void main(String[] a) throws Exception {
		String what = a.length > 0 ? a[0] : "inbox";
		if (what.equals("inbox")) {
			IpaInbox box = IpaInbox.device();
			IpaInbox.Geometry g = box.geometry;
			System.out.println("V=" + g.volumeEnd + " P=" + g.partition + " S=" + g.start + " E=" + g.end + " room=" + Ui.size(g.bytes())
			    + " (" + g.bytes() + ")");
			System.out.println("header present=" + box.present + " damaged=" + box.damaged + " entries=" + box.entries.size()
			    + " largest free=" + Ui.size(box.largestFree()));
			for (IpaInbox.Entry e : box.entries) {
				System.out.println("  " + e.name + " " + e.length + " @" + e.offset + " " + e.stateName());
			}
		} else if (what.equals("apps")) {
			Hfs h = Hfs.open(Ios.ROOT);
			long t = System.currentTimeMillis();
			List<IosApp> apps = IosApps.list(h, null);
			System.out.println(apps.size() + " apps in " + (System.currentTimeMillis() - t) + " ms");
			int icons = 0, cgbi = 0;
			for (IosApp x : apps) {
				byte[] ic = IosApps.icon(h, x);
				int[] wh = new int[2];
				boolean crushed = Png.isCgBI(ic), ok = ic != null && (!crushed || Png.decodeCgBI(ic, wh) != null);
				icons += ok ? 1 : 0;
				cgbi += crushed && ok ? 1 : 0;
				System.out.println((x.system ? "S " : "U ") + (x.hidden ? "h " : "  ") + x.name + " | " + x.bundleId + " | " + x.versionText()
				    + " | icon " + (ic == null ? "-" : (crushed ? "CgBI " + wh[0] + "x" + wh[1] : "png " + ic.length)));
			}
			System.out.println(icons + " icons, " + cgbi + " CgBI decoded");
		} else if (what.equals("search")) {
			Hfs h = Hfs.open(Ios.ROOT);
			HfsTools.Cancel c = new HfsTools.Cancel();
			long t = System.currentTimeMillis();
			long seen = HfsTools.search(h, Hfs.ROOT_ID, "/", a[1], 20, c, (e, p) -> System.out.println("  " + p), null);
			System.out.println(seen + " seen in " + (System.currentTimeMillis() - t) + " ms");
		}
		RootShell.close();
	}
}
