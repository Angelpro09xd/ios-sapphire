package com.sapphire.dualboot;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * The IPA inbox: Android cannot write into iOS's HFS+ volume (journaled, iOS's own), so apps for iOS go to the
 * raw space of the iosroot partition AFTER the volume, where iOS reads them at boot and installs them.
 *
 *   V = volume end (totalBlocks * blockSize from the volume header), P = partition size
 *   S = V rounded up to 1 MiB, + 1 MiB of guard
 *   E = P - 1025 * 4096 (the iOS kernel's console log ring at the partition's end) - 1 MiB of guard, rounded down to 1 MiB
 *
 * At S: a 4096-byte header (little-endian) then the data.
 *   0 "SPHIPAQ1" | 8 u32 version = 1 | 12 u32 count | 16 u64 inboxBytes (E - S) | 24 count entries of 256 bytes:
 *     0 u64 dataOffset (from S, 4096-aligned, >= 4096) | 8 u64 length | 16 u8[32] sha256 | 48 u32 state
 *     (0 pending, 1 installed, 2 failed: iOS sets 1 and 2) | 52 u32 reserved | 56 u8[192] UTF-8 name, NUL-padded |
 *     248 8 bytes of zeros
 *
 * Safety: every write goes through checkedWrite(), which reads the geometry again from the partition and
 * refuses anything not 4096-aligned or not wholly inside [S, E). The data is written (and synced) before the
 * header that points at it, so a header on the disk never names data that is not there; the new data only
 * goes to space no current entry uses. Afterwards everything is read back and its sha256 checked.
 * The device is behind Dev, so the layout is tested on the PC against a plain file (test/IpaInboxTest.java).
 */
final class IpaInbox {
	static final long MIB = 1 << 20;
	static final int BLOCK = 4096, HEADER = 4096, ENTRY = 256, ENTRIES_AT = 24, NAME_AT = 56, NAME_MAX = 192;
	static final int MAX_ENTRIES = 15, VERSION = 1;
	static final int PENDING = 0, INSTALLED = 1, FAILED = 2;
	static final byte[] MAGIC = "SPHIPAQ1".getBytes(StandardCharsets.US_ASCII);
	private static final int CHUNK = 4 << 20;

	/** Where the inbox lives: a block device (or, in the tests, a file standing in for one). */
	interface Dev {
		long size() throws IOException;

		/** LEN bytes from OFF (both multiples of 4096). */
		byte[] read(long off, int len) throws IOException;

		/** DATA at OFF; only ever called by checkedWrite. */
		void write(long off, byte[] data) throws IOException;

		void sync() throws IOException;
	}

	interface Progress {
		void on(String what, long done, long total);
	}

	/** One queued IPA. */
	static final class Entry {
		long offset, length;
		byte[] sha = new byte[32];
		int state;
		String name;

		String stateName() {
			return state == PENDING ? L.t("pendiente", "pending") : state == INSTALLED ? L.t("instalada", "installed") : state == FAILED ? L.t("falló", "failed") : L.t("estado ", "state ") + state;
		}

		String shaHex() {
			return hex(sha);
		}
	}

	static String hex(byte[] d) {
		StringBuilder s = new StringBuilder();
		for (byte b : d) {
			s.append(String.format("%02x", b & 0xff));
		}
		return s.toString();
	}

	/** S and E (absolute offsets in the partition), with what they came from. */
	static final class Geometry {
		final long volumeEnd, partition, start, end;

		Geometry(long volumeEnd, long partition) throws IOException {
			this.volumeEnd = volumeEnd;
			this.partition = partition;
			start = roundUp(volumeEnd, MIB) + MIB;
			end = ((partition - Ios.LOG_BLOCKS * (long) BLOCK - MIB) / MIB) * MIB;
			if (volumeEnd <= 0 || volumeEnd > partition) {
				throw new IOException(L.t("el volumen de iOS (", "the iOS volume (") + volumeEnd + L.t(") no cabe en su partición (", ") does not fit in its partition (") + partition + ")");
			}
			if (end <= start + 8 * MIB) {
				throw new IOException(L.t("no hay sitio para la bandeja tras el volumen de iOS (", "no room for the inbox after the iOS volume (") + Ui.size(Math.max(0, end - start)) + ")");
			}
		}

		long bytes() {
			return end - start;
		}

		@Override
		public boolean equals(Object o) {
			return o instanceof Geometry && ((Geometry) o).start == start && ((Geometry) o).end == end
			    && ((Geometry) o).volumeEnd == volumeEnd && ((Geometry) o).partition == partition;
		}

		@Override
		public int hashCode() {
			return Long.hashCode(start) ^ Long.hashCode(end);
		}
	}

	final Dev dev;
	final Geometry geometry;
	final List<Entry> entries;
	/** Why the header found at S was not used (null: a valid header, or none written yet). */
	final String damaged;
	final boolean present;

	private IpaInbox(Dev dev, Geometry g, List<Entry> entries, boolean present, String damaged) {
		this.dev = dev;
		this.geometry = g;
		this.entries = Collections.unmodifiableList(entries);
		this.present = present;
		this.damaged = damaged;
	}

	/** The geometry read afresh from the partition: the HFS+ volume header and the partition size. */
	static Geometry geometry(Dev d) throws IOException {
		byte[] vh = d.read(0, BLOCK);
		String sig = new String(vh, 1024, 2, StandardCharsets.US_ASCII);
		if (!sig.equals("H+") && !sig.equals("HX")) {
			throw new IOException(L.t("no hay un volumen HFS+ al principio de la partición de iOS: no se toca nada", "no HFS+ volume at the start of the iOS partition: nothing is touched"));
		}
		long bs = Hfs.be32(vh, 1024 + 40) & 0xffffffffL, total = Hfs.be32(vh, 1024 + 44) & 0xffffffffL;
		if (bs < 512 || bs > 65536 || (bs & (bs - 1)) != 0 || total == 0) {
			throw new IOException(L.t("cabecera del volumen de iOS incorrecta (bloque ", "bad iOS volume header (block ") + bs + ")");
		}
		return new Geometry(bs * total, d.size());
	}

	static IpaInbox open(Dev d) throws IOException {
		Geometry g = geometry(d);
		byte[] h = d.read(g.start, HEADER);
		if (!Arrays.equals(Arrays.copyOf(h, 8), MAGIC)) {
			return new IpaInbox(d, g, new ArrayList<>(), false, null);
		}
		try {
			return new IpaInbox(d, g, parse(h, g.bytes()), true, null);
		} catch (IOException e) {
			return new IpaInbox(d, g, new ArrayList<>(), true, e.getMessage());
		}
	}

	/** The entries of header H, every one checked against an inbox of INBOX bytes. */
	static List<Entry> parse(byte[] h, long inbox) throws IOException {
		ByteBuffer b = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
		int version = b.getInt(8), count = b.getInt(12);
		if (version != VERSION) {
			throw new IOException(L.t("bandeja de versión ", "inbox of unknown version ") + version + L.t(" desconocida", ""));
		}
		if (count < 0 || count > MAX_ENTRIES) {
			throw new IOException(L.t("bandeja con ", "inbox with ") + count + L.t(" entradas", " entries"));
		}
		List<Entry> l = new ArrayList<>();
		for (int i = 0; i < count; i++) {
			int o = ENTRIES_AT + i * ENTRY;
			Entry e = new Entry();
			e.offset = b.getLong(o);
			e.length = b.getLong(o + 8);
			System.arraycopy(h, o + 16, e.sha, 0, 32);
			e.state = b.getInt(o + 48);
			int n = 0;
			while (n < NAME_MAX && h[o + NAME_AT + n] != 0) {
				n++;
			}
			e.name = new String(h, o + NAME_AT, n, StandardCharsets.UTF_8);
			if (e.offset < HEADER || e.offset % BLOCK != 0 || e.length <= 0 || e.offset + e.length > inbox
			    || e.offset + e.length < e.offset) {
				throw new IOException(L.t("entrada ", "entry ") + i + L.t(" («", " (“") + e.name + L.t("») fuera de la bandeja", "”) outside the inbox"));
			}
			l.add(e);
		}
		/* no two entries may share bytes */
		List<Entry> s = sorted(l);
		for (int i = 1; i < s.size(); i++) {
			if (s.get(i).offset < roundUp(s.get(i - 1).offset + s.get(i - 1).length, BLOCK)) {
				throw new IOException(L.t("entradas solapadas en la bandeja", "overlapping entries in the inbox"));
			}
		}
		return l;
	}

	static byte[] header(List<Entry> l, long inbox) throws IOException {
		if (l.size() > MAX_ENTRIES) {
			throw new IOException(L.t("máximo ", "at most ") + MAX_ENTRIES + L.t(" IPAs en la bandeja", " IPAs in the inbox"));
		}
		byte[] h = new byte[HEADER];
		ByteBuffer b = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
		b.put(MAGIC).putInt(VERSION).putInt(l.size()).putLong(inbox);
		for (int i = 0; i < l.size(); i++) {
			Entry e = l.get(i);
			int o = ENTRIES_AT + i * ENTRY;
			b.putLong(o, e.offset).putLong(o + 8, e.length);
			System.arraycopy(e.sha, 0, h, o + 16, 32);
			b.putInt(o + 48, e.state).putInt(o + 52, 0);
			byte[] n = nameBytes(e.name);
			System.arraycopy(n, 0, h, o + NAME_AT, n.length);
		}
		parse(h, inbox);        /* what is written must read back as valid */
		return h;
	}

	/** NAME in UTF-8, cut at a character boundary to leave room for the NUL. */
	static byte[] nameBytes(String name) {
		byte[] n = name.getBytes(StandardCharsets.UTF_8);
		if (n.length < NAME_MAX) {
			return n;
		}
		int len = NAME_MAX - 1;
		while (len > 0 && (n[len] & 0xC0) == 0x80) {
			len--;              /* not in the middle of a multi-byte character */
		}
		return Arrays.copyOf(n, len);
	}

	/* ---------------- space ---------------- */

	long used() {
		long u = 0;
		for (Entry e : entries) {
			u += roundUp(e.length, BLOCK);
		}
		return u;
	}

	/** The largest IPA that fits now (the largest gap between entries). */
	long largestFree() {
		long best = 0, cursor = HEADER;
		for (Entry e : sorted(entries)) {
			best = Math.max(best, e.offset - cursor);
			cursor = Math.max(cursor, roundUp(e.offset + e.length, BLOCK));
		}
		return Math.max(best, geometry.bytes() - cursor);
	}

	/** The first gap (offset from S) that holds LEN bytes, or -1. */
	static long allocate(List<Entry> l, long inbox, long len) {
		long need = roundUp(len, BLOCK), cursor = HEADER;
		for (Entry e : sorted(l)) {
			if (e.offset - cursor >= need) {
				return cursor;
			}
			cursor = Math.max(cursor, roundUp(e.offset + e.length, BLOCK));
		}
		return inbox - cursor >= need ? cursor : -1;
	}

	/* ---------------- changes ---------------- */

	/** FILE queued as NAME; returns the inbox as it is on the disk afterwards. */
	IpaInbox add(File file, String name, Progress p) throws IOException {
		if (damaged != null) {
			throw new IOException(L.t("la cabecera de la bandeja está dañada (", "the inbox header is damaged (") + damaged + L.t("): vacíala antes", "): empty it first"));
		}
		if (entries.size() >= MAX_ENTRIES) {
			throw new IOException(L.t("la bandeja ya tiene ", "the inbox already holds ") + MAX_ENTRIES + " IPAs");
		}
		long len = file.length();
		if (len <= 0) {
			throw new IOException(L.t("fichero vacío", "empty file"));
		}
		long off = allocate(entries, geometry.bytes(), len);
		if (off < 0) {
			throw new IOException(L.t("no cabe: ", "does not fit: ") + Ui.size(len) + L.t(", y el hueco mayor de la bandeja es de ", ", and the largest gap in the inbox is ") + Ui.size(largestFree()));
		}
		MessageDigest md = sha256();
		try (InputStream in = new FileInputStream(file)) {
			byte[] buf = new byte[CHUNK];
			for (long done = 0; done < len; ) {
				int n = (int) Math.min(CHUNK, len - done), got = 0;
				while (got < n) {
					int k = in.read(buf, got, n - got);
					if (k < 0) {
						throw new IOException(L.t("el fichero se acortó mientras se copiaba", "the file got shorter while it was being copied"));
					}
					got += k;
				}
				md.update(buf, 0, n);
				/* the last piece padded with zeros to a whole block: the padding is inside this entry's space */
				byte[] w = Arrays.copyOf(buf, (int) roundUp(n, BLOCK));
				if (w.length > n) {
					Arrays.fill(w, n, w.length, (byte) 0);
				}
				checkedWrite(geometry.start + off + done, w);
				done += n;
				if (p != null) {
					p.on(L.t("Copiando", "Copying"), done, len);
				}
			}
			if (in.read() >= 0) {
				throw new IOException(L.t("el fichero creció mientras se copiaba", "the file grew while it was being copied"));
			}
		}
		dev.sync();
		Entry e = new Entry();
		e.offset = off;
		e.length = len;
		e.sha = md.digest();
		e.state = PENDING;
		e.name = name;
		List<Entry> l = new ArrayList<>(entries);
		l.add(e);
		IpaInbox after = commit(l);
		/* the data as it is on the partition now */
		if (!Arrays.equals(after.hash(e, p), e.sha)) {
			List<Entry> undo = new ArrayList<>(after.entries);
			undo.remove(undo.size() - 1);
			commit(undo);
			throw new IOException(L.t("lo leído de la partición no coincide con la IPA: se ha quitado de la bandeja", "what was read back from the partition does not match the IPA: it was removed from the inbox"));
		}
		return after;
	}

	/** The inbox without entry I (its space becomes free; nothing but the header is written). */
	IpaInbox remove(int i) throws IOException {
		if (i < 0 || i >= entries.size()) {
			throw new IOException(L.t("entrada inexistente", "no such entry"));
		}
		List<Entry> l = new ArrayList<>(entries);
		l.remove(i);
		return commit(l);
	}

	/** The inbox without the entries iOS has installed. */
	IpaInbox removeInstalled() throws IOException {
		List<Entry> l = new ArrayList<>();
		for (Entry e : entries) {
			if (e.state != INSTALLED) {
				l.add(e);
			}
		}
		return commit(l);
	}

	/** An empty header (also the way out of a damaged one). */
	IpaInbox clear() throws IOException {
		return commit(new ArrayList<>());
	}

	/** Writes the header for L, syncs, reads it back and checks it. */
	private IpaInbox commit(List<Entry> l) throws IOException {
		byte[] h = header(l, geometry.bytes());
		checkedWrite(geometry.start, h);
		dev.sync();
		IpaInbox now = open(dev);
		if (!now.present || now.damaged != null || !Arrays.equals(dev.read(geometry.start, HEADER), h)) {
			throw new IOException(L.t("la cabecera leída no coincide con la escrita", "the header read back does not match the one written"));
		}
		return now;
	}

	/** The sha256 of entry E's bytes as they are on the partition. */
	byte[] hash(Entry e, Progress p) throws IOException {
		MessageDigest md = sha256();
		for (long done = 0; done < e.length; ) {
			int n = (int) Math.min(CHUNK, e.length - done);
			byte[] b = dev.read(geometry.start + e.offset + done, (int) roundUp(n, BLOCK));
			md.update(b, 0, n);
			done += n;
			if (p != null) {
				p.on(L.t("Comprobando", "Checking"), done, e.length);
			}
		}
		return md.digest();
	}

	/**
	 * The one way anything is written: DATA at absolute OFF, only if the partition still has the same
	 * geometry (read again now) and [OFF, OFF + DATA.length) lies wholly within [S, E).
	 */
	void checkedWrite(long off, byte[] data) throws IOException {
		Geometry now = geometry(dev);
		if (!now.equals(geometry)) {
			throw new IOException(L.t("la partición de iOS ha cambiado desde que se leyó: no se escribe nada", "the iOS partition changed since it was read: nothing is written"));
		}
		long end = off + data.length;
		if (data.length == 0 || off % BLOCK != 0 || data.length % BLOCK != 0 || end < off || off < now.start || end > now.end) {
			throw new IOException(L.t("escritura fuera de la bandeja rechazada (", "write outside the inbox refused (") + off + "+" + data.length + L.t(", bandeja ", ", inbox ") + now.start
			    + ".." + now.end + ")");
		}
		dev.write(off, data);
	}

	/* ---------------- helpers ---------------- */

	static List<Entry> sorted(List<Entry> l) {
		List<Entry> s = new ArrayList<>(l);
		Collections.sort(s, (a, b) -> Long.compare(a.offset, b.offset));
		return s;
	}

	static long roundUp(long v, long to) {
		return (v + to - 1) / to * to;
	}

	static MessageDigest sha256() {
		try {
			return MessageDigest.getInstance("SHA-256");
		} catch (NoSuchAlgorithmException e) {
			throw new IllegalStateException(e);
		}
	}

	static byte[] sha256(File f) throws IOException {
		MessageDigest md = sha256();
		try (InputStream in = new FileInputStream(f)) {
			byte[] buf = new byte[1 << 20];
			for (int n; (n = in.read(buf)) > 0; ) {
				md.update(buf, 0, n);
			}
		}
		return md.digest();
	}

	/** iOS's root partition, through root: reads by the persistent shell, writes by dd (only via checkedWrite). */
	static final class BlockDev implements Dev {
		final String path;

		BlockDev(String path) {
			this.path = path;
		}

		@Override
		public long size() throws IOException {
			try {
				return Long.parseLong(Root.text("blockdev --getsize64 " + path));
			} catch (NumberFormatException e) {
				throw new IOException(L.t("tamaño de ", "size of ") + path + L.t(" ilegible", " unreadable"));
			}
		}

		@Override
		public byte[] read(long off, int len) throws IOException {
			return RootShell.read(path, off, len);
		}

		@Override
		public void write(long off, byte[] data) throws IOException {
			if (off % BLOCK != 0 || data.length % BLOCK != 0) {
				throw new IOException(L.t("escritura no alineada", "unaligned write"));
			}
			/* no count=: dd writes exactly what it is given on stdin, contiguously from the seek */
			Root.run("dd of=" + path + " bs=" + BLOCK + " seek=" + (off / BLOCK) + " conv=notrunc,fsync 2>/dev/null", data);
		}

		@Override
		public void sync() throws IOException {
			Root.text("sync");
		}
	}

	static IpaInbox device() throws IOException {
		return open(new BlockDev(Ios.ROOT));
	}
}
