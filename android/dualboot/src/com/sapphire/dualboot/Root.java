package com.sapphire.dualboot;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/** Commands run as root through Magisk's (or KernelSU's) su. */
final class Root {
	private Root() {
	}

	/** Runs CMD as root with IN (or nothing) on its stdin; returns its stdout. Throws if it fails. */
	static byte[] run(String cmd, byte[] in) throws IOException {
		Process p = new ProcessBuilder("su", "-c", cmd).start();
		ByteArrayOutputStream err = new ByteArrayOutputStream();
		Thread errReader = new Thread(() -> copy(p.getErrorStream(), err));
		errReader.start();
		try (OutputStream o = p.getOutputStream()) {
			if (in != null) {
				o.write(in);
			}
		}
		ByteArrayOutputStream out = new ByteArrayOutputStream();
		copy(p.getInputStream(), out);
		int rc;
		try {
			rc = p.waitFor();
			errReader.join();
		} catch (InterruptedException e) {
			throw new IOException("interrupted");
		}
		if (rc != 0) {
			throw new IOException(L.t("«", "“") + cmd + L.t("» falló (", "” failed (") + rc + "): " + err.toString().trim());
		}
		return out.toByteArray();
	}

	static byte[] read(String cmd) throws IOException {
		return run(cmd, null);
	}

	static String text(String cmd) throws IOException {
		return new String(run(cmd, null)).trim();
	}

	static boolean available() {
		try {
			return text("id -u").equals("0");
		} catch (IOException e) {
			return false;
		}
	}

	private static void copy(InputStream in, ByteArrayOutputStream out) {
		byte[] buf = new byte[65536];
		try {
			for (int n; (n = in.read(buf)) > 0; ) {
				out.write(buf, 0, n);
			}
		} catch (IOException ignored) {
		}
	}
}
