package com.sapphire.dualboot;

import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Buscar: files and folders by name anywhere under a folder of iOS (the whole volume by default), walked
 * breadth first through Hfs with the count of what has been seen, cancelable. A result opens its folder in
 * Archivos, or the file's actions.
 */
final class SearchView {
	static String scope = "/";
	private static String last = "";
	private static HfsTools.Cancel running;
	private static final int MAX = 500;

	private SearchView() {
	}

	static void build(MainActivity a, LinearLayout col, TextView sub) {
		stop();
		sub.setText(L.t("Por nombre, en ", "By name, in ") + (scope.equals("/") ? L.t("todo iOS", "all of iOS") : scope));
		LinearLayout bar = Ui.spaced(a, 8, false);
		EditText q = Ui.field(a, L.t("Nombre (o parte) a buscar", "Name (or part of it) to search for"));
		q.setText(last);
		bar.addView(q, new LinearLayout.LayoutParams(0, -2, 1));
		TextView go = Ui.button(a, L.t("Buscar", "Search"), Ui.TINT_IOS, null);
		bar.addView(go, new LinearLayout.LayoutParams(-2, a.dp(56)));
		col.addView(bar, Ui.lp(-1, -2, a.dp(12)));
		if (!scope.equals("/")) {
			LinearLayout sc = Ui.chipRow(a);
			sc.addView(Ui.pill(a, L.t("En ", "In ") + scope, true, null));
			sc.addView(Ui.pill(a, L.t("Todo iOS", "All of iOS"), false, v -> {
				scope = "/";
				a.show(MainActivity.PAGE_FILES);
			}));
			col.addView(sc, Ui.lp(-1, -2, a.dp(10)));
		}
		TextView status = Ui.text(a, L.t("Busca en el sistema de archivos de iOS, sin modificar nada. Recorrer todo iOS tarda unos minutos.", "Searches the iOS file system without changing anything. Walking all of iOS takes a few minutes."),
		    14, Ui.onSurfaceVariant, false);
		status.setPadding(a.dp(8), 0, a.dp(8), 0);
		col.addView(status, Ui.lp(-1, -2, a.dp(10)));
		TextView stop = Ui.button(a, L.t("Detener", "Stop"), "close", Ui.TINT_DANGER, v -> stop());
		stop.setVisibility(View.GONE);
		col.addView(stop, Ui.lp(-1, a.dp(56), a.dp(12)));
		LinearLayout list = Ui.list(a);
		list.setVisibility(View.GONE);
		col.addView(list, Ui.lp(-1, -2, a.dp(10)));

		Runnable start = () -> {
			String text = q.getText().toString().trim();
			if (text.length() < 2) {
				status.setText(L.t("Escribe al menos 2 letras.", "Type at least 2 letters."));
				return;
			}
			a.getSystemService(InputMethodManager.class).hideSoftInputFromWindow(q.getWindowToken(), 0);
			stop();
			last = text;
			list.removeAllViews();
			list.setVisibility(View.GONE);
			stop.setVisibility(View.VISIBLE);
			HfsTools.Cancel c = new HfsTools.Cancel();
			running = c;
			int[] found = {0};
			long[] shown = {0};
			long t0 = System.currentTimeMillis();
			a.background(() -> {
				Hfs h = a.hfs();
				int from = Hfs.ROOT_ID;
				if (!scope.equals("/")) {
					Hfs.Entry e = h.lookup(scope);
					if (e == null || !e.folder) {
						throw new java.io.IOException(L.t("no existe ", "does not exist: ") + scope);
					}
					from = e.id;
				}
				long seen = HfsTools.search(h, from, scope, text, MAX, c, (e, p) -> {
					found[0]++;
					a.runOnUiThread(() -> {
						list.setVisibility(View.VISIBLE);
						list.addView(row(a, e, p));
					});
				}, (n, where) -> {
					long now = System.currentTimeMillis();
					if (now - shown[0] > 150) {         /* not more often than the eye can follow */
						shown[0] = now;
						a.runOnUiThread(() -> status.setText(L.t("Buscando… ", "Searching… ") + n + L.t(" elementos vistos · ", " items seen · ") + found[0] + L.t(" encontrados\n", " found\n")
						    + (where.isEmpty() ? "/" : where)));
					}
				});
				long secs = (System.currentTimeMillis() - t0) / 1000;
				a.runOnUiThread(() -> {
					stop.setVisibility(View.GONE);
					status.setText((c.cancelled ? L.t("Detenida: ", "Stopped: ") : found[0] >= MAX ? L.t("Primeros ", "First ") + MAX + L.t(" resultados: ", " results: ") : L.t("Terminada: ", "Done: "))
					    + found[0] + L.t(" encontrados entre ", " found among ") + seen + L.t(" elementos (", " items (") + secs + " s).");
				});
				return null;
			}, err -> {
				stop.setVisibility(View.GONE);
				status.setText("⚠  " + err);
			});
		};
		go.setOnClickListener(v -> start.run());
		q.setOnEditorActionListener((v, id, ev) -> {
			if (id == EditorInfo.IME_ACTION_SEARCH) {
				start.run();
				return true;
			}
			return false;
		});
	}

	/** Stops a search still running (also when its page is left). */
	static void stop() {
		if (running != null) {
			running.cancelled = true;
			running = null;
		}
	}

	private static View row(MainActivity a, Hfs.Entry e, String p) {
		LinearLayout r = Ui.row(a);
		r.setPadding(a.dp(14), a.dp(12), a.dp(14), a.dp(12));
		r.addView(a.fileBadge(e), new LinearLayout.LayoutParams(a.dp(40), a.dp(40)));
		LinearLayout t = Ui.column(a);
		t.setPadding(a.dp(14), 0, 0, 0);
		TextView n = Ui.text(a, e.name, 16, Ui.onSurface, false);
		n.setSingleLine(true);
		n.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
		t.addView(n);
		String parent = p.substring(0, Math.max(1, p.lastIndexOf('/')));
		TextView s = Ui.text(a, parent + (e.folder ? "" : "  ·  " + Ui.size(e.size)), 13, Ui.onSurfaceVariant, false);
		s.setSingleLine(true);
		s.setEllipsize(android.text.TextUtils.TruncateAt.START);
		t.addView(s);
		r.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
		r.setOnClickListener(v -> {
			stop();
			if (e.folder) {
				a.openPath(p);
			} else {
				new Ui.Dialog(a).setTitle(e.name).setItems(new String[]{L.t("Abrir su carpeta", "Open its folder"), L.t("Ver / exportar…", "View / export…")}, (d, w) -> {
					if (w == 0) {
						a.openPath(parent);
					} else {
						a.fileActions(e);
					}
				}).show();
			}
		});
		return r;
	}
}
