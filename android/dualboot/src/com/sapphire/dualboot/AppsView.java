package com.sapphire.dualboot;

import android.graphics.Bitmap;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Apps de iOS: what is installed on iOS, read from its volume (IosApps): the user's apps and the system's,
 * with their icons when the bundle carries PNGs, a search, and per app its details, its Info.plist, its
 * folder in Archivos and an export as .ipa. Reading every bundle takes a while (half a minute), so the list
 * is kept for as long as the app runs.
 */
final class AppsView {
	private static List<IosApp> cache;
	private static final Map<String, Bitmap> icons = java.util.Collections.synchronizedMap(new HashMap<>());
	/* bumped on every render: an icon loader of an older list stops */
	private static volatile int generation;
	private static boolean showSystem, showHidden;
	private static String query = "";

	private AppsView() {
	}

	static void build(MainActivity a, LinearLayout col, TextView sub) {
		LinearLayout filters = Ui.chipRow(a);
		filters.addView(Ui.pill(a, L.t("Instaladas", "Installed"), !showSystem, v -> {
			showSystem = false;
			a.show(MainActivity.PAGE_FILES);
		}));
		filters.addView(Ui.pill(a, L.t("Del sistema", "System"), showSystem, v -> {
			showSystem = true;
			a.show(MainActivity.PAGE_FILES);
		}));
		if (showSystem) {
			filters.addView(Ui.pill(a, showHidden ? L.t("Ocultas: sí", "Hidden: yes") : L.t("Ocultas: no", "Hidden: no"), showHidden, v -> {
				showHidden = !showHidden;
				a.show(MainActivity.PAGE_FILES);
			}));
		}
		View space = new View(a);
		filters.addView(space, new LinearLayout.LayoutParams(0, 1, 1));
		filters.addView(Ui.iconButton(a, "refresh", Ui.STANDARD, v -> {
			cache = null;
			a.show(MainActivity.PAGE_FILES);
		}), new LinearLayout.LayoutParams(a.dp(40), a.dp(40)));
		col.addView(filters, Ui.lp(-1, -2, a.dp(12)));

		EditText search = Ui.field(a, L.t("Buscar por nombre o identificador", "Search by name or bundle ID"));
		search.setText(query);
		col.addView(search, Ui.lp(-1, -2, a.dp(12)));

		LinearLayout list = Ui.list(a);
		col.addView(list, Ui.lp(-1, -2, a.dp(12)));
		TextView loading = new Ui.LoadingText(a, L.t("Leyendo las apps de iOS…", "Reading the iOS apps…"));
		loading.setPadding(a.dp(18), a.dp(16), a.dp(18), a.dp(16));
		list.addView(loading);

		Runnable render = () -> {
			list.removeAllViews();
			List<IosApp> shown = filter();
			int user = 0, system = 0;
			for (IosApp x : cache) {
				if (x.system) {
					system++;
				} else {
					user++;
				}
			}
			sub.setText(user + L.t(" instaladas por ti · ", " installed by you · ") + system + L.t(" del sistema", " system"));
			if (shown.isEmpty()) {
				TextView t = Ui.text(a, query.isEmpty() ? L.t("No hay apps.", "No apps.") : L.t("Nada coincide con «", "Nothing matches “") + query + L.t("».", "”."), 14, Ui.TEXT3, false);
				t.setPadding(a.dp(14), a.dp(14), a.dp(14), a.dp(14));
				list.addView(t);
			}
			for (IosApp x : shown) {
				list.addView(row(a, x));
			}
			loadIcons(a, list, shown);
		};
		search.addTextChangedListener(new TextWatcher() {
			@Override
			public void beforeTextChanged(CharSequence s, int st, int c, int af) {
			}

			@Override
			public void onTextChanged(CharSequence s, int st, int b, int c) {
			}

			@Override
			public void afterTextChanged(Editable s) {
				query = s.toString().trim();
				if (cache != null) {
					render.run();
				}
			}
		});
		if (cache != null) {
			render.run();
			return;
		}
		a.background(() -> {
			List<IosApp> l = IosApps.list(a.hfs(), (done, total) -> {
				a.runOnUiThread(() -> loading.setText(L.t("Leyendo las apps de iOS… ", "Reading the iOS apps… ") + done + L.t(" de ", " of ") + total));
				return a.page() == MainActivity.PAGE_FILES && a.filesMode == MainActivity.APPS;
			});
			if (l.isEmpty()) {
				throw new java.io.IOException(L.t("no se encontró ninguna app", "no apps found"));
			}
			cache = l;
			a.runOnUiThread(() -> {
				if (loading.isAttachedToWindow()) {
					render.run();
				}
			});
			return null;
		}, err -> loading.setText("⚠  " + err));
	}

	private static List<IosApp> filter() {
		List<IosApp> out = new ArrayList<>();
		String q = query.toLowerCase(Locale.ROOT);
		for (IosApp x : cache) {
			if (!q.isEmpty()) {
				/* a search looks everywhere */
				if (x.name.toLowerCase(Locale.ROOT).contains(q) || x.bundleId.toLowerCase(Locale.ROOT).contains(q)) {
					out.add(x);
				}
			} else if (x.system == showSystem && (showHidden || !x.hidden)) {
				out.add(x);
			}
		}
		return out;
	}

	private static View row(MainActivity a, IosApp x) {
		LinearLayout r = Ui.row(a);
		r.setPadding(a.dp(14), a.dp(12), a.dp(14), a.dp(12));
		FrameLayout slot = new FrameLayout(a);
		slot.setTag(x.path);
		slot.addView(Ui.appIcon(a, icons.get(x.path), x.name, 44), new FrameLayout.LayoutParams(-1, -1));
		r.addView(slot, new LinearLayout.LayoutParams(a.dp(44), a.dp(44)));
		LinearLayout t = Ui.column(a);
		t.setPadding(a.dp(14), 0, a.dp(8), 0);
		TextView n = Ui.text(a, x.name, 16, Ui.onSurface, false);
		n.setSingleLine(true);
		n.setEllipsize(android.text.TextUtils.TruncateAt.END);
		t.addView(n);
		TextView s = Ui.text(a, x.bundleId + " · " + x.versionText() + (x.hidden ? L.t(" · oculta", " · hidden") : ""), 13, Ui.onSurfaceVariant, false);
		s.setSingleLine(true);
		s.setEllipsize(android.text.TextUtils.TruncateAt.END);
		t.addView(s);
		r.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
		r.addView(Ui.iconView(a, "chevron", Ui.onSurfaceVariant, 24));
		r.setOnClickListener(v -> details(a, x));
		return r;
	}

	/** Icons of SHOWN read one by one in the background, each put in its row's slot as it comes. */
	private static void loadIcons(MainActivity a, LinearLayout list, List<IosApp> shown) {
		int gen = ++generation;
		List<IosApp> todo = new ArrayList<>();
		for (IosApp x : shown) {
			if (!icons.containsKey(x.path)) {
				todo.add(x);
			}
		}
		if (todo.isEmpty()) {
			return;
		}
		new Thread(() -> {
			for (IosApp x : todo) {
				if (gen != generation) {
					return;
				}
				Bitmap bm;
				try {
					bm = Ui.icon(IosApps.icon(a.hfs(), x));
				} catch (Exception e) {
					bm = null;
				}
				Bitmap b = bm;
				icons.put(x.path, b);
				if (b == null) {
					continue;
				}
				a.runOnUiThread(() -> {
					View slot = list.findViewWithTag(x.path);
					if (slot instanceof FrameLayout) {
						((FrameLayout) slot).removeAllViews();
						((FrameLayout) slot).addView(Ui.appIcon(a, b, x.name, 44), new FrameLayout.LayoutParams(-1, -1));
					}
				});
			}
		}).start();
	}

	private static void details(MainActivity a, IosApp x) {
		LinearLayout col = a.sheet(x.name);
		LinearLayout head = Ui.row(a);
		head.addView(Ui.appIcon(a, icons.get(x.path), x.name, 76), new LinearLayout.LayoutParams(a.dp(76), a.dp(76)));
		LinearLayout t = Ui.column(a);
		t.setPadding(a.dp(14), 0, 0, 0);
		t.addView(Ui.text(a, x.name, 26, Ui.onSurface, true));
		t.addView(Ui.chip(a, x.system ? L.t("App del sistema", "System app") : L.t("Instalada por ti", "Installed by you"), x.system ? 0 : Ui.TINT_IOS), Ui.lp(-2, -2, a.dp(6)));
		head.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
		col.addView(head, Ui.lp(-1, -2, a.dp(8)));

		LinearLayout card = Ui.column(a);
		card.setBackground(Ui.surface(a, 24));
		card.setPadding(a.dp(18), a.dp(16), a.dp(18), a.dp(16));
		TextView info = Ui.text(a, "", 14, Ui.onSurfaceVariant, false);
		info.setTextIsSelectable(true);
		String base = L.t("Identificador: ", "Bundle ID: ") + x.bundleId + L.t("\nVersión: ", "\nVersion: ") + x.versionText()
		    + (x.minOs != null ? L.t("\niOS mínimo: ", "\nMinimum iOS: ") + x.minOs : "") + (x.executable != null ? L.t("\nEjecutable: ", "\nExecutable: ") + x.executable : "")
		    + L.t("\nCarpeta: ", "\nFolder: ") + x.path;
		info.setText(base + L.t("\nTamaño: calculando…", "\nSize: calculating…"));
		card.addView(info);
		col.addView(card, Ui.lp(-1, -2, a.dp(14)));
		a.background(() -> {
			long size = IosApps.size(a.hfs(), x.dirId);
			a.runOnUiThread(() -> info.setText(base + L.t("\nTamaño: ", "\nSize: ") + Ui.size(size)));
			return null;
		}, err -> info.setText(base + L.t("\nTamaño: ?", "\nSize: ?")));

		String folder = x.path.substring(x.path.lastIndexOf('/') + 1);
		String ipaName = (x.name.replaceAll("[\\\\/:*?\"<>|]", "_") + " " + (x.version != null ? x.version : "")).trim() + ".ipa";
		col.addView(Ui.button(a, L.t("Exportar como .ipa", "Export as .ipa"), "download", Ui.TINT_IOS, v -> a.confirm(L.t("Exportar ", "Export ") + x.name,
		    L.t("Se empaqueta la app tal como está en iOS (Payload/", "The app is packaged exactly as it is on iOS (Payload/") + folder + L.t(") en Descargas/iOS/", ") into Downloads/iOS/") + ipaName + L.t(".\n\nSirve como copia: ", ".\n\nUseful as a backup: ")
		    + L.t("las apps de la App Store van cifradas y no se pueden reinstalar en otro dispositivo.", "App Store apps are encrypted and cannot be reinstalled on another device."), L.t("Exportar", "Export"),
		    () -> a.exportZip(x.dirId, ipaName, "Payload/" + folder + "/"))), Ui.lp(-1, a.dp(64), a.dp(16)));
		LinearLayout two = new Ui.Group(a, false);
		two.addView(Ui.button(a, "Info.plist", "text", 0, v -> a.textDialog("Info.plist", MainActivity.plistText(x.info, ""))),
		    new LinearLayout.LayoutParams(0, a.dp(56), 1));
		two.addView(Ui.button(a, L.t("Ver en Archivos", "Show in Files"), "folder", 0, v -> a.openPath(x.path)), new LinearLayout.LayoutParams(0, a.dp(56), 1));
		col.addView(two, Ui.lp(-1, -2, a.dp(10)));
	}
}
