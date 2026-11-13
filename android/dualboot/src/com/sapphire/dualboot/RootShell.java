package com.sapphire.dualboot;

import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;

/**
 * One su process kept open for block reads: each read is a dd into its stdout, followed by a marker line,
 * so many small reads (HFS+ B-tree nodes) cost a dd each instead of a new su. Reads only.
 */
final class RootShell {
	private static Process proc;
	private static DataInputStream in;
	private static OutputStream out;

	private RootShell() {
	}

	private static synchronized void open() throws IOException {
		if (proc != null && proc.isAlive()) {
			return;
		}
		proc = new ProcessBuilder("su").redirectErrorStream(false).start();
		in = new DataInputStream(proc.getInputStream());
		out = proc.getOutputStream();
	}

	/** COUNT bytes of DEVICE from byte OFFSET (OFFSET and COUNT multiples of 512). */
	static synchronized byte[] read(String device, long offset, int count) throws IOException {
		if (offset % 512 != 0 || count % 512 != 0 || count <= 0) {
			throw new IOException(L.t("lectura no alineada", "unaligned read"));
		}
		open();
		int bs = (offset % 4096 == 0 && count % 4096 == 0) ? 4096 : 512;
		String cmd = "dd if='" + device + "' bs=" + bs + " skip=" + (offset / bs) + " count=" + (count / bs)
		    + " 2>/dev/null; echo; echo __SAPPHIRE_END__\n";
		out.write(cmd.getBytes());
		out.flush();
		byte[] buf = new byte[count];
		try {
			in.readFully(buf);
		} catch (IOException e) {
			close();
			throw e;
		}
		/* the newline and the marker line */
		StringBuilder line = new StringBuilder();
		for (int c; (c = in.read()) >= 0; ) {
			if (c == '\n') {
				if (line.toString().equals("__SAPPHIRE_END__")) {
					return buf;
				}
				line.setLength(0);
			} else {
				line.append((char) c);
			}
		}
		close();
		throw new IOException(L.t("el shell root se cerró", "the root shell closed"));
	}

	static synchronized void close() {
		if (proc != null) {
			proc.destroy();
		}
		proc = null;
	}
}
