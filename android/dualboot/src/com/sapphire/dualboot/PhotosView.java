package com.sapphire.dualboot;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.ImageDecoder;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * Fotos de iOS: the camera roll (/private/var/mobile/Media/DCIM/NNNAPPLE/IMG_*) as a grid, newest first.
 * Thumbnails come from the ones iOS made itself (PhotoData/Thumbnails/V2/..., small JPEGs) when there are,
 * otherwise from the photo, decoded small (HEIC too, through Android's ImageDecoder). Photos and videos
 * are exported to Download/iOS/Fotos, one, a selection or all.
 */
final class PhotosView {
	static final String DCIM = "/private/var/mobile/Media/DCIM", THUMBS = "/private/var/mobile/Media/PhotoData/Thumbnails/V2/DCIM";
	private static final int PAGE = 60;
	private static List<Object[]> cache;            /* {Hfs.Entry, folder name} */
	private static final Map<Integer, Bitmap> thumbs = java.util.Collections.synchronizedMap(new HashMap<>());
	private static final Set<Integer> selected = new HashSet<>();
	private static boolean selecting;
	private static int shown = PAGE;
	private static volatile int generation;

	private PhotosView() {
	}

	static boolean isVideo(String n) {
		String l = n.toLowerCase(Locale.ROOT);
		return l.endsWith(".mov") || l.endsWith(".mp4") || l.endsWith(".m4v");
	}

	static boolean isMedia(String n) {
		return MainActivity.isImage(n) || isVideo(n);
	}

	static void build(MainActivity a, LinearLayout col, TextView sub) {
		LinearLayout bar = Ui.chipRow(a);
		bar.addView(Ui.pill(a, selecting ? L.t("Seleccionando (", "Selecting (") + selected.size() + ")" : L.t("Seleccionar", "Select"), selecting, v -> {
			selecting = !selecting;
			selected.clear();
			a.show(MainActivity.PAGE_FILES);
		}));
		bar.addView(new View(a), new LinearLayout.LayoutParams(0, 1, 1));
		bar.addView(Ui.iconButton(a, "refresh", Ui.STANDARD, v -> {
			cache = null;
			a.show(MainActivity.PAGE_FILES);
		}), new LinearLayout.LayoutParams(a.dp(40), a.dp(40)));
		col.addView(bar, Ui.lp(-1, -2, a.dp(12)));
		if (selecting) {
			TextView exp = Ui.button(a, selected.isEmpty() ? L.t("Toca las fotos que quieras exportar", "Tap the photos you want to export") : L.t("Exportar ", "Export ") + selected.size() + L.t(" a Descargas", " to Downloads"),
			    "download", Ui.TINT_IOS, v -> {
				    List<Object[]> l = new ArrayList<>();
				    for (Object[] o : cache) {
					    if (selected.contains(((Hfs.Entry) o[0]).id)) {
						    l.add(o);
					    }
				    }
				    if (!l.isEmpty()) {
					    export(a, l);
				    }
			    });
			exp.setEnabled(!selected.isEmpty());
			col.addView(exp, Ui.lp(-1, a.dp(64), a.dp(12)));
		}

		LinearLayout grid = Ui.column(a);
		col.addView(grid, Ui.lp(-1, -2, a.dp(12)));
		TextView loading = new Ui.LoadingText(a, L.t("Buscando fotos…", "Looking for photos…"));
		loading.setBackground(new Ui.Shape(Ui.surfaceContainer, a.dp(24)));
		loading.setPadding(a.dp(18), a.dp(16), a.dp(18), a.dp(16));
		grid.addView(loading);

		a.background(() -> {
			if (cache == null) {
				cache = scan(a.hfs());
			}
			a.runOnUiThread(() -> render(a, col, grid, sub));
			return null;
		}, err -> loading.setText("⚠  " + err));
	}

	private static List<Object[]> scan(Hfs h) throws IOException {
		List<Object[]> l = new ArrayList<>();
		Hfs.Entry dcim = h.lookup(DCIM);
		if (dcim == null) {
			return l;
		}
		for (Hfs.Entry d : h.list(dcim.id)) {
			if (!d.folder || d.name.startsWith(".")) {
				continue;
			}
			for (Hfs.Entry f : h.list(d.id)) {
				if (!f.folder && isMedia(f.name)) {
					l.add(new Object[]{f, d.name});
				}
			}
		}
		Collections.sort(l, (x, y) -> Long.compare(((Hfs.Entry) y[0]).modified, ((Hfs.Entry) x[0]).modified));
		return l;
	}

	private static void render(MainActivity a, LinearLayout col, LinearLayout grid, TextView sub) {
		grid.removeAllViews();
		long bytes = 0;
		int videos = 0;
		for (Object[] o : cache) {
			bytes += ((Hfs.Entry) o[0]).size;
			videos += isVideo(((Hfs.Entry) o[0]).name) ? 1 : 0;
		}
		sub.setText(cache.size() - videos + L.t(" fotos · ", " photos · ") + videos + L.t(" vídeos · ", " videos · ") + Ui.size(bytes));
		if (cache.isEmpty()) {
			TextView t = Ui.text(a, L.t("No hay fotos en el carrete de iOS.\n\nCuando hagas fotos o guardes imágenes en iOS aparecerán aquí ", "There are no photos in the iOS camera roll.\n\nWhen you take photos or save images on iOS they will show up here ")
			    + L.t("(se leen de ", "(read from ") + DCIM + ").", 15, Ui.onSurfaceVariant, false);
			t.setBackground(new Ui.Shape(Ui.surfaceContainer, a.dp(28)));
			t.setPadding(a.dp(20), a.dp(20), a.dp(20), a.dp(20));
			grid.addView(t);
			return;
		}
		int n = Math.min(shown, cache.size());
		List<Object[]> page = cache.subList(0, n);
		LinearLayout row = null;
		for (int i = 0; i < page.size(); i++) {
			if (i % 3 == 0) {
				row = Ui.row(a);
				grid.addView(row, Ui.lp(-1, -2, i == 0 ? 0 : a.dp(3)));
			}
			row.addView(cell(a, page.get(i)), cellParams(a, i % 3));
		}
		for (int k = page.size() % 3; k != 0 && k < 3; k++) {
			row.addView(new View(a), cellParams(a, k));
		}
		if (n < cache.size()) {
			grid.addView(Ui.button(a, L.t("Mostrar más (", "Show more (") + (cache.size() - n) + ")", "add", 0, v -> {
				shown += PAGE * 2;
				a.show(MainActivity.PAGE_FILES);
			}), Ui.lp(-1, a.dp(64), a.dp(14)));
		}
		if (!selecting) {
			grid.addView(Ui.button(a, L.t("Exportar todas a Descargas", "Export all to Downloads"), "download", 0, v -> a.confirm(L.t("Exportar el carrete", "Export the camera roll"),
			    cache.size() + L.t(" ficheros, ", " files, ") + Ui.size(total(cache)) + L.t(", a Descargas/iOS/Fotos.", ", to Downloads/iOS/Fotos."), L.t("Exportar", "Export"), () -> export(a, cache))),
			    Ui.lp(-1, a.dp(56), a.dp(10)));
		}
		loadThumbs(a, grid, page);
	}

	private static long total(List<Object[]> l) {
		long t = 0;
		for (Object[] o : l) {
			t += ((Hfs.Entry) o[0]).size;
		}
		return t;
	}

	private static LinearLayout.LayoutParams cellParams(MainActivity a, int k) {
		LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, -2, 1);
		p.leftMargin = k == 0 ? 0 : a.dp(3);
		return p;
	}

	/** A square frame: the grid's cells are as tall as they are wide. */
	static final class Square extends FrameLayout {
		Square(Context c) {
			super(c);
		}

		@Override
		protected void onMeasure(int w, int h) {
			super.onMeasure(w, w);
		}
	}

	private static View cell(MainActivity a, Object[] o) {
		Hfs.Entry e = (Hfs.Entry) o[0];
		Square s = new Square(a);
		s.setTag(e.id);
		s.setBackground(new Ui.Shape(Ui.surfaceHigh, a.dp(10)));
		s.setClipToOutline(true);
		ImageView iv = new ImageView(a);
		iv.setScaleType(ImageView.ScaleType.CENTER_CROP);
		Bitmap bm = thumbs.get(e.id);
		if (bm != null) {
			iv.setImageBitmap(bm);
		}
		s.addView(iv, new FrameLayout.LayoutParams(-1, -1));
		if (isVideo(e.name)) {
			View play = Ui.iconBadge(a, "play", 0x66000000, 0xFFFFFFFF, 26, Ui.FULL);
			FrameLayout.LayoutParams pl = new FrameLayout.LayoutParams(a.dp(26), a.dp(26), Gravity.BOTTOM | Gravity.START);
			pl.setMargins(a.dp(6), 0, 0, a.dp(6));
			s.addView(play, pl);
		}
		if (selecting && selected.contains(e.id)) {
			View check = Ui.iconView(a, "check", Ui.onPrimary, 16);
			check.setBackground(new Ui.Shape(Ui.primary, Ui.FULL).stroke(Ui.surface, a.dp(2)));
			FrameLayout.LayoutParams cp = new FrameLayout.LayoutParams(a.dp(26), a.dp(26), Gravity.BOTTOM | Gravity.END);
			cp.setMargins(0, 0, a.dp(6), a.dp(6));
			s.addView(check, cp);
			/* selected: the photo shrinks into a rounder frame (M3 Expressive selection) */
			iv.setScaleX(0.86f);
			iv.setScaleY(0.86f);
			iv.setClipToOutline(true);
			iv.setOutlineProvider(new android.view.ViewOutlineProvider() {
				@Override
				public void getOutline(View v, android.graphics.Outline o) {
					o.setRoundRect(0, 0, v.getWidth(), v.getHeight(), a.dp(18));
				}
			});
			s.setBackground(new Ui.Shape(Ui.primaryContainer, a.dp(10)));
		}
		s.setOnClickListener(v -> {
			if (selecting) {
				if (!selected.remove(e.id)) {
					selected.add(e.id);
				}
				a.show(MainActivity.PAGE_FILES);
			} else {
				viewer(a, o);
			}
		});
		s.setOnLongClickListener(v -> {
			selecting = true;
			selected.add(e.id);
			a.show(MainActivity.PAGE_FILES);
			return true;
		});
		return s;
	}

	private static void loadThumbs(MainActivity a, LinearLayout grid, List<Object[]> page) {
		int gen = ++generation;
		List<Object[]> todo = new ArrayList<>();
		for (Object[] o : page) {
			if (!thumbs.containsKey(((Hfs.Entry) o[0]).id)) {
				todo.add(o);
			}
		}
		if (todo.isEmpty()) {
			return;
		}
		new Thread(() -> {
			for (Object[] o : todo) {
				if (gen != generation) {
					return;
				}
				Hfs.Entry e = (Hfs.Entry) o[0];
				Bitmap bm = thumbnail(a, e, (String) o[1]);
				if (bm == null) {
					continue;
				}
				thumbs.put(e.id, bm);
				a.runOnUiThread(() -> {
					View cell = grid.findViewWithTag(e.id);
					if (cell instanceof FrameLayout && ((FrameLayout) cell).getChildAt(0) instanceof ImageView) {
						((ImageView) ((FrameLayout) cell).getChildAt(0)).setImageBitmap(bm);
					}
				});
			}
		}).start();
	}

	/** iOS's own thumbnail when it has one, else the photo decoded small; null for what cannot be shown. */
	private static Bitmap thumbnail(MainActivity a, Hfs.Entry e, String folder) {
		try {
			Hfs h = a.hfs();
			Hfs.Entry dir = h.lookup(THUMBS + "/" + folder + "/" + e.name);
			if (dir != null && dir.folder) {
				Hfs.Entry best = null;
				for (Hfs.Entry t : h.list(dir.id)) {
					if (!t.folder && !t.compressed && t.name.toUpperCase(Locale.ROOT).endsWith(".JPG") && t.size < (2 << 20)
					    && (best == null || t.size > best.size)) {
						best = t;
					}
				}
				if (best != null) {
					Bitmap b = decode(h.head(best, (int) best.size), a.dp(130));
					if (b != null) {
						return b;
					}
				}
			}
			if (isVideo(e.name) || e.compressed || e.size > (48 << 20)) {
				return null;
			}
			return decode(h.head(e, (int) e.size), a.dp(130));
		} catch (Exception x) {
			return null;
		}
	}

	/** JPEG, PNG, HEIC... decoded to about SIDE pixels on its short side. */
	static Bitmap decode(byte[] b, int side) {
		try {
			return ImageDecoder.decodeBitmap(ImageDecoder.createSource(ByteBuffer.wrap(b)), (dec, info, src) -> {
				int w = info.getSize().getWidth(), h = info.getSize().getHeight();
				int sample = 1;
				while (Math.min(w, h) / (sample * 2) >= side) {
					sample *= 2;
				}
				dec.setTargetSampleSize(sample);
				dec.setAllocator(ImageDecoder.ALLOCATOR_SOFTWARE);
			});
		} catch (IOException | RuntimeException x) {
			return null;
		}
	}

	private static void viewer(MainActivity a, Object[] o) {
		Hfs.Entry e = (Hfs.Entry) o[0];
		LinearLayout col = a.sheet(e.name);
		ImageView iv = new ImageView(a);
		iv.setAdjustViewBounds(true);
		Bitmap t = thumbs.get(e.id);
		if (t != null) {
			iv.setImageBitmap(t);
		}
		col.addView(iv, Ui.lp(-1, -2, a.dp(8)));
		TextView info = Ui.text(a, Ui.size(e.size) + " · " + java.text.DateFormat.getDateTimeInstance().format(new java.util.Date(e.modified))
		    + " · " + o[1], 14, Ui.onSurfaceVariant, false);
		col.addView(info, Ui.lp(-1, -2, a.dp(10)));
		col.addView(Ui.button(a, L.t("Exportar a Descargas", "Export to Downloads"), "download", Ui.TINT_IOS, v -> {
			List<Object[]> one = new ArrayList<>();
			one.add(o);
			export(a, one);
		}), Ui.lp(-1, a.dp(56), a.dp(14)));
		if (!isVideo(e.name) && !e.compressed && e.size < (48 << 20)) {
			a.background(() -> {
				Bitmap b = decode(a.hfs().head(e, (int) e.size), a.getResources().getDisplayMetrics().widthPixels);
				if (b != null) {
					a.runOnUiThread(() -> iv.setImageBitmap(b));
				}
				return null;
			}, err -> info.setText("⚠  " + err));
		}
	}

	private static void export(MainActivity a, List<Object[]> l) {
		long all = total(l);
		a.task(L.t("Exportando ", "Exporting ") + (l.size() == 1 ? L.t("1 fichero", "1 file") : l.size() + L.t(" ficheros", " files")), true, t -> {
			Hfs h = a.hfs();
			long[] done = {0};
			int n = 0;
			for (Object[] o : l) {
				if (t.cancel.cancelled) {
					break;
				}
				Hfs.Entry e = (Hfs.Entry) o[0];
				Uri uri = a.newDownload("iOS/Fotos", e.name);
				long base = done[0];
				int index = n + 1;
				try (OutputStream out = a.getContentResolver().openOutputStream(uri)) {
					h.copy(e, out, k -> {
						done[0] = base + k;
						t.status(e.name + " (" + index + L.t(" de ", " of ") + l.size() + ")\n" + Ui.size(done[0]) + L.t(" de ", " of ") + Ui.size(all));
						t.fraction(all == 0 ? 1 : (double) done[0] / all);
					});
				} catch (IOException x) {
					a.getContentResolver().delete(uri, null, null);
					throw x;
				}
				done[0] = base + e.size;
				n++;
			}
			selecting = false;
			selected.clear();
			String msg = (t.cancel.cancelled ? L.t("Cancelado: ", "Cancelled: ") : "") + n + (n == 1 ? L.t(" fichero exportado", " file exported") : L.t(" ficheros exportados", " files exported"))
			    + L.t(" a Descargas/iOS/Fotos.", " to Downloads/iOS/Fotos.");
			return (Runnable) () -> {
				if (a.page() == MainActivity.PAGE_FILES) {
					a.show(MainActivity.PAGE_FILES);
				}
				a.textDialog("", msg);
			};
		});
	}
}
