package com.sapphire.dualboot;

import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.text.Editable;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.TextWatcher;
import android.text.style.BackgroundColorSpan;
import android.text.style.ForegroundColorSpan;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.regex.Pattern;

/**
 * iOS's kernel log (the console ring at the end of iosroot, Ios.log): filtered by text or to the lines that
 * matter (panics, errors, failures), panics on red, a jump to the first panic, and shared or copied whole.
 */
final class LogViewer {
	static final Pattern PANIC = Pattern.compile("(?i)panic|PÁNICO|double fault|kernel trap|Debugger message");
	static final Pattern PROBLEM = Pattern.compile("(?i)panic|PÁNICO|error|fail|fatal|assert|exception|denied|timeout|timed out|"
	    + "watchdog|EXC_|abort|crash|unable|cannot|can't|invalid");
	private static final int MAX_LINES = 3000;

	private LogViewer() {
	}

	static void open(MainActivity a) {
		a.task(L.t("Leyendo el registro", "Reading the log"), false, t -> {
			t.status(L.t("Leyendo el final de iosroot…", "Reading the end of iosroot…"));
			String log = Ios.log();
			a.save("ios-kernel-log", log.getBytes(StandardCharsets.UTF_8));
			return (Runnable) () -> show(a, log);
		});
	}

	private static void show(MainActivity a, String log) {
		String[] lines = log.split("\n");
		LinearLayout col = Ui.column(a);
		EditText q = Ui.field(a, L.t("Filtrar líneas", "Filter lines"));
		col.addView(q, Ui.lp(-1, -2, a.dp(4)));
		LinearLayout pills = Ui.chipRow(a);
		boolean[] onlyProblems = {false};
		TextView all = Ui.pill(a, L.t("Todo", "All"), true, null), probs = Ui.pill(a, L.t("Problemas", "Problems"), false, null);
		pills.addView(all);
		pills.addView(probs);
		pills.addView(new android.view.View(a), new LinearLayout.LayoutParams(0, 1, 1));
		TextView jump = Ui.pill(a, L.t("Ir al pánico", "Go to panic"), false, null);
		pills.addView(jump);
		col.addView(pills, Ui.lp(-1, -2, a.dp(10)));
		TextView count = Ui.text(a, "", 13, Ui.onSurfaceVariant, false);
		col.addView(count, Ui.lp(-1, -2, a.dp(6)));

		ScrollView sv = new ScrollView(a);
		HorizontalScrollView hs = new HorizontalScrollView(a);
		TextView text = new TextView(a);
		text.setTypeface(Typeface.MONOSPACE);
		text.setTextSize(10.5f);
		text.setTextColor(Ui.onSurfaceVariant);
		text.setHighlightColor(Ui.alpha(Ui.primary, 0.35f));
		text.setTextIsSelectable(true);
		text.setPadding(a.dp(14), a.dp(12), a.dp(14), a.dp(12));
		hs.addView(text);
		sv.addView(hs);
		sv.setBackground(Ui.surface(a, 20));
		sv.setClipToOutline(true);
		LinearLayout.LayoutParams svp = new LinearLayout.LayoutParams(-1, 0, 1);
		svp.topMargin = a.dp(8);
		col.addView(sv, svp);

		LinearLayout buttons = new Ui.Group(a, false);
		buttons.addView(Ui.button(a, L.t("Compartir", "Share"), "share", Ui.TINT_IOS, v -> share(a, log)), new LinearLayout.LayoutParams(0, a.dp(56), 1));
		buttons.addView(Ui.button(a, L.t("Copiar", "Copy"), "copy", 0, v -> {
			android.content.ClipboardManager cm = a.getSystemService(android.content.ClipboardManager.class);
			/* the clipboard goes through a binder transaction: the end of the log, where the latest boot is */
			String clip = log.length() > 400000 ? log.substring(log.length() - 400000) : log;
			cm.setPrimaryClip(android.content.ClipData.newPlainText(L.t("Registro de iOS", "iOS log"), clip));
			android.widget.Toast.makeText(a, L.t("Copiado", "Copied"), android.widget.Toast.LENGTH_SHORT).show();
		}), new LinearLayout.LayoutParams(0, a.dp(56), 1));
		col.addView(buttons, Ui.lp(-1, -2, a.dp(12)));

		int[] firstPanic = {-1};
		Runnable render = () -> {
			String f = q.getText().toString().toLowerCase(Locale.ROOT);
			List<String> keep = new ArrayList<>();
			int panics = 0;
			for (String l : lines) {
				if (!f.isEmpty() && !l.toLowerCase(Locale.ROOT).contains(f)) {
					continue;
				}
				if (onlyProblems[0] && !PROBLEM.matcher(l).find()) {
					continue;
				}
				keep.add(l);
			}
			int from = Math.max(0, keep.size() - MAX_LINES);
			SpannableStringBuilder s = new SpannableStringBuilder();
			if (from > 0) {
				s.append("… ").append(String.valueOf(from)).append(L.t(" líneas anteriores no mostradas (filtra para verlas)\n", " earlier lines not shown (filter to see them)\n"));
			}
			firstPanic[0] = -1;
			for (int i = from; i < keep.size(); i++) {
				String l = keep.get(i);
				int start = s.length();
				s.append(l).append('\n');
				if (PANIC.matcher(l).find()) {
					panics++;
					if (firstPanic[0] < 0) {
						firstPanic[0] = start;
					}
					s.setSpan(new BackgroundColorSpan(Ui.errorContainer), start, s.length() - 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
					s.setSpan(new ForegroundColorSpan(Ui.onErrorContainer), start, s.length() - 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
				} else if (PROBLEM.matcher(l).find()) {
					s.setSpan(new ForegroundColorSpan(Ui.TINT_WARN), start, s.length() - 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
				}
			}
			text.setText(s);
			count.setText(keep.size() + L.t(" de ", " of ") + lines.length + L.t(" líneas", " lines") + (panics > 0 ? " · " + panics + L.t(" con pánico", " with panic") : ""));
			jump.setAlpha(firstPanic[0] >= 0 ? 1f : 0.4f);
			sv.post(() -> sv.fullScroll(ScrollView.FOCUS_DOWN));
		};
		all.setOnClickListener(v -> {
			onlyProblems[0] = false;
			Ui.setPillSelected(all, true);
			Ui.setPillSelected(probs, false);
			render.run();
		});
		probs.setOnClickListener(v -> {
			onlyProblems[0] = true;
			Ui.setPillSelected(probs, true);
			Ui.setPillSelected(all, false);
			render.run();
		});
		jump.setOnClickListener(v -> {
			if (firstPanic[0] >= 0 && text.getLayout() != null) {
				int line = text.getLayout().getLineForOffset(firstPanic[0]);
				sv.smoothScrollTo(0, Math.max(0, text.getLayout().getLineTop(line) - a.dp(40)));
			}
		});
		q.addTextChangedListener(new TextWatcher() {
			@Override
			public void beforeTextChanged(CharSequence s, int st, int c, int af) {
			}

			@Override
			public void onTextChanged(CharSequence s, int st, int b, int c) {
			}

			@Override
			public void afterTextChanged(Editable e) {
				render.run();
			}
		});
		a.sheet(L.t("Registro del kernel de iOS", "iOS kernel log"), col, false);
		render.run();
	}

	/** The whole log as a .txt in Download/SapphireDualBoot, handed to the share sheet. */
	private static void share(MainActivity a, String log) {
		try {
			String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
			Uri uri = a.newDownload("SapphireDualBoot", "ios-kernel-log-" + stamp + ".txt");
			try (OutputStream o = a.getContentResolver().openOutputStream(uri)) {
				o.write(log.getBytes(StandardCharsets.UTF_8));
			}
			Intent i = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_STREAM, uri)
			    .putExtra(Intent.EXTRA_SUBJECT, L.t("Registro del kernel de iOS (sapphire)", "iOS kernel log (sapphire)")).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
			a.startActivity(Intent.createChooser(i, L.t("Compartir el registro", "Share the log")));
		} catch (Exception e) {
			a.textDialog("", "Error: " + e.getMessage());
		}
	}
}
