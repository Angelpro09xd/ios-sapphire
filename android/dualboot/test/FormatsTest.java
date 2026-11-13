package com.sapphire.dualboot;

import java.io.File;
import java.nio.file.Files;
import java.util.List;
import java.util.Map;

/**
 * FormatsTest: Plist (binary and XML), Png (CgBI icons) and IosApp.fromIpa against the fixtures that
 * test/mkfixtures.py writes (Python's plistlib and zlib as the reference). On the PC.
 */
public class FormatsTest {
	static int failures;

	static void check(boolean ok, String what) {
		System.out.println((ok ? "ok   " : "FAIL ") + what);
		if (!ok) {
			failures++;
		}
	}

	public static void main(String[] a) throws Exception {
		File dir = new File(a[0]);
		for (String f : new String[]{"info.bplist", "info.xml"}) {
			Map<String, Object> d = Plist.dict(Plist.parse(Files.readAllBytes(new File(dir, f).toPath())));
			check(d != null, f + ": a dict");
			check("Demo & Co".equals(d.get("CFBundleDisplayName")), f + ": string with &");
			check("Configuración ✓".equals(d.get("Unicode")), f + ": unicode string");
			check(Long.valueOf(1L << 40).equals(d.get("Big")), f + ": 2^40");
			check(Double.valueOf(0.5).equals(d.get("Ratio")), f + ": real");
			check(Boolean.TRUE.equals(d.get("Flag")), f + ": true");
			check(d.get("Blob") instanceof byte[] && ((byte[]) d.get("Blob")).length == 3 && ((byte[]) d.get("Blob"))[2] == 2, f + ": data");
			List<Object> fam = Plist.list(d.get("UIDeviceFamily"));
			check(fam != null && fam.size() == 2 && Long.valueOf(2).equals(fam.get(1)), f + ": array of ints");
			IosApp app = IosApp.fromInfo(d);
			check(app.bundleId.equals("com.example.demo") && app.versionText().equals("1.2.3 (45)"), f + ": app " + app.bundleId + " " + app.versionText());
			check(app.iconNames.contains("AppIcon60x60") && app.iconNames.contains("AppIcon"), f + ": icon names " + app.iconNames);
		}
		check(Plist.dict(Plist.parse("\"CFBundleDisplayName\" = \"Ajustes \\\"X\\\"\";".getBytes("UTF-8"))).get("CFBundleDisplayName")
		    .equals("Ajustes \"X\""), "text .strings");
		try {
			Plist.parse(new byte[]{'b', 'p', 'l', 'i', 's', 't', '0', '0', 1, 2, 3});
			check(false, "truncated bplist refused");
		} catch (java.io.IOException e) {
			check(true, "truncated bplist refused: " + e.getMessage());
		}

		byte[] png = Files.readAllBytes(new File(dir, "icon-cgbi.png").toPath());
		int[] wh = new int[2];
		int[] px = Png.decodeCgBI(png, wh);
		String[] exp = new String(Files.readAllBytes(new File(dir, "icon-cgbi.txt").toPath())).trim().split(" ");
		StringBuilder got = new StringBuilder();
		for (int i = 0; px != null && i < px.length; i++) {
			got.append(i > 0 ? " " : "").append(String.format("%08x", px[i]));
		}
		check(px != null && wh[0] == 3 && wh[1] == 2 && got.toString().equals(String.join(" ", exp)), "CgBI decode: " + got);

		IosApp ipa = IosApp.fromIpa(new File(dir, "demo.ipa"));
		check(ipa.name.equals("Demostración"), "IPA: Spanish name from UTF-16 InfoPlist.strings: " + ipa.name);
		check(ipa.path.equals("Demo.app") && ipa.minOs.equals("14.0"), "IPA: bundle Demo.app, iOS 14.0");
		check(ipa.icon != null && ipa.icon.length == png.length + 1, "IPA: the @3x iPhone icon chosen");
		check(ipa.size > 1000, "IPA: payload size " + ipa.size);
		System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
		System.exit(failures == 0 ? 0 : 1);
	}
}
