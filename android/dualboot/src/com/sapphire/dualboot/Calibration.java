package com.sapphire.dualboot;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * This phone's factory calibration, copied to iOS. Every phone has its own: the fingerprint sensor's
 * (Goodix, persist/goodix/gf_*.so: without them every touch fails with 1004) and the other sensors'
 * (persist/sensors). persist is only read.
 *
 * They go to iOS as an update in the inbox (IpaInbox), sapphire-calibration.supdate: a .tar.gz with
 * update.sh, fp/ and sensors/, which iOS applies at its next boot (userland/update/sapphire-update.sh,
 * kernel 0310 or later): the factory calibration (gf_cali_*) and the sensors' files are put in place,
 * the fingerprint files iOS refines by itself (gf_dynamic_*, gf_extra_*) only where missing. The
 * enrolled fingerprints are never touched. The installers' update mode sends the same thing.
 */
final class Calibration {
	static final String NAME = "sapphire-calibration.supdate";
	private static final String PERSIST = "/mnt/vendor/persist";
	private static final String WORK = "/data/local/tmp/sapphire-cal";

	private static final String UPDATE_SH = String.join("\n",
	    "#!/var/jb/bin/bash",
	    "# this phone's factory calibration, from Android's persist (Sapphire Dual Boot): never the enrolled fingers",
	    "D=$1",
	    "mkdir -p /var/root/fp",
	    "chmod 700 /var/root/fp",
	    "n=0",
	    "for f in $D/fp/*.so; do",
	    "\t[ -f \"$f\" ] || continue",
	    "\tb=${f##*/}",
	    "\tcase $b in",
	    "\tgf_cali*) ;;",
	    "\t*) [ -e /var/root/fp/$b ] && continue ;;",
	    "\tesac",
	    "\tcp \"$f\" /var/root/fp/$b && chmod 600 /var/root/fp/$b && n=$((n + 1))",
	    "done",
	    "echo \"fingerprint calibration: $n files\"",
	    "if [ -d $D/sensors ]; then",
	    "\tmkdir -p /var/root/sensfiles/mnt/vendor/persist",
	    "\trm -rf /var/root/sensfiles/mnt/vendor/persist/sensors",
	    "\tcp -R $D/sensors /var/root/sensfiles/mnt/vendor/persist/",
	    "\tchown -R 0:0 /var/root/sensfiles",
	    "\techo \"sensors calibration: copied\"",
	    "fi",
	    "launchctl kickstart -k system/com.sapphire.fpd 2>/dev/null",
	    "exit 0",
	    "");

	/** Packs the calibration and leaves it in iOS's inbox; returns a line for the user. */
	static String copyToIos(File cacheDir, IpaInbox.Progress p) throws IOException {
		Root.run("rm -rf " + WORK + " && mkdir -p " + WORK + "/fp && cat > " + WORK + "/update.sh && chmod 755 " + WORK + "/update.sh",
		    UPDATE_SH.getBytes(StandardCharsets.UTF_8));
		String counted = Root.text("n=0; for f in " + PERSIST + "/goodix/gf_*.so; do [ -f \"$f\" ] && cp \"$f\" " + WORK + "/fp/ && n=$((n+1)); done; "
		    + "s=0; [ -d " + PERSIST + "/sensors ] && cp -r " + PERSIST + "/sensors " + WORK + "/ && s=1; echo $n $s").trim();
		String[] f = counted.split("\\s+");
		int fp = f.length > 0 ? parse(f[0]) : 0;
		boolean sensors = f.length > 1 && "1".equals(f[1]);
		if (fp == 0 && !sensors) {
			Root.text("rm -rf " + WORK);
			throw new IOException(L.t("no hay calibración en " + PERSIST + " (¿persist sin montar?)", "there is no calibration in " + PERSIST + " (persist not mounted?)"));
		}
		byte[] tgz = Root.run("cd " + WORK + " && tar -czf - update.sh fp" + (sensors ? " sensors" : "") + " ; rm -rf " + WORK, null);
		if (tgz.length < 64 || (tgz[0] & 0xff) != 0x1f || (tgz[1] & 0xff) != 0x8b) {
			throw new IOException(L.t("no se pudo empaquetar la calibración", "the calibration could not be packed"));
		}
		File out = new File(cacheDir, "calibration.tgz");
		try (FileOutputStream o = new FileOutputStream(out)) {
			o.write(tgz);
		}
		try {
			IpaInbox box = IpaInbox.device();
			for (int i = box.entries.size() - 1; i >= 0; i--) {
				if (NAME.equals(box.entries.get(i).name)) {
					box = box.remove(i);
				}
			}
			box.add(out, NAME, p);
		} finally {
			out.delete();
		}
		return L.t("Calibración lista para iOS (huella: " + fp + " archivos" + (sensors ? ", sensores" : "") + "). Se aplica la próxima vez que arranques iOS.",
		    "Calibration ready for iOS (fingerprint: " + fp + " files" + (sensors ? ", sensors" : "") + "). It is applied the next time iOS starts.");
	}

	private static int parse(String s) {
		try {
			return Integer.parseInt(s);
		} catch (NumberFormatException e) {
			return 0;
		}
	}
}
