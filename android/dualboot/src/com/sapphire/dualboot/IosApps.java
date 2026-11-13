package com.sapphire.dualboot;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The apps installed on iOS, read from its volume through Hfs: the system ones in /Applications and the
 * user's in /private/var/containers/Bundle/Application/<UUID>/X.app. Read-only, like Hfs.
 */
final class IosApps {
	static final String SYSTEM = "/Applications", USER = "/private/var/containers/Bundle/Application";

	/*
	 * The names iOS shows in Spanish for its own apps: this image's bundles carry empty es.lproj folders
	 * (the localisations were stripped to save space), so their InfoPlist.strings cannot be read.
	 */
	private static final java.util.Map<String, String> SPANISH = new java.util.HashMap<>();

	static {
		String[] p = {"com.apple.Preferences", L.t("Ajustes", "Settings"), "com.apple.calculator", L.t("Calculadora", "Calculator"), "com.apple.mobilecal", L.t("Calendario", "Calendar"),
			"com.apple.camera", L.t("Cámara", "Camera"), "com.apple.mobiletimer", L.t("Reloj", "Clock"), "com.apple.compass", L.t("Brújula", "Compass"), "com.apple.MobileAddressBook",
			L.t("Contactos", "Contacts"), "com.apple.DocumentsApp", L.t("Archivos", "Files"), "com.apple.findmy", L.t("Buscar", "Find My"), "com.apple.Health", L.t("Salud", "Health"), "com.apple.Home",
			L.t("Casa", "Home"), "com.apple.Magnifier", L.t("Lupa", "Magnifier"), "com.apple.Maps", L.t("Mapas", "Maps"), "com.apple.measure", L.t("Medidas", "Measure"), "com.apple.MobileSMS",
			L.t("Mensajes", "Messages"), "com.apple.Music", L.t("Música", "Music"), "com.apple.mobilenotes", L.t("Notas", "Notes"), "com.apple.mobilephone", L.t("Teléfono", "Phone"),
			"com.apple.mobileslideshow", L.t("Fotos", "Photos"), "com.apple.reminders", L.t("Recordatorios", "Reminders"), "com.apple.stocks", L.t("Bolsa", "Stocks"), "com.apple.tips",
			L.t("Consejos", "Tips"), "com.apple.Translate", L.t("Traducir", "Translate"), "com.apple.VoiceMemos", L.t("Notas de voz", "Voice Memos"), "com.apple.weather", L.t("Tiempo", "Weather"),
			"com.apple.Passbook", "Wallet", "com.apple.tv", "TV", "com.apple.iBooks", L.t("Libros", "Books"), "com.apple.shortcuts", L.t("Atajos", "Shortcuts"),
			"com.apple.AppStore", "App Store", "com.apple.Bridge", "Watch", "com.apple.mobilemail", "Mail", "com.apple.mobilesafari", "Safari",
			"com.apple.podcasts", "Podcasts", "com.apple.freeform", "Freeform", "com.apple.Fitness", "Fitness", "com.apple.facetime", "FaceTime",
			"com.apple.MobileStore", "iTunes Store", "com.apple.news", "News", "com.apple.appleseed.FeedbackAssistant", L.t("Asistente de Feedback", "Feedback Assistant")};
		for (int i = 0; i < p.length; i += 2) {
			SPANISH.put(p[i], p[i + 1]);
		}
	}

	private IosApps() {
	}

	interface Progress {
		/** Returns false to stop. */
		boolean on(int done, int total);
	}

	static List<IosApp> list(Hfs h, Progress p) throws IOException {
		List<Object[]> bundles = new ArrayList<>();     /* {Hfs.Entry, path, system} */
		Hfs.Entry sys = h.lookup(SYSTEM);
		if (sys != null && sys.folder) {
			for (Hfs.Entry e : h.list(sys.id)) {
				if (e.folder && e.name.endsWith(".app")) {
					bundles.add(new Object[]{e, SYSTEM + "/" + e.name, true});
				}
			}
		}
		Hfs.Entry user = h.lookup(USER);
		if (user != null && user.folder) {
			for (Hfs.Entry uuid : h.list(user.id)) {
				if (!uuid.folder) {
					continue;
				}
				for (Hfs.Entry e : h.list(uuid.id)) {
					if (e.folder && e.name.endsWith(".app")) {
						bundles.add(new Object[]{e, USER + "/" + uuid.name + "/" + e.name, false});
					}
				}
			}
		}
		List<IosApp> out = new ArrayList<>();
		for (int i = 0; i < bundles.size(); i++) {
			if (p != null && !p.on(i, bundles.size())) {
				break;
			}
			Hfs.Entry dir = (Hfs.Entry) bundles.get(i)[0];
			try {
				IosApp a = app(h, dir);
				a.path = (String) bundles.get(i)[1];
				a.system = (Boolean) bundles.get(i)[2];
				String es = SPANISH.get(a.bundleId);
				if (es != null && a.system) {
					a.name = es;
				}
				out.add(a);
			} catch (IOException ignored) {
				/* a bundle without a readable Info.plist: not an app anyone can open */
			}
		}
		Collections.sort(out, (a, b) -> a.name.compareToIgnoreCase(b.name));
		return out;
	}

	/** The app in bundle folder DIR: its Info.plist, its Spanish name. */
	static IosApp app(Hfs h, Hfs.Entry dir) throws IOException {
		List<Hfs.Entry> files = h.list(dir.id);
		Hfs.Entry plist = find(files, "Info.plist");
		if (plist == null || plist.folder) {
			throw new IOException(L.t("sin Info.plist", "no Info.plist"));
		}
		IosApp a = IosApp.fromInfo(Plist.dict(Plist.parse(h.head(plist, 4 << 20))));
		a.dirId = dir.id;
		a.files = new ArrayList<>();
		for (Hfs.Entry e : files) {
			a.files.add(e.name);
		}
		Hfs.Entry es = find(files, "es.lproj");
		if (L.ES && es != null && es.folder) {
			Hfs.Entry s = find(h.list(es.id), "InfoPlist.strings");
			if (s != null && !s.folder && !s.compressed) {
				try {
					a.localize(Plist.dict(Plist.parse(h.head(s, 1 << 20))));
				} catch (IOException ignored) {
				}
			}
		}
		return a;
	}

	/** The PNG bytes of A's icon from its bundle, or null (only an asset catalogue, or a compressed file). */
	static byte[] icon(Hfs h, IosApp a) {
		try {
			String best = IosApp.bestIcon(a.iconNames, a.files);
			if (best == null) {
				return null;
			}
			Hfs.Entry e = find(h.list(a.dirId), best);
			return e == null || e.compressed || e.size > (4 << 20) ? null : h.head(e, 4 << 20);
		} catch (IOException e) {
			return null;
		}
	}

	/** Bytes of everything under folder ID (hard links counted once each place they appear). */
	static long size(Hfs h, int id) throws IOException {
		long n = 0;
		for (Hfs.Entry e : h.list(id)) {
			n += e.folder ? size(h, e.id) : e.size;
		}
		return n;
	}

	static Hfs.Entry find(List<Hfs.Entry> l, String name) {
		for (Hfs.Entry e : l) {
			if (e.name.equals(name)) {
				return e;
			}
		}
		return null;
	}
}
