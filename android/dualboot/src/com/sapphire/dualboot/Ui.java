package com.sapphire.dualboot;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ArgbEvaluator;
import android.animation.TimeInterpolator;
import android.animation.ValueAnimator;
import android.content.Context;
import android.content.DialogInterface;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.Animatable;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ImageSpan;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewPropertyAnimator;
import android.view.Window;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * The look: Material 3 Expressive (Android 16), built by hand on plain views -- the app has no Gradle and no
 * AndroidX, so the design tokens live here instead of in the Material Components library:
 *  - colour: the wallpaper's dynamic scheme (Android 12+; the system's own roles on 14+), else a sapphire seed,
 *    light or dark after the system;
 *  - shape: large rounded containers, full-round buttons that morph to a squarer shape while pressed, lists
 *    split in segments (big outer corners, small inner ones), connected button groups;
 *  - type: bolder, larger headlines (Google Sans Flex where the phone has it);
 *  - motion: springs (expressive spatial springs overshoot a little, effects springs do not);
 *  - the expressive loading indicator (a shape morphing through the M3 shape set) and the wavy progress bar.
 */
final class Ui {
	/* ================= colour ================= */

	static boolean dark = true;
	static int primary, onPrimary, primaryContainer, onPrimaryContainer, secondary, secondaryContainer, onSecondaryContainer,
	    tertiary, tertiaryContainer, onTertiaryContainer, error, onError, errorContainer, onErrorContainer,
	    surface, onSurface, onSurfaceVariant, surfaceLowest, surfaceLow, surfaceContainer, surfaceHigh, surfaceHighest, surfaceBright,
	    outline, outlineVariant, inverseSurface, inverseOnSurface, inversePrimary,
	    green, onGreen, greenContainer, onGreenContainer, amber, onAmber, amberContainer, onAmberContainer;
	/* the names the pages use: text levels and the meaning colours (iOS, Android, warning, danger) */
	static int TEXT, TEXT2, TEXT3, TINT_IOS, TINT_ANDROID, TINT_WARN, TINT_DANGER;

	/*
	 * The fallback scheme (no dynamic colour: Android 11), Material's tonal-spot scheme of a sapphire blue
	 * (#0F52BA), in this order: primary, onPrimary, primaryContainer, onPrimaryContainer, secondary,
	 * onSecondary, secondaryContainer, onSecondaryContainer, tertiary, onTertiary, tertiaryContainer,
	 * onTertiaryContainer, error, onError, errorContainer, onErrorContainer, surface, onSurface,
	 * onSurfaceVariant, surfaceContainerLowest, Low, (plain), High, Highest, surfaceBright, surfaceDim,
	 * outline, outlineVariant, inverseSurface, inverseOnSurface, inversePrimary.
	 */
	private static final int[] LIGHT = {0xFF475D92, 0xFFFFFFFF, 0xFFD9E2FF, 0xFF2F4578, 0xFF575E71, 0xFFFFFFFF, 0xFFDCE2F9, 0xFF404659,
		0xFF725572, 0xFFFFFFFF, 0xFFFDD7FA, 0xFF593D59, 0xFFBA1A1A, 0xFFFFFFFF, 0xFFFFDAD6, 0xFF93000A, 0xFFFAF8FF, 0xFF1A1B20,
		0xFF44464F, 0xFFFFFFFF, 0xFFF4F3FA, 0xFFEEEDF4, 0xFFE8E7EF, 0xFFE2E2E9, 0xFFFAF8FF, 0xFFDAD9E0, 0xFF757780, 0xFFC5C6D0,
		0xFF2F3036, 0xFFF1F0F7, 0xFFB0C6FF};
	private static final int[] DARK = {0xFFB0C6FF, 0xFF152E60, 0xFF2F4578, 0xFFD9E2FF, 0xFFC0C6DC, 0xFF2A3042, 0xFF404659, 0xFFDCE2F9,
		0xFFE0BBDD, 0xFF412742, 0xFF593D59, 0xFFFDD7FA, 0xFFFFB4AB, 0xFF690005, 0xFF93000A, 0xFFFFDAD6, 0xFF121318, 0xFFE2E2E9,
		0xFFC5C6D0, 0xFF0C0E13, 0xFF1A1B20, 0xFF1E1F25, 0xFF282A2F, 0xFF33343A, 0xFF38393F, 0xFF121318, 0xFF8F9099, 0xFF44464F,
		0xFFE2E2E9, 0xFF2F3036, 0xFF475D92};

	static {
		init(null);
	}

	private Ui() {
	}

	/** The scheme for C's light/dark mode and wallpaper (null: the fallback, dark). Call before building views. */
	static void init(Context c) {
		dark = c == null || (c.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES;
		int[] s = (dark ? DARK : LIGHT).clone();
		try {
			if (c != null && Build.VERSION.SDK_INT >= 34) {
				roles34(c, s);
			} else if (c != null && Build.VERSION.SDK_INT >= 31) {
				tones31(c, s);
			}
		} catch (RuntimeException e) {
			s = (dark ? DARK : LIGHT).clone();         /* a ROM without the palette: the seed */
		}
		int i = 0;
		primary = s[i++]; onPrimary = s[i++]; primaryContainer = s[i++]; onPrimaryContainer = s[i++];
		secondary = s[i++]; i++; secondaryContainer = s[i++]; onSecondaryContainer = s[i++];
		tertiary = s[i++]; i++; tertiaryContainer = s[i++]; onTertiaryContainer = s[i++];
		error = s[i++]; onError = s[i++]; errorContainer = s[i++]; onErrorContainer = s[i++];
		surface = s[i++]; onSurface = s[i++]; onSurfaceVariant = s[i++];
		surfaceLowest = s[i++]; surfaceLow = s[i++]; surfaceContainer = s[i++]; surfaceHigh = s[i++]; surfaceHighest = s[i++];
		surfaceBright = s[i++]; i++; outline = s[i++]; outlineVariant = s[i++]; inverseSurface = s[i++]; inverseOnSurface = s[i++];
		inversePrimary = s[i];
		/* fixed meaning colours (Android green, warning amber) as M3 custom colours: tone 80/30/90 dark, 40/90/10 light */
		green = dark ? 0xFF43E188 : 0xFF006D3B;
		onGreen = dark ? 0xFF00391C : 0xFFFFFFFF;
		greenContainer = dark ? 0xFF00522B : 0xFF9DF7B9;
		onGreenContainer = dark ? 0xFF9DF7B9 : 0xFF00210E;
		amber = dark ? 0xFFFFB868 : 0xFF885200;
		onAmber = dark ? 0xFF482900 : 0xFFFFFFFF;
		amberContainer = dark ? 0xFF673D00 : 0xFFFFDDBB;
		onAmberContainer = dark ? 0xFFFFDDBB : 0xFF2B1700;
		TEXT = onSurface;
		TEXT2 = onSurfaceVariant;
		TEXT3 = outline;
		TINT_IOS = primary;
		TINT_ANDROID = green;
		TINT_WARN = amber;
		TINT_DANGER = error;
	}

	/** Android 14+: the system's own colour roles (they follow the user's wallpaper and style choice). */
	private static void roles34(Context c, int[] s) {
		boolean d = dark;
		int[][] ids = {
			{android.R.color.system_primary_light, android.R.color.system_primary_dark},
			{android.R.color.system_on_primary_light, android.R.color.system_on_primary_dark},
			{android.R.color.system_primary_container_light, android.R.color.system_primary_container_dark},
			{android.R.color.system_on_primary_container_light, android.R.color.system_on_primary_container_dark},
			{android.R.color.system_secondary_light, android.R.color.system_secondary_dark},
			{android.R.color.system_on_secondary_light, android.R.color.system_on_secondary_dark},
			{android.R.color.system_secondary_container_light, android.R.color.system_secondary_container_dark},
			{android.R.color.system_on_secondary_container_light, android.R.color.system_on_secondary_container_dark},
			{android.R.color.system_tertiary_light, android.R.color.system_tertiary_dark},
			{android.R.color.system_on_tertiary_light, android.R.color.system_on_tertiary_dark},
			{android.R.color.system_tertiary_container_light, android.R.color.system_tertiary_container_dark},
			{android.R.color.system_on_tertiary_container_light, android.R.color.system_on_tertiary_container_dark},
			{android.R.color.system_error_light, android.R.color.system_error_dark},
			{android.R.color.system_on_error_light, android.R.color.system_on_error_dark},
			{android.R.color.system_error_container_light, android.R.color.system_error_container_dark},
			{android.R.color.system_on_error_container_light, android.R.color.system_on_error_container_dark},
			{android.R.color.system_surface_light, android.R.color.system_surface_dark},
			{android.R.color.system_on_surface_light, android.R.color.system_on_surface_dark},
			{android.R.color.system_on_surface_variant_light, android.R.color.system_on_surface_variant_dark},
			{android.R.color.system_surface_container_lowest_light, android.R.color.system_surface_container_lowest_dark},
			{android.R.color.system_surface_container_low_light, android.R.color.system_surface_container_low_dark},
			{android.R.color.system_surface_container_light, android.R.color.system_surface_container_dark},
			{android.R.color.system_surface_container_high_light, android.R.color.system_surface_container_high_dark},
			{android.R.color.system_surface_container_highest_light, android.R.color.system_surface_container_highest_dark},
			{android.R.color.system_surface_bright_light, android.R.color.system_surface_bright_dark},
			{android.R.color.system_surface_dim_light, android.R.color.system_surface_dim_dark},
			{android.R.color.system_outline_light, android.R.color.system_outline_dark},
			{android.R.color.system_outline_variant_light, android.R.color.system_outline_variant_dark},
		};
		for (int k = 0; k < ids.length; k++) {
			s[k] = c.getColor(ids[k][d ? 1 : 0]);
		}
		s[28] = c.getColor(d ? android.R.color.system_surface_light : android.R.color.system_surface_dark);         /* inverse surface */
		s[29] = c.getColor(d ? android.R.color.system_on_surface_light : android.R.color.system_on_surface_dark);
		s[30] = c.getColor(d ? android.R.color.system_primary_light : android.R.color.system_primary_dark);
	}

	/* the tonal palettes of Android 12/13: resources for tones 100, 99, 95, 90, 80 ... 10, 0 */
	private static final float[] TONES = {100, 99, 95, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0};

	private static int tone(Context c, int[] ids, float t) {
		for (int k = 0; k < TONES.length - 1; k++) {
			if (t <= TONES[k] && t >= TONES[k + 1]) {
				float f = (TONES[k] - t) / (TONES[k] - TONES[k + 1]);
				return blend(c.getColor(ids[k]), c.getColor(ids[k + 1]), f);
			}
		}
		return c.getColor(ids[ids.length - 1]);
	}

	/** Android 12/13: the M3 scheme built from the wallpaper's tonal palettes (the roles came in 14). */
	private static void tones31(Context c, int[] s) {
		int[] a1 = {android.R.color.system_accent1_0, android.R.color.system_accent1_10, android.R.color.system_accent1_50,
			android.R.color.system_accent1_100, android.R.color.system_accent1_200, android.R.color.system_accent1_300,
			android.R.color.system_accent1_400, android.R.color.system_accent1_500, android.R.color.system_accent1_600,
			android.R.color.system_accent1_700, android.R.color.system_accent1_800, android.R.color.system_accent1_900,
			android.R.color.system_accent1_1000};
		int[] a2 = {android.R.color.system_accent2_0, android.R.color.system_accent2_10, android.R.color.system_accent2_50,
			android.R.color.system_accent2_100, android.R.color.system_accent2_200, android.R.color.system_accent2_300,
			android.R.color.system_accent2_400, android.R.color.system_accent2_500, android.R.color.system_accent2_600,
			android.R.color.system_accent2_700, android.R.color.system_accent2_800, android.R.color.system_accent2_900,
			android.R.color.system_accent2_1000};
		int[] a3 = {android.R.color.system_accent3_0, android.R.color.system_accent3_10, android.R.color.system_accent3_50,
			android.R.color.system_accent3_100, android.R.color.system_accent3_200, android.R.color.system_accent3_300,
			android.R.color.system_accent3_400, android.R.color.system_accent3_500, android.R.color.system_accent3_600,
			android.R.color.system_accent3_700, android.R.color.system_accent3_800, android.R.color.system_accent3_900,
			android.R.color.system_accent3_1000};
		int[] n1 = {android.R.color.system_neutral1_0, android.R.color.system_neutral1_10, android.R.color.system_neutral1_50,
			android.R.color.system_neutral1_100, android.R.color.system_neutral1_200, android.R.color.system_neutral1_300,
			android.R.color.system_neutral1_400, android.R.color.system_neutral1_500, android.R.color.system_neutral1_600,
			android.R.color.system_neutral1_700, android.R.color.system_neutral1_800, android.R.color.system_neutral1_900,
			android.R.color.system_neutral1_1000};
		int[] n2 = {android.R.color.system_neutral2_0, android.R.color.system_neutral2_10, android.R.color.system_neutral2_50,
			android.R.color.system_neutral2_100, android.R.color.system_neutral2_200, android.R.color.system_neutral2_300,
			android.R.color.system_neutral2_400, android.R.color.system_neutral2_500, android.R.color.system_neutral2_600,
			android.R.color.system_neutral2_700, android.R.color.system_neutral2_800, android.R.color.system_neutral2_900,
			android.R.color.system_neutral2_1000};
		boolean d = dark;
		int[][] pal = {a1, a2, a3};
		for (int k = 0; k < 3; k++) {
			s[k * 4] = tone(c, pal[k], d ? 80 : 40);
			s[k * 4 + 1] = tone(c, pal[k], d ? 20 : 100);
			s[k * 4 + 2] = tone(c, pal[k], d ? 30 : 90);
			s[k * 4 + 3] = tone(c, pal[k], d ? 90 : 10);
		}
		/* surface, onSurface, onSurfaceVariant, lowest, low, container, high, highest, bright, dim, outline, outlineVariant, inverse, inverseOn */
		float[] surf = d ? new float[]{6, 90, 80, 4, 10, 12, 17, 22, 24, 6, 60, 30, 90, 20} : new float[]{98, 10, 30, 100, 96, 94, 92, 90, 98, 87, 50, 80, 20, 95};
		int[][] which = {n1, n1, n2, n1, n1, n1, n1, n1, n1, n1, n2, n2, n1, n1};
		for (int k = 0; k < surf.length; k++) {
			s[16 + k] = tone(c, which[k], surf[k]);
		}
		s[30] = tone(c, a1, d ? 40 : 80);
	}

	static int alpha(int color, float a) {
		return (color & 0x00FFFFFF) | (Math.round(a * 255) << 24);
	}

	static int blend(int a, int b, float t) {
		return (int) new ArgbEvaluator().evaluate(Math.max(0, Math.min(1, t)), a, b);
	}

	static int dp(Context c, float v) {
		return Math.round(v * c.getResources().getDisplayMetrics().density);
	}

	/* ================= type ================= */

	private static Typeface display;

	/** The headline family: Google Sans Flex / Google Sans where the phone has them, else the system sans. */
	private static Typeface displayFamily() {
		if (display == null) {
			display = Typeface.SANS_SERIF;
			for (String f : new String[]{"google-sans-flex", "google-sans"}) {
				Typeface t = Typeface.create(f, Typeface.NORMAL);
				if (t != null && t != Typeface.DEFAULT && !t.equals(Typeface.DEFAULT)) {
					display = t;
					break;
				}
			}
		}
		return display;
	}

	static Typeface font(int weight) {
		return Typeface.create(Typeface.SANS_SERIF, weight, false);
	}

	static Typeface displayFont(int weight) {
		return Typeface.create(displayFamily(), weight, false);
	}

	/** Text in the type scale: BOLD at 22sp and up is a headline (emphasized, heavier), below it a title. */
	static TextView text(Context c, String s, float sp, int color, boolean bold) {
		TextView t = new TextView(c);
		t.setText(s);
		t.setTextSize(sp);
		t.setTextColor(color);
		if (bold && sp >= 22) {
			t.setTypeface(displayFont(sp >= 30 ? 700 : 600));
			t.setLetterSpacing(-0.015f);
		} else if (bold) {
			t.setTypeface(font(sp >= 16 ? 600 : 500));
		} else {
			t.setTypeface(font(400));
			t.setLineSpacing(0, 1.12f);
		}
		return t;
	}

	static LinearLayout column(Context c) {
		LinearLayout l = new LinearLayout(c);
		l.setOrientation(LinearLayout.VERTICAL);
		return l;
	}

	static LinearLayout row(Context c) {
		LinearLayout l = new LinearLayout(c);
		l.setOrientation(LinearLayout.HORIZONTAL);
		l.setGravity(Gravity.CENTER_VERTICAL);
		return l;
	}

	/** A row (or column) whose children are spaced GAP dp apart. */
	static LinearLayout spaced(Context c, float gapDp, boolean vertical) {
		LinearLayout l = vertical ? column(c) : row(c);
		GradientDrawable gap = new GradientDrawable();
		gap.setColor(0);
		gap.setSize(dp(c, gapDp), dp(c, gapDp));
		l.setDividerDrawable(gap);
		l.setShowDividers(LinearLayout.SHOW_DIVIDER_MIDDLE);
		return l;
	}

	/** A row for chips: 8dp between them. */
	static LinearLayout chipRow(Context c) {
		return spaced(c, 8, false);
	}

	static LinearLayout.LayoutParams lp(int w, int h, int topMargin) {
		LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h);
		p.topMargin = topMargin;
		return p;
	}

	/* ================= motion: springs ================= */

	/** A damped spring as an interpolator (the M3 motion tokens are springs: damping ratio, stiffness). */
	static final class Spring implements TimeInterpolator {
		final double z, w;
		final long ms;

		Spring(double damping, double stiffness) {
			z = damping;
			w = Math.sqrt(stiffness);
			ms = Math.max(150, Math.min(1400, (long) (1000 * Math.log(500) / (z * w))));
		}

		@Override
		public float getInterpolation(float f) {
			if (f >= 1) {
				return 1;
			}
			double t = f * ms / 1000.0;
			if (z < 1) {
				double wd = w * Math.sqrt(1 - z * z);
				return (float) (1 - Math.exp(-z * w * t) * (Math.cos(wd * t) + z * w / wd * Math.sin(wd * t)));
			}
			return (float) (1 - Math.exp(-w * t) * (1 + w * t));
		}
	}

	/** Expressive spatial springs (they overshoot a little) and the effects spring (no overshoot). */
	static final Spring FAST = new Spring(0.6, 800), SPATIAL = new Spring(0.8, 380), SLOW = new Spring(0.8, 200),
	    EFFECTS = new Spring(1.0, 1600), MORPH = new Spring(0.6, 200);

	static ViewPropertyAnimator spring(View v, Spring s) {
		return v.animate().setInterpolator(s).setDuration(s.ms);
	}

	/* ================= shape ================= */

	/** "Fully rounded": half the short side. */
	static final float FULL = 100000f;

	/**
	 * A filled rounded shape whose four corners can morph (springs) and whose colour can fade: the
	 * background of every container, button and chip. Its outline follows it.
	 */
	static final class Shape extends Drawable {
		private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG);
		final float[] r = new float[4];             /* top-left, top-right, bottom-right, bottom-left (px, or FULL) */
		private final float[] radii = new float[8];
		private final Path path = new Path();
		private final RectF rect = new RectF();
		private ValueAnimator morph, tint;

		Shape(int color, float radiusPx) {
			fill.setColor(color);
			line.setStyle(Paint.Style.STROKE);
			java.util.Arrays.fill(r, radiusPx);
		}

		Shape stroke(int color, float widthPx) {
			line.setColor(color);
			line.setStrokeWidth(widthPx);
			invalidateSelf();
			return this;
		}

		int color() {
			return fill.getColor();
		}

		void setColor(int c) {
			if (tint != null) {
				tint.cancel();
			}
			fill.setColor(c);
			invalidateSelf();
		}

		/** Fades to colour TO (an effects spring). */
		void fadeTo(int to) {
			if (tint != null) {
				tint.cancel();
			}
			tint = ValueAnimator.ofArgb(fill.getColor(), to);
			tint.setDuration(EFFECTS.ms * 2);
			tint.setInterpolator(EFFECTS);
			tint.addUpdateListener(a -> {
				fill.setColor((int) a.getAnimatedValue());
				invalidateSelf();
			});
			tint.start();
		}

		Shape radii(float tl, float tr, float br, float bl) {
			if (morph != null) {
				morph.cancel();
			}
			r[0] = tl;
			r[1] = tr;
			r[2] = br;
			r[3] = bl;
			invalidateSelf();
			return this;
		}

		private float resolve(float v) {
			Rect b = getBounds();
			return Math.max(0, Math.min(v, Math.min(b.width(), b.height()) / 2f));
		}

		/** Morphs the corners to T (four values, or one for all) with spring S. */
		void morphTo(Spring s, float... t) {
			float[] to = t.length == 4 ? t.clone() : new float[]{t[0], t[0], t[0], t[0]};
			if (morph != null) {
				morph.cancel();
			}
			if (getBounds().isEmpty()) {
				radii(to[0], to[1], to[2], to[3]);
				return;
			}
			float[] from = {resolve(r[0]), resolve(r[1]), resolve(r[2]), resolve(r[3])};
			float[] end = {resolve(to[0]), resolve(to[1]), resolve(to[2]), resolve(to[3])};
			ValueAnimator m = ValueAnimator.ofFloat(0, 1);
			m.setDuration(s.ms);
			m.setInterpolator(s);
			m.addUpdateListener(a -> {
				float f = (float) a.getAnimatedValue();
				for (int i = 0; i < 4; i++) {
					r[i] = from[i] + (end[i] - from[i]) * f;
				}
				invalidateSelf();
			});
			m.addListener(new AnimatorListenerAdapter() {
				boolean cancelled;

				@Override
				public void onAnimationCancel(Animator a) {
					cancelled = true;
				}

				@Override
				public void onAnimationEnd(Animator a) {
					if (!cancelled) {
						System.arraycopy(to, 0, r, 0, 4);
						invalidateSelf();
					}
				}
			});
			morph = m;
			m.start();
		}

		private void build() {
			rect.set(getBounds());
			float sw = line.getStrokeWidth();
			rect.inset(sw / 2, sw / 2);
			float max = Math.max(0, Math.min(rect.width(), rect.height()) / 2);
			for (int i = 0; i < 4; i++) {
				radii[2 * i] = radii[2 * i + 1] = Math.min(Math.max(0, r[i]), max);
			}
			path.reset();
			path.addRoundRect(rect, radii, Path.Direction.CW);
		}

		@Override
		public void draw(Canvas c) {
			build();
			if ((fill.getColor() >>> 24) > 0) {
				c.drawPath(path, fill);
			}
			if (line.getStrokeWidth() > 0) {
				c.drawPath(path, line);
			}
		}

		@Override
		public void getOutline(Outline o) {
			build();
			if (radii[0] == radii[2] && radii[0] == radii[4] && radii[0] == radii[6]) {
				o.setRoundRect(getBounds(), radii[0]);
			} else {
				o.setPath(path);
			}
		}

		/** The same shape, opaque: the mask that keeps a ripple inside it. */
		Drawable mask() {
			Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
			p.setColor(0xFFFFFFFF);
			return new Plain() {
				@Override
				public void draw(Canvas c) {
					build();
					c.drawPath(path, p);
				}
			};
		}

		@Override
		public void setAlpha(int a) {
		}

		@Override
		public void setColorFilter(ColorFilter f) {
		}

		@Override
		public int getOpacity() {
			return PixelFormat.TRANSLUCENT;
		}
	}

	/** A Drawable with the boilerplate done. */
	abstract static class Plain extends Drawable {
		@Override
		public void setAlpha(int a) {
		}

		@Override
		public void setColorFilter(ColorFilter f) {
		}

		@Override
		public int getOpacity() {
			return PixelFormat.TRANSLUCENT;
		}
	}

	/** SHAPE with a state layer (ripple) in the content colour ON, kept inside the shape. */
	static Drawable ripple(Shape s, int on) {
		return new RippleDrawable(ColorStateList.valueOf(alpha(on, 0.16f)), s, s.mask());
	}

	/** V's shape S morphs to PRESSED while it is held, and springs back to REST (with a little bounce). */
	static void pressMorph(View v, Shape s, float[] rest, float[] pressed) {
		v.setOnTouchListener((view, e) -> {
			if (!view.isEnabled()) {
				return false;
			}
			int a = e.getActionMasked();
			if (a == MotionEvent.ACTION_DOWN) {
				s.morphTo(FAST, pressed);
			} else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) {
				s.morphTo(SPATIAL, rest);
			}
			return false;
		});
	}

	static float[] all(float v) {
		return new float[]{v, v, v, v};
	}

	/** A content container one tone above the page (inside sheets, empty states). */
	static Drawable surface(Context c, float radiusDp) {
		return new Shape(surfaceHigh, dp(c, radiusDp));
	}

	/* ================= icons ================= */

	/** A Material Symbol (res/drawable/m3_NAME.xml) tinted COLOR, SIZE dp square. */
	static Drawable symbol(Context c, String name, int color, float sizeDp) {
		int id = c.getResources().getIdentifier("m3_" + name, "drawable", c.getPackageName());
		Drawable d = c.getDrawable(id).mutate();
		d.setTint(color);
		d.setBounds(0, 0, dp(c, sizeDp), dp(c, sizeDp));
		return d;
	}

	static ImageView iconView(Context c, String name, int color, float sizeDp) {
		ImageView v = new ImageView(c);
		v.setImageDrawable(symbol(c, name, color, sizeDp));
		v.setScaleType(ImageView.ScaleType.CENTER);
		return v;
	}

	/** An icon on a tonal container of corner RADIUS dp (or FULL): the leading icon of a list row. */
	static View iconBadge(Context c, String name, int bg, int fg, float sizeDp, float radiusDp) {
		ImageView v = iconView(c, name, fg, sizeDp * 0.55f);
		v.setBackground(new Shape(bg, radiusDp >= FULL ? FULL : dp(c, radiusDp)));
		return v;
	}

	/* ================= buttons ================= */

	/** A button: a TextView that fades out when disabled. */
	static final class Button extends TextView {
		Button(Context c) {
			super(c);
		}

		@Override
		public void setEnabled(boolean on) {
			super.setEnabled(on);
			setAlpha(on ? 1f : 0.45f);
		}
	}

	/** A button. TINT: 0 tonal, TINT_IOS filled primary, TINT_WARN / TINT_DANGER / TINT_ANDROID their containers. */
	static TextView button(Context c, String text, int tint, View.OnClickListener l) {
		return button(c, text, null, tint, l);
	}

	/** A button with a leading Material Symbol ICON (null: none). */
	static TextView button(Context c, String text, String icon, int tint, View.OnClickListener l) {
		int bg, fg;
		if (tint == 0) {
			bg = secondaryContainer;
			fg = onSecondaryContainer;
		} else if (tint == TINT_IOS) {
			bg = primary;
			fg = onPrimary;
		} else if (tint == TINT_WARN) {
			bg = amberContainer;
			fg = onAmberContainer;
		} else if (tint == TINT_DANGER) {
			bg = errorContainer;
			fg = onErrorContainer;
		} else if (tint == TINT_ANDROID) {
			bg = greenContainer;
			fg = onGreenContainer;
		} else {
			bg = tint;
			fg = onSurface;
		}
		Button b = new Button(c);
		b.setTextColor(fg);
		b.setTextSize(16);
		b.setTypeface(font(600));
		b.setGravity(Gravity.CENTER);
		b.setMaxLines(2);
		b.setEllipsize(android.text.TextUtils.TruncateAt.END);
		b.setPadding(dp(c, 20), dp(c, 6), dp(c, 20), dp(c, 6));
		setLabel(b, text, icon, fg);
		Shape s = new Shape(bg, FULL);
		b.setBackground(ripple(s, fg));
		pressMorph(b, s, all(FULL), all(dp(c, 14)));
		b.setClickable(true);
		b.setOnClickListener(l);
		return b;
	}

	/** TEXT with ICON drawn inline before it (so a centred label keeps its icon next to it). */
	static void setLabel(TextView b, String text, String icon, int color) {
		if (icon == null) {
			b.setText(text);
			return;
		}
		SpannableString s = new SpannableString("   " + text);
		Drawable d = symbol(b.getContext(), icon, color, 20);
		s.setSpan(new ImageSpan(d, ImageSpan.ALIGN_CENTER), 0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
		b.setText(s);
	}

	static final int STANDARD = 0, TONAL = 1, FILLED = 2;

	/** An icon button (full round, squarer while pressed). STYLE: STANDARD, TONAL, FILLED. */
	static ImageView iconButton(Context c, String icon, int style, View.OnClickListener l) {
		int bg = style == FILLED ? primary : style == TONAL ? secondaryContainer : 0;
		int fg = style == FILLED ? onPrimary : style == TONAL ? onSecondaryContainer : onSurfaceVariant;
		ImageView v = new ImageView(c) {
			@Override
			public void setEnabled(boolean on) {
				super.setEnabled(on);
				setAlpha(on ? 1f : 0.38f);
			}
		};
		v.setImageDrawable(symbol(c, icon, fg, 24));
		v.setScaleType(ImageView.ScaleType.CENTER);
		Shape s = new Shape(bg, FULL);
		v.setBackground(ripple(s, fg));
		pressMorph(v, s, all(FULL), all(dp(c, 12)));
		v.setClickable(true);
		v.setOnClickListener(l);
		return v;
	}

	/**
	 * A button group: its buttons share the row, and the one pressed widens (15%) while its neighbours give
	 * way -- springs both ways. CONNECTED: 2dp apart (a segmented control), else 8dp.
	 */
	static final class Group extends LinearLayout {
		private ValueAnimator anim;

		Group(Context c, boolean connected) {
			super(c);
			setOrientation(HORIZONTAL);
			setGravity(Gravity.CENTER_VERTICAL);
			GradientDrawable gap = new GradientDrawable();
			gap.setColor(0);
			gap.setSize(dp(c, connected ? 2 : 8), 1);
			setDividerDrawable(gap);
			setShowDividers(SHOW_DIVIDER_MIDDLE);
		}

		@Override
		public boolean dispatchTouchEvent(MotionEvent e) {
			int a = e.getActionMasked();
			if (a == MotionEvent.ACTION_DOWN) {
				View hit = null;
				for (int i = 0; i < getChildCount(); i++) {
					View v = getChildAt(i);
					if (v.isEnabled() && e.getX() >= v.getLeft() && e.getX() < v.getRight()) {
						hit = v;
					}
				}
				if (hit != null) {
					weights(hit, FAST);
				}
			} else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) {
				weights(null, SPATIAL);
			}
			return super.dispatchTouchEvent(e);
		}

		private void weights(View pressed, Spring s) {
			int n = getChildCount();
			if (n < 2) {
				return;
			}
			float[] from = new float[n], to = new float[n];
			for (int i = 0; i < n; i++) {
				LayoutParams p = (LayoutParams) getChildAt(i).getLayoutParams();
				from[i] = p.weight;
				to[i] = pressed == null || p.weight == 0 ? p.weight == 0 ? 0 : 1f : getChildAt(i) == pressed ? 1.15f : 1f - 0.15f / (n - 1);
			}
			if (anim != null) {
				anim.cancel();
			}
			anim = ValueAnimator.ofFloat(0, 1);
			anim.setDuration(s.ms);
			anim.setInterpolator(s);
			anim.addUpdateListener(v -> {
				float f = (float) v.getAnimatedValue();
				for (int i = 0; i < n && i < getChildCount(); i++) {
					LayoutParams p = (LayoutParams) getChildAt(i).getLayoutParams();
					if (p.weight > 0) {
						p.weight = Math.max(0.01f, from[i] + (to[i] - from[i]) * f);
					}
				}
				requestLayout();
			});
			anim.start();
		}
	}

	/**
	 * A connected button group with one selected (the M3 Expressive segmented control): neighbours meet
	 * with small inner corners; the selected one fills with the primary colour and morphs to full round.
	 */
	static Group connected(Context c, String[] labels, int selected, java.util.function.IntConsumer l) {
		Group g = new Group(c, true);
		int n = labels.length;
		for (int i = 0; i < n; i++) {
			final int index = i;
			boolean on = i == selected;
			int bg = on ? primary : secondaryContainer, fg = on ? onPrimary : onSecondaryContainer;
			TextView t = text(c, labels[i], 14, fg, true);
			t.setGravity(Gravity.CENTER);
			t.setSingleLine(true);
			t.setEllipsize(android.text.TextUtils.TruncateAt.END);
			t.setPadding(dp(c, 6), 0, dp(c, 6), 0);
			float in = dp(c, 8);
			float[] rest = {i == 0 ? FULL : in, i == n - 1 ? FULL : in, i == n - 1 ? FULL : in, i == 0 ? FULL : in};
			Shape s = new Shape(bg, 0).radii(rest[0], rest[1], rest[2], rest[3]);
			t.setBackground(ripple(s, fg));
			float[] onRest = all(FULL);
			if (on) {
				t.post(() -> s.morphTo(MORPH, onRest));      /* the selection's shape change, as it appears */
			}
			pressMorph(t, s, on ? onRest : rest, all(dp(c, 6)));
			t.setClickable(true);
			t.setOnClickListener(v -> l.accept(index));
			g.addView(t, new LinearLayout.LayoutParams(0, dp(c, 48), 1));
		}
		return g;
	}

	/* ================= chips, fields, sliders ================= */

	/** A filter chip: outlined, or (selected) a tonal fill with a check. */
	static TextView pill(Context c, String text, boolean selected, View.OnClickListener l) {
		TextView t = text(c, text, 14, onSurfaceVariant, true);
		t.setGravity(Gravity.CENTER_VERTICAL);
		t.setMinHeight(dp(c, 36));
		t.setSingleLine(true);
		t.setCompoundDrawablePadding(dp(c, 6));
		setPillSelected(t, selected);
		t.setClickable(true);
		t.setOnClickListener(l);
		return t;
	}

	static void setPillSelected(TextView t, boolean on) {
		Context c = t.getContext();
		int fg = on ? onSecondaryContainer : onSurfaceVariant;
		Shape s = new Shape(on ? secondaryContainer : 0, dp(c, 10));
		if (!on) {
			s.stroke(outlineVariant, dp(c, 1));
		}
		t.setTextColor(fg);
		t.setBackground(ripple(s, fg));
		t.setCompoundDrawablesRelative(on ? symbol(c, "check", fg, 18) : null, null, null, null);
		t.setPadding(dp(c, on ? 10 : 16), 0, dp(c, 16), 0);
		pressMorph(t, s, all(dp(c, 10)), all(FULL));
	}

	/** A status chip (pending / installed / failed): the state colour's container. */
	static TextView chip(Context c, String text, int color) {
		int bg = surfaceHighest, fg = onSurfaceVariant;
		if (color == TINT_WARN) {
			bg = amberContainer;
			fg = onAmberContainer;
		} else if (color == TINT_ANDROID) {
			bg = greenContainer;
			fg = onGreenContainer;
		} else if (color == TINT_DANGER) {
			bg = errorContainer;
			fg = onErrorContainer;
		} else if (color == TINT_IOS) {
			bg = primaryContainer;
			fg = onPrimaryContainer;
		}
		TextView t = text(c, text, 12, fg, true);
		t.setPadding(dp(c, 12), dp(c, 6), dp(c, 12), dp(c, 6));
		t.setBackground(new Shape(bg, dp(c, 8)));
		return t;
	}

	/** A search bar: full-round, a tone above the page, with the search symbol. */
	static android.widget.EditText field(Context c, String hint) {
		android.widget.EditText e = new android.widget.EditText(c);
		e.setHint(hint);
		e.setHintTextColor(onSurfaceVariant);
		e.setTextColor(onSurface);
		e.setTextSize(16);
		e.setTypeface(font(400));
		e.setSingleLine(true);
		e.setMinHeight(dp(c, 56));
		e.setPadding(dp(c, 16), dp(c, 8), dp(c, 20), dp(c, 8));
		e.setBackground(new Shape(surfaceHigh, FULL));
		e.setCompoundDrawablesRelative(symbol(c, "search", onSurfaceVariant, 24), null, null, null);
		e.setCompoundDrawablePadding(dp(c, 12));
		e.setHighlightColor(alpha(primary, 0.35f));
		GradientDrawable cur = new GradientDrawable();
		cur.setColor(primary);
		cur.setSize(dp(c, 2), dp(c, 20));
		e.setTextCursorDrawable(cur);
		e.setImeOptions(android.view.inputmethod.EditorInfo.IME_ACTION_SEARCH);
		return e;
	}

	/** A slider in the M3 Expressive style: a thick rounded track, a slim handle with a gap on each side. */
	static void slider(SeekBar s) {
		Context c = s.getContext();
		Shape track = new Shape(secondaryContainer, FULL), active = new Shape(primary, FULL);
		android.graphics.drawable.LayerDrawable bar = new android.graphics.drawable.LayerDrawable(new Drawable[]{track,
			new android.graphics.drawable.ClipDrawable(active, Gravity.START, android.graphics.drawable.ClipDrawable.HORIZONTAL)});
		bar.setId(0, android.R.id.background);
		bar.setId(1, android.R.id.progress);
		s.setProgressDrawable(bar);
		GradientDrawable handle = new GradientDrawable();
		handle.setColor(primary);
		handle.setCornerRadius(dp(c, 2));
		android.graphics.drawable.LayerDrawable thumb = new android.graphics.drawable.LayerDrawable(new Drawable[]{handle});
		thumb.setLayerSize(0, dp(c, 4), dp(c, 44));
		thumb.setLayerInset(0, dp(c, 6), 0, dp(c, 6), 0);
		s.setThumb(thumb);
		s.setSplitTrack(true);                  /* the track is cut around the handle: M3's gap */
		s.setMinHeight(dp(c, 16));
		s.setMaxHeight(dp(c, 16));
		s.setPadding(dp(c, 8), dp(c, 14), dp(c, 8), dp(c, 14));
		s.setBackground(null);
	}

	/* ================= lists ================= */

	/** A tag key outside the framework's range (View.setTag(int, Object) only refuses framework ids). */
	private static final int SHAPE_KEY = 0x7f5a0001;

	/**
	 * A segmented list (Android 16's settings lists): each row its own container, 2dp apart, the group's
	 * outer corners large and the inner ones small; rows get their shape (and a ripple) as they are added.
	 */
	static final class ListGroup extends LinearLayout {
		private final int bg;

		ListGroup(Context c, int bg) {
			super(c);
			this.bg = bg;
			setOrientation(VERTICAL);
			GradientDrawable gap = new GradientDrawable();
			gap.setColor(0);
			gap.setSize(1, dp(c, 2));
			setDividerDrawable(gap);
			setShowDividers(SHOW_DIVIDER_MIDDLE);
		}

		@Override
		public void onViewAdded(View v) {
			super.onViewAdded(v);
			int i = indexOfChild(v);
			reshape(i - 1);
			reshape(i);
			reshape(i + 1);
		}

		@Override
		public void onViewRemoved(View v) {
			super.onViewRemoved(v);
			reshape(0);
			reshape(getChildCount() - 1);
		}

		private void reshape(int i) {
			if (i < 0 || i >= getChildCount()) {
				return;
			}
			View v = getChildAt(i);
			Object o = v.getTag(SHAPE_KEY);
			Shape s;
			if (o instanceof Shape) {
				s = (Shape) o;
			} else {
				s = new Shape(bg, 0);
				v.setTag(SHAPE_KEY, s);
				v.setBackground(ripple(s, onSurface));
			}
			float out = dp(getContext(), 24), in = dp(getContext(), 6);
			boolean first = i == 0, last = i == getChildCount() - 1;
			s.radii(first ? out : in, first ? out : in, last ? out : in, last ? out : in);
		}
	}

	/** A segmented list on the page (rows on surfaceContainer). */
	static ListGroup list(Context c) {
		return new ListGroup(c, surfaceContainer);
	}

	/** A segmented list inside a sheet (one tone up). */
	static ListGroup listHigh(Context c) {
		return new ListGroup(c, surfaceHigh);
	}

	/* ================= progress ================= */

	/**
	 * A usage / progress bar as M3 draws them now: the active part and the track as two capsules with a gap
	 * and a stop dot at the end. WAVY: the active part waves (the expressive progress indicator, for tasks).
	 */
	static final class Bar extends View {
		private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
		private final Path wave = new Path();
		float fraction;
		int color;
		boolean wavy;
		private float phase;
		private ValueAnimator anim;

		Bar(Context c, int color) {
			super(c);
			this.color = color;
		}

		private int trackColor() {
			return color == TINT_ANDROID ? greenContainer : color == TINT_WARN ? amberContainer : secondaryContainer;
		}

		@Override
		protected void onAttachedToWindow() {
			super.onAttachedToWindow();
			if (wavy) {
				anim = ValueAnimator.ofFloat(0, (float) (2 * Math.PI));
				anim.setDuration(1100);
				anim.setRepeatCount(ValueAnimator.INFINITE);
				anim.setInterpolator(null);
				anim.addUpdateListener(a -> {
					phase = (float) a.getAnimatedValue();
					invalidate();
				});
				anim.start();
			}
		}

		@Override
		protected void onDetachedFromWindow() {
			super.onDetachedFromWindow();
			if (anim != null) {
				anim.cancel();
				anim = null;
			}
		}

		@Override
		protected void onDraw(Canvas c) {
			float w = getWidth(), h = getHeight(), gap = dp(getContext(), 4);
			float f = Math.max(0, Math.min(1, fraction));
			p.setStyle(Paint.Style.FILL);
			if (!wavy) {
				float r = h / 2, fw = f <= 0 ? 0 : Math.max(h, w * f);
				if (fw + gap + h < w) {
					p.setColor(trackColor());
					c.drawRoundRect(fw == 0 ? 0 : fw + gap, 0, w, h, r, r, p);
					p.setColor(color);
					c.drawCircle(w - r, h / 2, Math.min(dp(getContext(), 2), r * 0.5f), p);
				} else {
					fw = w;
				}
				if (fw > 0) {
					p.setColor(color);
					c.drawRoundRect(0, 0, fw, h, r, r, p);
				}
				return;
			}
			float sw = dp(getContext(), 4), amp = dp(getContext(), 3), len = dp(getContext(), 40), cy = h / 2;
			float fw = w * f;
			p.setStyle(Paint.Style.STROKE);
			p.setStrokeCap(Paint.Cap.ROUND);
			p.setStrokeWidth(sw);
			if (fw + gap + sw < w) {
				p.setColor(trackColor());
				c.drawLine(fw <= 0 ? sw / 2 : fw + gap + sw, cy, w - sw / 2, cy, p);
				p.setStyle(Paint.Style.FILL);
				p.setColor(color);
				c.drawCircle(w - sw / 2, cy, sw / 2, p);
				p.setStyle(Paint.Style.STROKE);
			}
			if (fw > sw / 2) {
				wave.reset();
				boolean first = true;
				for (float x = sw / 2; x <= fw; x += 2) {
					float y = cy + amp * (float) Math.sin(2 * Math.PI * x / len - phase);
					if (first) {
						wave.moveTo(x, y);
						first = false;
					} else {
						wave.lineTo(x, y);
					}
				}
				p.setColor(color);
				c.drawPath(wave, p);
			}
		}
	}

	/* ---------------- the expressive loading indicator ---------------- */

	private static final int N = 120;
	private static float[][] shapes;

	/**
	 * The M3 shapes as radii at N angles: the loading indicator's sequence (soft burst, cookie 9, pentagon,
	 * pill, sunny, cookie 4, oval), then a 12-sided cookie used as decoration.
	 */
	private static synchronized float[][] shapes() {
		if (shapes == null) {
			shapes = new float[][]{cookie(10, 0.12f), cookie(9, 0.07f), polygon(5), superellipse(1f, 0.58f, 2.4f, 45),
				cookie(8, 0.05f), cookie(4, 0.11f), superellipse(1f, 0.78f, 2f, -30), cookie(12, 0.045f)};
		}
		return shapes;
	}

	static final int DECO_COOKIE12 = 7, DECO_BURST = 0, DECO_PENTAGON = 2;

	private static float[] cookie(int n, float a) {
		float[] r = new float[N];
		for (int k = 0; k < N; k++) {
			r[k] = 1 + a * (float) Math.cos(n * 2 * Math.PI * k / N);
		}
		return normalize(r);
	}

	private static float[] polygon(int n) {
		float[] r = new float[N];
		double seg = 2 * Math.PI / n;
		for (int k = 0; k < N; k++) {
			double t = 2 * Math.PI * k / N + Math.PI / 2;
			double m = ((t % seg) + seg) % seg - seg / 2;
			r[k] = (float) (Math.cos(Math.PI / n) / Math.cos(m));
		}
		return normalize(smooth(r, 4));
	}

	private static float[] superellipse(float a, float b, float pw, float rotDeg) {
		float[] r = new float[N];
		for (int k = 0; k < N; k++) {
			double t = 2 * Math.PI * k / N - Math.toRadians(rotDeg);
			double x = Math.abs(Math.cos(t)) / a, y = Math.abs(Math.sin(t)) / b;
			r[k] = (float) Math.pow(Math.pow(x, pw) + Math.pow(y, pw), -1 / pw);
		}
		return normalize(smooth(r, 2));
	}

	private static float[] smooth(float[] r, int passes) {
		for (int p = 0; p < passes; p++) {
			float[] o = new float[N];
			for (int k = 0; k < N; k++) {
				o[k] = (r[(k + N - 2) % N] + r[(k + N - 1) % N] + r[k] + r[(k + 1) % N] + r[(k + 2) % N]) / 5;
			}
			r = o;
		}
		return r;
	}

	private static float[] normalize(float[] r) {
		float m = 0;
		for (float v : r) {
			m = Math.max(m, v);
		}
		for (int k = 0; k < N; k++) {
			r[k] /= m;
		}
		return r;
	}

	/** Path P = the polar shape RADII around (CX, CY), radius R, turned ROT degrees. */
	static void polar(Path p, float[] radii, float cx, float cy, float r, float rot) {
		p.reset();
		double t0 = Math.toRadians(rot);
		for (int k = 0; k < N; k++) {
			double t = t0 + 2 * Math.PI * k / N;
			float x = cx + r * radii[k] * (float) Math.cos(t), y = cy + r * radii[k] * (float) Math.sin(t);
			if (k == 0) {
				p.moveTo(x, y);
			} else {
				p.lineTo(x, y);
			}
		}
		p.close();
	}

	/** The expressive loading indicator: a shape morphing through the set while it turns (a Drawable). */
	static final class Loading extends Drawable implements Animatable {
		private static final int STEP = 650;
		private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG), bg = new Paint(Paint.ANTI_ALIAS_FLAG);
		private final Path path = new Path();
		private final float[] cur = new float[N];
		private final boolean contained;
		private ValueAnimator anim;
		private float f;

		/** CONTAINED: on a primaryContainer circle (a loading indicator standing alone). */
		Loading(boolean contained) {
			this.contained = contained;
			p.setColor(contained ? onPrimaryContainer : primary);
			bg.setColor(primaryContainer);
		}

		@Override
		public void draw(Canvas c) {
			Rect b = getBounds();
			float cx = b.exactCenterX(), cy = b.exactCenterY(), size = Math.min(b.width(), b.height());
			if (contained) {
				c.drawCircle(cx, cy, size / 2, bg);
			}
			float[][] s = shapes();
			int n = s.length - 1;           /* the last one is decoration only */
			float pos = f * n;
			int i = Math.min(n - 1, (int) pos);
			float local = pos - i;
			float m = MORPH.getInterpolation(Math.min(1f, local * STEP / MORPH.ms));
			float[] a = s[i], z = s[(i + 1) % n];
			for (int k = 0; k < N; k++) {
				cur[k] = a[k] + (z[k] - a[k]) * m;
			}
			float rot = f * 450 + (i + m) * 90;     /* a whole number of turns per cycle: 7 x 90 + 450 = 1080 */
			polar(path, cur, cx, cy, size * (contained ? 0.34f : 0.47f), rot);
			c.drawPath(path, p);
		}

		@Override
		public boolean setVisible(boolean visible, boolean restart) {
			boolean changed = super.setVisible(visible, restart);
			if (visible) {
				start();
			} else {
				stop();
			}
			return changed;
		}

		@Override
		public void start() {
			if (anim != null) {
				return;
			}
			anim = ValueAnimator.ofFloat(0, 1);
			anim.setDuration((long) STEP * (shapes().length - 1));
			anim.setRepeatCount(ValueAnimator.INFINITE);
			anim.setInterpolator(null);
			anim.addUpdateListener(v -> {
				f = (float) v.getAnimatedValue();
				invalidateSelf();
			});
			anim.start();
		}

		@Override
		public void stop() {
			if (anim != null) {
				anim.cancel();
				anim = null;
			}
		}

		@Override
		public boolean isRunning() {
			return anim != null;
		}

		@Override
		public void setAlpha(int a) {
		}

		@Override
		public void setColorFilter(ColorFilter cf) {
		}

		@Override
		public int getOpacity() {
			return PixelFormat.TRANSLUCENT;
		}
	}

	/** A loading indicator view (lay it out at 48dp, the M3 size). */
	static ImageView loading(Context c, boolean contained) {
		ImageView v = new ImageView(c);
		v.setImageDrawable(new Loading(contained));
		return v;
	}

	/** A "reading…" line led by the loading indicator; the indicator goes when the text turns into an error ("⚠ …"). */
	static final class LoadingText extends TextView {
		private Loading spin;

		LoadingText(Context c, String s) {
			super(c);
			setTextSize(15);
			setTextColor(onSurfaceVariant);
			setTypeface(font(500));
			setGravity(Gravity.CENTER_VERTICAL);
			spin = new Loading(false);
			spin.setBounds(0, 0, dp(c, 28), dp(c, 28));
			setCompoundDrawablesRelative(spin, null, null, null);
			setCompoundDrawablePadding(dp(c, 14));
			setText(s);
		}

		@Override
		public void setText(CharSequence t, BufferType type) {
			super.setText(t, type);
			if (spin != null && t != null && t.toString().startsWith("⚠")) {
				spin.stop();
				spin = null;
				setCompoundDrawablesRelative(null, null, null, null);
				setTextColor(onSurface);
			}
		}
	}

	/** A large M3 shape as decoration (behind a hero card's content), turning into place with a spring. */
	static final class Deco extends View {
		private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
		private final Path path = new Path();
		private final int which;
		private float rot = -50;

		Deco(Context c, int color, int which) {
			super(c);
			this.which = which;
			p.setColor(color);
			ValueAnimator a = ValueAnimator.ofFloat(-50, 0);
			a.setStartDelay(100);
			a.setDuration(SLOW.ms);
			a.setInterpolator(SLOW);
			a.addUpdateListener(v -> {
				rot = (float) v.getAnimatedValue();
				invalidate();
			});
			a.start();
		}

		@Override
		protected void onDraw(Canvas c) {
			float s = Math.min(getWidth(), getHeight());
			polar(path, shapes()[which], getWidth() / 2f, getHeight() / 2f, s / 2, rot);
			c.drawPath(path, p);
		}
	}

	/* ================= dialogs ================= */

	/** Caps a view's height (a dialog's content scrolls inside it). */
	static final class MaxHeight extends FrameLayout {
		private final int max;

		MaxHeight(Context c, int max) {
			super(c);
			this.max = max;
		}

		@Override
		protected void onMeasure(int w, int h) {
			int size = MeasureSpec.getMode(h) == MeasureSpec.UNSPECIFIED ? max : Math.min(max, MeasureSpec.getSize(h));
			super.onMeasure(w, MeasureSpec.makeMeasureSpec(size, MeasureSpec.AT_MOST));
		}
	}

	/**
	 * An M3 dialog (28dp corners, surfaceContainerHigh, a headline, text buttons at the end) with the
	 * AlertDialog.Builder calls the app uses: title, message, a view, an item list, two buttons.
	 */
	static final class Dialog {
		private final Context c;
		private CharSequence title, message, pos, neg;
		private View view;
		private CharSequence[] items;
		private DialogInterface.OnClickListener itemsL, posL, negL;
		private boolean cancelable = true;

		Dialog(Context c) {
			this.c = c;
		}

		Dialog setTitle(CharSequence t) {
			title = t;
			return this;
		}

		Dialog setMessage(CharSequence m) {
			message = m;
			return this;
		}

		Dialog setView(View v) {
			view = v;
			return this;
		}

		Dialog setItems(CharSequence[] i, DialogInterface.OnClickListener l) {
			items = i;
			itemsL = l;
			return this;
		}

		Dialog setPositiveButton(CharSequence t, DialogInterface.OnClickListener l) {
			pos = t;
			posL = l;
			return this;
		}

		Dialog setNegativeButton(CharSequence t, DialogInterface.OnClickListener l) {
			neg = t;
			negL = l;
			return this;
		}

		Dialog setCancelable(boolean b) {
			cancelable = b;
			return this;
		}

		android.app.Dialog show() {
			android.app.Dialog d = new android.app.Dialog(c);
			d.requestWindowFeature(Window.FEATURE_NO_TITLE);
			LinearLayout p = column(c);
			p.setBackground(new Shape(surfaceHigh, dp(c, 28)));
			boolean buttons = pos != null || neg != null;
			p.setPadding(dp(c, 24), dp(c, 24), dp(c, 24), dp(c, buttons ? 16 : 24));
			int maxH = (int) (c.getResources().getDisplayMetrics().heightPixels * 0.6f);
			boolean any = false;
			if (title != null && title.length() > 0) {
				TextView t = text(c, title.toString(), 24, onSurface, true);
				t.setTypeface(displayFont(500));
				t.setMaxLines(3);
				t.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
				p.addView(t);
				any = true;
			}
			if (message != null) {
				ScrollView sv = new ScrollView(c);
				TextView m = text(c, message.toString(), 15, onSurfaceVariant, false);
				m.setLineSpacing(0, 1.2f);
				sv.addView(m);
				MaxHeight box = new MaxHeight(c, maxH);
				box.addView(sv);
				p.addView(box, lp(-1, -2, any ? dp(c, 16) : 0));
				any = true;
			}
			if (view != null) {
				MaxHeight box = new MaxHeight(c, maxH);
				box.addView(view);
				p.addView(box, lp(-1, -2, any ? dp(c, 16) : 0));
				any = true;
			}
			if (items != null) {
				ListGroup g = new ListGroup(c, surfaceHighest);
				for (int i = 0; i < items.length; i++) {
					final int which = i;
					TextView t = text(c, items[i].toString(), 16, onSurface, false);
					t.setGravity(Gravity.CENTER_VERTICAL);
					t.setMinHeight(dp(c, 56));
					t.setPadding(dp(c, 20), dp(c, 8), dp(c, 20), dp(c, 8));
					t.setClickable(true);
					t.setOnClickListener(v -> {
						d.dismiss();
						itemsL.onClick(d, which);
					});
					g.addView(t);
				}
				ScrollView sv = new ScrollView(c);
				sv.addView(g);
				MaxHeight box = new MaxHeight(c, maxH);
				box.addView(sv);
				p.addView(box, lp(-1, -2, any ? dp(c, 16) : 0));
			}
			if (buttons) {
				LinearLayout b = spaced(c, 8, false);
				b.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
				if (neg != null) {
					b.addView(textButton(d, neg, negL, DialogInterface.BUTTON_NEGATIVE), new LinearLayout.LayoutParams(-2, dp(c, 44)));
				}
				if (pos != null) {
					b.addView(textButton(d, pos, posL, DialogInterface.BUTTON_POSITIVE), new LinearLayout.LayoutParams(-2, dp(c, 44)));
				}
				p.addView(b, lp(-1, -2, dp(c, 20)));
			}
			d.setContentView(p);
			d.setCancelable(cancelable);
			Window w = d.getWindow();
			if (w != null) {
				w.setBackgroundDrawable(new ColorDrawable(0));
				w.setDimAmount(0.42f);
			}
			d.show();
			if (w != null) {
				int sw = c.getResources().getDisplayMetrics().widthPixels;
				w.setLayout(Math.min(sw - dp(c, 40), dp(c, 560)), ViewGroup.LayoutParams.WRAP_CONTENT);
			}
			p.setScaleX(0.86f);
			p.setScaleY(0.86f);
			p.setAlpha(0f);
			spring(p, SPATIAL).scaleX(1f).scaleY(1f).alpha(1f).start();
			return d;
		}

		private TextView textButton(android.app.Dialog d, CharSequence label, DialogInterface.OnClickListener l, int which) {
			TextView t = text(c, label.toString(), 15, primary, true);
			t.setGravity(Gravity.CENTER);
			t.setPadding(dp(c, 16), 0, dp(c, 16), 0);
			Shape s = new Shape(0, FULL);
			t.setBackground(ripple(s, primary));
			pressMorph(t, s, all(FULL), all(dp(c, 10)));
			t.setOnClickListener(v -> {
				if (l != null) {
					l.onClick(d, which);
				}
				d.dismiss();
			});
			return t;
		}
	}

	/* ================= iOS icons, sizes ================= */

	/** An iOS icon (PNG, Apple's crushed CgBI ones included), or null when it cannot be decoded. */
	static android.graphics.Bitmap icon(byte[] png) {
		if (png == null) {
			return null;
		}
		if (Png.isCgBI(png)) {
			int[] wh = new int[2];
			int[] px = Png.decodeCgBI(png, wh);
			return px == null ? null : android.graphics.Bitmap.createBitmap(px, wh[0], wh[1], android.graphics.Bitmap.Config.ARGB_8888);
		}
		return android.graphics.BitmapFactory.decodeByteArray(png, 0, png.length);
	}

	/** An app icon view: the bitmap with iOS's rounded corners, or a tonal tile with the name's initial. */
	static View appIcon(Context c, android.graphics.Bitmap bm, String name, int sizeDp) {
		int r = dp(c, sizeDp * 0.225f);
		if (bm != null) {
			ImageView iv = new ImageView(c);
			iv.setImageBitmap(bm);
			iv.setScaleType(ImageView.ScaleType.CENTER_CROP);
			iv.setClipToOutline(true);
			iv.setOutlineProvider(new android.view.ViewOutlineProvider() {
				@Override
				public void getOutline(View v, Outline o) {
					o.setRoundRect(0, 0, v.getWidth(), v.getHeight(), r);
				}
			});
			return iv;
		}
		TextView t = text(c, name == null || name.isEmpty() ? "?" : name.substring(0, 1).toUpperCase(java.util.Locale.ROOT),
		    sizeDp * 0.42f, onTertiaryContainer, true);
		t.setGravity(Gravity.CENTER);
		t.setBackground(new Shape(tertiaryContainer, r));
		return t;
	}

	static String size(long bytes) {
		if (bytes < 1024) {
			return bytes + " B";
		}
		String[] u = {"KB", "MB", "GB", "TB"};
		double v = bytes;
		int i = -1;
		do {
			v /= 1024;
			i++;
		} while (v >= 1024 && i < u.length - 1);
		return String.format(java.util.Locale.ROOT, v >= 100 ? "%.0f %s" : "%.1f %s", v, u[i]);
	}
}
