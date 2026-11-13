// Ui.cs: the look of Sapphire Installer -- Material 3 Expressive, painted by hand in WinForms (C# 5, no extra DLLs).
// Same family as the Android app (android/dualboot, res-extra/values*/themes.xml): seed primary #475D92 (light) /
// #B0C6FF (dark), surfaces #FAF8FF / #121318, and the tonal scheme of that seed (Theme below). Light or dark follows
// Windows' app theme (AppsUseLightTheme); SAPPHIRE_THEME=light|dark overrides it (testing, --shots).
//
// Building a page (Main.cs) with these pieces -- every size given to Add() is a 96-dpi pixel, Add() scales it;
// inside a component use Theme.S()/Theme.Sf():
//
//   GlassPage p = NewPage(L.T("Título", "Title"), L.T("Subtítulo", "Subtitle"));   the content card + headline
//   p.Cards.Add(new RectangleF(S(18), S(124), S(696), S(120)));    an inner tile (surface container) under controls
//   Add(p, new Txt(text, 11, FontStyle.Regular, Theme.Text), 34, 130, 660, 60);   body (Theme.Dim: secondary text)
//   Add(p, new Txt(text, 12, FontStyle.Bold, Theme.Text), ...);                     a section title
//   Pill b = Add(p, new Pill(L.T("Elegir…", "Choose…"), false), 546, 150, 152, 44); a pill button
//       b.Kind = Pill.Filled | Pill.Tonal | Pill.Outlined | Pill.TextOnly;  b.Tint = Theme.Danger (destructive)
//       b.Icon = Icons.Folder (Segoe Fluent Icons glyph), b.IconAfter = true for a trailing icon
//       connected button group: a.JoinRight = true; b.JoinLeft = true (inner corners small, outer round), 3 px apart
//   Add(p, new Note(text, Theme.Danger), 18, 290, 696, 80);    a tonal banner: Danger / Warn / Android (ok) / Accent
//   Add(p, new M3Check(), ...);  Add(p, new M3Radio(), ...);    checkbox / radio (use as CheckBox / RadioButton)
//   Add(p, new Bar(), 34, 380, 660, 12);                        wavy expressive progress: Fraction 0..1, Tint;
//                                                                Fraction <= 0 is indeterminate unless ZeroIsIndeterminate=false
//   Add(p, new Loader(), 46, 160, 64, 64);                      expressive loading indicator; Done = true -> check
//   Anim.Run(func) runs func every frame (~60 Hz) until it returns false; Spring is a damped spring for it.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace Sapphire
{
	static class Theme
	{
		public static readonly bool Dark = IsDark();

		static bool IsDark()
		{
			string o = Environment.GetEnvironmentVariable("SAPPHIRE_THEME");
			if (o == "dark" || o == "light") {
				return o == "dark";
			}
			try {
				object v = Microsoft.Win32.Registry.GetValue(@"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize", "AppsUseLightTheme", 1);
				return v is int && (int)v == 0;
			} catch {
				return false;
			}
		}

		static Color P(int light, int dark)
		{
			int c = Dark ? dark : light;
			return Color.FromArgb(255, (c >> 16) & 255, (c >> 8) & 255, c & 255);
		}

		/* ---- colour roles: the M3 tonal scheme of the seed (tone 40 / 80 primary) ---- */
		public static readonly Color Primary = P(0x475D92, 0xB0C6FF), OnPrimary = P(0xFFFFFF, 0x152E60);
		public static readonly Color PrimaryContainer = P(0xDAE2FF, 0x2F4578), OnPrimaryContainer = P(0x2F4578, 0xDAE2FF);
		public static readonly Color Secondary = P(0x575E71, 0xBFC6DC), SecondaryContainer = P(0xDCE2F9, 0x3F4759), OnSecondaryContainer = P(0x3F4759, 0xDCE2F9);
		public static readonly Color Tertiary = P(0x725572, 0xE0BBDD), TertiaryContainer = P(0xFDD7FA, 0x593D59), OnTertiaryContainer = P(0x593D59, 0xFDD7FA);
		public static readonly Color Error = P(0xBA1A1A, 0xFFB4AB), OnError = P(0xFFFFFF, 0x690005);
		public static readonly Color ErrorContainer = P(0xFFDAD6, 0x93000A), OnErrorContainer = P(0x93000A, 0xFFDAD6);
		public static readonly Color Surface = P(0xFAF8FF, 0x121318), OnSurface = P(0x1A1B21, 0xE3E2E9), OnSurfaceVariant = P(0x44464F, 0xC5C6D0);
		public static readonly Color Outline = P(0x757780, 0x8F9099), OutlineVariant = P(0xC5C6D0, 0x44464F);
		public static readonly Color ContainerLowest = P(0xFFFFFF, 0x0D0E13), ContainerLow = P(0xF4F3FA, 0x1A1B21), Container = P(0xEEEDF4, 0x1E1F25);
		public static readonly Color ContainerHigh = P(0xE8E7EF, 0x292A2F), ContainerHighest = P(0xE3E2E9, 0x34343A);
		/* custom colours harmonised with the seed: success (green, "Android") and warning (amber) */
		public static readonly Color Success = P(0x3B6939, 0xA1D39A), OnSuccess = P(0xFFFFFF, 0x0A390F);
		public static readonly Color SuccessContainer = P(0xBCF0B4, 0x23501F), OnSuccessContainer = P(0x23501F, 0xBCF0B4);
		public static readonly Color Warning = P(0x795900, 0xEFBF66), WarnContainer = P(0xFFDEA6, 0x5C4200), OnWarnContainer = P(0x5C4200, 0xFFDEA6);

		/* where things sit: the window, the content card on it, tiles inside the card */
		public static readonly Color Window = Dark ? Surface : Container;
		public static readonly Color Card = Dark ? Container : ContainerLowest;
		public static readonly Color Tile = Dark ? ContainerHigh : Container;

		/* the names Main.cs has always used */
		public static readonly Color Top = Window, Text = OnSurface, Dim = OnSurfaceVariant, Accent = Primary;
		public static readonly Color Android = Success, Danger = Error, Warn = Warning;
		public static readonly Color Glass = Tile, GlassEdge = OutlineVariant;

		/* ---- shape scale (dp): extra-small 4, small 8, medium 12, large 16, large-increased 20, extra-large 28,
		   extra-large-increased 32, extra-extra-large 48, full = height / 2 ---- */
		public const float ShapeS = 8, ShapeM = 12, ShapeL = 16, ShapeLI = 20, ShapeXL = 28, ShapeXXL = 48;

		/* ---- DPI ---- */
		public static float K = InitK();

		static float InitK()
		{
			try {
				using (Graphics g = Graphics.FromHwnd(IntPtr.Zero)) {
					return g.DpiX / 96f;
				}
			} catch {
				return 1f;
			}
		}

		public static int S(float v) { return (int)Math.Round(v * K); }
		public static float Sf(float v) { return v * K; }

		/* ---- type: Segoe UI Variable (Display for >= 14 pt, Text below), Segoe UI otherwise. Emphasized = Bold ---- */
		static HashSet<string> families;

		static string Pick(string a, string b)
		{
			if (families == null) {
				families = new HashSet<string>();
				using (InstalledFontCollection ifc = new InstalledFontCollection()) {
					foreach (FontFamily f in ifc.Families) {
						families.Add(f.Name);
					}
				}
			}
			return families.Contains(a) ? a : families.Contains(b) ? b : "Segoe UI";
		}

		public static Font F(float size, FontStyle style)
		{
			string fam = size >= 14 ? Pick("Segoe UI Variable Display", "Segoe UI") : Pick("Segoe UI Variable Text", "Segoe UI");
			return new Font(fam, size, style, GraphicsUnit.Point);
		}

		/* semibold: labels, buttons, titles */
		public static Font Semi(float size)
		{
			string fam = size >= 14 ? Pick("Segoe UI Variable Display Semib", "Segoe UI Semibold") : Pick("Segoe UI Variable Text Semibold", "Segoe UI Semibold");
			return new Font(fam, size, FontStyle.Regular, GraphicsUnit.Point);
		}

		public static Font Mono(float size)
		{
			return new Font(Pick("Cascadia Mono", "Consolas"), size, FontStyle.Regular, GraphicsUnit.Point);
		}

		public static Font IconFont(float size)
		{
			string f = Pick("Segoe Fluent Icons", "Segoe MDL2 Assets");
			return f == "Segoe UI" ? null : new Font(f, size, FontStyle.Regular, GraphicsUnit.Point);
		}

		/* ---- colour helpers ---- */
		public static Color Mix(Color top, Color under, float a)
		{
			a = Math.Max(0, Math.Min(1, a));
			return Color.FromArgb((int)(under.R + (top.R - under.R) * a), (int)(under.G + (top.G - under.G) * a), (int)(under.B + (top.B - under.B) * a));
		}

		public static Color A(Color c, float a) { return Color.FromArgb((int)(Math.Max(0, Math.Min(1, a)) * 255), c); }

		static bool Same(Color a, Color b) { return a.ToArgb() == b.ToArgb(); }

		/* the content colour for a filled colour role */
		public static Color On(Color c)
		{
			if (Same(c, Primary)) return OnPrimary;
			if (Same(c, Error)) return OnError;
			if (Same(c, Success)) return OnSuccess;
			if (Same(c, Warning)) return Dark ? Color.FromArgb(0x41, 0x2D, 0x00) : Color.White;
			return c.GetBrightness() > 0.6f ? OnSurface : Color.White;
		}

		/* a role's container and on-container: Danger / Warn / Android / Accent; anything else -> neutral tile */
		public static void Tones(Color c, out Color bg, out Color fg)
		{
			if (Same(c, Error)) { bg = ErrorContainer; fg = OnErrorContainer; }
			else if (Same(c, Warning)) { bg = WarnContainer; fg = OnWarnContainer; }
			else if (Same(c, Success)) { bg = SuccessContainer; fg = OnSuccessContainer; }
			else if (Same(c, Primary)) { bg = PrimaryContainer; fg = OnPrimaryContainer; }
			else { bg = Tile; fg = OnSurface; }
		}

		/* what is painted under a control: the card inside a page, the window outside */
		public static Color Under(Control c)
		{
			for (Control p = c.Parent; p != null; p = p.Parent) {
				if (p is GlassPage) return Card;
			}
			return Window;
		}

		/* ---- shapes ---- */
		public static GraphicsPath Round(RectangleF r, float rad)
		{
			return Round(r, rad, rad, rad, rad);
		}

		public static GraphicsPath Round(RectangleF r, float tl, float tr, float br, float bl)
		{
			GraphicsPath p = new GraphicsPath();
			float m = Math.Min(r.Width, r.Height) / 2;
			tl = Math.Min(tl, m); tr = Math.Min(tr, m); br = Math.Min(br, m); bl = Math.Min(bl, m);
			if (tl > 0.1f) p.AddArc(r.X, r.Y, tl * 2, tl * 2, 180, 90); else p.AddLine(r.X, r.Y, r.X, r.Y);
			if (tr > 0.1f) p.AddArc(r.Right - tr * 2, r.Y, tr * 2, tr * 2, 270, 90); else p.AddLine(r.Right, r.Y, r.Right, r.Y);
			if (br > 0.1f) p.AddArc(r.Right - br * 2, r.Bottom - br * 2, br * 2, br * 2, 0, 90); else p.AddLine(r.Right, r.Bottom, r.Right, r.Bottom);
			if (bl > 0.1f) p.AddArc(r.X, r.Bottom - bl * 2, bl * 2, bl * 2, 90, 90); else p.AddLine(r.X, r.Bottom, r.X, r.Bottom);
			p.CloseFigure();
			return p;
		}

		/* M3 Expressive's shape library, as polar shapes: n lobes of the given depth ("cookie", "soft burst", "clover"...),
		   morphing from (n1, d1) to (n2, d2) as t goes 0 -> 1; rot turns it (radians) */
		public static GraphicsPath Polar(RectangleF r, int n1, float d1, int n2, float d2, float t, float rot)
		{
			const int N = 160;
			PointF[] pts = new PointF[N];
			float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, R = Math.Min(r.Width, r.Height) / 2;
			for (int i = 0; i < N; i++) {
				double a = 2 * Math.PI * i / N;
				double f1 = 1 - d1 * (1 - Math.Cos(n1 * (a - rot))) / 2;
				double f2 = 1 - d2 * (1 - Math.Cos(n2 * (a - rot))) / 2;
				double f = f1 + (f2 - f1) * t;
				pts[i] = new PointF((float)(cx + R * f * Math.Cos(a)), (float)(cy + R * f * Math.Sin(a)));
			}
			GraphicsPath p = new GraphicsPath();
			p.AddPolygon(pts);
			return p;
		}

		public static GraphicsPath Cookie(RectangleF r, int lobes, float depth)
		{
			return Polar(r, lobes, depth, lobes, depth, 0, (float)(-Math.PI / 2));
		}

		/* an image filled into a shape, with smooth (anti-aliased) edges */
		public static void FillImage(Graphics g, Image img, GraphicsPath shape, RectangleF r)
		{
			g.SmoothingMode = SmoothingMode.AntiAlias;
			using (TextureBrush tb = new TextureBrush(img, WrapMode.Clamp)) {
				tb.TranslateTransform(r.X, r.Y);
				tb.ScaleTransform(r.Width / img.Width, r.Height / img.Height);
				g.FillPath(tb, shape);
			}
		}

		public static void Fill(Graphics g, GraphicsPath p, Color c)
		{
			using (SolidBrush b = new SolidBrush(c)) {
				g.FillPath(b, p);
			}
		}

		/* a tonal card (kept for older callers: GlassCard used to draw glass) */
		public static void GlassCard(Graphics g, RectangleF r, float rad, Color tint)
		{
			g.SmoothingMode = SmoothingMode.AntiAlias;
			using (GraphicsPath p = Round(r, rad)) {
				Fill(g, p, tint);
			}
		}

		/* ---- the window frame: dark title bar in dark mode, caption in the window's colour (Windows 11) ---- */
		[DllImport("dwmapi.dll")]
		static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

		[DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
		static extern int SetWindowTheme(IntPtr hwnd, string app, string idList);

		static int Ref(Color c) { return c.R | (c.G << 8) | (c.B << 16); }

		public static void StyleWindow(Form f)
		{
			try {
				int d = Dark ? 1 : 0;
				DwmSetWindowAttribute(f.Handle, 20, ref d, 4);       /* DWMWA_USE_IMMERSIVE_DARK_MODE */
				int cap = Ref(Window), txt = Ref(OnSurface);
				DwmSetWindowAttribute(f.Handle, 35, ref cap, 4);     /* DWMWA_CAPTION_COLOR (11+) */
				DwmSetWindowAttribute(f.Handle, 36, ref txt, 4);     /* DWMWA_TEXT_COLOR */
			} catch {
			}
		}

		/* dark scroll bars on a TextBox and friends */
		public static void StyleScroll(Control c)
		{
			if (!Dark) return;
			try {
				SetWindowTheme(c.Handle, "DarkMode_Explorer", null);
			} catch {
			}
		}
	}

	/* Segoe Fluent Icons glyphs (same code points in Segoe MDL2 Assets) */
	static class Icons
	{
		public const string Forward = "", Back = "", Download = "", Check = "", Folder = "",
		    File = "", Phone = "", Refresh = "", Play = "", Close = "", Warning = "";
	}

	/* ---- motion: one shared ~60 Hz timer, and a damped spring ---- */
	static class Anim
	{
		static Timer timer;
		static readonly List<Func<bool>> steps = new List<Func<bool>>();

		/* Off (--shots): every animation jumps to its end at once */
		public static bool Off;
		static bool sync;

		public static void Run(Func<bool> step)
		{
			if (Off) {
				if (sync) return;
				sync = true;
				try {
					for (int i = 0; i < 400 && step(); i++) {
					}
				} catch {
				} finally {
					sync = false;
				}
				return;
			}
			if (!steps.Contains(step)) {
				steps.Add(step);
			}
			if (timer == null) {
				timer = new Timer();
				timer.Interval = 15;
				timer.Tick += Tick;
			}
			timer.Start();
		}

		static void Tick(object sender, EventArgs e)
		{
			foreach (Func<bool> f in steps.ToArray()) {
				bool more;
				try {
					more = f();
				} catch {
					more = false;
				}
				if (!more) {
					steps.Remove(f);
				}
			}
			if (steps.Count == 0) {
				timer.Stop();
			}
		}
	}

	/* stiffness / damping as M3's spring tokens: fast spatial ~ (1400, 0.9 ratio), default spatial ~ (700, 0.9),
	   expressive (bouncy) ~ (380, 0.6 ratio) */
	class Spring
	{
		public float X, V, Target;
		readonly float k, d;

		public Spring(float x, float stiffness, float dampingRatio)
		{
			X = Target = x;
			k = stiffness;
			d = 2 * dampingRatio * (float)Math.Sqrt(stiffness);
		}

		public bool Step()
		{
			const float dt = 1 / 60f;
			for (int i = 0; i < 2; i++) {
				float a = k * (Target - X) - d * V;
				V += a * dt / 2;
				X += V * dt / 2;
			}
			if (Math.Abs(Target - X) < 0.002f && Math.Abs(V) < 0.02f) {
				X = Target;
				V = 0;
				return false;
			}
			return true;
		}
	}

	/* the window's background: the window colour (the sidebar is painted on it by Main) */
	class Backdrop : Panel
	{
		public Backdrop()
		{
			DoubleBuffered = true;
			SetStyle(ControlStyles.ResizeRedraw | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
		}

		protected override void OnPaintBackground(PaintEventArgs e)
		{
			using (SolidBrush b = new SolidBrush(Theme.Window)) {
				e.Graphics.FillRectangle(b, ClientRectangle);
			}
		}
	}

	/* a page: Cards[0] is the content card (extra-large corners), later Cards are tiles inside it */
	class GlassPage : Panel
	{
		public List<RectangleF> Cards = new List<RectangleF>();

		public GlassPage()
		{
			SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.ResizeRedraw, true);
			BackColor = Color.Transparent;
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			for (int i = 0; i < Cards.Count; i++) {
				using (GraphicsPath p = Theme.Round(Cards[i], Theme.Sf(i == 0 ? Theme.ShapeXL : Theme.ShapeLI))) {
					Theme.Fill(g, p, i == 0 ? Theme.Card : Theme.Tile);
				}
			}
			base.OnPaint(e);
		}
	}

	/* text on the card, wrapping to its width */
	class Txt : Label
	{
		public Txt(string text, float size, FontStyle style, Color color)
		{
			Text = text;
			Font = Theme.F(size, style);
			ForeColor = color;
			BackColor = Color.Transparent;
			AutoSize = false;
			UseMnemonic = false;
		}
	}

	/* a tonal banner: its colour from ForeColor (Theme.Danger, Warn, Android, Accent), as tall as its text */
	class Note : Txt
	{
		public Note(string text, Color tone) : base(text, 10.5f, FontStyle.Regular, tone)
		{
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			if (string.IsNullOrEmpty(Text)) {
				return;
			}
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			Color bg, fg;
			Theme.Tones(ForeColor, out bg, out fg);
			int px = Theme.S(18), py = Theme.S(12);
			TextFormatFlags tf = TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix | TextFormatFlags.Left | TextFormatFlags.Top;
			Size ts = TextRenderer.MeasureText(g, Text, Font, new Size(Width - 2 * px, 0), tf);
			float h = Math.Min(Height - 1, ts.Height + 2 * py);
			using (GraphicsPath p = Theme.Round(new RectangleF(0.5f, 0.5f, Width - 1, h), Theme.Sf(Theme.ShapeL))) {
				Theme.Fill(g, p, bg);
			}
			TextRenderer.DrawText(g, Text, Font, new Rectangle(px, py, Width - 2 * px, (int)h - py), fg, tf);
		}
	}

	/* a pill button: filled / tonal / outlined / text; springs into a squarer shape when pressed */
	class Pill : Control
	{
		public const int Filled = 0, Tonal = 1, Outlined = 2, TextOnly = 3;
		public int Kind;
		public Color Tint = Theme.Accent;
		public string Icon;
		public bool IconAfter, JoinLeft, JoinRight;
		readonly Spring hover = new Spring(0, 1400, 1f), press = new Spring(0, 380, 0.55f);
		Font iconFont;

		public bool Primary { get { return Kind == Filled; } set { Kind = value ? Filled : Tonal; } }

		public Pill(string text, bool primary)
		{
			Text = text;
			Primary = primary;
			Font = Theme.Semi(11);
			iconFont = Theme.IconFont(12);
			Cursor = Cursors.Hand;
			SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
			BackColor = Color.Transparent;
			Size = new Size(150, 40);
		}

		void Go() { Anim.Run(StepAnim); }

		bool StepAnim()
		{
			if (IsDisposed) return false;
			bool a = hover.Step(), b = press.Step();
			Invalidate();
			return a || b;
		}

		protected override void OnMouseEnter(EventArgs e) { hover.Target = 1; Go(); base.OnMouseEnter(e); }
		protected override void OnMouseLeave(EventArgs e) { hover.Target = 0; press.Target = 0; Go(); base.OnMouseLeave(e); }
		protected override void OnMouseDown(MouseEventArgs e) { if (Enabled) { press.Target = 1; Go(); } base.OnMouseDown(e); }
		protected override void OnMouseUp(MouseEventArgs e) { press.Target = 0; Go(); base.OnMouseUp(e); }
		protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
		protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }

		protected override void Dispose(bool disposing)
		{
			if (disposing && iconFont != null) {
				iconFont.Dispose();
			}
			base.Dispose(disposing);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			RectangleF r = new RectangleF(0.5f, 0.5f, Width - 1.5f, Height - 1.5f);
			float pr = Math.Max(-0.2f, Math.Min(1.3f, press.X));
			float full = r.Height / 2, outer = full - (full - Theme.Sf(Theme.ShapeM)) * pr * 0.75f;
			float inner = Theme.Sf(Theme.ShapeS) * (1 - 0.5f * Math.Max(0, pr));
			float l = JoinLeft ? inner : outer, rr = JoinRight ? inner : outer;
			Color under = Theme.Under(this), fill = Color.Empty, fg, border = Color.Empty;
			bool danger = Tint.ToArgb() == Theme.Danger.ToArgb();
			switch (Kind) {
			case Filled: fill = Tint; fg = Theme.On(Tint); break;
			case Tonal: fill = danger ? Theme.ErrorContainer : Theme.SecondaryContainer; fg = danger ? Theme.OnErrorContainer : Theme.OnSecondaryContainer; break;
			case Outlined: border = Theme.OutlineVariant; fg = danger ? Theme.Error : Theme.OnSurfaceVariant; break;
			default: fg = Tint; break;
			}
			if (!Enabled) {
				fill = Kind == Filled || Kind == Tonal ? Theme.Mix(Theme.OnSurface, under, 0.10f) : Color.Empty;
				border = Kind == Outlined ? Theme.Mix(Theme.OnSurface, under, 0.12f) : Color.Empty;
				fg = Theme.Mix(Theme.OnSurface, fill.IsEmpty ? under : fill, 0.38f);
			}
			using (GraphicsPath p = Theme.Round(r, l, rr, rr, l)) {
				if (!fill.IsEmpty) {
					Theme.Fill(g, p, fill);
				}
				float st = Enabled ? hover.X * 0.08f + Math.Max(0, press.X) * 0.10f : 0;
				if (st > 0.005f) {
					Theme.Fill(g, p, Theme.A(fg, st));
				}
				if (!border.IsEmpty) {
					using (Pen pen = new Pen(border, Math.Max(1, Theme.Sf(1)))) {
						g.DrawPath(pen, p);
					}
				}
			}
			TextFormatFlags tf = TextFormatFlags.NoPadding | TextFormatFlags.SingleLine | TextFormatFlags.NoPrefix | TextFormatFlags.VerticalCenter | TextFormatFlags.Left;
			Size ts = TextRenderer.MeasureText(g, Text, Font, new Size(10000, Height), tf);
			bool icon = !string.IsNullOrEmpty(Icon) && iconFont != null;
			Size isz = icon ? TextRenderer.MeasureText(g, Icon, iconFont, new Size(1000, Height), tf) : Size.Empty;
			int gap = icon ? Theme.S(8) : 0, total = ts.Width + isz.Width + gap;
			if (total > Width - Theme.S(16)) {
				icon = false;
				total = ts.Width;
				gap = 0;
				isz = Size.Empty;
			}
			int x = (Width - total) / 2;
			int tx = icon && !IconAfter ? x + isz.Width + gap : x;
			int ix = IconAfter ? x + ts.Width + gap : x;
			TextRenderer.DrawText(g, Text, Font, new Rectangle(tx, 0, ts.Width + 2, Height), fg, tf);
			if (icon) {
				TextRenderer.DrawText(g, Icon, iconFont, new Rectangle(ix, 1, isz.Width + 2, Height), fg, tf);
			}
		}
	}

	/* the sidebar's stepper: done = filled circle with a check, current = pill indicator (springs between steps) */
	class Steps : Control
	{
		public string[] Names = new string[0];
		public int Current;
		readonly Spring ind = new Spring(0, 380, 0.7f);
		bool placed;
		Font font, bold, small;

		public Steps()
		{
			SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
			BackColor = Color.Transparent;
			Font = font = Theme.F(10.5f, FontStyle.Regular);
			bold = Theme.Semi(10.5f);
			small = Theme.Semi(9);
		}

		public float Row { get { return Theme.Sf(52); } }

		bool StepAnim()
		{
			if (IsDisposed) return false;
			bool more = ind.Step();
			Invalidate();
			return more;
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			if (!placed || !Visible) {
				ind.X = ind.Target = Current;
				placed = true;
			} else if (ind.Target != Current) {
				ind.Target = Current;
				Anim.Run(StepAnim);
			}
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			float row = Row, dot = Theme.Sf(30), cx = Theme.Sf(12) + dot / 2;
			/* the active indicator */
			RectangleF ir = new RectangleF(0, ind.X * row + Theme.Sf(4), Width - 1, row - Theme.Sf(8));
			using (GraphicsPath p = Theme.Round(ir, ir.Height / 2)) {
				Theme.Fill(g, p, Theme.SecondaryContainer);
			}
			for (int i = 0; i < Names.Length; i++) {
				float cy = i * row + row / 2;
				if (i + 1 < Names.Length) {
					using (Pen pen = new Pen(i < Current ? Theme.Primary : Theme.OutlineVariant, Theme.Sf(2))) {
						pen.StartCap = pen.EndCap = LineCap.Round;
						g.DrawLine(pen, cx, cy + dot / 2 + Theme.Sf(5), cx, cy + row - dot / 2 - Theme.Sf(5));
					}
				}
				RectangleF d = new RectangleF(cx - dot / 2, cy - dot / 2, dot, dot);
				Color ink;
				if (i < Current) {
					using (GraphicsPath p = Theme.Cookie(d, 8, 0.10f)) {
						Theme.Fill(g, p, Theme.Primary);
					}
					ink = Theme.OnPrimary;
					DrawCheck(g, d, ink);
				} else if (i == Current) {
					using (SolidBrush b = new SolidBrush(Theme.Primary)) {
						g.FillEllipse(b, d);
					}
					TextRenderer.DrawText(g, (i + 1).ToString(), small, Rectangle.Round(d), Theme.OnPrimary, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
				} else {
					using (Pen pen = new Pen(Theme.Outline, Theme.Sf(1.5f))) {
						g.DrawEllipse(pen, d.X + 1, d.Y + 1, d.Width - 2, d.Height - 2);
					}
					TextRenderer.DrawText(g, (i + 1).ToString(), small, Rectangle.Round(d), Theme.OnSurfaceVariant, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
				}
				Color tc = i == Current ? Theme.OnSecondaryContainer : i < Current ? Theme.OnSurface : Theme.OnSurfaceVariant;
				TextRenderer.DrawText(g, Names[i], i == Current ? bold : font, new Rectangle((int)(cx + dot / 2 + Theme.Sf(12)), (int)(cy - row / 2), Width, (int)row), tc,
				    TextFormatFlags.SingleLine | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.EndEllipsis);
			}
		}

		public static void DrawCheck(Graphics g, RectangleF d, Color c)
		{
			using (Pen pen = new Pen(c, Math.Max(1.5f, d.Width * 0.09f))) {
				pen.StartCap = pen.EndCap = LineCap.Round;
				pen.LineJoin = LineJoin.Round;
				g.DrawLines(pen, new PointF[] {
					new PointF(d.X + d.Width * 0.29f, d.Y + d.Height * 0.52f),
					new PointF(d.X + d.Width * 0.44f, d.Y + d.Height * 0.66f),
					new PointF(d.X + d.Width * 0.72f, d.Y + d.Height * 0.36f) });
			}
		}
	}

	/* the space split (an expressive slider): iOS track (left), Android track (right), the handle between with gaps */
	class SplitBar : Control
	{
		public long Total, Min1, Min2, Value, Original;  /* bytes; Value = iOS's share */
		public event EventHandler Changed;
		bool drag;
		readonly Spring grip = new Spring(0, 1400, 0.9f);
		Font font;

		public SplitBar()
		{
			SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
			BackColor = Color.Transparent;
			Cursor = Cursors.SizeWE;
			Height = 70;
			font = Theme.Semi(10.5f);
		}

		RectangleF Bar() { return new RectangleF(Theme.Sf(4), Theme.Sf(13), Width - Theme.Sf(8), Height - Theme.Sf(26)); }

		float X(long v) { RectangleF b = Bar(); return Total > 0 ? b.X + b.Width * (float)((double)v / Total) : b.X; }

		void SetFromX(int x)
		{
			RectangleF b = Bar();
			double f = Math.Max(0, Math.Min(1, (x - b.X) / b.Width));
			long gib = 1L << 30;
			long v = (long)Math.Round(f * Total / gib) * gib;
			if (Math.Abs(v - Original) < gib / 2) {
				v = Original;                   /* snaps back to the current split */
			}
			v = Math.Max(Min1, Math.Min(Total - Min2, v));
			if (v != Value) {
				Value = v;
				Invalidate();
				if (Changed != null) {
					Changed(this, EventArgs.Empty);
				}
			}
		}

		bool StepAnim()
		{
			if (IsDisposed) return false;
			bool more = grip.Step();
			Invalidate();
			return more;
		}

		protected override void OnMouseDown(MouseEventArgs e) { drag = true; grip.Target = 1; Anim.Run(StepAnim); SetFromX(e.X); }
		protected override void OnMouseMove(MouseEventArgs e) { if (drag) SetFromX(e.X); }
		protected override void OnMouseUp(MouseEventArgs e) { drag = false; grip.Target = 0; Anim.Run(StepAnim); }

		protected override void OnPaint(PaintEventArgs e)
		{
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			RectangleF b = Bar();
			float x = X(Value);
			float hw = Theme.Sf(4) - Theme.Sf(2) * grip.X, gap = Theme.Sf(6), outer = Theme.Sf(Theme.ShapeL), inner = Theme.Sf(4);
			RectangleF left = new RectangleF(b.X, b.Y, Math.Max(0, x - hw / 2 - gap - b.X), b.Height);
			RectangleF right = new RectangleF(x + hw / 2 + gap, b.Y, Math.Max(0, b.Right - (x + hw / 2 + gap)), b.Height);
			if (left.Width > 1) {
				using (GraphicsPath p = Theme.Round(left, outer, inner, inner, outer)) {
					Theme.Fill(g, p, Theme.Primary);
				}
			}
			if (right.Width > 1) {
				using (GraphicsPath p = Theme.Round(right, inner, outer, outer, inner)) {
					Theme.Fill(g, p, Theme.SuccessContainer);
				}
			}
			if (Original > 0 && Original != Value) {
				/* where the split is now: a stop dot on the track */
				float ox = X(Original), ds = Theme.Sf(6);
				Color dc = ox < x ? Theme.OnPrimary : Theme.OnSuccessContainer;
				using (SolidBrush sb = new SolidBrush(dc)) {
					g.FillEllipse(sb, ox - ds / 2, b.Y + b.Height / 2 - ds / 2, ds, ds);
				}
			}
			RectangleF knob = new RectangleF(x - hw / 2, b.Y - Theme.Sf(8), hw, b.Height + Theme.Sf(16));
			using (GraphicsPath p = Theme.Round(knob, hw / 2)) {
				Theme.Fill(g, p, Theme.Primary);
			}
			TextFormatFlags tf = TextFormatFlags.SingleLine | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding;
			Size s1 = TextRenderer.MeasureText("iOS", font), s2 = TextRenderer.MeasureText("Android", font);
			if (left.Width > s1.Width + Theme.Sf(24)) {
				TextRenderer.DrawText(g, "iOS", font, new Rectangle((int)(left.X + Theme.Sf(16)), (int)b.Y, s1.Width + 4, (int)b.Height), Theme.OnPrimary, tf);
			}
			if (right.Width > s2.Width + Theme.Sf(24)) {
				TextRenderer.DrawText(g, "Android", font, new Rectangle((int)(right.Right - Theme.Sf(16) - s2.Width), (int)b.Y, s2.Width + 4, (int)b.Height), Theme.OnSuccessContainer, tf);
			}
		}
	}

	/* the expressive linear progress indicator: a wavy active track, a gap, the flat track, a stop dot.
	   Fraction <= 0 sweeps (indeterminate) unless ZeroIsIndeterminate is false. */
	class Bar : Control
	{
		public double Fraction;
		public Color Tint = Theme.Accent;
		public bool ZeroIsIndeterminate = true;
		float shown = -1, phase, sweep;

		public Bar()
		{
			SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
			BackColor = Color.Transparent;
			Height = 10;
		}

		bool Indeterminate { get { return ZeroIsIndeterminate && Fraction <= 0; } }

		protected override void OnInvalidated(InvalidateEventArgs e)
		{
			base.OnInvalidated(e);
			if (Visible && IsHandleCreated) {
				Anim.Run(StepAnim);
			}
		}

		protected override void OnVisibleChanged(EventArgs e)
		{
			base.OnVisibleChanged(e);
			Invalidate();
		}

		bool StepAnim()
		{
			if (IsDisposed || !Visible || !IsHandleCreated) return false;
			float target = (float)Math.Max(0, Math.Min(1, Fraction));
			shown = shown < 0 ? target : shown + (target - shown) * 0.12f;
			phase += 0.11f;
			sweep = (sweep + 0.009f) % 1f;
			Invalidate();
			return true;
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			float H = Height, stroke = Math.Min(Theme.Sf(4), H * 0.4f), amp = Math.Max(0, (H - stroke) / 2 - 0.5f);
			float cy = H / 2, x0 = stroke / 2 + 0.5f, x1 = Width - stroke / 2 - 0.5f, gap = Theme.Sf(4) + stroke;
			float wave = Theme.Sf(30);
			Color track, tint = Tint, dummy;
			Theme.Tones(Tint, out track, out dummy);
			if (track.ToArgb() == Theme.Tile.ToArgb()) {
				track = Theme.SecondaryContainer;
			}
			using (Pen act = new Pen(tint, stroke), trk = new Pen(track, stroke)) {
				act.StartCap = act.EndCap = trk.StartCap = trk.EndCap = LineCap.Round;
				act.LineJoin = LineJoin.Round;
				if (Indeterminate) {
					/* a wavy segment sweeping across the track */
					float t = sweep * 1.5f - 0.25f;
					float hs = x0 + (x1 - x0) * Math.Max(0, Math.Min(1, t + 0.25f)), ts = x0 + (x1 - x0) * Math.Max(0, Math.Min(1, t - 0.15f));
					if (ts > x0 + gap) g.DrawLine(trk, x0, cy, ts - gap, cy);
					if (hs + gap < x1) g.DrawLine(trk, hs + gap, cy, x1, cy);
					Wave(g, act, ts, hs, cy, amp, wave);
					return;
				}
				float f = Math.Max(0, shown);
				float xe = x0 + (x1 - x0) * f;
				float a = amp * Math.Max(0, Math.Min(1, Math.Min(f, 1 - f) * 12));    /* flat at 0 and at 100 % */
				if (xe + gap < x1) {
					g.DrawLine(trk, Math.Max(x0, xe + gap), cy, x1, cy);
					using (SolidBrush sb = new SolidBrush(tint)) {
						g.FillEllipse(sb, x1 - stroke / 2, cy - stroke / 2, stroke, stroke);
					}
				}
				if (xe > x0 + 0.5f) {
					Wave(g, act, x0, xe, cy, a, wave);
				} else if (!ZeroIsIndeterminate) {
					using (SolidBrush sb = new SolidBrush(tint)) {
						g.FillEllipse(sb, x0 - stroke / 2, cy - stroke / 2, stroke, stroke);
					}
				}
			}
		}

		void Wave(Graphics g, Pen pen, float xa, float xb, float cy, float a, float wl)
		{
			if (xb - xa < 1) {
				return;
			}
			int n = Math.Max(2, (int)((xb - xa) / 1.5f) + 1);
			PointF[] pts = new PointF[n];
			for (int i = 0; i < n; i++) {
				float x = xa + (xb - xa) * i / (n - 1);
				pts[i] = new PointF(x, cy + a * (float)Math.Sin(2 * Math.PI * x / wl - phase));
			}
			g.DrawLines(pen, pts);
		}
	}

	/* the expressive loading indicator: a shape morphing through M3's shape library, turning, inside a container
	   circle. Done = true settles it into a filled cookie with a check. */
	class Loader : Control
	{
		public bool Done;
		static readonly int[] Lobes = { 9, 4, 7, 2, 5, 12 };
		static readonly float[] Depth = { 0.12f, 0.22f, 0.16f, 0.26f, 0.20f, 0.08f };
		int idx;
		float t, rot, done = -1;

		public Loader()
		{
			SetStyle(ControlStyles.SupportsTransparentBackColor | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
			BackColor = Color.Transparent;
		}

		protected override void OnInvalidated(InvalidateEventArgs e)
		{
			base.OnInvalidated(e);
			if (Visible && IsHandleCreated) {
				Anim.Run(StepAnim);
			}
		}

		protected override void OnVisibleChanged(EventArgs e) { base.OnVisibleChanged(e); Invalidate(); }

		bool StepAnim()
		{
			if (IsDisposed || !Visible || !IsHandleCreated) return false;
			if (done < 0) {
				done = Done ? 1 : 0;
			}
			if (Done) {
				done = Math.Min(1, done + 0.06f);
			} else {
				done = Math.Max(0, done - 0.06f);
			}
			t += 1 / 40f;
			if (t >= 1) {
				t = 0;
				idx = (idx + 1) % Lobes.Length;
			}
			rot += 0.035f + 0.09f * (float)Math.Sin(Math.PI * t);     /* turns faster while morphing */
			Invalidate();
			return !(Done && done >= 1);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			float s = Math.Min(Width, Height) - 1;
			RectangleF r = new RectangleF((Width - s) / 2, (Height - s) / 2, s, s);
			float dn = done < 0 ? (Done ? 1 : 0) : done;
			float ease = 1 - (1 - dn) * (1 - dn);
			if (dn < 1) {
				using (SolidBrush b = new SolidBrush(Theme.Mix(Theme.PrimaryContainer, Theme.Under(this), 1 - ease))) {
					g.FillEllipse(b, r);
				}
			}
			float k = 0.68f + 0.32f * ease;
			RectangleF ir = new RectangleF(r.X + r.Width * (1 - k) / 2, r.Y + r.Height * (1 - k) / 2, r.Width * k, r.Height * k);
			int n = idx, m = (idx + 1) % Lobes.Length;
			float te = t < 0.5f ? 4 * t * t * t : 1 - (float)Math.Pow(-2 * t + 2, 3) / 2;
			float d1 = Depth[n] + (0.10f - Depth[n]) * ease, d2 = Depth[m] + (0.10f - Depth[m]) * ease;
			using (GraphicsPath p = Theme.Polar(ir, ease > 0.5f ? 9 : Lobes[n], d1, ease > 0.5f ? 9 : Lobes[m], d2, te * (1 - ease), rot * (1 - ease))) {
				Theme.Fill(g, p, Theme.Primary);
			}
			if (ease > 0.3f) {
				Steps.DrawCheck(g, ir, Theme.Mix(Theme.OnPrimary, Theme.Primary, (ease - 0.3f) / 0.7f));
			}
		}
	}

	/* M3 checkbox */
	class M3Check : CheckBox
	{
		bool hover, down, init;
		readonly Spring pop = new Spring(0, 700, 0.5f);

		public M3Check()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.SupportsTransparentBackColor | ControlStyles.ResizeRedraw, true);
			BackColor = Color.Transparent;
			ForeColor = Theme.Text;
			Font = Theme.F(10.5f, FontStyle.Regular);
			Cursor = Cursors.Hand;
		}

		bool StepAnim() { if (IsDisposed) return false; bool m = pop.Step(); Invalidate(); return m; }

		protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
		protected override void OnMouseLeave(EventArgs e) { hover = down = false; Invalidate(); base.OnMouseLeave(e); }
		protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
		protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }

		protected override void OnCheckedChanged(EventArgs e)
		{
			if (init && Checked) {
				pop.X = 0.4f;
				pop.Target = 1;
				Anim.Run(StepAnim);
			}
			base.OnCheckedChanged(e);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			if (!init) {
				pop.X = pop.Target = 1;
				init = true;
			}
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			bool err = ForeColor.ToArgb() == Theme.Danger.ToArgb();
			Color on = err ? Theme.Error : Theme.Primary, off = err ? Theme.Error : Theme.OnSurfaceVariant;
			float box = Theme.Sf(18), cx = Theme.Sf(20), cy = Height / 2f;
			M3Radio.StateLayer(g, this, cx, cy, Checked ? on : Theme.OnSurface, hover, down, Focused && ShowFocusCues);
			RectangleF br = new RectangleF(cx - box / 2, cy - box / 2, box, box);
			if (Checked) {
				using (GraphicsPath p = Theme.Round(br, Theme.Sf(3))) {
					Theme.Fill(g, p, Enabled ? on : Theme.Mix(Theme.OnSurface, Theme.Under(this), 0.38f));
				}
				float k = Math.Max(0.2f, pop.X);
				RectangleF cr = new RectangleF(cx - box * k / 2, cy - box * k / 2, box * k, box * k);
				Steps.DrawCheck(g, new RectangleF(cr.X - cr.Width * 0.1f, cr.Y - cr.Height * 0.1f, cr.Width * 1.2f, cr.Height * 1.2f), err ? Theme.OnError : Theme.OnPrimary);
			} else {
				float w = Theme.Sf(2);
				using (GraphicsPath p = Theme.Round(new RectangleF(br.X + w / 2, br.Y + w / 2, box - w, box - w), Theme.Sf(2)))
				using (Pen pen = new Pen(Enabled ? off : Theme.Mix(Theme.OnSurface, Theme.Under(this), 0.38f), w)) {
					g.DrawPath(pen, p);
				}
			}
			M3Radio.Label(g, this);
		}
	}

	/* M3 radio button */
	class M3Radio : RadioButton
	{
		bool hover, down, init;
		readonly Spring dot = new Spring(0, 700, 0.5f);

		public M3Radio()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.SupportsTransparentBackColor | ControlStyles.ResizeRedraw, true);
			BackColor = Color.Transparent;
			ForeColor = Theme.Text;
			Font = Theme.F(10.5f, FontStyle.Regular);
			Cursor = Cursors.Hand;
		}

		bool StepAnim() { if (IsDisposed) return false; bool m = dot.Step(); Invalidate(); return m; }

		protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
		protected override void OnMouseLeave(EventArgs e) { hover = down = false; Invalidate(); base.OnMouseLeave(e); }
		protected override void OnMouseDown(MouseEventArgs e) { down = true; Invalidate(); base.OnMouseDown(e); }
		protected override void OnMouseUp(MouseEventArgs e) { down = false; Invalidate(); base.OnMouseUp(e); }

		protected override void OnCheckedChanged(EventArgs e)
		{
			dot.Target = Checked ? 1 : 0;
			if (init) {
				Anim.Run(StepAnim);
			} else {
				dot.X = dot.Target;
			}
			base.OnCheckedChanged(e);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			if (!init) {
				dot.X = dot.Target = Checked ? 1 : 0;
				init = true;
			}
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			float size = Theme.Sf(20), w = Theme.Sf(2), cx = Theme.Sf(20), cy = Height / 2f;
			Color c = !Enabled ? Theme.Mix(Theme.OnSurface, Theme.Under(this), 0.38f) : Checked ? Theme.Primary : Theme.OnSurfaceVariant;
			StateLayer(g, this, cx, cy, Checked ? Theme.Primary : Theme.OnSurface, hover, down, Focused && ShowFocusCues);
			using (Pen pen = new Pen(c, w)) {
				g.DrawEllipse(pen, cx - size / 2 + w / 2, cy - size / 2 + w / 2, size - w, size - w);
			}
			float d = Theme.Sf(10) * Math.Max(0, dot.X);
			if (d > 0.5f) {
				using (SolidBrush b = new SolidBrush(c)) {
					g.FillEllipse(b, cx - d / 2, cy - d / 2, d, d);
				}
			}
			Label(g, this);
		}

		internal static void StateLayer(Graphics g, ButtonBase c, float cx, float cy, Color col, bool hover, bool down, bool focus)
		{
			float a = !c.Enabled ? 0 : down ? 0.12f : hover || focus ? 0.08f : 0;
			if (a <= 0) return;
			float sl = Math.Min(c.Height - 1, Theme.Sf(36));
			using (SolidBrush b = new SolidBrush(Theme.A(col, a))) {
				g.FillEllipse(b, cx - sl / 2, cy - sl / 2, sl, sl);
			}
		}

		internal static void Label(Graphics g, ButtonBase c)
		{
			Color tc = c.Enabled ? c.ForeColor : Theme.Mix(Theme.OnSurface, Theme.Under(c), 0.38f);
			TextRenderer.DrawText(g, c.Text, c.Font, new Rectangle(Theme.S(42), 0, c.Width - Theme.S(42), c.Height), tc,
			    TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix);
		}
	}

	/* the page transition (M3 shared axis X): the old page slides out and fades, the new one slides in.
	   Works on two snapshots, so the real controls are never moved. */
	class Fader : Control
	{
		static Fader cur;
		Bitmap a, b;
		int dir, frame;
		const int Frames = 20;

		Fader()
		{
			SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.Opaque, true);
		}

		public static Bitmap Snap(Control host, Rectangle r)
		{
			Finish();
			try {
				using (Bitmap full = new Bitmap(host.Width, host.Height)) {
					host.DrawToBitmap(full, new Rectangle(0, 0, full.Width, full.Height));
					return full.Clone(Rectangle.Intersect(r, new Rectangle(0, 0, full.Width, full.Height)), full.PixelFormat);
				}
			} catch {
				return null;
			}
		}

		public static void Run(Control host, Rectangle r, Bitmap before, Bitmap after, int dir)
		{
			Finish();
			if (before == null || after == null) {
				if (before != null) before.Dispose();
				if (after != null) after.Dispose();
				return;
			}
			Fader f = new Fader();
			f.a = before;
			f.b = after;
			f.dir = dir;
			f.Bounds = r;
			host.Controls.Add(f);
			f.BringToFront();
			cur = f;
			Anim.Run(f.StepAnim);
		}

		public static void Finish()
		{
			if (cur != null) {
				Fader f = cur;
				cur = null;
				if (f.Parent != null) {
					f.Parent.Controls.Remove(f);
				}
				f.Dispose();
			}
		}

		bool StepAnim()
		{
			if (IsDisposed) return false;
			if (++frame >= Frames) {
				if (cur == this) Finish();
				return false;
			}
			Invalidate();
			return true;
		}

		protected override void Dispose(bool disposing)
		{
			if (disposing) {
				if (a != null) a.Dispose();
				if (b != null) b.Dispose();
				a = b = null;
			}
			base.Dispose(disposing);
		}

		protected override void OnPaint(PaintEventArgs e)
		{
			Graphics g = e.Graphics;
			using (SolidBrush bg = new SolidBrush(Theme.Window)) {
				g.FillRectangle(bg, ClientRectangle);
			}
			float t = (float)frame / Frames, ease = 1 - (float)Math.Pow(1 - t, 4);
			float dist = Theme.Sf(40);
			Draw(g, a, 1 - Math.Min(1, t / 0.35f), -dir * dist * ease);
			Draw(g, b, Math.Max(0, Math.Min(1, (t - 0.2f) / 0.55f)), dir * dist * (1 - ease));
		}

		static void Draw(Graphics g, Bitmap bmp, float alpha, float dx)
		{
			if (bmp == null || alpha <= 0.01f) return;
			ColorMatrix cm = new ColorMatrix();
			cm.Matrix33 = alpha;
			using (ImageAttributes ia = new ImageAttributes()) {
				ia.SetColorMatrix(cm);
				g.DrawImage(bmp, new Rectangle((int)dx, 0, bmp.Width, bmp.Height), 0, 0, bmp.Width, bmp.Height, GraphicsUnit.Pixel, ia);
			}
		}
	}
}
