package com.sapphire.dualboot;

import android.app.Activity;
import android.view.ViewGroup;
import android.content.ContentValues;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.provider.MediaStore;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.DateFormat;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Sapphire Dual Boot: Android and iOS on one Redmi Note 13 4G (sapphire).
 * Home: which system boots, the switch to iOS, the iOS image installed, storage of both.
 * iOS: iOS's root (HFS+) read-only from Android -- files (previewed, exported, folders as .zip), its apps
 *   (AppsView), its photos (PhotosView), a search by name (SearchView).
 * IPAs: iOS apps queued in the inbox after iOS's volume, installed by iOS on its next boot (IpaPage, IpaInbox).
 * Espacio: how the storage is split between iOS and Android, and a slider to plan a new split.
 * Herramientas: installing the iOS image, iOS's kernel log (LogViewer), the slot table, restarts, device info.
 * The Quick Settings tile (BootIosTile) and the launcher shortcut only open the confirmation of the switch.
 */
public class MainActivity extends Activity {
	private static final int PICK_IMAGE = 1;
	static final int PICK_IPA = 2;
	/** Asks (always with the confirmation) to restart into iOS: the Quick Settings tile and the launcher shortcut. */
	static final String ACTION_BOOT_IOS = "com.sapphire.dualboot.BOOT_IOS";
	static final int PAGE_HOME = 0, PAGE_FILES = 1, PAGE_IPAS = 2, PAGE_SPACE = 3, PAGE_TOOLS = 4;
	private FrameLayout root, pages;
	private LinearLayout navBar;
	private final List<TextView> tabs = new ArrayList<>();
	private final List<ImageView> tabIcons = new ArrayList<>();
	private final List<Ui.Shape> tabPills = new ArrayList<>();
	private static final String[] TAB_ICONS = {"home", "phone", "download", "pie", "build"};
	private int page = 0;
	private final List<View> sheets = new ArrayList<>();
	/* the system bars' insets (edge to edge): until they arrive, a guess */
	private int insetTop, insetBottom;
	private boolean schemeStale;

	/* files tab state: the folder open, and which view of iOS (files, apps, photos, search) */
	private Hfs hfs;
	final List<Hfs.Entry> path = new ArrayList<>();
	static final int FILES = 0, APPS = 1, PHOTOS = 2, SEARCH = 3;
	int filesMode = FILES;

	@Override
	protected void onCreate(Bundle saved) {
		super.onCreate(saved);
		Window w = getWindow();
		w.setDecorFitsSystemWindows(false);
		w.setStatusBarColor(Color.TRANSPARENT);
		w.setNavigationBarColor(Color.TRANSPARENT);
		insetTop = dp(32);
		insetBottom = dp(16);
		buildShell();
		show(0);
		publishShortcut();
		handle(getIntent());
	}

	/** The window's content: the colour scheme, the pages, the navigation bar, the system bars' insets. */
	private void buildShell() {
		Ui.init(this);
		Window w = getWindow();
		int light = Ui.dark ? 0 : android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
		    | android.view.WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS;
		/* the decor view first: before it exists (onCreate, no setContentView yet) the insets controller is null */
		w.getDecorView().setBackgroundColor(Ui.surface);
		android.view.WindowInsetsController ic = w.getInsetsController();
		if (ic != null) {
			ic.setSystemBarsAppearance(light, android.view.WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS
			    | android.view.WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS);
		}
		tabs.clear();
		tabIcons.clear();
		tabPills.clear();
		root = new FrameLayout(this);
		root.setBackgroundColor(Ui.surface);
		pages = new FrameLayout(this);
		root.addView(pages, new FrameLayout.LayoutParams(-1, -1));
		navBar = tabBar();
		root.addView(navBar, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
		root.setOnApplyWindowInsetsListener((v, in) -> {
			android.graphics.Insets i = in.getInsets(android.view.WindowInsets.Type.systemBars() | android.view.WindowInsets.Type.displayCutout());
			if (i.top != insetTop || i.bottom != insetBottom) {
				insetTop = i.top;
				insetBottom = i.bottom;
				padNavBar();
				if (pages.getChildCount() > 0) {
					padPage(pages.getChildAt(0));
				}
			}
			return in;
		});
		padNavBar();
		setContentView(root);
	}

	/** Light / dark changed while the app runs (uiMode is handled here, not by a restart): repaint, when nothing is open. */
	@Override
	public void onConfigurationChanged(android.content.res.Configuration c) {
		super.onConfigurationChanged(c);
		boolean dark = (c.uiMode & android.content.res.Configuration.UI_MODE_NIGHT_MASK) == android.content.res.Configuration.UI_MODE_NIGHT_YES;
		if (dark != Ui.dark) {
			schemeStale = true;
			repaintIfIdle();
		}
	}

	private void repaintIfIdle() {
		if (schemeStale && sheets.isEmpty()) {
			schemeStale = false;
			buildShell();
			show(page);
		}
	}

	@Override
	protected void onNewIntent(Intent i) {
		super.onNewIntent(i);
		setIntent(i);
		handle(i);
	}

	/** The tile's and the shortcut's request: the same confirmation, then the same checked path, as the home button. */
	private void handle(Intent i) {
		/* deep links: am start ... --ei page 1 --ei mode 2 (Fotos), also how each page is checked on the phone with the screen off */
		if (i != null && i.hasExtra("page")) {
			filesMode = Math.max(FILES, Math.min(SEARCH, i.getIntExtra("mode", FILES)));
			int p = i.getIntExtra("page", PAGE_HOME);
			i.removeExtra("page");
			show(Math.max(PAGE_HOME, Math.min(PAGE_TOOLS, p)));
		}
		if (i != null && ACTION_BOOT_IOS.equals(i.getAction())) {
			i.setAction(Intent.ACTION_MAIN);        /* once: not again on a rotation */
			show(PAGE_HOME);
			askBootIos();
		}
	}

	void askBootIos() {
		confirm(L.t("Reiniciar en iOS", "Restart to iOS"), L.t("El teléfono arrancará iOS y se quedará en iOS en cada reinicio, hasta que vuelvas a Android ", "The phone will boot iOS and stay in iOS on every restart, until you go back to Android ")
		    + L.t("desde la app «Android» de iOS.", "from the “Android” app on iOS."), L.t("Reiniciar", "Restart"), this::bootIos);
	}

	/** A long-press launcher shortcut, "Reiniciar en iOS" (it opens the confirmation; it never switches by itself). */
	private void publishShortcut() {
		try {
			android.content.pm.ShortcutManager sm = getSystemService(android.content.pm.ShortcutManager.class);
			Intent i = new Intent(ACTION_BOOT_IOS).setClass(this, MainActivity.class);
			android.content.pm.ShortcutInfo s = new android.content.pm.ShortcutInfo.Builder(this, "boot-ios")
			    .setShortLabel(L.t("Reiniciar en iOS", "Restart to iOS")).setLongLabel(L.t("Reiniciar en iOS (pide confirmación)", "Restart to iOS (asks for confirmation)"))
			    .setIcon(android.graphics.drawable.Icon.createWithResource(this, R_mipmap_ic_launcher())).setIntent(i).build();
			sm.setDynamicShortcuts(java.util.Collections.singletonList(s));
		} catch (RuntimeException ignored) {
			/* launchers without shortcuts */
		}
	}

	/** iOS's volume, opened once and shared (Hfs is safe to use from several threads). */
	synchronized Hfs hfs() throws IOException {
		if (hfs == null) {
			hfs = Hfs.open(Ios.ROOT);
		}
		return hfs;
	}

	/* ================= navigation: the M3 Expressive navigation bar ================= */

	/** Five destinations: an icon in an active-indicator pill (it springs open when chosen) over a label. */
	private LinearLayout tabBar() {
		LinearLayout bar = Ui.row(this);
		bar.setBackgroundColor(Ui.surfaceContainer);
		bar.setGravity(Gravity.TOP);
		bar.setClickable(true);             /* touches on the bar do not reach the page under it */
		String[] labels = {L.t("Inicio", "Home"), "iOS", "IPAs", L.t("Espacio", "Space"), L.t("Herram.", "Tools")};
		for (int i = 0; i < labels.length; i++) {
			final int index = i;
			LinearLayout item = Ui.column(this);
			item.setGravity(Gravity.CENTER_HORIZONTAL);
			FrameLayout slot = new FrameLayout(this);
			View pill = new View(this);
			Ui.Shape ps = new Ui.Shape(Ui.secondaryContainer, Ui.FULL);
			pill.setBackground(ps);
			pill.setScaleX(0f);
			slot.addView(pill, new FrameLayout.LayoutParams(dp(56), dp(32), Gravity.CENTER));
			ImageView icon = Ui.iconView(this, TAB_ICONS[i], Ui.onSurfaceVariant, 24);
			slot.addView(icon, new FrameLayout.LayoutParams(dp(56), dp(32), Gravity.CENTER));
			item.addView(slot, new LinearLayout.LayoutParams(dp(56), dp(32)));
			TextView t = Ui.text(this, labels[i], 12, Ui.onSurfaceVariant, true);
			t.setSingleLine(true);
			t.setGravity(Gravity.CENTER);
			item.addView(t, Ui.lp(-2, -2, dp(4)));
			Ui.Shape ripple = new Ui.Shape(0, Ui.FULL);
			slot.setForeground(Ui.ripple(ripple, Ui.onSurface));
			slot.setDuplicateParentStateEnabled(true);
			item.setOnClickListener(v -> show(index));
			tabs.add(t);
			tabIcons.add(icon);
			tabPills.add(ps);
			pill.setTag(ps);
			bar.addView(item, new LinearLayout.LayoutParams(0, -2, 1));
		}
		return bar;
	}

	private void padNavBar() {
		if (navBar != null) {
			navBar.setPadding(0, dp(12), 0, dp(16) + insetBottom);
		}
	}

	/** Room at the top for the status bar and at the bottom for the navigation bar. */
	private void padPage(View v) {
		if (v instanceof ScrollView && "page".equals(v.getTag())) {
			v.setPadding(dp(16), insetTop + dp(20), dp(16), insetBottom + dp(80) + dp(28));
		}
	}

	void show(int i) {
		int old = page;
		page = i;
		SearchView.stop();          /* a search does not outlive its page */
		for (int k = 0; k < tabs.size(); k++) {
			boolean on = k == i;
			tabs.get(k).setTextColor(on ? Ui.onSurface : Ui.onSurfaceVariant);
			tabs.get(k).setTypeface(Ui.font(on ? 700 : 500));
			tabIcons.get(k).setImageDrawable(Ui.symbol(this, TAB_ICONS[k], on ? Ui.onSecondaryContainer : Ui.onSurfaceVariant, 24));
			View pill = (View) ((View) tabIcons.get(k).getParent()).findViewWithTag(tabPills.get(k));
			if (pill != null) {
				if (on && (old != i || pill.getScaleX() == 0f)) {
					pill.setScaleX(0.3f);
					pill.setAlpha(0f);
					Ui.spring(pill, Ui.FAST).scaleX(1f).alpha(1f).start();
				} else if (!on) {
					pill.animate().cancel();
					pill.setScaleX(0f);
				}
			}
		}
		pages.removeAllViews();
		View v;
		switch (i) {
		case PAGE_FILES:
			v = filesPage();
			break;
		case PAGE_IPAS:
			v = IpaPage.build(this);
			break;
		case PAGE_SPACE:
			v = spacePage();
			break;
		case PAGE_TOOLS:
			v = toolsPage();
			break;
		default:
			v = homePage();
		}
		padPage(v);
		v.setAlpha(0f);
		v.setTranslationY(dp(old == i ? 0 : 28));
		pages.addView(v, new FrameLayout.LayoutParams(-1, -1));
		v.animate().alpha(1f).setDuration(Ui.EFFECTS.ms).setInterpolator(Ui.EFFECTS).start();
		v.animate().translationY(0).setDuration(Ui.SPATIAL.ms).setInterpolator(Ui.SPATIAL).start();
	}

	int page() {
		return page;
	}

	@Override
	public void onBackPressed() {
		if (!sheets.isEmpty()) {
			closeSheet();
		} else if (page == PAGE_FILES && filesMode != FILES) {
			filesMode = FILES;
			show(PAGE_FILES);
		} else if (page == PAGE_FILES && !path.isEmpty()) {
			path.remove(path.size() - 1);
			show(PAGE_FILES);
		} else if (page != PAGE_HOME) {
			show(PAGE_HOME);
		} else {
			super.onBackPressed();
		}
	}

	/* ================= sheets: content over the page ================= */

	/**
	 * A sheet over the page (details, viewers, confirmations with content): the page dimmed behind, a dark
	 * surface rising from the bottom with its title and a glass close button. Back closes it.
	 */
	LinearLayout sheet(String title) {
		ScrollView s = new ScrollView(this);
		LinearLayout col = Ui.column(this);
		s.addView(col);
		sheet(title, s, true);
		return col;
	}

	/** A sheet holding CONTENT as it is (SCROLLS: whether CONTENT is a scrolling view of its own). */
	View sheet(String title, View content, boolean scrolls) {
		FrameLayout over = new FrameLayout(this);
		View scrim = new View(this);
		scrim.setBackgroundColor(0x66000000);
		over.addView(scrim, new FrameLayout.LayoutParams(-1, -1));
		over.setClickable(true);
		over.setOnClickListener(v -> closeSheet());
		LinearLayout panel = Ui.column(this);
		float r = dp(28);
		panel.setBackground(new Ui.Shape(Ui.surfaceLow, 0).radii(r, r, 0, 0));
		panel.setClickable(true);
		panel.setPadding(dp(20), dp(10), dp(20), dp(24) + insetBottom);
		View grabber = new View(this);
		grabber.setBackground(new Ui.Shape(Ui.alpha(Ui.onSurfaceVariant, 0.4f), Ui.FULL));
		LinearLayout.LayoutParams gp = new LinearLayout.LayoutParams(dp(32), dp(4));
		gp.gravity = Gravity.CENTER_HORIZONTAL;
		gp.topMargin = dp(6);
		panel.addView(grabber, gp);
		LinearLayout head = Ui.row(this);
		TextView t = Ui.text(this, title, 24, Ui.onSurface, true);
		t.setTypeface(Ui.displayFont(600));
		t.setSingleLine(true);
		t.setEllipsize(android.text.TextUtils.TruncateAt.END);
		head.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
		head.addView(Ui.iconButton(this, "close", Ui.TONAL, v -> closeSheet()), new LinearLayout.LayoutParams(dp(44), dp(44)));
		panel.addView(head, Ui.lp(-1, -2, dp(12)));
		panel.addView(content, new LinearLayout.LayoutParams(-1, scrolls ? -2 : 0, scrolls ? 0 : 1));
		FrameLayout.LayoutParams pp = new FrameLayout.LayoutParams(-1, scrolls ? -2 : -1, Gravity.BOTTOM);
		pp.topMargin = insetTop + dp(40);
		over.addView(panel, pp);
		root.addView(over, new FrameLayout.LayoutParams(-1, -1));
		sheets.add(over);
		panel.setTranslationY(getResources().getDisplayMetrics().heightPixels * 0.6f);
		scrim.setAlpha(0f);
		scrim.animate().alpha(1f).setDuration(Ui.EFFECTS.ms * 2).setInterpolator(Ui.EFFECTS).start();
		Ui.spring(panel, Ui.SPATIAL).translationY(0).start();
		return over;
	}

	void closeSheet() {
		if (sheets.isEmpty()) {
			return;
		}
		/* a running task's sheet: closing it cancels the task when it can be cancelled, and never just hides it */
		Object busy = sheets.get(sheets.size() - 1).getTag();
		if (busy instanceof Task) {
			if (((Task) busy).cancelable) {
				((Task) busy).cancel.cancelled = true;
				((Task) busy).status(L.t("Cancelando…", "Cancelling…"));
			}
			return;
		}
		View v = sheets.remove(sheets.size() - 1);
		ViewGroup host = root;
		if (v instanceof FrameLayout && ((FrameLayout) v).getChildCount() > 1) {
			View panel = ((FrameLayout) v).getChildAt(1);
			panel.animate().translationY(panel.getHeight()).setDuration(220).setInterpolator(new android.view.animation.PathInterpolator(0.3f, 0f, 0.8f, 0.15f)).start();
		}
		v.animate().alpha(0f).setStartDelay(60).setDuration(180).withEndAction(() -> {
			host.removeView(v);
			repaintIfIdle();
		}).start();
	}

	/* ================= tasks: long jobs with live progress ================= */

	/** What a running task can tell its sheet, and the flag its Cancelar button sets. */
	final class Task {
		final HfsTools.Cancel cancel = new HfsTools.Cancel();
		boolean cancelable;
		TextView text;
		Ui.Bar bar;

		void status(String s) {
			runOnUiThread(() -> text.setText(s));
		}

		void fraction(double f) {
			runOnUiThread(() -> {
				bar.setVisibility(View.VISIBLE);
				bar.fraction = (float) f;
				bar.invalidate();
			});
		}
	}

	interface TaskJob {
		/** A String result is shown as a message, a Runnable is run on the UI thread. */
		Object run(Task t) throws Exception;
	}

	/** JOB on its own thread with a sheet showing its progress (and a Cancelar button when CANCELABLE). */
	void task(String title, boolean cancelable, TaskJob job) {
		Task t = new Task();
		t.cancelable = cancelable;
		LinearLayout col = Ui.column(this);
		LinearLayout line = Ui.row(this);
		line.addView(Ui.loading(this, true), new LinearLayout.LayoutParams(dp(48), dp(48)));
		t.text = Ui.text(this, "…", 15, Ui.onSurfaceVariant, false);
		LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(0, -2, 1);
		tp.leftMargin = dp(16);
		line.addView(t.text, tp);
		col.addView(line, Ui.lp(-1, -2, dp(12)));
		t.bar = new Ui.Bar(this, Ui.TINT_IOS);
		t.bar.wavy = true;
		t.bar.setVisibility(View.INVISIBLE);
		col.addView(t.bar, Ui.lp(-1, dp(16), dp(16)));
		if (cancelable) {
			TextView c = Ui.button(this, L.t("Cancelar", "Cancel"), "close", 0, null);
			c.setOnClickListener(v -> {
				t.cancel.cancelled = true;
				c.setText(L.t("Cancelando…", "Cancelling…"));
				c.setEnabled(false);
			});
			col.addView(c, Ui.lp(-1, dp(52), dp(16)));
		}
		View s = sheet(title, col, true);
		s.setOnClickListener(null);         /* not dismissed by a tap outside while it runs */
		s.setTag(t);
		new Thread(() -> {
			Object r;
			try {
				r = job.run(t);
			} catch (Exception e) {
				android.util.Log.w("SapphireDualBoot", title, e);
				r = t.cancel.cancelled ? L.t("Cancelado.", "Cancelled.") : "Error: " + e.getMessage();
			}
			Object res = r;
			runOnUiThread(() -> {
				if (sheets.contains(s)) {
					sheets.remove(s);
					((ViewGroup) s.getParent()).removeView(s);
				}
				if (res == null || res instanceof String) {
					repaintIfIdle();
				}
				if (res instanceof String) {
					textDialog("", (String) res);
				} else if (res instanceof Runnable) {
					((Runnable) res).run();
				}
			});
		}).start();
	}

	/* ================= Downloads ================= */

	/** A new file Download/FOLDER/NAME through MediaStore (no storage permission needed). */
	Uri newDownload(String folder, String name) throws IOException {
		ContentValues cv = new ContentValues();
		cv.put(MediaStore.Downloads.DISPLAY_NAME, name);
		cv.put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/" + folder);
		Uri uri = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv);
		if (uri == null) {
			throw new IOException(L.t("no se pudo crear el fichero en Descargas", "could not create the file in Downloads"));
		}
		return uri;
	}

	/** Folder ID of iOS as Download/iOS/NAME.zip (an .ipa when PREFIX is "Payload/X.app/"), with progress. */
	void exportZip(int id, String name, String prefix) {
		task(L.t("Exportando ", "Exporting ") + name, true, t -> {
			t.status(L.t("Midiendo la carpeta…", "Measuring the folder…"));
			long[] total = HfsTools.measure(hfs(), id, t.cancel);
			Uri uri = newDownload("iOS", name);
			int skipped;
			try (OutputStream o = new java.io.BufferedOutputStream(getContentResolver().openOutputStream(uri), 1 << 20)) {
				skipped = HfsTools.zip(hfs(), id, prefix, o, t.cancel, (done, where) -> {
					t.status(Ui.size(done) + L.t(" de ", " of ") + Ui.size(total[1]) + "\n" + where);
					t.fraction(total[1] == 0 ? 1 : (double) done / total[1]);
				});
			} catch (IOException e) {
				getContentResolver().delete(uri, null, null);
				throw e;
			}
			return L.t("Exportado a Descargas/iOS/", "Exported to Downloads/iOS/") + name + " (" + total[0] + L.t(" ficheros, ", " files, ") + Ui.size(total[1]) + ")"
			    + (skipped > 0 ? ".\n\n" + skipped + L.t(" no se pudieron copiar (comprimidos por iOS o enlaces): la lista va en OMITIDOS.txt.", " could not be copied (compressed by iOS, or links): they are listed in SKIPPED.txt.") : ".");
		});
	}

	/** Opens Archivos at iOS path P ("/private/var/mobile"). */
	void openPath(String p) {
		background(() -> {
			List<Hfs.Entry> chain = new ArrayList<>();
			StringBuilder sofar = new StringBuilder();
			for (String part : p.split("/")) {
				if (part.isEmpty()) {
					continue;
				}
				sofar.append('/').append(part);
				Hfs.Entry e = hfs().lookup(sofar.toString());
				if (e == null || !e.folder) {
					throw new IOException(L.t("no existe la carpeta ", "no such folder ") + sofar);
				}
				chain.add(e);
			}
			runOnUiThread(() -> {
				closeAllSheets();
				path.clear();
				path.addAll(chain);
				filesMode = FILES;
				show(PAGE_FILES);
			});
			return null;
		}, err -> textDialog("", "⚠  " + err));
	}

	void closeAllSheets() {
		while (!sheets.isEmpty()) {
			root.removeView(sheets.remove(sheets.size() - 1));
		}
	}

	/** The current folder of Archivos as a path ("/" at the root). */
	String currentPath() {
		StringBuilder s = new StringBuilder();
		for (Hfs.Entry e : path) {
			s.append('/').append(e.name);
		}
		return s.length() == 0 ? "/" : s.toString();
	}

	/** A scrolling page with room for the status bar and the floating tab bar. */
	LinearLayout pageColumn(ScrollView[] holder) {
		ScrollView s = new ScrollView(this);
		s.setClipToPadding(false);
		s.setTag("page");
		s.setOverScrollMode(View.OVER_SCROLL_IF_CONTENT_SCROLLS);
		padPage(s);
		LinearLayout col = Ui.column(this);
		s.addView(col);
		holder[0] = s;
		return col;
	}

	/** The page's headline (large, emphasized) and its supporting line. */
	TextView pageTitle(LinearLayout col, String title, String subtitle) {
		TextView t = Ui.text(this, title, 36, Ui.onSurface, true);
		t.setPadding(dp(4), dp(12), dp(4), 0);
		col.addView(t);
		TextView sub = Ui.text(this, subtitle, 15, Ui.onSurfaceVariant, false);
		sub.setPadding(dp(4), 0, dp(4), 0);
		col.addView(sub, Ui.lp(-1, -2, dp(4)));
		return sub;
	}

	/** A content card: a large rounded container one tone above the page, its heading in the primary colour. */
	LinearLayout card(LinearLayout col, String heading) {
		LinearLayout c = Ui.column(this);
		c.setBackground(new Ui.Shape(Ui.surfaceContainer, dp(28)));
		c.setPadding(dp(20), dp(18), dp(20), dp(20));
		if (heading != null) {
			c.addView(Ui.text(this, heading, 14, Ui.primary, true));
		}
		col.addView(c, Ui.lp(-1, -2, dp(12)));
		return c;
	}

	/** Monospace text in an inset well (device details, slots). */
	TextView mono(LinearLayout card, String s) {
		TextView d = Ui.text(this, s, 12, Ui.onSurfaceVariant, false);
		d.setTypeface(Typeface.MONOSPACE);
		d.setLineSpacing(0, 1.15f);
		d.setBackground(new Ui.Shape(Ui.surfaceHigh, dp(16)));
		d.setPadding(dp(14), dp(12), dp(14), dp(12));
		card.addView(d, Ui.lp(-1, -2, dp(10)));
		return d;
	}

	/* ================= Inicio ================= */

	private View homePage() {
		ScrollView[] h = new ScrollView[1];
		LinearLayout col = pageColumn(h);

		LinearLayout head = Ui.row(this);
		head.setPadding(dp(4), dp(8), 0, 0);
		ImageView logo = new ImageView(this);
		logo.setImageResource(R_mipmap_ic_launcher());
		logo.setClipToOutline(true);
		logo.setOutlineProvider(new android.view.ViewOutlineProvider() {
			@Override
			public void getOutline(View v, android.graphics.Outline o) {
				o.setRoundRect(0, 0, v.getWidth(), v.getHeight(), dp(20));
			}
		});
		head.addView(logo, new LinearLayout.LayoutParams(dp(60), dp(60)));
		LinearLayout titles = Ui.column(this);
		titles.setPadding(dp(16), 0, 0, 0);
		TextView name = Ui.text(this, "Sapphire Dual Boot", 26, Ui.onSurface, true);
		name.setTypeface(Ui.displayFont(700));
		titles.addView(name);
		titles.addView(Ui.text(this, L.t("Android + iOS en tu Redmi Note 13", "Android + iOS on your Redmi Note 13"), 14, Ui.onSurfaceVariant, false));
		head.addView(titles, new LinearLayout.LayoutParams(0, -2, 1));
		col.addView(head);

		/* status chip: which system runs (tonal: green for Android, primary for iOS) */
		TextView pill = Ui.text(this, L.t("●  Leyendo el estado…", "●  Reading the state…"), 14, Ui.onSurfaceVariant, true);
		Ui.Shape pg = new Ui.Shape(Ui.surfaceHigh, Ui.FULL);
		pill.setBackground(pg);
		pill.setPadding(dp(16), dp(10), dp(18), dp(10));
		col.addView(pill, Ui.lp(-2, -2, dp(22)));

		/* hero: the iOS image and the switch, in a primary container with a big M3 shape behind */
		FrameLayout hero = new FrameLayout(this);
		hero.setBackground(new Ui.Shape(Ui.primaryContainer, dp(36)));
		hero.setClipToOutline(true);
		Ui.Deco deco = new Ui.Deco(this, Ui.alpha(Ui.onPrimaryContainer, 0.08f), Ui.DECO_COOKIE12);
		FrameLayout.LayoutParams dpl = new FrameLayout.LayoutParams(dp(230), dp(230), Gravity.TOP | Gravity.END);
		dpl.topMargin = -dp(70);
		dpl.rightMargin = -dp(60);
		hero.addView(deco, dpl);
		LinearLayout ios = Ui.column(this);
		ios.setPadding(dp(24), dp(22), dp(24), dp(24));
		hero.addView(ios, new FrameLayout.LayoutParams(-1, -2));
		ios.addView(Ui.text(this, "iOS", 14, Ui.alpha(Ui.onPrimaryContainer, 0.8f), true));
		TextView iosTitle = Ui.text(this, "iOS 16.7.16", 40, Ui.onPrimaryContainer, true);
		iosTitle.setTypeface(Ui.displayFont(800));
		ios.addView(iosTitle, Ui.lp(-1, -2, dp(2)));
		TextView iosInfo = Ui.text(this, "…", 14, Ui.alpha(Ui.onPrimaryContainer, 0.85f), false);
		ios.addView(iosInfo, Ui.lp(-1, -2, dp(6)));
		TextView go = Ui.button(this, L.t("Reiniciar en iOS", "Restart to iOS"), "restart", Ui.TINT_IOS, v -> askBootIos());
		go.setTextSize(19);
		ios.addView(go, Ui.lp(-1, dp(72), dp(20)));
		col.addView(hero, Ui.lp(-1, -2, dp(14)));

		LinearLayout st = card(col, L.t("Almacenamiento", "Storage"));
		TextView iosLine = Ui.text(this, "iOS", 15, Ui.onSurface, true);
		st.addView(iosLine, Ui.lp(-1, -2, dp(12)));
		Ui.Bar iosBar = new Ui.Bar(this, Ui.TINT_IOS);
		st.addView(iosBar, Ui.lp(-1, dp(14), dp(8)));
		TextView andLine = Ui.text(this, "Android", 15, Ui.onSurface, true);
		st.addView(andLine, Ui.lp(-1, -2, dp(18)));
		Ui.Bar andBar = new Ui.Bar(this, Ui.TINT_ANDROID);
		st.addView(andBar, Ui.lp(-1, dp(14), dp(8)));

		LinearLayout sl = card(col, L.t("Ranuras", "Slots"));
		TextView slots = mono(sl, "…");

		background(() -> {
			if (!Root.available()) {
				throw new IOException(L.t("Sin root: concede superusuario a Sapphire Dual Boot en Magisk.", "No root: grant superuser to Sapphire Dual Boot in Magisk."));
			}
			Gpt g = Gpt.load();
			char act = g.activeSlot(), iosSlot = Ios.slot();
			String state = Ios.bootState();
			String kernel = Ios.kernelInfo();
			boolean back = Ios.canReturn();
			Hfs v = Hfs.open(Ios.ROOT);
			long iosUsed = v.usedBytes(), iosTotal = v.totalBytes();
			String version = v.iosVersion();
			StatFs fs = new StatFs(Environment.getDataDirectory().getPath());
			long aTotal = fs.getTotalBytes(), aUsed = aTotal - fs.getAvailableBytes();
			String sd = g.describe('a') + "\n" + g.describe('b') + "\nboot LUN: " + Ios.bootLun();
			runOnUiThread(() -> {
				boolean androidNow = act != iosSlot;
				pill.setText((androidNow ? L.t("●  Android activo", "●  Android active") : L.t("●  iOS activo", "●  iOS active")) + L.t("  ·  ranura ", "  ·  slot ") + act);
				pg.fadeTo(androidNow ? Ui.greenContainer : Ui.primaryContainer);
				pill.setTextColor(androidNow ? Ui.onGreenContainer : Ui.onPrimaryContainer);
				if (version != null) {
					iosTitle.setText("iOS " + version);
				}
				iosInfo.setText(L.t("Ranura ", "Slot ") + iosSlot + " · " + state + "\n" + kernel
				    + (back ? L.t("\nPuede volver a Android con un botón", "\nCan go back to Android with one button") : L.t("\n⚠ Imagen antigua: instala la 0306 o posterior", "\n⚠ Old image: install 0306 or later")));
				iosLine.setText("iOS  ·  " + Ui.size(iosUsed) + L.t(" de ", " of ") + Ui.size(iosTotal));
				iosBar.fraction = (float) iosUsed / iosTotal;
				iosBar.invalidate();
				andLine.setText("Android  ·  " + Ui.size(aUsed) + L.t(" de ", " of ") + Ui.size(aTotal));
				andBar.fraction = (float) aUsed / aTotal;
				andBar.invalidate();
				slots.setText(sd);
			});
			return null;
		}, err -> {
			pill.setText("⚠  " + err);
			pg.fadeTo(Ui.errorContainer);
			pill.setTextColor(Ui.onErrorContainer);
		});
		return h[0];
	}

	private void bootIos() {
		progress(L.t("Cambiando a iOS…", "Switching to iOS…"), () -> {
			String state = Ios.bootState();
			if (!state.startsWith(L.t("iOS (raíz en iosroot)", "iOS (root in iosroot)"))) {
				throw new IOException("boot_" + Ios.slot() + L.t(" no tiene una imagen de iOS válida (", " has no valid iOS image (") + state + L.t("). Instálala en Herramientas.", "). Install it in Tools."));
			}
			if (!Ios.canReturn()) {
				throw new IOException(L.t("La imagen de iOS es anterior al kernel 0306: desde ella no se podría volver a Android. Instala una más nueva.", "The iOS image is older than kernel 0306: it could not go back to Android. Install a newer one."));
			}
			save("sapphire-gpt-antes-de-ios", Gpt.load().backup());
			/*
			 * The switch is Android's boot HAL's (bootctl, Magisk's): the GPT the way the bootloader expects it
			 * -- active bits and the X_a/X_b type GUIDs -- and the UFS boot LUN, which the bootloader checks too.
			 */
			String bootctl = getApplicationInfo().nativeLibraryDir + "/libbootctl.so";
			Root.text(bootctl + " set-active-boot-slot " + (Ios.slot() == 'a' ? 0 : 1));
			if (Gpt.load().activeSlot() != Ios.slot()) {
				throw new IOException(L.t("bootctl no cambió la ranura: no se reinicia", "bootctl did not change the slot: not restarting"));
			}
			Root.text("sync; svc power reboot || reboot");
			return L.t("Reiniciando en iOS…", "Restarting to iOS…");
		});
	}

	/* ================= Archivos iOS ================= */

	private View filesPage() {
		ScrollView[] h = new ScrollView[1];
		LinearLayout col = pageColumn(h);
		String[] titles = {L.t("Archivos iOS", "iOS Files"), L.t("Apps de iOS", "iOS Apps"), L.t("Fotos de iOS", "iOS Photos"), L.t("Buscar en iOS", "Search iOS")};
		TextView sub = pageTitle(col, titles[filesMode], L.t("Solo lectura · la partición de iOS vista desde Android", "Read-only · the iOS partition seen from Android"));

		/* which view of iOS: a connected button group (the selected one fills and morphs to full round) */
		String[] modes = {L.t("Archivos", "Files"), "Apps", L.t("Fotos", "Photos"), L.t("Buscar", "Search")};
		col.addView(Ui.connected(this, modes, filesMode, m -> {
			filesMode = m;
			show(PAGE_FILES);
		}), Ui.lp(-1, -2, dp(18)));
		if (filesMode == APPS) {
			AppsView.build(this, col, sub);
			return h[0];
		} else if (filesMode == PHOTOS) {
			PhotosView.build(this, col, sub);
			return h[0];
		} else if (filesMode == SEARCH) {
			SearchView.build(this, col, sub);
			return h[0];
		}

		/* path bar: an up button (glass), the breadcrumb, the folder's menu */
		LinearLayout bar = Ui.spaced(this, 8, false);
		View up = Ui.iconButton(this, "up", Ui.TONAL, v -> onBackPressed());
		up.setEnabled(!path.isEmpty());
		bar.addView(up, new LinearLayout.LayoutParams(dp(48), dp(48)));
		HorizontalScrollView hs = new HorizontalScrollView(this);
		hs.setHorizontalScrollBarEnabled(false);
		hs.setBackground(new Ui.Shape(Ui.surfaceHigh, Ui.FULL));
		hs.setClipToOutline(true);
		StringBuilder crumb = new StringBuilder("/");
		for (Hfs.Entry e : path) {
			crumb.append(e.name).append("/");
		}
		TextView crumbs = Ui.text(this, crumb.toString(), 14, Ui.onSurfaceVariant, true);
		crumbs.setGravity(Gravity.CENTER_VERTICAL);
		crumbs.setPadding(dp(18), 0, dp(18), 0);
		hs.addView(crumbs, new FrameLayout.LayoutParams(-2, dp(48)));
		hs.post(() -> hs.fullScroll(View.FOCUS_RIGHT));
		bar.addView(hs, new LinearLayout.LayoutParams(0, dp(48), 1));
		Hfs.Entry here = path.isEmpty() ? null : path.get(path.size() - 1);
		bar.addView(Ui.iconButton(this, "more", Ui.TONAL, v -> folderActions(here, currentPath())), new LinearLayout.LayoutParams(dp(48), dp(48)));
		col.addView(bar, Ui.lp(-1, -2, dp(14)));

		LinearLayout list = Ui.list(this);
		col.addView(list, Ui.lp(-1, -2, dp(12)));
		TextView loading = new Ui.LoadingText(this, L.t("Leyendo…", "Reading…"));
		loading.setPadding(dp(18), dp(16), dp(18), dp(16));
		list.addView(loading);

		int dir = path.isEmpty() ? Hfs.ROOT_ID : path.get(path.size() - 1).id;
		background(() -> {
			List<Hfs.Entry> entries = hfs().list(dir);
			runOnUiThread(() -> {
				list.removeAllViews();
				sub.setText(entries.size() + L.t(" elementos · volumen ", " items · ") + (hfs.caseSensitive ? "HFSX" : "HFS+") + L.t(" de ", " volume of ")
				    + Ui.size(hfs.totalBytes()));
				int shown = 0;
				for (Hfs.Entry e : entries) {
					if (shown++ >= 3000) {
						break;
					}
					list.addView(fileRow(e));
				}
				if (entries.isEmpty()) {
					TextView empty = Ui.text(this, L.t("  Carpeta vacía", "  Empty folder"), 15, Ui.onSurfaceVariant, false);
					empty.setPadding(dp(14), dp(18), dp(14), dp(18));
					list.addView(empty);
				}
			});
			return null;
		}, err -> loading.setText("⚠  " + err));
		return h[0];
	}

	View fileRow(Hfs.Entry e) {
		LinearLayout r = Ui.row(this);
		r.setPadding(dp(14), dp(12), dp(14), dp(12));
		r.addView(fileBadge(e), new LinearLayout.LayoutParams(dp(40), dp(40)));
		LinearLayout t = Ui.column(this);
		t.setPadding(dp(14), 0, dp(8), 0);
		TextView name = Ui.text(this, e.name, 16, Ui.onSurface, false);
		name.setSingleLine(true);
		name.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
		t.addView(name);
		String meta = (e.folder ? e.size + L.t(" elementos", " items") : Ui.size(e.size)) + "  ·  "
		    + DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(new Date(e.modified))
		    + (e.compressed ? L.t("  ·  comprimido", "  ·  compressed") : "") + (e.hardLink ? L.t("  ·  enlace", "  ·  link") : "");
		TextView m = Ui.text(this, meta, 13, Ui.onSurfaceVariant, false);
		m.setSingleLine(true);
		m.setEllipsize(android.text.TextUtils.TruncateAt.END);
		t.addView(m);
		r.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
		if (e.folder) {
			r.addView(Ui.iconView(this, "chevron", Ui.onSurfaceVariant, 24));
		}
		r.setOnClickListener(v -> {
			if (e.folder) {
				path.add(e);
				show(PAGE_FILES);
			} else {
				fileActions(e);
			}
		});
		if (e.folder) {
			r.setOnLongClickListener(v -> {
				folderActions(e, currentPath().equals("/") ? "/" + e.name : currentPath() + "/" + e.name);
				return true;
			});
		}
		return r;
	}

	/** A file's leading icon: folders on a primary container (rounded square), files on a neutral circle. */
	View fileBadge(Hfs.Entry e) {
		if (e.folder) {
			return Ui.iconBadge(this, "folder", Ui.primaryContainer, Ui.onPrimaryContainer, 40, 14);
		}
		String icon = e.symlink ? "link" : isImage(e.name) ? "image" : isText(e.name) ? "text" : "file";
		return isImage(e.name) ? Ui.iconBadge(this, icon, Ui.tertiaryContainer, Ui.onTertiaryContainer, 40, Ui.FULL)
		    : Ui.iconBadge(this, icon, Ui.surfaceHighest, Ui.onSurfaceVariant, 40, Ui.FULL);
	}

	/** A folder's menu (E null: the volume's root). */
	void folderActions(Hfs.Entry e, String p) {
		String name = e == null ? "iOS" : e.name;
		int id = e == null ? Hfs.ROOT_ID : e.id;
		String[] actions = {L.t("Exportar como .zip a Descargas", "Export as .zip to Downloads"), L.t("Calcular tamaño", "Calculate size"), L.t("Buscar aquí", "Search here"), L.t("Información", "Info")};
		new Ui.Dialog(this).setTitle(p).setItems(actions, (d, which) -> {
			if (which == 0) {
				confirm(L.t("Exportar ", "Export ") + name, L.t("Se copiará «", "“") + p + L.t("» entera a Descargas/iOS/", "” will be copied whole to Downloads/iOS/") + name + L.t(".zip. Las carpetas grandes ", ".zip. Large folders ")
				    + L.t("tardan: puedes cancelar en cualquier momento.", "take a while: you can cancel at any time."), L.t("Exportar", "Export"), () -> exportZip(id, name + ".zip", name + "/"));
			} else if (which == 1) {
				task(L.t("Tamaño de ", "Size of ") + name, true, t -> {
					t.status(L.t("Recorriendo…", "Walking the folder…"));
					long[] n = HfsTools.measure(hfs(), id, t.cancel);
					return t.cancel.cancelled ? L.t("Cancelado.", "Cancelled.") : p + "\n\n" + n[0] + L.t(" ficheros\n", " files\n") + Ui.size(n[1]) + " (" + n[1] + " bytes)";
				});
			} else if (which == 2) {
				SearchView.scope = p;
				filesMode = SEARCH;
				show(PAGE_FILES);
			} else if (e != null) {
				info(e);
			} else if (hfs != null) {
				textDialog("iOS", L.t("Volumen ", "Volume ") + hfs.signature + L.t(", bloques de ", ", blocks of ") + hfs.blockSize + " bytes\n" + Ui.size(hfs.usedBytes())
				    + L.t(" usados de ", " used of ") + Ui.size(hfs.totalBytes()));
			}
		}).show();
	}

	void fileActions(Hfs.Entry e) {
		String[] actions = {L.t("Ver", "View"), L.t("Exportar a Descargas", "Export to Downloads"), L.t("Información", "Info")};
		new Ui.Dialog(this).setTitle(e.name).setItems(actions, (d, which) -> {
			if (which == 0) {
				preview(e);
			} else if (which == 1) {
				export(e);
			} else {
				info(e);
			}
		}).show();
	}

	private void preview(Hfs.Entry e) {
		progress(L.t("Abriendo…", "Opening…"), () -> {
			if (isImage(e.name)) {
				byte[] b = hfs.head(e, 32 << 20);
				Bitmap bm = Ui.icon(b);         /* Apple's crushed PNGs too */
				if (bm == null) {
					throw new IOException(L.t("no se pudo decodificar la imagen", "could not decode the image"));
				}
				runOnUiThread(() -> {
					ImageView iv = new ImageView(this);
					iv.setImageBitmap(bm);
					iv.setAdjustViewBounds(true);
					new Ui.Dialog(this).setTitle(e.name).setView(iv).setPositiveButton(L.t("Cerrar", "Close"), null).show();
				});
				return null;
			}
			byte[] b = hfs.head(e, 256 << 10);
			String s = null;
			if (b.length >= 8 && new String(b, 0, 8, java.nio.charset.StandardCharsets.US_ASCII).equals("bplist00") && b.length == e.size) {
				try {
					s = L.t("(plist binario, mostrado como texto)\n\n", "(binary plist, shown as text)\n\n") + plistText(Plist.parse(b), "");
				} catch (IOException ignored) {
				}
			}
			if (s == null) {
				s = looksBinary(b) ? hexdump(b, 4096) : new String(b, java.nio.charset.StandardCharsets.UTF_8);
			}
			String text = s;
			runOnUiThread(() -> textDialog(e.name, text));
			return null;
		});
	}

	/** A parsed plist as indented text (binary plists are unreadable as they are). */
	static String plistText(Object o, String in) {
		StringBuilder s = new StringBuilder();
		if (o instanceof java.util.Map) {
			for (java.util.Map.Entry<?, ?> e : ((java.util.Map<?, ?>) o).entrySet()) {
				Object v = e.getValue();
				boolean nested = v instanceof java.util.Map || v instanceof List;
				s.append(in).append(e.getKey()).append(nested ? ":\n" + plistText(v, in + "  ") : " = " + plistText(v, "") + "\n");
			}
		} else if (o instanceof List) {
			for (Object v : (List<?>) o) {
				boolean nested = v instanceof java.util.Map || v instanceof List;
				s.append(in).append(nested ? "-\n" + plistText(v, in + "  ") : "- " + plistText(v, "") + "\n");
			}
		} else if (o instanceof byte[]) {
			return "<" + ((byte[]) o).length + " bytes>";
		} else {
			return String.valueOf(o);
		}
		return s.toString();
	}

	private void export(Hfs.Entry e) {
		progress(L.t("Exportando ", "Exporting ") + e.name + "…", () -> {
			ContentValues cv = new ContentValues();
			cv.put(MediaStore.Downloads.DISPLAY_NAME, e.name);
			cv.put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/iOS");
			Uri uri = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv);
			if (uri == null) {
				throw new IOException(L.t("no se pudo crear el fichero en Descargas", "could not create the file in Downloads"));
			}
			try (OutputStream o = getContentResolver().openOutputStream(uri)) {
				hfs.copy(e, o, null);
			}
			return L.t("Exportado a Descargas/iOS/", "Exported to Downloads/iOS/") + e.name;
		});
	}

	private void info(Hfs.Entry e) {
		String s = L.t("Nombre: ", "Name: ") + e.name + L.t("\nTipo: ", "\nType: ") + (e.folder ? L.t("carpeta", "folder") : e.symlink ? L.t("enlace simbólico", "symbolic link") : L.t("fichero", "file"))
		    + L.t("\nTamaño: ", "\nSize: ") + (e.folder ? e.size + L.t(" elementos", " items") : Ui.size(e.size) + " (" + e.size + " bytes)")
		    + L.t("\nModificado: ", "\nModified: ") + new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date(e.modified))
		    + L.t("\nPermisos: ", "\nPermissions: ") + String.format(Locale.ROOT, "%o", e.mode & 07777) + "  uid " + e.uid + "  gid " + e.gid
		    + L.t("\nID de catálogo: ", "\nCatalog ID: ") + e.id + (e.compressed ? L.t("\nComprimido (decmpfs)", "\nCompressed (decmpfs)") : "") + (e.hardLink ? L.t("\nEnlace duro", "\nHard link") : "");
		textDialog(e.name, s);
	}

	static boolean isImage(String n) {
		String l = n.toLowerCase(Locale.ROOT);
		return l.endsWith(".png") || l.endsWith(".jpg") || l.endsWith(".jpeg") || l.endsWith(".gif") || l.endsWith(".heic") || l.endsWith(".webp");
	}

	private static boolean isText(String n) {
		String l = n.toLowerCase(Locale.ROOT);
		return l.endsWith(".txt") || l.endsWith(".plist") || l.endsWith(".log") || l.endsWith(".json") || l.endsWith(".strings")
		    || l.endsWith(".conf") || l.endsWith(".xml") || l.endsWith(".sh") || l.endsWith(".md");
	}

	private static boolean looksBinary(byte[] b) {
		int n = Math.min(b.length, 4096), bad = 0;
		for (int i = 0; i < n; i++) {
			int c = b[i] & 0xff;
			if (c == 0 || (c < 9) || (c > 13 && c < 32)) {
				bad++;
			}
		}
		return n > 0 && bad * 20 > n;
	}

	private static String hexdump(byte[] b, int max) {
		StringBuilder s = new StringBuilder();
		int n = Math.min(b.length, max);
		for (int i = 0; i < n; i += 16) {
			s.append(String.format(Locale.ROOT, "%06x  ", i));
			StringBuilder a = new StringBuilder();
			for (int j = 0; j < 16; j++) {
				if (i + j < n) {
					int c = b[i + j] & 0xff;
					s.append(String.format(Locale.ROOT, "%02x ", c));
					a.append(c >= 32 && c < 127 ? (char) c : '.');
				} else {
					s.append("   ");
				}
			}
			s.append(' ').append(a).append('\n');
		}
		return s.toString();
	}

	/* ================= Espacio ================= */

	private View spacePage() {
		ScrollView[] h = new ScrollView[1];
		LinearLayout col = pageColumn(h);
		pageTitle(col, L.t("Espacio", "Space"), L.t("Cómo se reparte el almacenamiento entre iOS y Android", "How the storage is split between iOS and Android"));

		LinearLayout now = card(col, L.t("Ahora", "Now"));
		TextView nowText = Ui.text(this, L.t("Leyendo…", "Reading…"), 15, Ui.onSurface, false);
		now.addView(nowText, Ui.lp(-1, -2, dp(10)));
		SplitBar split = new SplitBar(this);
		now.addView(split, Ui.lp(-1, dp(24), dp(14)));
		now.addView(legend(), Ui.lp(-1, -2, dp(12)));

		LinearLayout plan = card(col, L.t("Nuevo reparto", "New split"));
		TextView planText = Ui.text(this, "", 26, Ui.onSurface, true);
		plan.addView(planText, Ui.lp(-1, -2, dp(8)));
		TextView planSub = Ui.text(this, "", 14, Ui.onSurfaceVariant, false);
		plan.addView(planSub, Ui.lp(-1, -2, dp(4)));
		SplitBar planBar = new SplitBar(this);
		plan.addView(planBar, Ui.lp(-1, dp(24), dp(14)));
		SeekBar slider = new SeekBar(this);
		Ui.slider(slider);
		slider.setEnabled(false);
		plan.addView(slider, Ui.lp(-1, -2, dp(10)));
		TextView limits = Ui.text(this, "", 13, Ui.onSurfaceVariant, false);
		plan.addView(limits, Ui.lp(-1, -2, dp(4)));

		TextView apply = Ui.button(this, L.t("Aplicar este reparto…", "Apply this split…"), "pie", Ui.TINT_WARN, null);
		col.addView(apply, Ui.lp(-1, dp(64), dp(16)));

		TextView note = Ui.text(this, L.t("El cambio de tamaño mueve la frontera entre las dos particiones: iOS encoge o ", "Resizing moves the boundary between the two partitions: iOS shrinks or ")
		    + L.t("agranda su volumen (HFS+, en vivo), la tabla de particiones se reescribe con copia de seguridad y Android ", "grows its volume (HFS+, live), the partition table is rewritten with a backup and Android ")
		    + L.t("ajusta su sistema de archivos. Dar espacio a iOS borra los datos de Android si no caben.", "adjusts its file system. Giving space to iOS erases Android's data if it does not fit."), 13, Ui.onSurfaceVariant, false);
		note.setPadding(dp(8), 0, dp(8), 0);
		col.addView(note, Ui.lp(-1, -2, dp(14)));

		background(() -> {
			long iosPart = Long.parseLong(Root.text("blockdev --getsize64 " + Ios.ROOT));
			long andPart = Long.parseLong(Root.text("blockdev --getsize64 /dev/block/by-name/userdata"));
			Hfs v = Hfs.open(Ios.ROOT);
			StatFs fs = new StatFs(Environment.getDataDirectory().getPath());
			long iosUsed = v.usedBytes(), andUsed = fs.getTotalBytes() - fs.getAvailableBytes();
			long total = iosPart + andPart;
			long gib = 1L << 30;
			long min = ((iosUsed + 8 * gib) / gib) * gib;                  /* iOS keeps 8 GB free at least */
			long max = ((total - andUsed - 8 * gib) / gib) * gib;          /* Android keeps 8 GB free at least */
			runOnUiThread(() -> {
				nowText.setText("iOS " + Ui.size(iosPart) + " (" + Ui.size(iosUsed) + L.t(" usados)\nAndroid ", " used)\nAndroid ") + Ui.size(andPart)
				    + " (" + Ui.size(andUsed) + L.t(" usados)", " used)"));
				split.set((float) iosPart / total, (float) iosUsed / total, (float) andUsed / total);
				int steps = (int) ((max - min) / gib);
				slider.setMax(Math.max(steps, 0));
				slider.setProgress((int) Math.max(0, Math.min(steps, (iosPart - min) / gib)));
				slider.setEnabled(steps > 0);
				limits.setText(L.t("Mínimo para iOS ", "Minimum for iOS ") + Ui.size(min) + L.t(" · máximo ", " · maximum ") + Ui.size(max) + L.t(" (8 GB libres en cada lado)", " (8 GB free on each side)"));
				SeekBar.OnSeekBarChangeListener l = new SeekBar.OnSeekBarChangeListener() {
					@Override
					public void onProgressChanged(SeekBar s, int p, boolean user) {
						long ios = min + (long) p * gib, and = total - ios;
						planText.setText("iOS " + Ui.size(ios) + "  ·  Android " + Ui.size(and));
						long d = ios - iosPart;
						planSub.setText(d == 0 ? L.t("Igual que ahora", "Same as now") : d > 0 ? L.t("iOS gana ", "iOS gains ") + Ui.size(d) + L.t(" que pierde Android", " from Android")
						    : L.t("Android gana ", "Android gains ") + Ui.size(-d) + L.t(" que pierde iOS", " from iOS"));
						planBar.set((float) ios / total, (float) iosUsed / total, (float) andUsed / total);
					}

					@Override
					public void onStartTrackingTouch(SeekBar s) {
					}

					@Override
					public void onStopTrackingTouch(SeekBar s) {
					}
				};
				slider.setOnSeekBarChangeListener(l);
				l.onProgressChanged(slider, slider.getProgress(), false);
				apply.setOnClickListener(x -> {
					long ios = min + (long) slider.getProgress() * gib;
					if (ios == ((iosPart / gib) * gib) || Math.abs(ios - iosPart) < gib) {
						textDialog(L.t("Nuevo reparto", "New split"), L.t("Es el mismo reparto que ahora.", "It is the same split as now."));
						return;
					}
					getSharedPreferences("plan", 0).edit().putLong("ios", ios).apply();
					textDialog(L.t("Reparto guardado", "Split saved"), "Plan: iOS " + Ui.size(ios) + ", Android " + Ui.size(total - ios) + ".\n\n"
					    + L.t("Aplicarlo necesita tres pasos en orden, con el teléfono enchufado:\n", "Applying it takes three steps in order, with the phone plugged in:\n")
					    + (ios < iosPart ? L.t("1. En iOS: encoger el volumen a ", "1. In iOS: shrink the volume to ") + Ui.size(ios) + L.t(" (en vivo, sin borrar nada).\n", " (live, without erasing anything).\n")
					    + L.t("2. En el recovery: mover la frontera de las particiones (con copia de la tabla).\n", "2. In recovery: move the partition boundary (with a copy of the table).\n")
					    + L.t("3. En el recovery: agrandar el sistema de archivos de Android.\n", "3. In recovery: grow Android's file system.\n")
					    : L.t("1. En el recovery: copia de seguridad de Android (sus datos se borran: f2fs no encoge).\n", "1. In recovery: back up Android (its data is erased: f2fs does not shrink).\n")
					    + L.t("2. En el recovery: mover la frontera y formatear Android.\n", "2. In recovery: move the boundary and format Android.\n")
					    + L.t("3. En iOS: agrandar el volumen hasta su nueva partición.\n", "3. In iOS: grow the volume to fill its new partition.\n"))
					    + L.t("\nLa ejecución automática de estos pasos llegará en la próxima versión; el plan queda guardado.", "\nRunning these steps automatically will come in the next version; the plan is saved."));
				});
			});
			return null;
		}, err -> nowText.setText("⚠  " + err));
		return h[0];
	}

	/** The split bar's key: used / free for each side, as dots. */
	private View legend() {
		LinearLayout l = Ui.spaced(this, 14, false);
		String[] names = {L.t("iOS usado", "iOS used"), L.t("iOS libre", "iOS free"), L.t("Android usado", "Android used"), L.t("Android libre", "Android free")};
		int[] colors = {Ui.primary, Ui.primaryContainer, Ui.green, Ui.greenContainer};
		for (int i = 0; i < names.length; i++) {
			TextView t = Ui.text(this, names[i], 12, Ui.onSurfaceVariant, false);
			android.graphics.drawable.GradientDrawable dot = new android.graphics.drawable.GradientDrawable();
			dot.setShape(android.graphics.drawable.GradientDrawable.OVAL);
			dot.setColor(colors[i]);
			dot.setSize(dp(10), dp(10));
			dot.setBounds(0, 0, dp(10), dp(10));
			t.setCompoundDrawablesRelative(dot, null, null, null);
			t.setCompoundDrawablePadding(dp(6));
			t.setSingleLine(true);
			l.addView(t);
		}
		HorizontalScrollView hs = new HorizontalScrollView(this);
		hs.setHorizontalScrollBarEnabled(false);
		hs.addView(l);
		return hs;
	}

	/**
	 * Storage as two segments with a gap (M3 Expressive): iOS's partition (primary, its free part in the
	 * container tone) then Android's (green); outer ends full-round, the ends that meet squarer.
	 */
	static final class SplitBar extends View {
		private final android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
		private final android.graphics.Path seg = new android.graphics.Path();
		private float iosPart, iosUsed, andUsed;

		SplitBar(android.content.Context c) {
			super(c);
		}

		void set(float iosPart, float iosUsed, float andUsed) {
			this.iosPart = iosPart;
			this.iosUsed = iosUsed;
			this.andUsed = andUsed;
			invalidate();
		}

		private void segment(android.graphics.Canvas c, float x0, float x1, float h, boolean left, int free, int used, float usedTo) {
			if (x1 - x0 < 1) {
				return;
			}
			float full = h / 2, in = Ui.dp(getContext(), 6);
			float a = left ? full : in, b = left ? in : full;
			seg.reset();
			seg.addRoundRect(x0, 0, x1, h, new float[]{a, a, b, b, b, b, a, a}, android.graphics.Path.Direction.CW);
			c.save();
			c.clipPath(seg);
			p.setColor(free);
			c.drawRect(x0, 0, x1, h, p);
			p.setColor(used);
			c.drawRect(x0, 0, Math.min(x1, usedTo), h, p);
			c.restore();
		}

		@Override
		protected void onDraw(android.graphics.Canvas c) {
			float w = getWidth(), h = getHeight(), gap = Ui.dp(getContext(), 3);
			float split = w * iosPart;
			segment(c, 0, split - gap, h, true, Ui.primaryContainer, Ui.primary, w * iosUsed);
			segment(c, split + gap, w, h, false, Ui.greenContainer, Ui.green, split + gap + w * andUsed);
		}
	}

	/* ================= Herramientas ================= */

	private View toolsPage() {
		ScrollView[] h = new ScrollView[1];
		LinearLayout col = pageColumn(h);
		pageTitle(col, L.t("Herramientas", "Tools"), L.t("Imagen de iOS, registros y arranque", "iOS image, logs and boot"));

		col.addView(Ui.button(this, L.t("Instalar o actualizar la imagen de iOS…", "Install or update the iOS image…"), "install", Ui.TINT_IOS, v -> pickImage()),
		    Ui.lp(-1, dp(72), dp(20)));

		LinearLayout actions = Ui.list(this);
		actions.addView(actionRow("text", L.t("Ver el registro del kernel de iOS", "View the iOS kernel log"), v -> LogViewer.open(this)));
		actions.addView(actionRow("copy", L.t("Copiar la calibración de este teléfono a iOS", "Copy this phone's calibration to iOS"), v -> confirm(
		    L.t("Calibración para iOS", "Calibration for iOS"),
		    L.t("Cada teléfono tiene la suya: la del sensor de huellas y la de los demás sensores. Se lee de persist (sin tocarlo) y se deja en la bandeja de iOS, que la pone en su sitio al arrancar. Tus huellas registradas en iOS se conservan.",
		        "Every phone has its own: the fingerprint sensor's and the other sensors'. It is read from persist (left untouched) and left in iOS's inbox, which puts it in place when iOS starts. The fingerprints enrolled in iOS are kept."),
		    L.t("Copiar", "Copy"), () -> progress(L.t("Copiando la calibración…", "Copying the calibration…"), () -> Calibration.copyToIos(getCacheDir(), null)))));
		actions.addView(actionRow("save", L.t("Guardar copia de la tabla de ranuras", "Back up the slot table"), v -> progress(L.t("Copiando…", "Copying…"), () -> {
			Gpt g = Gpt.load();
			return L.t("Copia guardada en ", "Backup saved to ") + save("sapphire-gpt-" + g.disk.substring(g.disk.lastIndexOf('/') + 1), g.backup());
		})));
		col.addView(actions, Ui.lp(-1, -2, dp(14)));

		/* restarts: a button group (the one pressed widens) */
		Ui.Group restarts = new Ui.Group(this, false);
		TextView rec = Ui.button(this, "Recovery", "power", 0, v -> confirm(L.t("Reiniciar en recovery", "Restart to recovery"), L.t("Se reinicia en OrangeFox.", "Restarts into OrangeFox."),
		    L.t("Reiniciar", "Restart"), () -> progress(L.t("Reiniciando…", "Restarting…"), () -> Root.text("reboot recovery"))));
		TextView fb = Ui.button(this, "Fastboot", "power", 0, v -> confirm(L.t("Reiniciar en fastboot", "Restart to fastboot"), L.t("Se reinicia en el modo fastboot.", "Restarts into fastboot mode."),
		    L.t("Reiniciar", "Restart"), () -> progress(L.t("Reiniciando…", "Restarting…"), () -> Root.text("reboot bootloader"))));
		restarts.addView(rec, new LinearLayout.LayoutParams(0, dp(56), 1));
		restarts.addView(fb, new LinearLayout.LayoutParams(0, dp(56), 1));
		col.addView(restarts, Ui.lp(-1, -2, dp(14)));

		/* the device, both systems: one card per side, plain text that can be copied for a bug report */
		LinearLayout andCard = card(col, "Android");
		TextView ad = mono(andCard, L.t("Leyendo…", "Reading…"));
		ad.setTextIsSelectable(true);
		LinearLayout iosCard = card(col, "iOS");
		TextView id = mono(iosCard, L.t("Leyendo…", "Reading…"));
		id.setTextIsSelectable(true);
		LinearLayout det = card(col, L.t("Arranque", "Boot"));
		TextView d = mono(det, L.t("Leyendo…", "Reading…"));
		d.setTextIsSelectable(true);
		String[] all = new String[3];
		col.addView(Ui.button(this, L.t("Copiar toda la información", "Copy all the information"), "copy", 0, v -> {
			android.content.ClipboardManager cm = getSystemService(android.content.ClipboardManager.class);
			cm.setPrimaryClip(android.content.ClipData.newPlainText("Sapphire", "ANDROID\n" + all[0] + "\n\niOS\n" + all[1] + L.t("\n\nARRANQUE\n", "\n\nBOOT\n") + all[2]));
			android.widget.Toast.makeText(this, L.t("Copiado", "Copied"), android.widget.Toast.LENGTH_SHORT).show();
		}), Ui.lp(-1, dp(56), dp(14)));
		background(() -> {
			StatFs fs = new StatFs(Environment.getDataDirectory().getPath());
			String a = L.t("Modelo: ", "Model: ") + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL + " (" + android.os.Build.DEVICE + ")"
			    + "\nAndroid " + android.os.Build.VERSION.RELEASE + " (API " + android.os.Build.VERSION.SDK_INT + ")"
			    + L.t("\nCompilación: ", "\nBuild: ") + android.os.Build.DISPLAY
			    + L.t("\nParche: ", "\nPatch: ") + android.os.Build.VERSION.SECURITY_PATCH
			    + "\nKernel: " + System.getProperty("os.version")
			    + L.t("\nRanura: ", "\nSlot: ") + Root.text("getprop ro.boot.slot_suffix")
			    + L.t("\nDatos: ", "\nData: ") + Ui.size(fs.getTotalBytes() - fs.getAvailableBytes()) + L.t(" usados de ", " used of ") + Ui.size(fs.getTotalBytes())
			    + L.t("\nEncendido hace ", "\nUp for ") + uptime(android.os.SystemClock.elapsedRealtime());
			all[0] = a;
			runOnUiThread(() -> ad.setText(a));
			Hfs v = hfs();
			String ver = v.iosVersion();
			String i = L.t("Versión: iOS ", "Version: iOS ") + (ver == null ? "?" : ver)
			    + "\nKernel: " + Ios.kernelInfo()
			    + L.t("\nImagen en boot_", "\nImage in boot_") + Ios.slot() + ": " + Ios.bootState()
			    + L.t("\nVolver a Android: ", "\nBack to Android: ") + (Ios.canReturn() ? L.t("sí (kernel 0306 o posterior)", "yes (kernel 0306 or later)") : L.t("NO (imagen antigua)", "NO (old image)"))
			    + L.t("\nVolumen: ", "\nVolume: ") + (v.caseSensitive ? "HFSX" : "HFS+") + ", " + Ui.size(v.usedBytes()) + L.t(" usados de ", " used of ") + Ui.size(v.totalBytes())
			    + "\niosroot: " + Ios.rootState();
			try {
				IpaInbox box = IpaInbox.device();
				i += L.t("\nBandeja de IPAs: ", "\nIPA inbox: ") + box.entries.size() + " apps, " + Ui.size(box.largestFree()) + L.t(" libres de ", " free of ")
				    + Ui.size(box.geometry.bytes());
			} catch (IOException x) {
				i += L.t("\nBandeja de IPAs: ", "\nIPA inbox: ") + x.getMessage();
			}
			all[1] = i;
			String ii = i;
			runOnUiThread(() -> id.setText(ii));
			Gpt g = Gpt.load();
			String s = L.t("Ranura de iOS: _", "iOS slot: _") + Ios.slot() + "\n" + g.describe('a') + "\n" + g.describe('b') + "\nboot LUN: " + Ios.bootLun()
			    + L.t("\nDisco de ranuras: ", "\nSlot disk: ") + g.disk + " (" + g.count + L.t(" entradas)", " entries)");
			all[2] = s;
			runOnUiThread(() -> d.setText(s));
			return null;
		}, err -> d.setText("⚠  " + err));

		LinearLayout quick = card(col, L.t("Accesos rápidos", "Shortcuts"));
		quick.addView(Ui.text(this, L.t("• Ajustes rápidos: edita el panel de notificaciones y arrastra el botón «Reiniciar en iOS».\n", "• Quick Settings: edit the notification panel and drag the “Restart to iOS” tile.\n")
		    + L.t("• Pantalla de inicio: mantén pulsado el icono de la app → «Reiniciar en iOS».\n", "• Home screen: long-press the app icon → “Restart to iOS”.\n")
		    + L.t("Los dos abren esta app y piden confirmación antes de cambiar nada.", "Both open this app and ask for confirmation before changing anything."), 14, Ui.onSurfaceVariant, false), Ui.lp(-1, -2, dp(8)));

		LinearLayout about = card(col, L.t("Acerca de", "About"));
		about.addView(Ui.text(this, L.t("Sapphire Dual Boot 2.0\n#iOS4Sapphire\nHecho con ❤️ por Angelpro09_Devv", "Sapphire Dual Boot 2.0\n#iOS4Sapphire\nMade with ❤️ by Angelpro09_Devv"), 15, Ui.onSurface, false),
		    Ui.lp(-1, -2, dp(8)));
		return h[0];
	}

	/** A list row that is an action: a leading icon on a tonal shape, the label, a chevron. */
	View actionRow(String icon, String label, View.OnClickListener l) {
		LinearLayout r = Ui.row(this);
		r.setPadding(dp(14), dp(14), dp(14), dp(14));
		r.addView(Ui.iconBadge(this, icon, Ui.secondaryContainer, Ui.onSecondaryContainer, 40, 14), new LinearLayout.LayoutParams(dp(40), dp(40)));
		TextView t = Ui.text(this, label, 16, Ui.onSurface, false);
		t.setPadding(dp(16), 0, dp(8), 0);
		r.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
		r.addView(Ui.iconView(this, "chevron", Ui.onSurfaceVariant, 24));
		r.setOnClickListener(l);
		return r;
	}

	private void pickImage() {
		Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
		i.addCategory(Intent.CATEGORY_OPENABLE);
		i.setType("*/*");
		startActivityForResult(i, PICK_IMAGE);
	}

	static String uptime(long ms) {
		long m = ms / 60000;
		return (m >= 1440 ? m / 1440 + " d " : "") + (m / 60) % 24 + " h " + m % 60 + " min";
	}

	@Override
	protected void onActivityResult(int request, int result, Intent data) {
		if (request == PICK_IPA) {
			if (result == RESULT_OK && data != null) {
				IpaPage.picked(this, data);
			}
			return;
		}
		if (request != PICK_IMAGE || result != RESULT_OK || data == null || data.getData() == null) {
			return;
		}
		Uri uri = data.getData();
		progress(L.t("Comprobando la imagen…", "Checking the image…"), () -> {
			byte[] image = readAll(uri);
			long part = Long.parseLong(Root.text("blockdev --getsize64 " + Ios.boot()));
			String why = Ios.refuse(image, part);
			if (why != null) {
				throw new IOException(L.t("No se instala: ", "Not installed: ") + why);
			}
			char s = Ios.slot();
			runOnUiThread(() -> confirm(L.t("Instalar iOS", "Install iOS"), L.t("Imagen válida (", "Valid image (") + image.length / 1048576 + L.t(" MB). Se grabará en boot_", " MB). It will be written to boot_") + s
			    + L.t(" (la ranura de iOS; Android sigue en la otra) y se borrará el resto de esa partición.", " (the iOS slot; Android stays on the other one) and the rest of that partition will be erased."), L.t("Grabar", "Write"),
			    () -> progress(L.t("Grabando boot_", "Writing boot_") + s + "…", () -> {
				    Ios.install(image);
				    return L.t("Imagen de iOS instalada y comprobada en boot_", "iOS image installed and verified in boot_") + s + ".";
			    })));
			return null;
		});
	}

	/* ================= plumbing ================= */

	interface Job {
		Object run() throws Exception;
	}

	interface Err {
		void on(String message);
	}

	void background(Job job, Err err) {
		new Thread(() -> {
			try {
				job.run();
			} catch (Exception e) {
				android.util.Log.w("SapphireDualBoot", "background", e);
				runOnUiThread(() -> err.on(e.getMessage()));
			}
		}).start();
	}

	/** A job with a small modal progress dialog; a String result is shown as a message. */
	void progress(String what, Job job) {
		LinearLayout line = Ui.row(this);
		line.addView(Ui.loading(this, true), new LinearLayout.LayoutParams(dp(48), dp(48)));
		TextView t = Ui.text(this, what, 16, Ui.onSurface, false);
		t.setPadding(dp(18), 0, 0, 0);
		line.addView(t, new LinearLayout.LayoutParams(0, -2, 1));
		android.app.Dialog d = new Ui.Dialog(this).setView(line).setCancelable(false).show();
		new Thread(() -> {
			Object r;
			try {
				r = job.run();
			} catch (Exception e) {
				r = "Error: " + e.getMessage();
			}
			Object res = r;
			runOnUiThread(() -> {
				d.dismiss();
				if (res instanceof String) {
					textDialog("", (String) res);
				}
			});
		}).start();
	}

	void confirm(String title, String message, String yes, Runnable r) {
		new Ui.Dialog(this).setTitle(title).setMessage(message).setPositiveButton(yes, (d, w) -> r.run())
		    .setNegativeButton(L.t("Cancelar", "Cancel"), null).show();
	}

	void textDialog(String title, String s) {
		ScrollView sv = new ScrollView(this);
		TextView t = new TextView(this);
		t.setText(s);
		t.setTextIsSelectable(true);
		t.setTypeface(Typeface.MONOSPACE);
		t.setTextSize(12);
		t.setTextColor(Ui.onSurfaceVariant);
		t.setHighlightColor(Ui.alpha(Ui.primary, 0.35f));
		t.setLineSpacing(0, 1.12f);
		sv.addView(t);
		new Ui.Dialog(this).setTitle(title).setView(sv).setPositiveButton(L.t("Cerrar", "Close"), null).show();
	}

	private byte[] readAll(Uri uri) throws IOException {
		try (InputStream in = getContentResolver().openInputStream(uri)) {
			ByteArrayOutputStream out = new ByteArrayOutputStream();
			byte[] buf = new byte[1 << 20];
			for (int n; (n = in.read(buf)) > 0; ) {
				out.write(buf, 0, n);
				if (out.size() > 256 << 20) {
					throw new IOException(L.t("archivo demasiado grande", "file too large"));
				}
			}
			return out.toByteArray();
		}
	}

	/** Into Download/SapphireDualBoot/ (as root: no storage permission needed); returns the path. */
	String save(String name, byte[] data) throws IOException {
		String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
		String p = "/sdcard/Download/SapphireDualBoot/" + name + "-" + stamp + (name.contains("log") ? ".txt" : ".bin");
		Root.run("mkdir -p /sdcard/Download/SapphireDualBoot && cat > '" + p + "'", data);
		return p;
	}

	private static String tail(String s, int max) {
		return s.length() <= max ? s : "…\n" + s.substring(s.length() - max);
	}

	int dp(float v) {
		return Ui.dp(this, v);
	}

	private int R_mipmap_ic_launcher() {
		return getResources().getIdentifier("ic_launcher", "mipmap", getPackageName());
	}
}
