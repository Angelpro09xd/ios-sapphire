package com.sapphire.dualboot;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * An iOS app as its Info.plist describes it: from an IPA (a zip: Payload/X.app/...) or from a bundle on iOS's
 * volume (IosApps). Plain Java: the icon stays PNG bytes (maybe CgBI), Icons makes the Bitmap.
 */
final class IosApp {
	String name, bundleId, version, build, minOs, executable, path;
	boolean system, hidden;
	long size = -1;                 /* bytes of the bundle (IPA: uncompressed payload), -1 unknown */
	byte[] icon;
	final List<String> iconNames = new ArrayList<>();
	Map<String, Object> info;
	/* on iOS's volume: the bundle folder and the names in it */
	int dirId;
	List<String> files;

	static IosApp fromInfo(Map<String, Object> d) throws IOException {
		if (d == null) {
			throw new IOException(L.t("Info.plist sin diccionario", "Info.plist without a dictionary"));
		}
		IosApp a = new IosApp();
		a.info = d;
		a.executable = Plist.string(d, "CFBundleExecutable");
		a.bundleId = or(Plist.string(d, "CFBundleIdentifier"), "?");
		a.name = clean(or(Plist.string(d, "CFBundleDisplayName"), or(Plist.string(d, "CFBundleName"), or(a.executable, a.bundleId))));
		a.version = or(Plist.string(d, "CFBundleShortVersionString"), null);
		a.build = or(Plist.string(d, "CFBundleVersion"), null);
		a.minOs = Plist.string(d, "MinimumOSVersion");
		List<Object> tags = Plist.list(d.get("SBAppTags"));
		a.hidden = tags != null && tags.contains("hidden");
		/* icon base names, most specific first: CFBundleIcons (iPhone), then ~ipad, then the old keys */
		for (String k : new String[]{"CFBundleIcons", "CFBundleIcons~ipad"}) {
			Map<String, Object> primary = Plist.dict(Plist.dict(d.get(k)) == null ? null : Plist.dict(d.get(k)).get("CFBundlePrimaryIcon"));
			if (primary != null) {
				addNames(a.iconNames, primary.get("CFBundleIconFiles"));
				addNames(a.iconNames, primary.get("CFBundleIconName"));
			}
		}
		addNames(a.iconNames, d.get("CFBundleIconFiles"));
		addNames(a.iconNames, d.get("CFBundleIconFile"));
		return a;
	}

	private static void addNames(List<String> to, Object o) {
		if (o instanceof String) {
			String s = ((String) o).replaceAll("(?i)\\.png$", "");
			if (!s.isEmpty() && !to.contains(s)) {
				to.add(s);
			}
		} else if (o instanceof List) {
			for (Object x : (List<?>) o) {
				addNames(to, x);
			}
		}
	}

	/** The name iOS shows in Spanish, from es.lproj/InfoPlist.strings, when there is one. */
	void localize(Map<String, Object> strings) {
		String n = strings == null ? null : or(Plist.string(strings, "CFBundleDisplayName"), Plist.string(strings, "CFBundleName"));
		if (n != null && !clean(n).isEmpty()) {
			name = clean(n);
		}
	}

	/* names carry invisible direction marks (WhatsApp's starts with U+200E): they break sorting and the initials */
	private static String clean(String s) {
		return s == null ? null : s.replaceAll("\\p{Cf}", "").trim();
	}

	String versionText() {
		if (version == null && build == null) {
			return L.t("versión ?", "version ?");
		}
		return (version != null ? version : build) + (build != null && version != null && !build.equals(version) ? " (" + build + ")" : "");
	}

	private static final Pattern SIZE = Pattern.compile("(\\d+(?:\\.\\d+)?)x\\d+");

	/**
	 * The best icon among FILES (names in the bundle's top folder) for the base NAMES: the largest one --
	 * the size in the name (60x60) times the scale (@2x, @3x) -- iPhone ones before ~ipad ones.
	 */
	static String bestIcon(List<String> names, Collection<String> files) {
		String best = null;
		double bestScore = -1;
		for (String f : files) {
			String l = f.toLowerCase(Locale.ROOT);
			if (!l.endsWith(".png")) {
				continue;
			}
			boolean named = false;
			for (String n : names) {
				if (l.startsWith(n.toLowerCase(Locale.ROOT))) {
					named = true;
				}
			}
			/* bundles with no icon keys still often carry Icon*.png / AppIcon*.png */
			if (!named && !(names.isEmpty() && (l.startsWith("appicon") || l.startsWith("icon")))) {
				continue;
			}
			Matcher m = SIZE.matcher(l);
			double side = m.find() ? Double.parseDouble(m.group(1)) : 57;
			double scale = l.contains("@3x") ? 3 : l.contains("@2x") ? 2 : 1;
			double score = side * scale * (l.contains("~ipad") ? 0.5 : 1) * (l.contains("small") ? 0.3 : 1);
			if (score > bestScore) {
				bestScore = score;
				best = f;
			}
		}
		return best;
	}

	/* ---------------- IPAs ---------------- */

	/** The app inside IPA FILE (Payload/X.app/Info.plist), with its icon bytes and payload size. */
	static IosApp fromIpa(File file) throws IOException {
		try (ZipFile z = new ZipFile(file)) {
			String app = null;
			long total = 0;
			for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
				ZipEntry e = en.nextElement();
				String n = e.getName();
				if (n.contains("..")) {
					continue;
				}
				if (n.startsWith("Payload/") && e.getSize() > 0) {
					total += e.getSize();
				}
				if (app == null && n.matches("Payload/[^/]+\\.app/Info\\.plist")) {
					app = n.substring(0, n.length() - "Info.plist".length());
				}
			}
			if (app == null) {
				throw new IOException(L.t("no es una IPA: falta Payload/<app>.app/Info.plist", "not an IPA: Payload/<app>.app/Info.plist is missing"));
			}
			IosApp a = fromInfo(Plist.dict(Plist.parse(read(z, z.getEntry(app + "Info.plist"), 4 << 20))));
			a.path = app.substring("Payload/".length(), app.length() - 1);
			a.size = total;
			ZipEntry strings = z.getEntry(app + "es.lproj/InfoPlist.strings");
			if (L.ES && strings != null) {
				try {
					a.localize(Plist.dict(Plist.parse(read(z, strings, 1 << 20))));
				} catch (IOException ignored) {
				}
			}
			List<String> top = new ArrayList<>();
			for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
				String n = en.nextElement().getName();
				if (n.startsWith(app) && n.indexOf('/', app.length()) < 0) {
					top.add(n.substring(app.length()));
				}
			}
			String icon = bestIcon(a.iconNames, top);
			if (icon != null) {
				a.icon = read(z, z.getEntry(app + icon), 2 << 20);
			}
			return a;
		}
	}

	private static byte[] read(ZipFile z, ZipEntry e, int max) throws IOException {
		if (e == null) {
			throw new IOException(L.t("entrada del zip inexistente", "zip entry does not exist"));
		}
		try (InputStream in = z.getInputStream(e)) {
			ByteArrayOutputStream o = new ByteArrayOutputStream();
			byte[] b = new byte[65536];
			for (int n; (n = in.read(b)) > 0; ) {
				o.write(b, 0, n);
				if (o.size() > max) {
					throw new IOException(L.t("«", "“") + e.getName() + L.t("» demasiado grande", "” too large"));
				}
			}
			return o.toByteArray();
		}
	}

	private static String or(String a, String b) {
		return a != null && !a.isEmpty() ? a : b;
	}
}
