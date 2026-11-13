// Tools.cs: adb/fastboot (Google's platform-tools, fetched on first use), downloads from the GitHub release,
// and reading the user's IPSW (a zip: BuildManifest.plist says which disk images are which).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using System.Xml;

namespace Sapphire
{
	static class Paths
	{
		public static readonly string Data = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SapphireInstaller");
		public static string Tools { get { return Path.Combine(Data, "platform-tools"); } }
		public static string Release { get { return Path.Combine(Data, "release"); } }
		public static string Adb { get { return Path.Combine(Tools, "adb.exe"); } }
		public static string Fastboot { get { return Path.Combine(Tools, "fastboot.exe"); } }
	}

	static class Net
	{
		public const string Repo = "Angelpro09xd/ios-sapphire";
		public const string PlatformTools = "https://dl.google.com/android/repository/platform-tools-latest-windows.zip";

		static Net()
		{
			ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
		}

		public static void Download(string url, string dest, Action<long, long> progress, CancellationToken ct)
		{
			HttpWebRequest rq = (HttpWebRequest)WebRequest.Create(url);
			rq.UserAgent = "SapphireInstaller";
			rq.AllowAutoRedirect = true;
			using (HttpWebResponse rs = (HttpWebResponse)rq.GetResponse())
			using (Stream s = rs.GetResponseStream()) {
				string tmp = dest + ".part";
				long total = rs.ContentLength, done = 0;
				using (FileStream f = File.Create(tmp)) {
					byte[] buf = new byte[1 << 16];
					int n;
					while ((n = s.Read(buf, 0, buf.Length)) > 0) {
						ct.ThrowIfCancellationRequested();
						f.Write(buf, 0, n);
						done += n;
						if (progress != null) {
							progress(done, total);
						}
					}
				}
				if (File.Exists(dest)) {
					File.Delete(dest);
				}
				File.Move(tmp, dest);
			}
		}

		public static string Get(string url)
		{
			HttpWebRequest rq = (HttpWebRequest)WebRequest.Create(url);
			rq.UserAgent = "SapphireInstaller";
			rq.Accept = "application/vnd.github+json";
			using (HttpWebResponse rs = (HttpWebResponse)rq.GetResponse())
			using (StreamReader r = new StreamReader(rs.GetResponseStream(), Encoding.UTF8)) {
				return r.ReadToEnd();
			}
		}

		/* the latest release's assets: name -> download url */
		public static Dictionary<string, string> ReleaseAssets(out string tag)
		{
			JavaScriptSerializer js = new JavaScriptSerializer();
			/* the newest release, pre-releases included (releases/latest skips those): the first that has the recovery ZIP */
			System.Collections.ArrayList all = js.Deserialize<System.Collections.ArrayList>(Get("https://api.github.com/repos/" + Repo + "/releases?per_page=10"));
			Dictionary<string, object> rel = null;
			foreach (object o in all) {
				Dictionary<string, object> r = (Dictionary<string, object>)o;
				bool draft = r.ContainsKey("draft") && (bool)r["draft"];
				if (!draft && r.ContainsKey("assets") && Get_HasAsset(r, Installer.RecoveryZip)) {
					rel = r;
					break;
				}
			}
			if (rel == null) {
				throw new Exception(L.T("No hay ninguna release de Sapphire con el instalador en github.com/", "There is no Sapphire release with the installer at github.com/") + Repo + ".");
			}
			tag = rel.ContainsKey("tag_name") ? (string)rel["tag_name"] : "?";
			Dictionary<string, string> d = new Dictionary<string, string>();
			foreach (object o in (System.Collections.ArrayList)rel["assets"]) {
				Dictionary<string, object> a = (Dictionary<string, object>)o;
				d[(string)a["name"]] = (string)a["browser_download_url"];
			}
			return d;
		}

		static bool Get_HasAsset(Dictionary<string, object> rel, string name)
		{
			foreach (object o in (System.Collections.ArrayList)rel["assets"]) {
				if ((string)((Dictionary<string, object>)o)["name"] == name) {
					return true;
				}
			}
			return false;
		}

		/* platform-tools: only adb, fastboot and the two DLLs adb needs */
		public static void EnsurePlatformTools(Action<string, double> status, CancellationToken ct)
		{
			if (File.Exists(Paths.Adb) && File.Exists(Paths.Fastboot)) {
				return;
			}
			Directory.CreateDirectory(Paths.Tools);
			string zip = Path.Combine(Paths.Data, "platform-tools.zip");
			Download(PlatformTools, zip, delegate(long d, long t) { status(L.T("Descargando adb y fastboot de Google", "Downloading adb and fastboot from Google"), t > 0 ? (double)d / t : 0); }, ct);
			using (ZipArchive z = ZipFile.OpenRead(zip)) {
				foreach (ZipArchiveEntry e in z.Entries) {
					string n = e.Name.ToLowerInvariant();
					if (n == "adb.exe" || n == "fastboot.exe" || n == "adbwinapi.dll" || n == "adbwinusbapi.dll") {
						e.ExtractToFile(Path.Combine(Paths.Tools, e.Name), true);
					}
				}
			}
			File.Delete(zip);
		}
	}

	/* a running adb/fastboot command */
	static class Adb
	{
		public static string Serial;            /* the phone, once found */

		static ProcessStartInfo Psi(string exe, string args)
		{
			ProcessStartInfo p = new ProcessStartInfo(exe, (Serial != null && exe == Paths.Adb ? "-s " + Serial + " " : "") + args);
			p.UseShellExecute = false;
			p.CreateNoWindow = true;
			p.RedirectStandardOutput = true;
			p.RedirectStandardError = true;
			p.StandardOutputEncoding = Encoding.UTF8;
			return p;
		}

		public static int Run(string exe, string args, out string output, int timeoutMs)
		{
			using (Process p = Process.Start(Psi(exe, args))) {
				StringBuilder sb = new StringBuilder();
				p.OutputDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null) lock (sb) sb.AppendLine(e.Data); };
				p.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null) lock (sb) sb.AppendLine(e.Data); };
				p.BeginOutputReadLine();
				p.BeginErrorReadLine();
				if (!p.WaitForExit(timeoutMs)) {
					try { p.Kill(); } catch { }
					output = sb.ToString();
					return -1;
				}
				p.WaitForExit();
				output = sb.ToString().TrimEnd();
				return p.ExitCode;
			}
		}

		public static string Shell(string cmd)
		{
			string o;
			Run(Paths.Adb, "shell " + Quote(cmd), out o, 60000);
			return o;
		}

		public static string Quote(string s)
		{
			return "\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
		}

		/*
		 * a command on the phone whose output lines are read as they come (the installer's protocol:
		 * STEP text / PROGRESS done total / ERROR text), with stdin fed from a stream (adb exec-in: binary-safe)
		 */
		public static int Stream(string args, Stream input, long inputLength, Action<string> line, Action<long> sent, CancellationToken ct)
		{
			ProcessStartInfo psi = Psi(Paths.Adb, args);
			psi.RedirectStandardInput = input != null;
			using (Process p = Process.Start(psi)) {
				p.OutputDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null && line != null) line(e.Data); };
				p.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null && line != null) line(e.Data); };
				p.BeginOutputReadLine();
				p.BeginErrorReadLine();
				if (input != null) {
					Stream o = p.StandardInput.BaseStream;
					byte[] buf = new byte[1 << 20];
					long done = 0;
					int n;
					while (done < inputLength && (n = input.Read(buf, 0, (int)Math.Min(buf.Length, inputLength - done))) > 0) {
						if (ct.IsCancellationRequested) {
							try { p.Kill(); } catch { }
							ct.ThrowIfCancellationRequested();
						}
						o.Write(buf, 0, n);
						done += n;
						if (sent != null) {
							sent(done);
						}
					}
					o.Flush();
					o.Close();
				}
				while (!p.WaitForExit(250)) {
					if (ct.IsCancellationRequested) {
						try { p.Kill(); } catch { }
						ct.ThrowIfCancellationRequested();
					}
				}
				p.WaitForExit();
				return p.ExitCode;
			}
		}

		/* "recovery", "device" (Android), "sideload", "fastboot", or null: the first sapphire found */
		public static string State(out string serial)
		{
			string o;
			serial = null;
			if (Run(Paths.Adb, "devices", out o, 15000) == 0) {
				foreach (string l in o.Split('\n')) {
					string[] f = l.Trim().Split('\t');
					if (f.Length == 2 && f[1] != "offline" && f[1] != "unauthorized" && l.Trim() != "List of devices attached") {
						serial = f[0];
						return f[1];
					}
					if (f.Length == 2 && f[1] == "unauthorized") {
						serial = f[0];
						return "unauthorized";
					}
				}
			}
			if (Run(Paths.Fastboot, "devices", out o, 15000) == 0) {
				foreach (string l in o.Split('\n')) {
					string[] f = l.Trim().Split('\t');
					if (f.Length == 2 && f[1].Trim() == "fastboot") {
						serial = f[0];
						return "fastboot";
					}
				}
			}
			return null;
		}
	}

	/* the user's IPSW: iOS version, and where the root file system and the app cryptex are */
	class Ipsw
	{
		public string File, Version, Build, Devices, RootDmg, SysDmg, AppDmg;
		public long RootSize, SysSize, AppSize;

		public static Ipsw Open(string path)
		{
			Ipsw w = new Ipsw();
			w.File = path;
			using (ZipArchive z = ZipFile.OpenRead(path)) {
				ZipArchiveEntry bm = z.GetEntry("BuildManifest.plist");
				if (bm == null) {
					throw new Exception(L.T("No es una IPSW: falta BuildManifest.plist.", "Not an IPSW: BuildManifest.plist is missing."));
				}
				XmlDocument x = new XmlDocument();
				x.XmlResolver = null;
				using (Stream s = bm.Open()) {
					XmlReaderSettings xs = new XmlReaderSettings();
					xs.DtdProcessing = DtdProcessing.Ignore;
					x.Load(XmlReader.Create(s, xs));
				}
				XmlNode top = x.SelectSingleNode("/plist/dict");
				w.Version = Str(top, "ProductVersion");
				w.Build = Str(top, "ProductBuildVersion");
				XmlNode types = Val(top, "SupportedProductTypes");
				List<string> ts = new List<string>();
				if (types != null) {
					foreach (XmlNode n in types.SelectNodes("string")) {
						ts.Add(n.InnerText);
					}
				}
				w.Devices = string.Join(", ", ts.ToArray());
				if (ts.IndexOf("iPhone10,3") < 0) {
					throw new Exception(L.T("Esta IPSW es para ", "This IPSW is for ") + w.Devices + L.T(". Sapphire usa la del iPhone X (iPhone10,3).", ". Sapphire uses the iPhone X's (iPhone10,3)."));
				}
				if (!w.Version.StartsWith("16.")) {
					throw new Exception(L.T("Esta IPSW es de iOS ", "This IPSW is iOS ") + w.Version + L.T(". Sapphire funciona con iOS 16 (16.7.16).", ". Sapphire works with iOS 16 (16.7.16)."));
				}
				/* the D22AP Erase identity names the three images: OS (root), Cryptex1,SystemOS, Cryptex1,AppOS */
				XmlNode ids = Val(top, "BuildIdentities");
				if (ids != null) {
					foreach (XmlNode id in ids.SelectNodes("dict")) {
						XmlNode info = Val(id, "Info");
						XmlNode man = Val(id, "Manifest");
						if (info == null || man == null) {
							continue;
						}
						string cls = Str(info, "DeviceClass").ToLowerInvariant(), variant = Str(info, "Variant");
						if (cls != "d22ap" || variant.IndexOf("Erase", StringComparison.OrdinalIgnoreCase) < 0) {
							continue;
						}
						w.RootDmg = ImagePath(man, "OS");
						w.SysDmg = ImagePath(man, "Cryptex1,SystemOS");
						w.AppDmg = ImagePath(man, "Cryptex1,AppOS");
						break;
					}
				}
				if (w.RootDmg == null || w.SysDmg == null || w.AppDmg == null) {
					throw new Exception(L.T("La IPSW no tiene la identidad de borrado del D22AP con sus tres imágenes (sistema, SystemOS, AppOS).", "The IPSW lacks the D22AP erase identity with its three images (system, SystemOS, AppOS)."));
				}
				w.RootSize = Size(z, w.RootDmg);
				w.SysSize = Size(z, w.SysDmg);
				w.AppSize = Size(z, w.AppDmg);
			}
			return w;
		}

		static long Size(ZipArchive z, string entry)
		{
			ZipArchiveEntry e = z.GetEntry(entry);
			if (e == null) {
				throw new Exception(L.T("Falta ", "Missing ") + entry + L.T(" dentro de la IPSW (¿descarga incompleta?).", " inside the IPSW (incomplete download?)."));
			}
			return e.Length;
		}

		static XmlNode Val(XmlNode dict, string key)
		{
			foreach (XmlNode k in dict.SelectNodes("key")) {
				if (k.InnerText == key) {
					XmlNode v = k.NextSibling;
					while (v != null && v.NodeType != XmlNodeType.Element) {
						v = v.NextSibling;
					}
					return v;
				}
			}
			return null;
		}

		static string Str(XmlNode dict, string key)
		{
			XmlNode v = Val(dict, key);
			return v != null ? v.InnerText : "?";
		}

		static string ImagePath(XmlNode manifest, string key)
		{
			XmlNode e = Val(manifest, key);
			XmlNode info = e != null ? Val(e, "Info") : null;
			XmlNode p = info != null ? Val(info, "Path") : null;
			return p != null ? p.InnerText : null;
		}
	}

	/* a boot image for the iOS slot: an Android boot image ("ANDROID!") carrying the kernel */
	static class Kernel
	{
		public static string Check(string path)
		{
			FileInfo fi = new FileInfo(path);
			if (fi.Length < 4096 || fi.Length > (128L << 20)) {
				return L.T("El kernel debe ser una imagen de arranque de hasta 128 MB.", "The kernel must be a boot image of up to 128 MB.");
			}
			byte[] h = new byte[8];
			using (FileStream f = System.IO.File.OpenRead(path)) {
				f.Read(h, 0, 8);
			}
			if (Encoding.ASCII.GetString(h) != "ANDROID!") {
				return L.T("Ese archivo no es una imagen de arranque (le falta la cabecera ANDROID!).", "That file is not a boot image (no ANDROID! header).");
			}
			return null;
		}
	}

	static class Fmt
	{
		public static string Gb(long b)
		{
			if (b < (1L << 20)) {
				return Math.Max(1, (b + 1023) >> 10) + " KB";
			}
			if (b < (1L << 30)) {
				return (b >> 20) + " MB";
			}
			return (b / (double)(1L << 30)).ToString("0.#") + " GB";
		}
	}
}
