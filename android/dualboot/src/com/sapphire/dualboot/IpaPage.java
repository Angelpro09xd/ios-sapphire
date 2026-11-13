package com.sapphire.dualboot;

import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

/**
 * IPAs: iOS apps queued from Android in the inbox after iOS's volume (IpaInbox); iOS installs them on its
 * next boot and marks each one installed or failed. What Android knows of each app (name, bundle, version,
 * icon) is kept in the app's files by the IPA's sha256, since the inbox header only holds the file name.
 */
final class IpaPage {
	private IpaPage() {
	}

	static View build(MainActivity a) {
		ScrollView[] h = new ScrollView[1];
		LinearLayout col = a.pageColumn(h);
		TextView sub = a.pageTitle(col, "IPAs", L.t("Instala apps en iOS desde Android", "Install apps on iOS from Android"));

		LinearLayout box = a.card(col, L.t("Bandeja", "Inbox"));
		TextView state = Ui.text(a, L.t("Leyendo la bandeja…", "Reading the inbox…"), 22, Ui.onSurface, true);
		box.addView(state, Ui.lp(-1, -2, dp(a, 8)));
		Ui.Bar bar = new Ui.Bar(a, Ui.TINT_IOS);
		box.addView(bar, Ui.lp(-1, dp(a, 14), dp(a, 12)));
		TextView room = Ui.text(a, "", 13, Ui.onSurfaceVariant, false);
		box.addView(room, Ui.lp(-1, -2, dp(a, 10)));

		TextView add = Ui.button(a, L.t("Añadir IPAs…", "Add IPAs…"), "add", Ui.TINT_IOS, v -> pick(a));
		add.setTextSize(18);
		col.addView(add, Ui.lp(-1, dp(a, 72), dp(a, 16)));

		LinearLayout list = Ui.list(a);
		col.addView(list, Ui.lp(-1, -2, dp(a, 14)));
		LinearLayout actions = Ui.column(a);
		col.addView(actions, Ui.lp(-1, -2, 0));

		LinearLayout how = a.card(col, L.t("Cómo funciona", "How it works"));
		how.addView(Ui.text(a, L.t("Android no puede escribir dentro del sistema de archivos de iOS, así que las IPAs se copian a una ", "Android cannot write inside the iOS file system, so the IPAs are copied to an ")
		    + L.t("bandeja: el espacio libre de la partición de iOS que queda justo después de su volumen. Nada de iOS se toca.\n\n", "inbox: the free space of the iOS partition right after its volume. Nothing of iOS is touched.\n\n")
		    + L.t("La próxima vez que arranques iOS, leerá la bandeja e instalará las apps. Al volver a Android verás aquí si cada una ", "The next time you boot iOS, it will read the inbox and install the apps. Back in Android you will see here whether each one ")
		    + L.t("se instaló o falló.\n\n", "was installed or failed.\n\n")
		    + L.t("Caben hasta ", "Up to ") + IpaInbox.MAX_ENTRIES + L.t(" apps a la vez. Las IPAs deben estar firmadas o ser instalables en tu iOS ", " apps fit at a time. The IPAs must be signed or installable on your iOS ")
		    + L.t("(por ejemplo, con TrollStore o AppSync).", "(for example, with TrollStore or AppSync)."), 14, Ui.onSurfaceVariant, false), Ui.lp(-1, -2, dp(a, 8)));

		a.background(() -> {
			IpaInbox b = IpaInbox.device();
			a.runOnUiThread(() -> fill(a, b, sub, state, bar, room, list, actions));
			return null;
		}, err -> {
			state.setText("⚠  " + err);
			add.setEnabled(false);
			add.setAlpha(0.4f);
		});
		return h[0];
	}

	private static void fill(MainActivity a, IpaInbox b, TextView sub, TextView state, Ui.Bar bar, TextView room, LinearLayout list,
	    LinearLayout actions) {
		int pending = 0, installed = 0, failed = 0;
		for (IpaInbox.Entry e : b.entries) {
			pending += e.state == IpaInbox.PENDING ? 1 : 0;
			installed += e.state == IpaInbox.INSTALLED ? 1 : 0;
			failed += e.state == IpaInbox.FAILED ? 1 : 0;
		}
		sub.setText(pending == 0 ? L.t("Instala apps en iOS desde Android", "Install apps on iOS from Android") : pending + (pending == 1 ? L.t(" app se instalará", " app will be installed") : L.t(" apps se instalarán", " apps will be installed"))
		    + L.t(" al arrancar iOS", " when iOS boots"));
		state.setText(b.damaged != null ? L.t("⚠  Cabecera dañada: ", "⚠  Damaged header: ") + b.damaged
		    : b.entries.isEmpty() ? L.t("Vacía", "Empty") : b.entries.size() + (b.entries.size() == 1 ? " app" : " apps") + "  ·  " + Ui.size(b.used()) + L.t(" ocupados", " used"));
		bar.fraction = (float) b.used() / b.geometry.bytes();
		bar.invalidate();
		room.setText(L.t("Libre: ", "Free: ") + Ui.size(b.largestFree()) + L.t(" de ", " of ") + Ui.size(b.geometry.bytes()) + L.t(" (tras el volumen de iOS, ", " (after the iOS volume, ")
		    + L.t("a ", "at ") + String.format(Locale.ROOT, "%.2f", b.geometry.start / 1073741824.0) + L.t(" GiB del inicio de iosroot)", " GiB from the start of iosroot)"));

		list.removeAllViews();
		if (b.entries.isEmpty()) {
			TextView t = Ui.text(a, L.t("No hay apps en la bandeja.\nPulsa «Añadir IPAs…» para elegir una o varias.", "No apps in the inbox.\nTap “Add IPAs…” to pick one or more."), 15, Ui.onSurfaceVariant, false);
			t.setPadding(dp(a, 18), dp(a, 18), dp(a, 18), dp(a, 18));
			list.addView(t);
		}
		for (int i = 0; i < b.entries.size(); i++) {
			list.addView(row(a, b, i));
		}
		actions.removeAllViews();
		if (installed > 0) {
			actions.addView(Ui.button(a, L.t("Quitar las ya instaladas (", "Remove the installed ones (") + installed + ")", "check", 0, v -> change(a, L.t("Quitando…", "Removing…"), () -> b.removeInstalled())),
			    Ui.lp(-1, dp(a, 56), dp(a, 12)));
		}
		if (!b.entries.isEmpty() || b.damaged != null) {
			actions.addView(Ui.button(a, L.t("Vaciar la bandeja", "Empty the inbox"), "delete", Ui.TINT_DANGER, v -> a.confirm(L.t("Vaciar la bandeja", "Empty the inbox"),
			    L.t("Se quitarán todas las apps de la bandeja", "All the apps will be removed from the inbox") + (b.entries.isEmpty() ? "" : L.t("; las pendientes ya no se instalarán", "; the pending ones will no longer be installed"))
			    + L.t(". Las ya instaladas en iOS no se tocan.", ". Those already installed on iOS are not touched."), L.t("Vaciar", "Empty"), () -> change(a, L.t("Vaciando…", "Emptying…"), b::clear))), Ui.lp(-1, dp(a, 56), dp(a, 10)));
		}
	}

	private static View row(MainActivity a, IpaInbox b, int i) {
		IpaInbox.Entry e = b.entries.get(i);
		String[] meta = meta(a, e.shaHex());
		LinearLayout r = Ui.row(a);
		r.setPadding(dp(a, 14), dp(a, 12), dp(a, 14), dp(a, 12));
		Bitmap icon = BitmapFactory.decodeFile(new File(dir(a), e.shaHex() + ".png").getPath());
		String name = meta != null ? meta[0] : e.name;
		r.addView(Ui.appIcon(a, icon, name, 46), new LinearLayout.LayoutParams(dp(a, 46), dp(a, 46)));
		LinearLayout t = Ui.column(a);
		t.setPadding(dp(a, 12), 0, dp(a, 8), 0);
		TextView n = Ui.text(a, name, 16, Ui.onSurface, false);
		n.setSingleLine(true);
		n.setEllipsize(android.text.TextUtils.TruncateAt.END);
		t.addView(n);
		TextView s = Ui.text(a, (meta != null ? meta[1] + " · " + meta[2] + "\n" : "") + e.name + " · " + Ui.size(e.length), 13, Ui.onSurfaceVariant, false);
		t.addView(s);
		r.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
		int color = e.state == IpaInbox.PENDING ? Ui.TINT_WARN : e.state == IpaInbox.INSTALLED ? Ui.TINT_ANDROID : Ui.TINT_DANGER;
		r.addView(Ui.chip(a, e.stateName(), color));
		r.setOnClickListener(v -> {
			String[] acts = {L.t("Quitar de la bandeja", "Remove from the inbox"), L.t("Comprobar lo copiado", "Verify the copy"), L.t("Detalles", "Details")};
			new Ui.Dialog(a).setTitle(name).setItems(acts, (d, w) -> {
				if (w == 0) {
					a.confirm(L.t("Quitar ", "Remove ") + name, e.state == IpaInbox.PENDING ? L.t("Todavía no se ha instalado: si la quitas, iOS no la instalará.", "Not installed yet: if you remove it, iOS will not install it.")
					    : L.t("Solo se quita de la bandeja; ", "It is only removed from the inbox; ") + (e.state == IpaInbox.INSTALLED ? L.t("en iOS sigue instalada.", "it stays installed on iOS.") : L.t("en iOS no está.", "it is not on iOS.")),
					    L.t("Quitar", "Remove"), () -> change(a, L.t("Quitando…", "Removing…"), () -> b.remove(i)));
				} else if (w == 1) {
					a.task(L.t("Comprobando ", "Checking ") + name, false, t2 -> {
						byte[] got = b.hash(e, (what, done, total) -> {
							t2.status(what + "… " + Ui.size(done) + L.t(" de ", " of ") + Ui.size(total));
							t2.fraction((double) done / total);
						});
						return Arrays.equals(got, e.sha) ? L.t("✓ La copia en la bandeja está intacta (sha256 coincide).", "✓ The copy in the inbox is intact (sha256 matches).")
						    : L.t("⚠ La copia en la bandeja NO coincide con la IPA original. Quítala y vuelve a añadirla.", "⚠ The copy in the inbox does NOT match the original IPA. Remove it and add it again.");
					});
				} else {
					a.textDialog(name, L.t("Fichero: ", "File: ") + e.name + L.t("\nTamaño: ", "\nSize: ") + Ui.size(e.length) + " (" + e.length + " bytes)"
					    + (meta != null ? "\nApp: " + meta[0] + L.t("\nIdentificador: ", "\nBundle ID: ") + meta[1] + L.t("\nVersión: ", "\nVersion: ") + meta[2]
					    + (meta.length > 3 ? L.t("\niOS mínimo: ", "\nMinimum iOS: ") + meta[3] : "") : "")
					    + L.t("\nEstado: ", "\nState: ") + e.stateName() + "\nsha256: " + e.shaHex()
					    + L.t("\nPosición: iosroot + ", "\nPosition: iosroot + ") + (b.geometry.start + e.offset) + L.t(" (bandeja + ", " (inbox + ") + e.offset + ")");
				}
			}).show();
		});
		return r;
	}

	interface Change {
		IpaInbox run() throws IOException;
	}

	/** A change to the inbox (header only), then the page again. */
	private static void change(MainActivity a, String what, Change c) {
		a.task(what, false, t -> {
			t.status(what);
			c.run();
			return (Runnable) () -> refresh(a);
		});
	}

	private static void refresh(MainActivity a) {
		if (a.page() == MainActivity.PAGE_IPAS) {
			a.show(MainActivity.PAGE_IPAS);
		}
	}

	/* ---------------- adding ---------------- */

	private static void pick(MainActivity a) {
		Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
		i.addCategory(Intent.CATEGORY_OPENABLE);
		i.setType("*/*");
		i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/octet-stream", "application/x-itunes-ipa", "application/zip",
			"application/x-ios-app", "*/*"});
		i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
		a.startActivityForResult(i, MainActivity.PICK_IPA);
	}

	/** One IPA read and checked, ready to be queued. */
	private static final class Picked {
		File file;
		String fileName;
		IosApp app;
		byte[] sha;
	}

	static void picked(MainActivity a, Intent data) {
		List<Uri> uris = new ArrayList<>();
		ClipData clip = data.getClipData();
		if (clip != null) {
			for (int i = 0; i < clip.getItemCount(); i++) {
				uris.add(clip.getItemAt(i).getUri());
			}
		} else if (data.getData() != null) {
			uris.add(data.getData());
		}
		if (uris.isEmpty()) {
			return;
		}
		a.task(L.t("Leyendo ", "Reading ") + (uris.size() == 1 ? L.t("la IPA", "the IPA") : uris.size() + " IPAs"), true, t -> {
			IpaInbox box = IpaInbox.device();
			File in = new File(a.getCacheDir(), "ipa-in");
			in.mkdirs();
			List<Picked> ok = new ArrayList<>();
			StringBuilder problems = new StringBuilder();
			for (int i = 0; i < uris.size() && !t.cancel.cancelled; i++) {
				Uri u = uris.get(i);
				String name = displayName(a, u);
				t.status(L.t("Leyendo ", "Reading ") + name + " (" + (i + 1) + L.t(" de ", " of ") + uris.size() + ")…");
				File f = new File(in, System.nanoTime() + ".ipa");
				try {
					copy(a, u, f, box.geometry.bytes(), t);
					Picked p = new Picked();
					p.file = f;
					p.fileName = name;
					p.app = IosApp.fromIpa(f);
					p.sha = IpaInbox.sha256(f);
					for (IpaInbox.Entry e : box.entries) {
						if (Arrays.equals(e.sha, p.sha) && e.state == IpaInbox.PENDING) {
							throw new IOException(L.t("ya está en la bandeja, pendiente", "already in the inbox, pending"));
						}
					}
					for (Picked q : ok) {
						if (Arrays.equals(q.sha, p.sha)) {
							throw new IOException(L.t("repetida", "duplicate"));
						}
					}
					ok.add(p);
				} catch (IOException x) {
					f.delete();
					problems.append("• ").append(name).append(": ").append(x.getMessage()).append('\n');
				}
			}
			if (t.cancel.cancelled) {
				for (Picked p : ok) {
					p.file.delete();
				}
				return L.t("Cancelado.", "Cancelled.");
			}
			return (Runnable) () -> confirm(a, box, ok, problems.toString());
		});
	}

	private static void copy(MainActivity a, Uri u, File f, long max, MainActivity.Task t) throws IOException {
		try (InputStream i = a.getContentResolver().openInputStream(u); OutputStream o = new FileOutputStream(f)) {
			if (i == null) {
				throw new IOException(L.t("no se puede abrir", "cannot be opened"));
			}
			byte[] b = new byte[1 << 20];
			long n = 0;
			for (int k; (k = i.read(b)) > 0; ) {
				o.write(b, 0, k);
				n += k;
				if (n > max) {
					throw new IOException(L.t("más grande que toda la bandeja (", "larger than the whole inbox (") + Ui.size(max) + ")");
				}
				if (t.cancel.cancelled) {
					throw new IOException(L.t("cancelado", "cancelled"));
				}
			}
		}
	}

	/** The sheet that shows what was read and asks before anything is written. */
	private static void confirm(MainActivity a, IpaInbox box, List<Picked> ok, String problems) {
		LinearLayout col = a.sheet(ok.isEmpty() ? L.t("No se puede añadir", "Cannot add") : L.t("Añadir a la bandeja", "Add to the inbox"));
		long total = 0;
		LinearLayout list = Ui.listHigh(a);
		for (Picked p : ok) {
			total += IpaInbox.roundUp(p.file.length(), IpaInbox.BLOCK);
			LinearLayout r = Ui.row(a);
			r.setPadding(dp(a, 14), dp(a, 12), dp(a, 14), dp(a, 12));
			r.addView(Ui.appIcon(a, Ui.icon(p.app.icon), p.app.name, 52), new LinearLayout.LayoutParams(dp(a, 52), dp(a, 52)));
			LinearLayout t = Ui.column(a);
			t.setPadding(dp(a, 12), 0, 0, 0);
			t.addView(Ui.text(a, p.app.name, 17, Ui.onSurface, true));
			t.addView(Ui.text(a, p.app.bundleId + L.t("\nVersión ", "\nVersion ") + p.app.versionText() + (p.app.minOs != null ? " · iOS " + p.app.minOs + L.t(" o posterior", " or later") : "")
			    + "\n" + Ui.size(p.file.length()) + " · " + p.fileName, 13, Ui.onSurfaceVariant, false));
			r.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
			list.addView(r);
		}
		if (!ok.isEmpty()) {
			col.addView(list, Ui.lp(-1, -2, dp(a, 10)));
		}
		boolean fits = total <= box.largestFree() && box.entries.size() + ok.size() <= IpaInbox.MAX_ENTRIES;
		String text = (problems.isEmpty() ? "" : L.t("No se añadirán:\n", "Will not be added:\n") + problems + "\n")
		    + (ok.isEmpty() ? "" : L.t("Ocupan ", "They take ") + Ui.size(total) + L.t("; en la bandeja hay ", "; the inbox has ") + Ui.size(box.largestFree()) + L.t(" libres.", " free.")
		    + (fits ? "" : L.t("\n⚠ No caben todas: se añadirán en orden hasta llenar la bandeja.", "\n⚠ Not all of them fit: they will be added in order until the inbox is full."))
		    + L.t("\n\nSe instalarán la próxima vez que arranques iOS.", "\n\nThey will be installed the next time you boot iOS."));
		col.addView(Ui.text(a, text, 14, Ui.onSurfaceVariant, false), Ui.lp(-1, -2, dp(a, 14)));
		if (ok.isEmpty()) {
			return;
		}
		TextView go = Ui.button(a, ok.size() == 1 ? L.t("Añadir a la bandeja", "Add to the inbox") : L.t("Añadir ", "Add ") + ok.size() + " apps", "add", Ui.TINT_IOS, null);
		go.setOnClickListener(v -> {
			a.closeSheet();
			write(a, ok);
		});
		col.addView(go, Ui.lp(-1, dp(a, 64), dp(a, 16)));
	}

	private static void write(MainActivity a, List<Picked> ok) {
		a.task(L.t("Copiando a la bandeja", "Copying to the inbox"), false, t -> {
			IpaInbox box = IpaInbox.device();
			int done = 0;
			StringBuilder problems = new StringBuilder();
			for (Picked p : ok) {
				try {
					saveMeta(a, p);
					String label = p.app.name + " (" + (done + 1) + L.t(" de ", " of ") + ok.size() + ")";
					box = box.add(p.file, p.fileName, (what, d, total) -> {
						t.status(what + " " + label + "\n" + Ui.size(d) + L.t(" de ", " of ") + Ui.size(total));
						t.fraction((double) d / total);
					});
					done++;
				} catch (IOException x) {
					problems.append("• ").append(p.app.name).append(": ").append(x.getMessage()).append('\n');
				} finally {
					p.file.delete();
				}
			}
			String msg = (done == 0 ? L.t("No se añadió ninguna app.", "No app was added.") : done == 1 ? L.t("1 app copiada y comprobada en la bandeja.", "1 app copied to the inbox and verified.")
			    : done + L.t(" apps copiadas y comprobadas en la bandeja.", " apps copied to the inbox and verified."))
			    + (done > 0 ? L.t("\nSe instalarán la próxima vez que arranques iOS.", "\nThey will be installed the next time you boot iOS.") : "")
			    + (problems.length() > 0 ? L.t("\n\nProblemas:\n", "\n\nProblems:\n") + problems : "");
			return (Runnable) () -> {
				refresh(a);
				a.textDialog("", msg);
			};
		});
	}

	/* ---------------- what Android remembers of each queued IPA ---------------- */

	private static File dir(MainActivity a) {
		File d = new File(a.getFilesDir(), "ipas");
		d.mkdirs();
		return d;
	}

	private static void saveMeta(MainActivity a, Picked p) throws IOException {
		String hex = IpaInbox.hex(p.sha);
		String m = p.app.name + "\n" + p.app.bundleId + "\n" + p.app.versionText() + "\n" + (p.app.minOs == null ? "?" : p.app.minOs);
		Files.write(new File(dir(a), hex + ".txt").toPath(), m.getBytes(StandardCharsets.UTF_8));
		Bitmap icon = Ui.icon(p.app.icon);
		if (icon != null) {
			try (OutputStream o = new FileOutputStream(new File(dir(a), hex + ".png"))) {
				icon.compress(Bitmap.CompressFormat.PNG, 100, o);
			}
		}
	}

	private static String[] meta(MainActivity a, String hex) {
		try {
			String[] m = new String(Files.readAllBytes(new File(dir(a), hex + ".txt").toPath()), StandardCharsets.UTF_8).split("\n");
			return m.length >= 3 ? m : null;
		} catch (IOException e) {
			return null;
		}
	}

	private static String displayName(MainActivity a, Uri u) {
		try (Cursor c = a.getContentResolver().query(u, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
			if (c != null && c.moveToFirst() && c.getString(0) != null) {
				return c.getString(0);
			}
		} catch (RuntimeException ignored) {
		}
		String s = u.getLastPathSegment();
		return s == null ? "app.ipa" : s.substring(s.lastIndexOf('/') + 1);
	}

	private static int dp(MainActivity a, float v) {
		return a.dp(v);
	}
}
