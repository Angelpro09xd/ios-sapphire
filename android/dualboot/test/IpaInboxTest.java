package com.sapphire.dualboot;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Random;

/**
 * IpaInboxTest: the inbox's layout and range checks against a plain FILE standing in for iosroot (on the PC,
 * never on the phone). The fake partition: an HFSX volume header saying 4 MiB, then 28 MiB more, the last
 * 1025 blocks being the "log ring". Checks that nothing outside [S, E) ever changes.
 *   java -cp out/test:$ANDROID_JAR com.sapphire.dualboot.IpaInboxTest   (test/run-tests.sh)
 */
public class IpaInboxTest {
	static int failures;

	static final class FileDev implements IpaInbox.Dev {
		final RandomAccessFile f;
		int writes;

		FileDev(File file) throws IOException {
			f = new RandomAccessFile(file, "rw");
		}

		public long size() throws IOException {
			return f.length();
		}

		public byte[] read(long off, int len) throws IOException {
			if (off % 4096 != 0 || len % 4096 != 0) {
				throw new IOException("unaligned read");
			}
			byte[] b = new byte[len];
			f.seek(off);
			f.readFully(b);
			return b;
		}

		public void write(long off, byte[] data) throws IOException {
			writes++;
			f.seek(off);
			f.write(data);
		}

		public void sync() throws IOException {
			f.getFD().sync();
		}
	}

	static void check(boolean ok, String what) {
		System.out.println((ok ? "ok   " : "FAIL ") + what);
		if (!ok) {
			failures++;
		}
	}

	static void expectFail(String what, Runnable2 r) {
		try {
			r.run();
			check(false, what + " (was not refused)");
		} catch (IOException e) {
			check(true, what + " -> " + e.getMessage());
		}
	}

	interface Runnable2 {
		void run() throws IOException;
	}

	static byte[] digest(RandomAccessFile f, long from, long to) throws Exception {
		MessageDigest md = MessageDigest.getInstance("SHA-256");
		byte[] b = new byte[1 << 20];
		f.seek(from);
		for (long p = from; p < to; ) {
			int n = (int) Math.min(b.length, to - p);
			f.readFully(b, 0, n);
			md.update(b, 0, n);
			p += n;
		}
		return md.digest();
	}

	static File ipa(File dir, String name, int len, long seed) throws IOException {
		File f = new File(dir, name);
		byte[] b = new byte[len];
		new Random(seed).nextBytes(b);
		try (FileOutputStream o = new FileOutputStream(f)) {
			o.write(b);
		}
		return f;
	}

	static void fakeVolume(RandomAccessFile f, int blockSize, int blocks) throws IOException {
		byte[] vh = new byte[512];
		vh[0] = 'H';
		vh[1] = 'X';
		ByteBuffer.wrap(vh).putInt(40, blockSize).putInt(44, blocks);
		f.seek(1024);
		f.write(vh);
	}

	public static void main(String[] a) throws Exception {
		long MIB = 1 << 20;
		File dir = new File(a.length > 0 ? a[0] : System.getProperty("java.io.tmpdir"), "ipainbox-test");
		dir.mkdirs();
		File part = new File(dir, "iosroot.img");
		part.delete();
		long P = 4 * MIB + 1025 * 4096L + 28 * MIB;
		try (RandomAccessFile f = new RandomAccessFile(part, "rw")) {
			f.setLength(P);
			/* recognisable bytes on both sides of the inbox, to see that they never change */
			byte[] junk = new byte[1 << 20];
			new Random(7).nextBytes(junk);
			for (long o = 0; o < P; o += junk.length) {
				f.seek(o);
				f.write(junk, 0, (int) Math.min(junk.length, P - o));
			}
			fakeVolume(f, 4096, 1024);           /* V = 4 MiB */
		}
		FileDev dev = new FileDev(part);
		IpaInbox.Geometry g = IpaInbox.geometry(dev);
		long S = 5 * MIB, E = ((P - 1025 * 4096L - MIB) / MIB) * MIB;
		check(g.volumeEnd == 4 * MIB, "V = 4 MiB");
		check(g.start == S, "S = roundUp(V, 1 MiB) + 1 MiB = " + g.start);
		check(g.end == E && E == 31 * MIB, "E = (P - 1025*4096 - 1 MiB) rounded down = " + g.end);
		byte[] before = digest(dev.f, 0, S), after = digest(dev.f, E, P);

		IpaInbox box = IpaInbox.open(dev);
		check(!box.present && box.entries.isEmpty() && box.damaged == null, "a fresh partition has no inbox (random bytes, no magic)");
		check(box.largestFree() == E - S - 4096, "free = inbox - header");

		File a1 = ipa(dir, "A.ipa", 1_000_001, 1), a2 = ipa(dir, "B.ipa", 3 * 4096, 2), a3 = ipa(dir, "C.ipa", 5_500_000, 3);
		box = box.add(a1, "Alpha.ipa", null);
		box = box.add(a2, "Beta.ipa", null);
		box = box.add(a3, "Gamma.ipa", null);
		check(box.present && box.entries.size() == 3, "three entries");
		IpaInbox.Entry e0 = box.entries.get(0), e1 = box.entries.get(1), e2 = box.entries.get(2);
		check(e0.offset == 4096, "first data right after the header");
		check(e1.offset == 4096 + IpaInbox.roundUp(1_000_001, 4096), "second packed after the first, 4096-aligned: " + e1.offset);
		check(e2.offset == e1.offset + 3 * 4096, "third packed after the second");
		check(Arrays.equals(e0.sha, IpaInbox.sha256(a1)) && Arrays.equals(box.hash(e2, null), IpaInbox.sha256(a3)), "sha256 of the bytes on the partition");
		check(e0.state == IpaInbox.PENDING && e0.name.equals("Alpha.ipa"), "pending, name kept");

		/* the raw header, byte by byte, as iOS will read it */
		byte[] h = dev.read(S, 4096);
		ByteBuffer hb = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
		check(new String(h, 0, 8, "US-ASCII").equals("SPHIPAQ1") && hb.getInt(8) == 1 && hb.getInt(12) == 3 && hb.getLong(16) == E - S,
		    "header: magic, version 1, count 3, inboxBytes");
		check(hb.getLong(24 + 256) == e1.offset && hb.getLong(24 + 256 + 8) == 3 * 4096 && new String(h, 24 + 256 + 56, 8, "UTF-8").equals("Beta.ipa"),
		    "entry 1 at 24+256: offset, length, name at +56");
		byte[] onDisk = new byte[1_000_001];
		dev.f.seek(S + 4096);
		dev.f.readFully(onDisk);
		check(Arrays.equals(onDisk, java.nio.file.Files.readAllBytes(a1.toPath())), "data bytes at S + 4096");

		/* removing frees the gap, which the next IPA that fits reuses */
		box = box.remove(1);
		check(box.entries.size() == 2, "removed one");
		File small = ipa(dir, "D.ipa", 2 * 4096 + 5, 4);
		box = box.add(small, "Delta.ipa", null);
		check(box.entries.get(2).offset == e1.offset, "the gap reused: " + box.entries.get(2).offset);
		File big = ipa(dir, "E.ipa", 4 * 4096, 5);
		box = box.add(big, "Epsilon.ipa", null);
		check(box.entries.get(3).offset == e2.offset + IpaInbox.roundUp(5_500_000, 4096), "too big for the gap: goes to the end");

		/* refusals */
		File huge = ipa(dir, "H.ipa", (int) (E - S), 6);
		IpaInbox fb = box;
		expectFail("an IPA larger than the free space", () -> fb.add(huge, "Huge.ipa", null));
		expectFail("write just before S", () -> fb.checkedWrite(S - 4096, new byte[4096]));
		expectFail("write over E", () -> fb.checkedWrite(E - 4096, new byte[8192]));
		expectFail("write at E", () -> fb.checkedWrite(E, new byte[4096]));
		expectFail("write into the volume", () -> fb.checkedWrite(0, new byte[4096]));
		expectFail("write into the log ring", () -> fb.checkedWrite(P - 4096, new byte[4096]));
		expectFail("unaligned write", () -> fb.checkedWrite(S + 512, new byte[4096]));
		expectFail("unaligned length", () -> fb.checkedWrite(S, new byte[100]));
		expectFail("empty write", () -> fb.checkedWrite(S, new byte[0]));
		expectFail("overflowing range", () -> fb.checkedWrite(Long.MAX_VALUE - 4095, new byte[4096]));

		/* iOS marks one installed and one failed; "limpiar instaladas" keeps the failed one */
		byte[] hh = dev.read(S, 4096);
		ByteBuffer.wrap(hh).order(ByteOrder.LITTLE_ENDIAN).putInt(24 + 48, 1).putInt(24 + 256 + 48, 2);
		dev.f.seek(S);
		dev.f.write(hh);
		box = IpaInbox.open(dev);
		check(box.entries.get(0).state == IpaInbox.INSTALLED && box.entries.get(1).state == IpaInbox.FAILED, "states read: instalada, falló");
		box = box.removeInstalled();
		check(box.entries.size() == 3 && box.entries.get(0).name.equals("Gamma.ipa"), "installed ones cleared");

		/* at most 15 */
		box = box.clear();
		File tiny = ipa(dir, "T.ipa", 10, 8);
		for (int i = 0; i < 15; i++) {
			box = box.add(tiny, "t" + i + ".ipa", null);
		}
		IpaInbox full = box;
		expectFail("a 16th IPA", () -> full.add(tiny, "t15.ipa", null));

		/* names: UTF-8 cut at a character boundary, NUL left */
		String longName = "ñ".repeat(150) + ".ipa";
		byte[] nb = IpaInbox.nameBytes(longName);
		check(nb.length <= 191 && nb.length % 2 == 0 && new String(nb, "UTF-8").equals("ñ".repeat(95)), "long name cut at 190 bytes");
		box = box.clear().add(tiny, longName, null);
		check(box.entries.get(0).name.equals("ñ".repeat(95)), "long name read back");

		/* a damaged header is reported and must be cleared first */
		hh = dev.read(S, 4096);
		ByteBuffer.wrap(hh).order(ByteOrder.LITTLE_ENDIAN).putLong(24, 3);    /* unaligned offset */
		dev.f.seek(S);
		dev.f.write(hh);
		box = IpaInbox.open(dev);
		check(box.present && box.damaged != null, "damaged header noticed: " + box.damaged);
		IpaInbox db = box;
		expectFail("adding to a damaged inbox", () -> db.add(tiny, "x.ipa", null));
		box = box.clear();
		check(box.entries.isEmpty() && box.damaged == null, "cleared");

		/* the geometry changing under an open inbox (the volume grown) stops every write */
		IpaInbox stale = box;
		try (RandomAccessFile f = new RandomAccessFile(part, "rw")) {
			fakeVolume(f, 4096, 1024 + 512);
		}
		expectFail("write after the volume grew", () -> stale.add(tiny, "x.ipa", null));
		check(IpaInbox.geometry(dev).start == 7 * MIB, "new S follows the volume (6 MiB end -> 7 MiB)");
		try (RandomAccessFile f = new RandomAccessFile(part, "rw")) {
			fakeVolume(f, 4096, 1024);
		}

		/* outside [S, E): never touched */
		check(Arrays.equals(digest(dev.f, 0, S), before), "bytes before S unchanged (volume)");
		check(Arrays.equals(digest(dev.f, E, P), after), "bytes from E on unchanged (guard + log ring)");

		/* no room at all */
		File tight = new File(dir, "tight.img");
		try (RandomAccessFile f = new RandomAccessFile(tight, "rw")) {
			f.setLength(4 * MIB + 1025 * 4096L + 9 * MIB);
			fakeVolume(f, 4096, 1024);
		}
		expectFail("partition with < 8 MiB after the volume", () -> IpaInbox.open(new FileDev(tight)));
		File nohfs = new File(dir, "nohfs.img");
		try (RandomAccessFile f = new RandomAccessFile(nohfs, "rw")) {
			f.setLength(64 * MIB);
		}
		expectFail("no HFS+ volume", () -> IpaInbox.open(new FileDev(nohfs)));

		System.out.println(failures == 0 ? "ALL PASSED" : failures + " FAILED");
		System.exit(failures == 0 ? 0 : 1);
	}
}
