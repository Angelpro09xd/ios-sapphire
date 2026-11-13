// Main.cs: Sapphire Installer's window -- a wizard: Bienvenida, Teléfono, iOS y kernel, Espacio, Confirmar,
// Instalación. Developer option: SapphireInstaller.exe --local FOLDER (release files from a folder).
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Sapphire
{
	static class Program
	{
		[DllImport("user32.dll")]
		static extern bool SetProcessDPIAware();

		[STAThread]
		static void Main(string[] args)
		{
			try { SetProcessDPIAware(); } catch { }
			Application.EnableVisualStyles();
			Application.SetCompatibleTextRenderingDefault(false);
			string local = null;
			for (int i = 0; i + 1 < args.Length; i++) {
				if (args[i] == "--local") {
					local = Path.GetFullPath(args[i + 1]);
				}
			}
			for (int i = 0; i + 1 < args.Length; i++) {
				if (args[i] == "--shots") {
					MainForm.Shots(args[i + 1]);
					return;
				}
				if (args[i] == "--check-ipsw") {
					/* developer: what the IPSW page would read, to a text file next to it */
					string r;
					try {
						Ipsw w = Ipsw.Open(args[i + 1]);
						r = "ok " + w.Version + " " + w.Build + "\r\n" + w.Devices + "\r\nroot " + w.RootDmg + " " + w.RootSize +
						    "\r\nsysos " + w.SysDmg + " " + w.SysSize + "\r\nappos " + w.AppDmg + " " + w.AppSize;
					} catch (Exception e) {
						r = "error " + e.Message;
					}
					File.WriteAllText(args[i + 1] + ".check.txt", r);
					return;
				}
			}
			/* the release's files next to the exe (a shared ZIP, unpacked): used as they are, nothing downloaded */
			if (local == null) {
				string here = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
				if (File.Exists(Path.Combine(here, Installer.RecoveryZip)) && File.Exists(Path.Combine(here, Installer.Overlay))) {
					local = here;
				}
			}
			Application.Run(new MainForm(local));
		}
	}

	class MainForm : Form
	{
		static readonly string[] Pages = { L.T("Bienvenida", "Welcome"), L.T("Teléfono", "Phone"), L.T("iOS y kernel", "iOS and kernel"), L.T("Espacio", "Storage"), L.T("Confirmar", "Confirm"), L.T("Instalación", "Install") };

		readonly string local;
		readonly Backdrop back = new Backdrop();
		readonly Steps steps = new Steps();
		readonly Pill next = new Pill(L.T("Siguiente", "Next"), true), prev = new Pill(L.T("Atrás", "Back"), false);
		readonly Bar stepBar = new Bar();      /* the sidebar's overall progress */
		GlassPage page;
		int current;
		float k;                                /* DPI scale */
		Image logo;
		CancellationTokenSource cts;
		bool busy, installed;

		/* what the user chose */
		PhoneInfo phone;
		Ipsw ipsw;
		string kernel;
		long iosBytes;
		bool bootIos;
		bool updateMode;        /* update Sapphire: iOS and its data kept, the iOS and Storage steps skipped */

		public MainForm(string local)
		{
			this.local = local;
			using (Graphics g = CreateGraphics()) {
				k = g.DpiX / 96f;
			}
			Theme.K = k;
			Theme.StyleWindow(this);
			Text = "Sapphire Installer";
			ClientSize = new Size(S(1000), S(660));
			FormBorderStyle = FormBorderStyle.FixedSingle;
			MaximizeBox = false;
			StartPosition = FormStartPosition.CenterScreen;
			BackColor = Theme.Window;
			Font = Theme.F(10, FontStyle.Regular);
			Stream ls = Assembly.GetExecutingAssembly().GetManifestResourceStream("logo.jpg");
			if (ls != null) {
				logo = Image.FromStream(ls);
				Icon = MakeIcon(logo);
			}
			back.Dock = DockStyle.Fill;
			Controls.Add(back);
			back.Paint += PaintSidebar;

			steps.Names = Pages;
			steps.Bounds = new Rectangle(S(16), S(184), S(220), S(52 * Pages.Length));
			back.Controls.Add(steps);
			stepBar.Bounds = new Rectangle(S(28), S(616), S(190), S(12));
			stepBar.ZeroIsIndeterminate = false;
			back.Controls.Add(stepBar);
			/* a connected button group, bottom right, under the card's right edge */
			prev.Bounds = new Rectangle(S(679), S(598), S(150), S(48));
			next.Bounds = new Rectangle(S(832), S(598), S(156), S(48));
			prev.Kind = Pill.Tonal;
			prev.Icon = Icons.Back;
			prev.Click += delegate { if (!busy && current > 0) Show(updateMode && current == 4 ? 1 : current - 1); };
			next.Click += delegate { Next(); };
			back.Controls.Add(prev);
			back.Controls.Add(next);
			FormClosing += delegate(object s, FormClosingEventArgs e) {
				if (busy && current == 5 && !installed) {
					if (MessageBox.Show(this, L.T("La instalación está en marcha. Si cierras ahora, iOS quedará a medias (Android no se ve afectado salvo que estuviera repartiendo el espacio).\n\n¿Cerrar de todos modos?", "The install is running. If you close now, iOS is left half done (Android is not affected unless the storage was being split).\n\nClose anyway?"), "Sapphire", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) {
						e.Cancel = true;
						return;
					}
				}
				if (cts != null) {
					cts.Cancel();
				}
			};
			Show(0);
		}

		int S(float v) { return (int)Math.Round(v * k); }

		static Icon MakeIcon(Image img)
		{
			Bitmap b = new Bitmap(64, 64);
			using (Graphics g = Graphics.FromImage(b)) {
				g.SmoothingMode = SmoothingMode.AntiAlias;
				g.InterpolationMode = InterpolationMode.HighQualityBicubic;
				using (GraphicsPath p = Theme.Round(new RectangleF(0, 0, 64, 64), 14)) {
					g.SetClip(p);
					g.DrawImage(img, 0, 0, 64, 64);
				}
			}
			return Icon.FromHandle(b.GetHicon());
		}

		void PaintSidebar(object sender, PaintEventArgs e)
		{
			Graphics g = e.Graphics;
			g.SmoothingMode = SmoothingMode.AntiAlias;
			g.InterpolationMode = InterpolationMode.HighQualityBicubic;
			if (logo != null) {
				/* the logo in an expressive "cookie" shape */
				RectangleF lr = new RectangleF(S(28), S(26), S(68), S(68));
				using (GraphicsPath p = Theme.Cookie(lr, 9, 0.09f)) {
					Theme.FillImage(g, logo, p, RectangleF.Inflate(lr, S(3), S(3)));
				}
			}
			using (Font f = Theme.F(21, FontStyle.Bold)) {
				TextRenderer.DrawText(g, "Sapphire", f, new Point(S(24), S(102)), Theme.Text);
			}
			using (Font f = Theme.F(9.5f, FontStyle.Regular)) {
				TextRenderer.DrawText(g, L.T("iOS en tu Redmi Note 13", "iOS on your Redmi Note 13"), f, new Point(S(28), S(142)), Theme.Dim);
			}
			using (Font f = Theme.Semi(9.5f)) {
				TextRenderer.DrawText(g, string.Format(L.T("Paso {0} de {1}", "Step {0} of {1}"), current + 1, Pages.Length), f, new Point(S(28), S(588)), Theme.Dim);
			}
		}

		/* ---- pages ---- */

		GlassPage NewPage(string title, string subtitle)
		{
			GlassPage p = new GlassPage();
			p.Bounds = new Rectangle(S(256), S(12), ClientSize.Width - S(268), S(570));
			p.Cards.Add(new RectangleF(0, 0, p.Width - 1, p.Height - 1));
			Add(p, new Txt(title, 22, FontStyle.Bold, Theme.Text), 30, 24, 660, 48);
			if (subtitle != null) {
				Add(p, new Txt(subtitle, 11, FontStyle.Regular, Theme.Dim), 34, 74, 660, 48);
			}
			return p;
		}

		T Add<T>(Control parent, T c, float x, float y, float w, float h) where T : Control
		{
			c.Bounds = new Rectangle(S(x), S(y), S(w), S(h));
			parent.Controls.Add(c);
			return c;
		}

		/* developer: every page rendered to PNG files, with made-up phone data (no phone, no install) */
		public static void Shots(string dir)
		{
			Anim.Off = true;
			MainForm f = new MainForm(null);
			f.demo = true;
			f.phone = PhoneInfo.Parse("device=sapphire\nslot=a\nandroid_slot=a\niosroot_bytes=43082846208\nuserdata_bytes=72152203264\n");
			f.ipsw = new Ipsw();
			f.ipsw.File = "iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw";
			f.ipsw.Version = "16.7.16";
			f.ipsw.Build = "20H392";
			f.ipsw.RootSize = 6800L << 20;
			f.ipsw.SysSize = 2900L << 20;
			f.ipsw.AppSize = 900L << 20;
			f.kernel = "sapphire-ios-0307.img";
			f.Show();
			for (int i = 0; i < 6; i++) {
				if (i == 3) {
					f.iosBytes = 50L << 30;
				}
				f.Show(i);
				Application.DoEvents();
				using (Bitmap b = new Bitmap(f.back.Width, f.back.Height)) {
					f.back.DrawToBitmap(b, new Rectangle(0, 0, b.Width, b.Height));
					b.Save(Path.Combine(dir, "page" + i + ".png"));
				}
			}
			/* the page transition half-way (back to Espacio, which does nothing on its own), from the screen */
			Anim.Off = false;
			f.demo = false;
			f.Activate();
			f.Show(3);
			for (int n = 0; n < 4; n++) {
				for (DateTime end = DateTime.Now.AddMilliseconds(45); DateTime.Now < end; Thread.Sleep(5)) {
					Application.DoEvents();
				}
				using (Bitmap b = new Bitmap(f.back.Width, f.back.Height)) {
					using (Graphics g = Graphics.FromImage(b)) {
						g.CopyFromScreen(f.back.PointToScreen(Point.Empty), Point.Empty, b.Size);
					}
					b.Save(Path.Combine(dir, "transition" + n + ".png"));
				}
			}
			f.Close();
		}

		bool demo;

		void Show(int i)
		{
			/* the page transition: snapshots of the old and the new page (not in --shots, not before the window shows) */
			bool animate = !demo && page != null && IsHandleCreated && Visible;
			int from = current;
			Bitmap before = animate ? Fader.Snap(back, page.Bounds) : null;
			current = i;
			steps.Current = i;
			steps.Invalidate();
			stepBar.Fraction = (double)i / (Pages.Length - 1);
			stepBar.Invalidate();
			if (page != null) {
				back.Controls.Remove(page);
				page.Dispose();
			}
			switch (i) {
			case 0: page = Welcome(); break;
			case 1: page = Phone(); break;
			case 2: page = Files(); break;
			case 3: page = Space(); break;
			case 4: page = Confirm(); break;
			default: page = Install(); break;
			}
			back.Controls.Add(page);
			prev.Visible = i > 0 && i < 5;
			prev.JoinRight = next.JoinLeft = i > 0 && i < 5;
			next.Text = i == 4 ? (updateMode ? L.T("Actualizar", "Update") : L.T("Instalar", "Install")) : i == 5 ? L.T("Cerrar", "Close") : i == 0 ? L.T("Empezar", "Start") : L.T("Siguiente", "Next");
			next.Tint = i == 4 ? Theme.Danger : Theme.Accent;
			next.Icon = i == 4 ? Icons.Download : i == 5 ? Icons.Check : Icons.Forward;
			next.IconAfter = i < 4;
			Nav();
			back.Invalidate(true);
			if (animate) {
				Fader.Run(back, page.Bounds, before, Fader.Snap(back, page.Bounds), i >= from ? 1 : -1);
			}
		}

		void Next()
		{
			if (busy || !next.Enabled) {
				return;
			}
			if (current == 5) {
				Close();
				return;
			}
			Show(updateMode && current == 1 ? 4 : current + 1);
		}

		void Nav()
		{
			bool ok = true;
			switch (current) {
			case 1: ok = phone != null && (!updateMode || phone.HasIos); break;
			case 2: ok = ipsw != null && kernel != null; break;
			case 3: ok = iosBytes > 0; break;
			case 4: ok = confirmOk; break;
			case 5: ok = installed || !busy; next.Visible = installed || failed; break;
			}
			if (current != 5) {
				next.Visible = true;
			}
			next.Enabled = ok && !busy;
			next.Invalidate();
		}

		GlassPage Welcome()
		{
			GlassPage p = NewPage(L.T("Bienvenido", "Welcome"), L.T("Este asistente instala iOS en tu Redmi Note 13 4G (sapphire) junto a Android.", "This wizard installs iOS on your Redmi Note 13 4G (sapphire), next to Android."));
			string body =
			    L.T("Qué vas a necesitar:\n\n", "What you need:\n\n") +
			    L.T("•  El bootloader desbloqueado y OrangeFox Recovery instalado.\n", "•  An unlocked bootloader and OrangeFox Recovery installed.\n") +
			    L.T("•  La IPSW de iOS 16.7.16 del iPhone X (iPhone10,3; la descargas tú: Sapphire no distribuye\n", "•  The iPhone X's iOS 16.7.16 IPSW (iPhone10,3; you download it: Sapphire does not distribute\n") +
			    L.T("    archivos de Apple). El resto (kernel incluido) se descarga solo.\n", "    Apple's files). Everything else (the kernel too) is downloaded for you.\n") +
			    L.T("•  Un cable USB y unos 30 minutos. El PC hace de mando: el teléfono hace el trabajo.\n\n", "•  A USB cable and about 30 minutes. The PC gives the orders: the phone does the work.\n\n") +
			    L.T("Después tendrás Android e iOS en el mismo teléfono. Cambias de uno a otro con la app\n", "Then you have Android and iOS on the same phone. You switch between them with the\n") +
			    L.T("Sapphire Dual Boot (Android) y la app Android (iOS).", "Sapphire Dual Boot app (Android) and the Android app (iOS).");
			p.Cards.Add(new RectangleF(S(18), S(124), S(696), S(240)));
			Add(p, new Txt(body, 11, FontStyle.Regular, Theme.Text), 34, 142, 670, 214);
			Add(p, new Txt(L.T("Sapphire: Angelpro09xd y colaboradores · Kernel XNU de Apple (código abierto) · OrangeFox · Magisk · Procursus/ElleKit\n", "Sapphire: Angelpro09xd and contributors · Apple's XNU kernel (open source) · OrangeFox · Magisk · Procursus/ElleKit\n") +
			    L.T("Sapphire no está afiliado a Apple ni a Xiaomi.", "Sapphire is not affiliated with Apple or Xiaomi."), 9, FontStyle.Regular, Theme.Dim), 34, 470, 680, 50);
			if (local != null) {
				Add(p, new Txt(L.T("Archivos de Sapphire desde esta carpeta: ", "Sapphire's files from this folder: ") + local, 9, FontStyle.Italic, Theme.Warn), 34, 520, 680, 24);
			}
			/* install from scratch, or update what is installed */
			RadioButton mi = Add(p, new M3Radio(), 26, 378, 680, 34);
			mi.Text = L.T("Instalar iOS desde cero", "Install iOS from scratch");
			RadioButton mu = Add(p, new M3Radio(), 26, 412, 680, 34);
			mu.Text = L.T("Actualizar Sapphire (iOS, tus apps y tus datos se conservan; sin IPSW)", "Update Sapphire (iOS, your apps and your data are kept; no IPSW)");
			foreach (RadioButton r in new RadioButton[] { mi, mu }) {
				r.BackColor = Color.Transparent;
				r.ForeColor = Theme.Text;
			}
			mi.Checked = !updateMode;
			mu.Checked = updateMode;
			mu.CheckedChanged += delegate {
				updateMode = mu.Checked;
				if (updateMode) {
					bootIos = true;
				}
			};
			return p;
		}

		/* ---- Teléfono ---- */
		Txt phoneState, phoneDetail;
		Pill phoneAction;
		Loader phoneLoader;
		Bar phoneBar;
		System.Windows.Forms.Timer poll;
		string lastState;

		GlassPage Phone()
		{
			GlassPage p = NewPage(L.T("Conecta el teléfono", "Connect the phone"), L.T("Por USB. El asistente lo lleva a OrangeFox y carga allí Sapphire: verás los créditos en su pantalla.", "Over USB. The wizard takes it to OrangeFox and loads Sapphire there: its screen shows the credits."));
			p.Cards.Add(new RectangleF(S(18), S(130), S(696), S(176)));
			phoneLoader = Add(p, new Loader(), 38, 150, 72, 72);
			phoneState = Add(p, new Txt(L.T("Buscando…", "Looking…"), 15, FontStyle.Bold, Theme.Text), 130, 148, 570, 34);
			phoneDetail = Add(p, new Txt("", 10.5f, FontStyle.Regular, Theme.Dim), 132, 186, 570, 112);
			phoneAction = Add(p, new Pill("", true), 34, 322, 280, 48);
			phoneAction.Visible = false;
			phoneAction.Click += PhoneAction;
			phoneBar = Add(p, new Bar(), 34, 390, 664, 12);
			phoneBar.Visible = false;
			if (phone != null) {
				ShowPhone();
				return p;
			}
			lastState = null;
			poll = new System.Windows.Forms.Timer();
			poll.Interval = 1500;
			poll.Tick += delegate { Poll(); };
			p.Disposed += delegate { poll.Stop(); poll.Dispose(); };
			RunBusy(L.T("Preparando adb y fastboot", "Getting adb and fastboot ready"), delegate(Action<string, double> st, CancellationToken ct) {
				Net.EnsurePlatformTools(st, ct);
			}, delegate(Exception e) {
				if (e != null) {
					phoneState.Text = L.T("No se pudo descargar adb", "adb could not be downloaded");
					phoneDetail.Text = e.Message + L.T("\n\nComprueba la conexión a Internet y vuelve a entrar en este paso.", "\n\nCheck the Internet connection and come back to this step.");
					return;
				}
				poll.Start();
				Poll();
			});
			return p;
		}

		bool polling;

		void Poll()
		{
			if (polling || busy || phone != null) {
				return;
			}
			polling = true;
			Task.Factory.StartNew(delegate {
				string serial;
				string state = Adb.State(out serial);
				string model = null;
				if (state == "device" || state == "recovery") {
					Adb.Serial = serial;
					model = Adb.Shell("getprop ro.product.device").Trim() + " · " + Adb.Shell("getprop ro.product.model").Trim();
				}
				BeginInvoke((Action)delegate {
					polling = false;
					ShowState(state, model);
				});
			});
		}

		void ShowState(string state, string model)
		{
			if (phone != null || IsDisposed || phoneState == null || phoneState.IsDisposed) {
				return;
			}
			lastState = state;
			phoneAction.Visible = true;
			switch (state) {
			case null:
				phoneState.Text = L.T("Conecta el teléfono por USB", "Connect the phone over USB");
				phoneDetail.Text = L.T("Si está en Android, activa la depuración USB (Ajustes › Ajustes adicionales › Opciones de desarrollador).\nTambién vale con el teléfono ya en OrangeFox o en fastboot.", "In Android, turn on USB debugging (Settings › Additional settings › Developer options).\nThe phone already in OrangeFox or in fastboot works too.");
				phoneAction.Visible = false;
				break;
			case "unauthorized":
				phoneState.Text = L.T("Acepta el aviso en el teléfono", "Accept the prompt on the phone");
				phoneDetail.Text = L.T("Marca «Permitir siempre desde este ordenador» y pulsa Permitir.", "Tick \"Always allow from this computer\" and tap Allow.");
				phoneAction.Visible = false;
				break;
			case "device":
				phoneState.Text = L.T("Teléfono en Android", "Phone in Android");
				phoneDetail.Text = model + L.T("\nHay que reiniciarlo a OrangeFox. Guarda lo que estés haciendo.", "\nIt has to restart into OrangeFox. Save what you are doing.");
				phoneAction.Text = L.T("Reiniciar a OrangeFox", "Restart into OrangeFox");
				break;
			case "fastboot":
				phoneState.Text = L.T("Teléfono en fastboot", "Phone in fastboot");
				phoneDetail.Text = L.T("Lo arranco en OrangeFox.", "It will be started in OrangeFox.");
				phoneAction.Text = L.T("Arrancar OrangeFox", "Start OrangeFox");
				break;
			case "sideload":
				phoneState.Text = L.T("Teléfono en modo sideload", "Phone in sideload mode");
				phoneDetail.Text = L.T("Cancela el sideload en OrangeFox (vuelve al menú) para continuar.", "Cancel sideload in OrangeFox (back to its menu) to go on.");
				phoneAction.Visible = false;
				break;
			case "recovery":
				phoneState.Text = L.T("Teléfono en OrangeFox", "Phone in OrangeFox");
				phoneDetail.Text = model + L.T("\nListo para cargar Sapphire. Su pantalla mostrará los créditos y el progreso.", "\nReady to load Sapphire. Its screen will show the credits and the progress.");
				phoneAction.Text = L.T("Cargar Sapphire", "Load Sapphire");
				break;
			default:
				phoneState.Text = "Estado: " + state;
				phoneAction.Visible = false;
				break;
			}
		}

		void PhoneAction(object sender, EventArgs e)
		{
			if (busy) {
				return;
			}
			string state = lastState;
			if (state == "device" || state == "fastboot") {
				RunBusy("Reiniciando", delegate(Action<string, double> st, CancellationToken ct) {
					string o;
					if (state == "device") {
						Adb.Run(Paths.Adb, "reboot recovery", out o, 30000);
					} else {
						Adb.Run(Paths.Fastboot, "reboot recovery", out o, 30000);
					}
					st(L.T("Esperando a OrangeFox", "Waiting for OrangeFox"), 0.5);
					for (int i = 0; i < 90; i++) {
						ct.ThrowIfCancellationRequested();
						string s;
						if (Adb.State(out s) == "recovery") {
							Adb.Serial = s;
							return;
						}
						Thread.Sleep(1000);
					}
					throw new Exception(L.T("OrangeFox no apareció. Si el teléfono pide desbloquear (patrón/PIN), hazlo en su pantalla.", "OrangeFox did not show up. If the phone asks to be unlocked (pattern/PIN), do it on its screen."));
				}, delegate(Exception ex) {
					if (ex != null) {
						phoneDetail.Text = ex.Message;
					}
					Poll();
				});
			} else if (state == "recovery") {
				RunBusy(L.T("Cargando Sapphire", "Loading Sapphire"), delegate(Action<string, double> st, CancellationToken ct) {
					PhoneInfo pi = Installer.LoadZip(local, st, ct);
					Installer.LastInfo = pi;
					phone = pi;
				}, delegate(Exception ex) {
					if (ex != null) {
						phoneState.Text = L.T("No se pudo cargar Sapphire", "Sapphire could not be loaded");
						phoneDetail.Text = ex.Message;
						return;
					}
					ShowPhone();
				});
			}
		}

		void ShowPhone()
		{
			phoneState.Text = L.T("Sapphire está cargado en el teléfono ✓", "Sapphire is loaded on the phone ✓");
			phoneDetail.Text = L.T("Dispositivo: ", "Device: ") + phone.Device + L.T("   ·   Android en el slot ", "   ·   Android in slot ") + phone.AndroidSlot + "\n" +
			    (phone.HasIos ? L.T("iOS instalado ahora: ", "iOS installed now: ") + Fmt.Gb(phone.IosBytes) + L.T(" (se borrará y se instalará de nuevo)", " (it will be erased and installed again)") : L.T("iOS no está instalado: se le hará sitio en el paso Espacio.", "iOS is not installed: room is made for it in the Storage step.")) + "\n" +
			    L.T("Espacio para repartir: ", "Space to split: ") + Fmt.Gb(phone.Total);
			phoneAction.Visible = false;
			phoneBar.Visible = false;
			phoneLoader.Done = true;
			phoneLoader.Invalidate();
			Nav();
		}

		/* ---- iOS y kernel ---- */
		Txt ipswInfo, kernelInfo;

		GlassPage Files()
		{
			GlassPage p = NewPage(L.T("iOS y kernel", "iOS and kernel"), L.T("Elige la IPSW de iOS 16 y el kernel de Sapphire.", "Pick the iOS 16 IPSW and Sapphire's kernel."));
			p.Cards.Add(new RectangleF(S(18), S(124), S(696), S(116)));
			Add(p, new Txt(L.T("IPSW de iOS", "iOS IPSW"), 12, FontStyle.Bold, Theme.Text), 34, 140, 400, 26);
			ipswInfo = Add(p, new Txt(ipsw != null ? IpswText() : L.T("Ninguna elegida.", "None chosen."), 10.5f, FontStyle.Regular, Theme.Dim), 34, 170, 500, 60);
			Pill pick = Add(p, new Pill(L.T("Elegir IPSW…", "Choose IPSW…"), false), 530, 146, 168, 44);
			pick.Icon = Icons.File;
			pick.Click += delegate { PickIpsw(); };

			p.Cards.Add(new RectangleF(S(18), S(256), S(696), S(156)));
			Add(p, new Txt("Kernel", 12, FontStyle.Bold, Theme.Text), 34, 272, 400, 26);
			kernelInfo = Add(p, new Txt(kernel != null && kernel != Installer.ReleaseKernel ? kernel : "", 10.5f, FontStyle.Regular, Theme.Dim), 34, 302, 500, 60);
			Pill pk = Add(p, new Pill(L.T("Elegir kernel…", "Choose kernel…"), false), 530, 278, 168, 44);
			pk.Kind = Pill.Outlined;
			pk.Icon = Icons.Folder;
			CheckBox auto = Add(p, new M3Check(), 26, 366, 660, 34);
			auto.Text = L.T("Usar el kernel de la release de Sapphire (recomendado)", "Use the kernel from the Sapphire release (recommended)");
			auto.ForeColor = Theme.Text;
			auto.BackColor = Color.Transparent;
			auto.Checked = kernel == null || kernel == Installer.ReleaseKernel;
			EventHandler sync = delegate {
				pk.Enabled = !auto.Checked;
				if (auto.Checked) {
					kernel = Installer.ReleaseKernel;
					kernelInfo.Text = L.T("Se descarga con los demás archivos de Sapphire (", "Downloaded with Sapphire's other files (") + Installer.KernelAsset + ").";
					kernelInfo.ForeColor = Theme.Dim;
				} else if (kernel == Installer.ReleaseKernel) {
					kernel = null;
					kernelInfo.Text = L.T("Ninguno elegido.", "None chosen.");
				}
				pk.Invalidate();
				Nav();
			};
			auto.CheckedChanged += sync;
			pk.Click += delegate { PickKernel(); };
			sync(null, EventArgs.Empty);
			return p;
		}

		string IpswText()
		{
			return "iOS " + ipsw.Version + " (" + ipsw.Build + ") · " + Path.GetFileName(ipsw.File) + "\n" +
			    L.T("Sistema ", "System ") + Fmt.Gb(ipsw.RootSize) + " · SystemOS " + Fmt.Gb(ipsw.SysSize) + " · apps " + Fmt.Gb(ipsw.AppSize);
		}

		void PickIpsw()
		{
			using (OpenFileDialog d = new OpenFileDialog()) {
				d.Filter = L.T("IPSW de iOS (*.ipsw)|*.ipsw|Todos los archivos|*.*", "iOS IPSW (*.ipsw)|*.ipsw|All files|*.*");
				if (d.ShowDialog(this) != DialogResult.OK) {
					return;
				}
				string f = d.FileName;
				ipswInfo.Text = L.T("Leyendo…", "Reading…");
				RunBusy(L.T("Leyendo la IPSW", "Reading the IPSW"), delegate(Action<string, double> st, CancellationToken ct) {
					ipsw = Ipsw.Open(f);
				}, delegate(Exception ex) {
					if (ex != null) {
						ipsw = null;
						ipswInfo.Text = ex.Message;
						ipswInfo.ForeColor = Theme.Danger;
					} else {
						ipswInfo.Text = IpswText();
						ipswInfo.ForeColor = Theme.Dim;
					}
					Nav();
				});
			}
		}

		void PickKernel()
		{
			using (OpenFileDialog d = new OpenFileDialog()) {
				d.Filter = L.T("Imagen de arranque (*.img)|*.img|Todos los archivos|*.*", "Boot image (*.img)|*.img|All files|*.*");
				if (d.ShowDialog(this) != DialogResult.OK) {
					return;
				}
				string err = Kernel.Check(d.FileName);
				if (err != null) {
					kernel = null;
					kernelInfo.Text = err;
					kernelInfo.ForeColor = Theme.Danger;
				} else {
					kernel = d.FileName;
					kernelInfo.Text = Path.GetFileName(kernel) + " · " + (new FileInfo(kernel).Length >> 20) + " MB";
					kernelInfo.ForeColor = Theme.Dim;
				}
				Nav();
			}
		}

		/* ---- Espacio ---- */
		const long MinIos = 24L << 30, MinAndroid = 16L << 30;
		Txt spaceText, spaceWarn;
		SplitBar split;

		GlassPage Space()
		{
			GlassPage p = NewPage(L.T("Espacio", "Storage"), L.T("Arrastra para repartir el almacenamiento entre iOS y Android.", "Drag to split the storage between iOS and Android."));
			split = Add(p, new SplitBar(), 18, 150, 696, 84);
			split.Total = phone.Total;
			split.Min1 = MinIos;
			split.Min2 = MinAndroid;
			split.Original = phone.HasIos ? phone.IosBytes : 0;
			if (iosBytes == 0) {
				iosBytes = phone.HasIos ? phone.IosBytes : Math.Max(MinIos, Math.Min(40L << 30, phone.Total / 3));
			}
			split.Value = iosBytes;
			spaceText = Add(p, new Txt("", 14, FontStyle.Bold, Theme.Text), 34, 250, 660, 32);
			spaceWarn = Add(p, new Note("", Theme.Warn), 18, 294, 696, 110);
			Add(p, new Txt(L.T("iOS necesita al menos ", "iOS needs at least ") + Fmt.Gb(MinIos) + L.T(" (el sistema ocupa unos 14 GB) y Android al menos ", " (the system takes about 14 GB) and Android at least ") + Fmt.Gb(MinAndroid) + ".", 9.5f, FontStyle.Regular, Theme.Dim), 34, 420, 660, 40);
			split.Changed += delegate { iosBytes = split.Value; SpaceText(); };
			SpaceText();
			return p;
		}

		void SpaceText()
		{
			spaceText.Text = "iOS " + Fmt.Gb(iosBytes) + "   ·   Android " + Fmt.Gb(phone.Total - iosBytes);
			if (!phone.HasIos) {
				spaceWarn.Text = L.T("⚠  Para hacer sitio a iOS hay que borrar Android por completo: apps, fotos, todo.\nHaz una copia antes. Android quedará como recién instalado.", "⚠  Making room for iOS erases Android completely: apps, photos, everything.\nBack up first. Android will be like freshly installed.");
				spaceWarn.ForeColor = Theme.Danger;
			} else if (iosBytes != phone.IosBytes) {
				spaceWarn.Text = L.T("⚠  Cambiar el reparto borra todos los datos de Android (apps, fotos, todo).\nHaz una copia antes. Si dejas el reparto como está (", "⚠  Changing the split erases all of Android's data (apps, photos, everything).\nBack up first. If you keep the split as it is (") + Fmt.Gb(phone.IosBytes) + L.T("), Android no se toca.", "), Android is not touched.");
				spaceWarn.ForeColor = Theme.Danger;
			} else {
				spaceWarn.Text = L.T("✓  El reparto actual: los datos de Android se conservan.", "✓  The current split: Android's data is kept.");
				spaceWarn.ForeColor = Theme.Android;
			}
			Nav();
		}

		bool Repartition { get { return phone != null && (!phone.HasIos || iosBytes != phone.IosBytes); } }

		/* ---- Confirmar ---- */
		bool confirmOk;

		GlassPage Confirm()
		{
			GlassPage p = NewPage(L.T("Confirmar", "Confirm"), L.T("Revisa lo que va a pasar.", "Check what is going to happen."));
			if (updateMode) {
				return ConfirmUpdate(p);
			}
			string s =
			    "•  iOS " + ipsw.Version + " (" + ipsw.Build + L.T(") se instala de cero en una partición de ", ") is installed from scratch in a partition of ") + Fmt.Gb(iosBytes) + ".\n" +
			    "•  Kernel: " + (kernel == Installer.ReleaseKernel ? L.T("el de la release (", "the release's (") + Installer.KernelAsset + ")" : Path.GetFileName(kernel)) + ".\n" +
			    L.T("•  Se aplican los ajustes y tweaks de Sapphire; los tweaks de terceros se instalan luego desde Sileo.\n", "•  Sapphire's settings and tweaks are applied; third-party tweaks can be installed from Sileo afterwards.\n") +
			    (Repartition ? L.T("•  Se reparte el espacio de nuevo: SE BORRAN LOS DATOS DE ANDROID.\n", "•  The storage is split again: ANDROID'S DATA IS ERASED.\n") : L.T("•  Android y sus datos no se tocan.\n", "•  Android and its data are not touched.\n"));
			p.Cards.Add(new RectangleF(S(18), S(122), S(696), S(130)));
			Add(p, new Txt(s, 11, FontStyle.Regular, Theme.Text), 34, 138, 670, 108);
			confirmOk = false;
			CheckBox c1 = Add(p, new M3Check(), 26, 284, 680, 34);
			c1.Text = phone.HasIos ? L.T("Entiendo que el iOS actual se borra por completo", "I understand the current iOS is erased completely") : L.T("Entiendo que se instala iOS desde cero", "I understand iOS is installed from scratch");
			CheckBox c2 = null;
			if (Repartition) {
				c2 = Add(p, new M3Check(), 26, 318, 680, 34);
				c2.Text = L.T("Entiendo que se borran todos los datos de Android", "I understand all of Android's data is erased");
				c2.ForeColor = Theme.Danger;
			}
			foreach (CheckBox c in new CheckBox[] { c1, c2 }) {
				if (c == null) {
					continue;
				}
				c.BackColor = Color.Transparent;
				if (c != c2) {
					c.ForeColor = Theme.Text;
				}
				c.CheckedChanged += delegate { confirmOk = c1.Checked && (c2 == null || c2.Checked); Nav(); };
			}
			Add(p, new Txt(L.T("Al terminar:", "When it is done:"), 12, FontStyle.Bold, Theme.Text), 34, 370, 300, 26);
			RadioButton ra = Add(p, new M3Radio(), 26, 398, 680, 34);
			ra.Text = L.T("Arrancar Android e instalar la app Sapphire Dual Boot (recomendado)", "Boot Android and install the Sapphire Dual Boot app (recommended)");
			RadioButton ri = Add(p, new M3Radio(), 26, 432, 680, 34);
			ri.Text = L.T("Arrancar iOS directamente", "Boot iOS right away");
			foreach (RadioButton r in new RadioButton[] { ra, ri }) {
				r.BackColor = Color.Transparent;
				r.ForeColor = Theme.Text;
			}
			ra.Checked = !bootIos;
			ri.Checked = bootIos;
			ri.CheckedChanged += delegate { bootIos = ri.Checked; };
			Add(p, new Note(L.T("No desconectes el cable durante la instalación.", "Do not unplug the cable during the install."), Theme.Warn), 18, 486, 696, 50);
			return p;
		}

		GlassPage ConfirmUpdate(GlassPage p)
		{
			string s =
			    L.T("•  Se instala la última actualización de Sapphire y su kernel.\n", "•  The latest Sapphire update and its kernel are installed.\n") +
			    L.T("•  iOS, tus apps, tus datos y Android no se tocan.\n", "•  iOS, your apps, your data and Android are not touched.\n") +
			    L.T("•  iOS aplica la actualización al arrancar (unos segundos).\n", "•  iOS applies the update as it starts (a few seconds).\n");
			p.Cards.Add(new RectangleF(S(18), S(122), S(696), S(110)));
			Add(p, new Txt(s, 11, FontStyle.Regular, Theme.Text), 34, 138, 670, 90);
			confirmOk = true;
			Add(p, new Txt(L.T("Al terminar:", "When it is done:"), 12, FontStyle.Bold, Theme.Text), 34, 260, 300, 26);
			RadioButton ri = Add(p, new M3Radio(), 26, 288, 680, 34);
			ri.Text = L.T("Arrancar iOS (recomendado: así se aplica la actualización)", "Boot iOS (recommended: that is when the update is applied)");
			RadioButton ra = Add(p, new M3Radio(), 26, 322, 680, 34);
			ra.Text = L.T("Volver a Android (se aplica la próxima vez que arranques iOS)", "Back to Android (applied the next time you start iOS)");
			foreach (RadioButton r in new RadioButton[] { ra, ri }) {
				r.BackColor = Color.Transparent;
				r.ForeColor = Theme.Text;
			}
			ri.Checked = bootIos;
			ra.Checked = !bootIos;
			ri.CheckedChanged += delegate { bootIos = ri.Checked; };
			Add(p, new Note(L.T("No desconectes el cable durante la actualización.", "Do not unplug the cable during the update."), Theme.Warn), 18, 486, 696, 50);
			Nav();
			return p;
		}

		/* ---- Instalación ---- */
		Txt instTitle, instStep;
		Bar instBar;
		TextBox log;
		bool failed;

		GlassPage Install()
		{
			GlassPage p = NewPage(updateMode ? L.T("Actualizando", "Updating") : L.T("Instalando", "Installing"), null);
			instTitle = p.Controls[0] as Txt;
			instStep = Add(p, new Txt("", 13, FontStyle.Bold, Theme.Text), 34, 80, 670, 34);
			instBar = Add(p, new Bar(), 34, 122, 664, 12);
			log = Add(p, new TextBox(), 36, 166, 664, 372);
			log.Multiline = true;
			log.ReadOnly = true;
			log.ScrollBars = ScrollBars.Vertical;
			log.BorderStyle = BorderStyle.None;
			log.BackColor = Theme.Tile;
			log.ForeColor = Theme.OnSurface;
			log.Font = Theme.Mono(9.5f);
			Theme.StyleScroll(log);
			p.Cards.Add(new RectangleF(S(18), S(150), S(696), S(400)));

			Plan plan = new Plan();
			plan.Ipsw = ipsw;
			plan.Kernel = kernel;
			plan.IosBytes = iosBytes;
			plan.Repartition = Repartition;
			plan.BootIos = bootIos;
			plan.LocalRelease = local;
			plan.Update = updateMode;
			if (plan.Kernel == null) {
				plan.Kernel = Installer.ReleaseKernel;
			}
			Installer inst = new Installer();
			inst.Log += delegate(string s) { Ui(delegate { log.AppendText(s + "\r\n"); }); };
			inst.StepStarted += delegate(int i, string t) { Ui(delegate { instStep.Text = (i + 1) + "/" + inst.Titles.Length + "  " + t; log.AppendText("== " + t + "\r\n"); }); };
			inst.Progress += delegate(double f) { Ui(delegate { instBar.Fraction = f; instBar.Invalidate(); }); };
			if (demo) {
				instStep.Text = "4/7  Enviar iOS al teléfono";
				instBar.Fraction = 0.42;
				log.Text = "== Descargar los archivos de Sapphire\r\n== Preparar el teléfono\r\n== Repartir el espacio\r\n   El reparto no cambia: los datos de Android se conservan.\r\n== Enviar iOS al teléfono\r\nEnviando 098-12345-001.dmg (6.6 GB)\r\n";
				return p;
			}
			busy = true;
			failed = false;
			cts = new CancellationTokenSource();
			CancellationToken ct = cts.Token;
			Nav();
			Task.Factory.StartNew(delegate {
				Exception err = null;
				try {
					inst.Run(plan, ct);
				} catch (Exception e) {
					err = e;
				}
				Ui(delegate {
					busy = false;
					if (err == null) {
						installed = true;
						instTitle.Text = L.T("¡Listo!", "Done!");
						instStep.Text = updateMode ? (bootIos ? L.T("iOS está arrancando y aplicará la actualización.", "iOS is starting and will apply the update.") :
						    L.T("El teléfono vuelve a Android. La actualización se aplica al arrancar iOS.", "The phone goes back to Android. The update is applied when iOS starts.")) :
						    bootIos ? L.T("iOS está arrancando. El primer arranque puede tardar 15 minutos: no lo reinicies.", "iOS is starting. The first boot can take 15 minutes: do not restart it.") :
						    L.T("El teléfono vuelve a Android. Abre Sapphire Dual Boot para ir a iOS (el primer arranque tarda hasta 15 min).", "The phone goes back to Android. Open Sapphire Dual Boot to go to iOS (the first boot takes up to 15 min).");
						instBar.Fraction = 1;
						instBar.Tint = Theme.Android;
					} else {
						failed = true;
						instTitle.Text = L.T("La instalación se detuvo", "The install stopped");
						instStep.Text = err is OperationCanceledException ? L.T("Cancelada.", "Cancelled.") : err.Message;
						instStep.ForeColor = Theme.Danger;
						log.AppendText("\r\n✗ " + err + "\r\n");
						log.AppendText(L.T("\r\nAndroid sigue funcionando. Puedes volver a intentarlo: el teléfono sigue en OrangeFox.\r\n", "\r\nAndroid still works. You can try again: the phone is still in OrangeFox.\r\n"));
						instBar.Tint = Theme.Danger;
					}
					instBar.Invalidate();
					Nav();
				});
			}, TaskCreationOptions.LongRunning);
			return p;
		}

		void Ui(Action a)
		{
			if (IsDisposed) {
				return;
			}
			try {
				BeginInvoke(a);
			} catch (InvalidOperationException) {
			}
		}

		/* a short job off the UI thread, with its status in the phone page's bar (if shown) */
		void RunBusy(string what, Action<Action<string, double>, CancellationToken> job, Action<Exception> done)
		{
			busy = true;
			Nav();
			cts = new CancellationTokenSource();
			CancellationToken ct = cts.Token;
			if (phoneBar != null && !phoneBar.IsDisposed) {
				phoneBar.Visible = true;
				phoneBar.Fraction = 0;
				phoneDetail.Text = what + "…";
			}
			Action<string, double> st = delegate(string s, double f) {
				Ui(delegate {
					if (phoneBar != null && !phoneBar.IsDisposed) {
						phoneBar.Fraction = f;
						phoneBar.Invalidate();
						phoneDetail.Text = s + "…";
					}
				});
			};
			Task.Factory.StartNew(delegate {
				Exception err = null;
				try {
					job(st, ct);
				} catch (Exception e) {
					err = e;
				}
				Ui(delegate {
					busy = false;
					if (phoneBar != null && !phoneBar.IsDisposed) {
						phoneBar.Visible = false;
					}
					done(err);
					Nav();
				});
			});
		}
	}
}
