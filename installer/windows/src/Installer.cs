// Installer.cs: what the PC does, step by step. The phone does the heavy work in OrangeFox
// (sapphire.sh in the recovery ZIP); the PC feeds it the release files and the user's IPSW over adb.
//
// Phone protocol (sapphire.sh COMMAND ...): output lines
//   STEP text            what the phone is doing now
//   PROGRESS done total  progress of that step
//   KEY=value            information (info)
//   ERROR text           failure (the command also exits non-zero)
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Threading;

namespace Sapphire
{
	class PhoneInfo
	{
		public string Device = "?", Slot = "?", AndroidSlot = "?";
		public long IosBytes, UserdataBytes, Total, Sector = 4096;
		public bool HasIos;

		public static PhoneInfo Parse(string text)
		{
			PhoneInfo p = new PhoneInfo();
			foreach (string l in text.Split('\n')) {
				int eq = l.IndexOf('=');
				if (eq < 0) {
					continue;
				}
				string k = l.Substring(0, eq).Trim(), v = l.Substring(eq + 1).Trim();
				long n;
				long.TryParse(v, out n);
				switch (k) {
				case "device": p.Device = v; break;
				case "slot": p.Slot = v; break;
				case "android_slot": p.AndroidSlot = v; break;
				case "iosroot_bytes": p.IosBytes = n; p.HasIos = n > 0; break;
				case "userdata_bytes": p.UserdataBytes = n; break;
				case "sector": p.Sector = n; break;
				}
			}
			p.Total = p.IosBytes + p.UserdataBytes;
			return p;
		}
	}

	class Plan
	{
		public Ipsw Ipsw;
		public string Kernel;
		public long IosBytes;           /* the iOS partition wanted */
		public bool Repartition;        /* the split changes: Android's data is erased */
		public bool BootIos;            /* start iOS when done (else Android, with the app installed) */
		public string LocalRelease;     /* developer: a folder with the release files instead of GitHub */
		public bool Update;             /* update mode: iOS and its data kept; an update (.supdate) + the kernel */
	}

	class Installer
	{
		public const string RecoveryZip = "sapphire-recovery.zip";
		public const string Overlay = "sapphire-overlay.tar.gz";
		public const string AndroidApp = "SapphireDualBoot.apk";
		public const string KernelAsset = "sapphire-kernel.img";
		public const string ReleaseKernel = "<release>";       /* Plan.Kernel: the release's, fetched at step 0 */
		const string Sh = "/tmp/sapphire/sapphire.sh";

		public event Action<string> Log;
		public event Action<int, string> StepStarted;   /* index, title */
		public event Action<double> Progress;           /* of the current step */

		public static readonly string[] StepTitles = {
			L.T("Descargar los archivos de Sapphire", "Download Sapphire's files"),
			L.T("Preparar el teléfono", "Prepare the phone"),
			L.T("Repartir el espacio", "Split the storage"),
			L.T("Enviar iOS al teléfono", "Send iOS to the phone"),
			L.T("Construir el sistema de iOS", "Build iOS's system"),
			L.T("Instalar el kernel", "Install the kernel"),
			L.T("Terminar", "Finish"),
		};

		public static readonly string[] UpdateStepTitles = {
			L.T("Descargar la actualización", "Download the update"),
			L.T("Preparar el teléfono", "Prepare the phone"),
			L.T("Enviar la actualización", "Send the update"),
			L.T("Instalar el kernel", "Install the kernel"),
			L.T("Terminar", "Finish"),
		};
		public string[] Titles = StepTitles;

		void Say(string s) { if (Log != null) Log(s); }
		void Step(int i) { if (StepStarted != null) StepStarted(i, Titles[i]); Frac(0); }

		/* the update of a release: the newest *.supdate in the local folder, or the release's asset of that kind */
		public static string FetchUpdate(string local, Action<string, double> status, CancellationToken ct)
		{
			if (local != null) {
				string best = null;
				foreach (string f in Directory.GetFiles(local, "*.supdate")) {
					if (best == null || string.CompareOrdinal(Path.GetFileName(f), Path.GetFileName(best)) > 0) {
						best = f;
					}
				}
				if (best == null) {
					throw new Exception(L.T("No hay ninguna actualización (.supdate) en ", "There is no update (.supdate) in ") + local);
				}
				return best;
			}
			string tag;
			foreach (string name in Net.ReleaseAssets(out tag).Keys) {
				if (name.EndsWith(".supdate")) {
					return Fetch(name, null, status, ct);
				}
			}
			throw new Exception(L.T("La release ", "The release ") + tag + L.T(" no trae actualización (.supdate).", " has no update (.supdate)."));
		}

		/* update mode: iOS, its apps and data stay; the update goes to the inbox (iOS applies it at its next boot) */
		void RunUpdate(Plan plan, CancellationToken ct)
		{
			Titles = UpdateStepTitles;
			Step(0);
			string upd = FetchUpdate(plan.LocalRelease, delegate(string s, double f) { Frac(f); }, ct);
			if (plan.Kernel == ReleaseKernel) {
				plan.Kernel = Fetch(KernelAsset, plan.LocalRelease, delegate(string s, double f) { Frac(f); }, ct);
				string bad = Sapphire.Kernel.Check(plan.Kernel);
				if (bad != null) {
					throw new Exception(L.T("El kernel de la release no es válido: ", "The release's kernel is not valid: ") + bad);
				}
			}
			Say(L.T("Actualización: ", "Update: ") + Path.GetFileName(upd));

			Step(1);
			string st;
			if (Adb.State(out st) != "recovery" || Adb.Shell("test -x " + Sh + " && echo ok").Trim() != "ok") {
				throw new Exception(L.T("El teléfono ya no está en OrangeFox con Sapphire cargado. Vuelve al paso Teléfono.", "The phone is no longer in OrangeFox with Sapphire loaded. Go back to the Phone step."));
			}
			Phone("shell " + Sh + " check", null, 0, ct);
			Phone("shell " + Sh + " screen-on", null, 0, ct);
			Screen(L.T("Actualizando: no desconectes el cable", "Updating: do not unplug the cable"));

			Step(2);
			using (FileStream f = File.OpenRead(upd)) {
				Say(L.T("Enviando la actualización (", "Sending the update (") + Fmt.Gb(f.Length) + ")");
				Send("update " + Path.GetFileName(upd) + " " + f.Length, f, f.Length, ct);
			}

			Step(3);
			Screen(L.T("Instalando el kernel", "Installing the kernel"));
			using (FileStream f = File.OpenRead(plan.Kernel)) {
				Send("kernel " + f.Length, f, f.Length, ct);
			}

			Step(4);
			Screen(L.T("Terminado", "Done"));
			Phone("shell " + Sh + " finish", null, 0, ct);
			Restart(plan, null, ct, L.T("iOS está arrancando: aplicará la actualización al empezar (unos segundos más de lo normal).", "iOS is starting: it applies the update as it starts (a few seconds longer than usual)."));
			Frac(1);
		}
		void Frac(double f) { if (Progress != null) Progress(f); }

		/* the release files: from GitHub (cached by tag), or a local folder */
		public static string Fetch(string name, string local, Action<string, double> status, CancellationToken ct)
		{
			if (local != null) {
				string p = Path.Combine(local, name);
				if (!File.Exists(p)) {
					throw new Exception(L.T("Falta ", "Missing ") + name + " en " + local);
				}
				return p;
			}
			string tag;
			Dictionary<string, string> assets = Net.ReleaseAssets(out tag);
			if (!assets.ContainsKey(name)) {
				throw new Exception(L.T("La release ", "The release ") + tag + L.T(" no tiene ", " has no ") + name + ".");
			}
			string dir = Path.Combine(Paths.Release, tag);
			Directory.CreateDirectory(dir);
			string dest = Path.Combine(dir, name);
			if (!File.Exists(dest)) {
				Net.Download(assets[name], dest, delegate(long d, long t) { status(L.T("Descargando ", "Downloading ") + name, t > 0 ? (double)d / t : 0); }, ct);
			}
			return dest;
		}

		/* the phone in OrangeFox with the ZIP loaded: its screen shows the credits, sapphire.sh answers */
		public static PhoneInfo LoadZip(string local, Action<string, double> status, CancellationToken ct)
		{
			string zip = Fetch(RecoveryZip, local, status, ct);
			/* OrangeFox restarts its adbd while it starts: loaded only once adb answers steadily, and tried again */
			for (int attempt = 1; ; attempt++) {
				WaitRecoveryReady(status, ct);
				try {
					return LoadZipOnce(zip, status, ct);
				} catch (Exception e) {
					if (attempt == 3 || e is OperationCanceledException) {
						throw;
					}
					status(L.T("OrangeFox aún no estaba listo: reintentando…", "OrangeFox was not ready yet: trying again…"), 0.1);
					Thread.Sleep(5000);
				}
			}
		}

		/* adb in recovery answering 3 times in a row, then 10 s more for OrangeFox to settle */
		static void WaitRecoveryReady(Action<string, double> status, CancellationToken ct)
		{
			status(L.T("Esperando a que OrangeFox termine de arrancar", "Waiting for OrangeFox to finish starting"), 0.1);
			int ok = 0;
			for (int i = 0; i < 120 && ok < 3; i++) {
				ct.ThrowIfCancellationRequested();
				string o;
				if (Adb.Run(Paths.Adb, "shell echo ready", out o, 10000) == 0 && o.Trim() == "ready") {
					ok++;
				} else {
					ok = 0;
				}
				Thread.Sleep(1000);
			}
			for (int i = 0; i < 10; i++) {
				ct.ThrowIfCancellationRequested();
				Thread.Sleep(1000);
			}
		}

		static PhoneInfo LoadZipOnce(string zip, Action<string, double> status, CancellationToken ct)
		{
			string o;
			status(L.T("Cargando Sapphire en el recovery", "Loading Sapphire into the recovery"), 0.3);
			Adb.Run(Paths.Adb, "shell rm -rf /tmp/sapphire /tmp/sapphire.zip", out o, 30000);
			if (Adb.Run(Paths.Adb, "push " + Adb.Quote(zip) + " /tmp/sapphire.zip", out o, 300000) != 0) {
				throw new Exception(L.T("No se pudo copiar el ZIP al teléfono: ", "The ZIP could not be copied to the phone: ") + o);
			}
			status(L.T("Instalando el ZIP en OrangeFox", "Installing the ZIP in OrangeFox"), 0.6);
			Adb.Run(Paths.Adb, "shell twrp install /tmp/sapphire.zip", out o, 300000);
			string info;
			if (Adb.Run(Paths.Adb, "shell " + Sh + " info", out info, 60000) != 0 || info.IndexOf("device=") < 0) {
				throw new Exception(L.T("El ZIP no arrancó en el recovery.\n", "The ZIP did not start in the recovery.\n") + o + "\n" + info);
			}
			PhoneInfo p = PhoneInfo.Parse(info);
			/* the phone's messages and screen in this PC's language */
			Adb.Run(Paths.Adb, "shell " + Sh + " lang " + L.Code, out o, 30000);
			/* the installer's screen: started from adb (adbd is its own service; anything the ZIP starts dies with OrangeFox's UI) */
			Adb.Run(Paths.Adb, "shell " + Sh + " screen-on", out o, 30000);
			status(L.T("Listo", "Ready"), 1);
			return p;
		}

		static void Screen(string text)
		{
			Adb.Shell(Sh + " screen " + Adb.Quote(text));
		}

		/* a phone command with the protocol's lines turned into log and progress */
		int Phone(string args, Stream input, long length, CancellationToken ct)
		{
			return Phone(args, input, length, ct, 0);
		}

		/* OKRC: an exit code that also means success (part: 3, "restart the recovery") */
		int Phone(string args, Stream input, long length, CancellationToken ct, int okrc)
		{
			string err = null;
			int rc = Adb.Stream(args, input, length, delegate(string l) {
				if (l.StartsWith("PROGRESS ")) {
					string[] f = l.Split(' ');
					double d, t;
					if (f.Length == 3 && double.TryParse(f[1], out d) && double.TryParse(f[2], out t) && t > 0) {
						Frac(d / t);
					}
				} else if (l.StartsWith("STEP ")) {
					Say(l.Substring(5));
				} else if (l.StartsWith("ERROR ")) {
					err = l.Substring(6);
					Say("✗ " + err);
				} else {
					Say("   " + l);
				}
			}, input != null ? (Action<long>)delegate(long s) { Frac((double)s / length); } : null, ct);
			if (rc != 0 && (okrc == 0 || rc != okrc)) {
				throw new Exception(err != null ? err : L.T("El teléfono devolvió el error ", "The phone returned error ") + rc + " (" + args + ").");
			}
			if (err != null && rc == 0) {
				throw new Exception(err);       /* an ERROR line is a failure even if the exit code got lost */
			}
			return rc;
		}

		const int Port = 5550;

		static uint[] crcTable;

		static uint Crc(uint crc, byte[] b, int n)
		{
			if (crcTable == null) {
				uint[] t = new uint[256];
				for (uint i = 0; i < 256; i++) {
					uint c = i;
					for (int k = 0; k < 8; k++) {
						c = (c & 1) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1;
					}
					t[i] = c;
				}
				crcTable = t;
			}
			for (int i = 0; i < n; i++) {
				crc = crcTable[(crc ^ b[i]) & 0xff] ^ (crc >> 8);
			}
			return crc;
		}

		/*
		 * LENGTH bytes of INPUT to the phone: CMD (sapphire.sh recv/kernel) starts sapphire-recv, which listens
		 * on the phone's 127.0.0.1:5550; adb forward carries a TCP connection there. The phone answers with the
		 * CRC32 of what it wrote, compared with ours. (adb exec-in lost data and exit codes.)
		 */
		void Send(string cmd, Stream input, long length, CancellationToken ct)
		{
			string o;
			if (Adb.Run(Paths.Adb, "forward tcp:" + Port + " tcp:" + Port, out o, 30000) != 0) {
				throw new Exception("adb forward: " + o);
			}
			ManualResetEvent listening = new ManualResetEvent(false);
			string err = null, recv = null;
			int rc = -1;
			Exception fail = null;
			Thread phone = new Thread(delegate() {
				try {
					rc = Adb.Stream("shell " + Sh + " " + cmd, null, 0, delegate(string l) {
						if (l.StartsWith("LISTEN")) {
							listening.Set();
						} else if (l.StartsWith("RECV ")) {
							recv = l.Trim();
						} else if (l.StartsWith("ERROR ")) {
							err = l.Substring(6);
							Say("✗ " + err);
						} else if (l.StartsWith("STEP ")) {
							Say(l.Substring(5));
						} else if (!l.StartsWith("PROGRESS ")) {
							Say("   " + l);
						}
					}, null, ct);
				} catch (Exception e) {
					fail = e;
				}
				listening.Set();
			});
			phone.IsBackground = true;
			phone.Start();
			if (!listening.WaitOne(90000) || fail != null || err != null || !phone.IsAlive && rc != 0) {
				phone.Join(5000);
				throw new Exception(err ?? (fail != null ? fail.Message : L.T("El teléfono no se puso a escuchar (", "The phone did not start listening (") + cmd + ")."));
			}
			uint crc = 0xffffffffu;
			using (System.Net.Sockets.TcpClient tc = new System.Net.Sockets.TcpClient()) {
				tc.Connect("127.0.0.1", Port);
				tc.NoDelay = false;
				tc.ReceiveTimeout = 180000;
				using (System.Net.Sockets.NetworkStream ns = tc.GetStream()) {
					byte[] buf = new byte[1 << 20];
					long done = 0;
					while (done < length) {
						ct.ThrowIfCancellationRequested();
						int n = input.Read(buf, 0, (int)Math.Min(buf.Length, length - done));
						if (n <= 0) {
							throw new Exception(L.T("El archivo de origen se acabó antes de tiempo.", "The source file ended too early."));
						}
						ns.Write(buf, 0, n);
						crc = Crc(crc, buf, n);
						done += n;
						Frac((double)done / length);
					}
					ns.Flush();
					/*
					 * Kept open until the phone has it all: sapphire-recv knows the size and needs no end of stream,
					 * and a half-close right after a small write let adb forward drop the bytes (an update of
					 * 7 KB arrived as 0). The phone's own account (adb forward drops the socket's answer):
					 * "RECV <bytes> <crc32>" once written and synced, then sapphire.sh exits.
					 */
					phone.Join();
				}
			}
			if (fail != null) {
				throw fail;
			}
			string want = "RECV " + length + " " + (crc ^ 0xffffffffu).ToString("x8");
			if (err != null || recv != want) {
				throw new Exception(err ?? (L.T("El teléfono no recibió los datos bien (", "The phone did not receive the data correctly (") + (recv ?? L.T("sin respuesta", "no answer")) + "; esperado " + want + ")."));
			}
			if (rc != 0) {
				throw new Exception(L.T("El teléfono devolvió el error ", "The phone returned error ") + rc + " (" + cmd + ").");
			}
			Adb.Run(Paths.Adb, "forward --remove tcp:" + Port, out o, 15000);
		}

		public void Run(Plan plan, CancellationToken ct)
		{
			if (plan.Update) {
				RunUpdate(plan, ct);
				return;
			}
			Step(0);
			string overlay = Fetch(Overlay, plan.LocalRelease, delegate(string s, double f) { Frac(f); }, ct);
			if (plan.Kernel == ReleaseKernel) {
				plan.Kernel = Fetch(KernelAsset, plan.LocalRelease, delegate(string s, double f) { Frac(f); }, ct);
				string bad = Sapphire.Kernel.Check(plan.Kernel);
				if (bad != null) {
					throw new Exception(L.T("El kernel de la release no es válido: ", "The release's kernel is not valid: ") + bad);
				}
			}
			string apk = null;
			if (!plan.BootIos) {
				try {
					apk = Fetch(AndroidApp, plan.LocalRelease, delegate(string s, double f) { Frac(f); }, ct);
				} catch (Exception e) {
					Say(L.T("La app de Android no está en la release (", "The Android app is not in the release (") + e.Message + L.T("); se puede instalar luego.", "); it can be installed later."));
				}
			}

			Step(1);
			string st;
			if (Adb.State(out st) != "recovery" || Adb.Shell("test -x " + Sh + " && echo ok").Trim() != "ok") {
				throw new Exception(L.T("El teléfono ya no está en OrangeFox con Sapphire cargado. Vuelve al paso Teléfono.", "The phone is no longer in OrangeFox with Sapphire loaded. Go back to the Phone step."));
			}
			Phone("shell " + Sh + " check", null, 0, ct);
			Phone("shell " + Sh + " screen-on", null, 0, ct);
			Screen(L.T("Instalando: no desconectes el cable", "Installing: do not unplug the cable"));

			Step(2);
			/* a split done by an earlier run that stopped before Android was formatted: remembered on this PC */
			string pending = Path.Combine(Paths.Data, "format-pending-" + (Adb.Serial ?? "phone"));
			if (plan.Repartition) {
				/* the new table is read at the next boot: OrangeFox restarts, the ZIP is loaded again */
				Phone("shell " + Sh + " part " + plan.IosBytes, null, 0, ct, 3);
				Directory.CreateDirectory(Paths.Data);
				File.WriteAllText(pending, DateTime.Now.ToString("s"));
				Say(L.T("Reiniciando OrangeFox para leer la nueva tabla", "Restarting OrangeFox to read the new partition table"));
				string ro;
				Adb.Run(Paths.Adb, "reboot recovery", out ro, 30000);
				Thread.Sleep(5000);
				if (!WaitFor("recovery", 180, ct)) {
					throw new Exception(L.T("OrangeFox no volvió tras repartir. La tabla nueva ya está escrita: vuelve a abrir el instalador con el teléfono en OrangeFox.", "OrangeFox did not come back after the split. The new table is written: open the installer again with the phone in OrangeFox."));
				}
				LastInfo = LoadZip(plan.LocalRelease, delegate(string s, double f) { Say(s); }, ct);
				Phone("shell " + Sh + " screen-on", null, 0, ct);
				Phone("shell " + Sh + " format-data", null, 0, ct);
				File.Delete(pending);
			} else if (File.Exists(pending)) {
				Say(L.T("La instalación anterior repartió el espacio pero no llegó a formatear Android: se formatea ahora.",
				    "The previous install split the storage but did not get to format Android: formatting it now."));
				Phone("shell " + Sh + " format-data", null, 0, ct);
				File.Delete(pending);
			} else {
				Say(L.T("El reparto no cambia: los datos de Android se conservan.", "The split does not change: Android's data is kept."));
			}

			Step(3);
			Screen(L.T("Recibiendo iOS ", "Receiving iOS ") + plan.Ipsw.Version);
			Phone("shell " + Sh + " format", null, 0, ct);
			using (ZipArchive z = ZipFile.OpenRead(plan.Ipsw.File)) {
				SendDmg(z, plan.Ipsw.RootDmg, "root", ct);
				SendDmg(z, plan.Ipsw.SysDmg, "sysos", ct);
				SendDmg(z, plan.Ipsw.AppDmg, "appos", ct);
			}
			using (FileStream f = File.OpenRead(overlay)) {
				Say(L.T("Enviando los archivos de Sapphire (", "Sending Sapphire's files (") + Fmt.Gb(f.Length) + ")");
				Send("recv overlay " + f.Length, f, f.Length, ct);
			}

			Step(4);
			Screen(L.T("Construyendo iOS", "Building iOS"));
			/* the phone's RTC offset from this PC's clock: iOS's first boot is not in 1970 */
			long epoch = (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds;
			Phone("shell " + Sh + " rtc " + epoch, null, 0, ct);
			Phone("shell " + Sh + " build", null, 0, ct);
			/* 0.1.8: the release's update too -- what changed after the overlay was made (fixes, new daemons, launchd
			 * cache edits) -- into the inbox; iOS applies it at its first boot, as in update mode. A release without one
			 * goes on without it. */
			{
				string upd = null;
				try {
					upd = FetchUpdate(plan.LocalRelease, delegate(string s2, double f2) { Frac(f2); }, ct);
				} catch (Exception) {
					upd = null;
				}
				if (upd != null) {
					using (FileStream f = File.OpenRead(upd)) {
						Say(L.T("Enviando la actualización de la release (", "Sending the release's update (") + Path.GetFileName(upd) + ")");
						Send("update " + Path.GetFileName(upd) + " " + f.Length, f, f.Length, ct);
					}
				}
			}

			Step(5);
			Screen(L.T("Instalando el kernel", "Installing the kernel"));
			using (FileStream f = File.OpenRead(plan.Kernel)) {
				Send("kernel " + f.Length, f, f.Length, ct);
			}

			Step(6);
			Screen(L.T("Terminado", "Done"));
			Phone("shell " + Sh + " finish", null, 0, ct);
			Restart(plan, apk, ct, L.T("iOS está arrancando. El primer arranque puede tardar 15 minutos (la manzana o el logo un buen rato): no lo reinicies.", "iOS is starting. The first boot can take 15 minutes (the logo or the Apple logo for a long while): do not restart it."));
			Frac(1);
		}

		/* the end: into iOS (its slot made active) or Android (with the app installed, when there is one) */
		void Restart(Plan plan, string apk, CancellationToken ct, string iosNote)
		{
			string o;
			if (plan.BootIos) {
				/* the slot switch as Android's boot HAL does it: fastboot set_active swaps the GPT and the boot LUN */
				Say(L.T("Reiniciando al bootloader para arrancar iOS", "Restarting into the bootloader to start iOS"));
				Adb.Run(Paths.Adb, "reboot bootloader", out o, 30000);
				WaitFor("fastboot", 120, ct);
				PhoneInfo p = LastInfo;
				string ios = p != null && p.AndroidSlot == "a" ? "b" : "a";
				if (Adb.Run(Paths.Fastboot, "set_active " + ios, out o, 30000) != 0) {
					throw new Exception("fastboot set_active " + ios + ": " + o);
				}
				Adb.Run(Paths.Fastboot, "reboot", out o, 30000);
				Say(iosNote);
			} else {
				Say(L.T("Reiniciando a Android", "Restarting into Android"));
				Adb.Run(Paths.Adb, "reboot", out o, 30000);
				if (apk != null) {
					Say(L.T("Esperando a Android para instalar la app Sapphire Dual Boot (activa la depuración USB si la pide)", "Waiting for Android to install the Sapphire Dual Boot app (allow USB debugging if asked)"));
					if (WaitFor("device", 300, ct)) {
						Adb.Run(Paths.Adb, "wait-for-device shell while [ \"$(getprop sys.boot_completed)\" != 1 ]; do sleep 2; done", out o, 300000);
						if (Adb.Run(Paths.Adb, "install -r " + Adb.Quote(apk), out o, 120000) == 0) {
							Say(L.T("App instalada. Ábrela para cambiar a iOS cuando quieras.", "App installed. Open it to switch to iOS whenever you want."));
						} else {
							Say(L.T("No se pudo instalar la app: ", "The app could not be installed: ") + o);
						}
					} else {
						Say(L.T("Android no apareció por adb: instala la app luego desde la release.", "Android did not show up over adb: install the app later from the release."));
					}
				}
			}
		}

		public static PhoneInfo LastInfo;

		void SendDmg(ZipArchive z, string entry, string name, CancellationToken ct)
		{
			ZipArchiveEntry e = z.GetEntry(entry);
			Say(L.T("Enviando ", "Sending ") + entry + " (" + Fmt.Gb(e.Length) + ")");
			using (Stream s = e.Open()) {
				Send("recv " + name + " " + e.Length, s, e.Length, ct);
			}
		}

		static bool WaitFor(string state, int seconds, CancellationToken ct)
		{
			for (int i = 0; i < seconds; i++) {
				ct.ThrowIfCancellationRequested();
				string s;
				if (Adb.State(out s) == state) {
					Adb.Serial = s;
					return true;
				}
				Thread.Sleep(1000);
			}
			return false;
		}
	}
}
