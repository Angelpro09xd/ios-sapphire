package com.sapphire.dualboot;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.zip.CRC32;

/**
 * The GPT of the UFS LUN that holds boot_a/boot_b (sde on sapphire), where Qualcomm's bootloader keeps the
 * A/B state in each partition's attributes (bits as in QcomModulePkg PartitionTableUpdate.h):
 *   48-49 priority, 50 active, 51-53 retry count, 54 successful, 55 unbootable.
 * The slot whose partitions carry the active bit is booted; a slot marked successful does not use up
 * its retries, so it stays the one booted until the active bits are moved again.
 *
 * Every change is checked before it is written (signature, header and entry CRCs of both copies, the
 * two entry arrays identical), written backup first, primary last, and read back afterwards.
 */
final class Gpt {
	static final long ACTIVE = 1L << 50, SUCCESSFUL = 1L << 54, UNBOOTABLE = 1L << 55;
	static final long RETRY_MASK = 7L << 51;

	final String disk;
	final int sector;
	final byte[] header, entries, backupHeader, backupEntries;
	final long entriesLba, backupLba, backupEntriesLba;
	final int count, size;

	private Gpt(String disk, int sector, byte[] header, byte[] entries, byte[] backupHeader, byte[] backupEntries) {
		this.disk = disk;
		this.sector = sector;
		this.header = header;
		this.entries = entries;
		this.backupHeader = backupHeader;
		this.backupEntries = backupEntries;
		ByteBuffer h = le(header);
		this.backupLba = h.getLong(32);
		this.entriesLba = h.getLong(72);
		this.count = h.getInt(80);
		this.size = h.getInt(84);
		this.backupEntriesLba = le(backupHeader).getLong(72);
	}

	static String bootDisk() throws IOException {
		String part = Root.text("readlink -f /dev/block/by-name/boot_a");
		String disk = part.replaceAll("[0-9]+$", "");
		if (!disk.matches("/dev/block/sd[a-z]")) {
			throw new IOException(L.t("disco inesperado para boot_a: ", "unexpected disk for boot_a: ") + part);
		}
		return disk;
	}

	static Gpt load() throws IOException {
		String disk = bootDisk();
		int ss = Integer.parseInt(Root.text("cat /sys/block/" + disk.substring(disk.lastIndexOf('/') + 1) + "/queue/logical_block_size"));
		byte[] h = sectors(disk, ss, 1, 1);
		check(h, 1, L.t("principal", "primary"));
		ByteBuffer hb = le(h);
		long alt = hb.getLong(32), entLba = hb.getLong(72);
		int n = hb.getInt(80), esz = hb.getInt(84);
		if (esz != 128 || n <= 0 || n > 1024) {
			throw new IOException(L.t("tabla GPT con formato inesperado (", "GPT table in an unexpected format (") + n + L.t(" entradas de ", " entries of ") + esz + ")");
		}
		int span = (n * esz + ss - 1) / ss;
		byte[] e = Arrays.copyOf(sectors(disk, ss, entLba, span), n * esz);
		if (crc(e, 0, e.length) != hb.getInt(88)) {
			throw new IOException(L.t("CRC de las entradas principales incorrecto", "wrong CRC of the primary entries"));
		}
		byte[] bh = sectors(disk, ss, alt, 1);
		check(bh, alt, L.t("de reserva", "backup"));
		ByteBuffer bhb = le(bh);
		byte[] be = Arrays.copyOf(sectors(disk, ss, bhb.getLong(72), span), n * esz);
		if (crc(be, 0, be.length) != bhb.getInt(88)) {
			throw new IOException(L.t("CRC de las entradas de reserva incorrecto", "wrong CRC of the backup entries"));
		}
		if (!Arrays.equals(e, be)) {
			throw new IOException(L.t("las dos copias de la GPT no coinciden: no se toca nada", "the two copies of the GPT differ: nothing is touched"));
		}
		return new Gpt(disk, ss, h, e, bh, be);
	}

	String name(int i) {
		return new String(entries, i * size + 56, 72, StandardCharsets.UTF_16LE).replace("\u0000", "");
	}

	long attributes(int i) {
		return le(entries).getLong(i * size + 48);
	}

	int find(String name) {
		for (int i = 0; i < count; i++) {
			if (name(i).equals(name)) {
				return i;
			}
		}
		return -1;
	}

	/** 'a', 'b', or '?' when boot_a and boot_b do not say one clearly. */
	char activeSlot() {
		int a = find("boot_a"), b = find("boot_b");
		if (a < 0 || b < 0) {
			return '?';
		}
		boolean aa = (attributes(a) & ACTIVE) != 0, ba = (attributes(b) & ACTIVE) != 0;
		return aa == ba ? '?' : aa ? 'a' : 'b';
	}

	String describe(char slot) {
		int i = find("boot_" + slot);
		if (i < 0) {
			return "boot_" + slot + L.t(": no existe", ": does not exist");
		}
		long at = attributes(i);
		return String.format(L.t("boot_%c: %s, %s, %d reintentos%s", "boot_%c: %s, %s, %d retries%s"), slot, (at & ACTIVE) != 0 ? L.t("activa", "active") : L.t("inactiva", "inactive"),
		    (at & SUCCESSFUL) != 0 ? L.t("buena", "good") : L.t("sin confirmar", "unconfirmed"), (at & RETRY_MASK) >>> 51,
		    (at & UNBOOTABLE) != 0 ? L.t(", NO ARRANCABLE", ", UNBOOTABLE") : "");
	}

	static final long PRIORITY_MASK = 3L << 48;

	/**
	 * The entries with SLOT active, changed the way Qualcomm's bootloader changes them (fastboot set_active:
	 * QcomModulePkg SetActiveSlot -> SwitchPtnSlots + MarkPtnActive; compared against what this phone's ABL
	 * wrote, see NOTES): the type GUIDs of every X_a/X_b pair swapped -- the active slot's partitions carry
	 * the real type GUIDs, the inactive slot's a "parked" one, and the bootloader's ValidateSlotGuids refuses
	 * to load anything (Device Error, every slot, even fastboot boot) when the active bits and the GUIDs
	 * disagree -- the active bit moved, boot_SLOT given priority 3, 7 retries, not unbootable, and the
	 * other slot's boot priority 2. SUCCESSFUL: also mark boot_SLOT successful, so the bootloader does not
	 * count its retries down (iOS never marks its boots good, and would be dropped after seven boots).
	 * Nothing is changed when SLOT is already the active one.
	 */
	byte[] withActive(char slot, boolean successful) throws IOException {
		if (slot != 'a' && slot != 'b') {
			throw new IOException(L.t("ranura no válida", "invalid slot"));
		}
		char now = activeSlot();
		if (now == '?') {
			throw new IOException(L.t("la ranura activa no está clara (boot_a/boot_b): no se toca nada", "the active slot is unclear (boot_a/boot_b): nothing is touched"));
		}
		byte[] e = entries.clone();
		ByteBuffer eb = le(e);
		int swapped = 0;
		if (now != slot) {
			for (int i = 0; i < count; i++) {
				String n = name(i);
				if (!n.endsWith("_a")) {
					continue;
				}
				int j = find(n.substring(0, n.length() - 2) + "_b");
				if (j < 0) {
					continue;
				}
				byte[] ta = Arrays.copyOfRange(e, i * size, i * size + 16);
				System.arraycopy(e, j * size, e, i * size, 16);
				System.arraycopy(ta, 0, e, j * size, 16);
				swapped++;
			}
			if (swapped < 10) {
				throw new IOException(L.t("muy pocas parejas _a/_b (", "too few _a/_b pairs (") + swapped + L.t("): no se toca nada", "): nothing is touched"));
			}
		}
		for (int i = 0; i < count; i++) {
			String n = name(i);
			if (!n.endsWith("_a") && !n.endsWith("_b")) {
				continue;
			}
			long at = eb.getLong(i * size + 48);
			boolean mine = n.charAt(n.length() - 1) == slot;
			at = mine ? at | ACTIVE : at & ~ACTIVE;
			if (n.equals("boot_" + slot)) {
				at = (at & ~UNBOOTABLE & ~RETRY_MASK & ~PRIORITY_MASK) | (7L << 51) | (3L << 48);
				at = successful ? at | SUCCESSFUL : at & ~SUCCESSFUL;
			} else if (n.equals("boot_" + (slot == 'a' ? 'b' : 'a')) && now != slot) {
				at = (at & ~PRIORITY_MASK & ~RETRY_MASK) | (2L << 48) | (7L << 51);
			}
			eb.putLong(i * size + 48, at);
		}
		return e;
	}

	/** Writes NEWENTRIES into both copies of the table (backup first) and checks what is on the disk afterwards. */
	void commit(byte[] newEntries) throws IOException {
		if (newEntries.length != count * size) {
			throw new IOException(L.t("tamaño de entradas incorrecto", "wrong entries size"));
		}
		int crc = crc(newEntries, 0, newEntries.length);
		byte[] h = rehead(header, crc), bh = rehead(backupHeader, crc);
		int span = (count * size + sector - 1) / sector;
		byte[] padded = Arrays.copyOf(newEntries, span * sector);
		/* what lies after the entries in their last sector stays as it was */
		byte[] oldTail = sectors(disk, sector, entriesLba, span);
		System.arraycopy(oldTail, count * size, padded, count * size, padded.length - count * size);
		write(backupEntriesLba, padded);
		write(backupLba, h.length == sector ? bh : Arrays.copyOf(bh, sector));
		write(entriesLba, padded);
		write(1, h);
		Root.text("sync");
		Gpt again = load();
		if (!Arrays.equals(again.entries, newEntries)) {
			throw new IOException(L.t("lo leído tras escribir no coincide", "what was read back after writing does not match"));
		}
	}

	private void write(long lba, byte[] data) throws IOException {
		if (data.length % sector != 0) {
			throw new IOException(L.t("escritura no alineada", "unaligned write"));
		}
		Root.run("dd of=" + disk + " bs=" + sector + " seek=" + lba + " count=" + (data.length / sector) + " conv=notrunc,fsync 2>/dev/null", data);
	}

	private static byte[] rehead(byte[] old, int entriesCrc) {
		byte[] h = old.clone();
		ByteBuffer b = le(h);
		int hs = b.getInt(12);
		b.putInt(88, entriesCrc);
		b.putInt(16, 0);
		b.putInt(16, crc(h, 0, hs));
		return h;
	}

	private static void check(byte[] h, long lba, String which) throws IOException {
		ByteBuffer b = le(h);
		if (!new String(h, 0, 8, StandardCharsets.US_ASCII).equals("EFI PART")) {
			throw new IOException(L.t("cabecera GPT ", "GPT header (") + which + L.t(" sin firma", ") without signature"));
		}
		int hs = b.getInt(12);
		if (hs < 92 || hs > h.length || b.getLong(24) != lba) {
			throw new IOException(L.t("cabecera GPT ", "GPT header (") + which + L.t(" inesperada", ") unexpected"));
		}
		byte[] z = h.clone();
		le(z).putInt(16, 0);
		if (crc(z, 0, hs) != b.getInt(16)) {
			throw new IOException(L.t("CRC de la cabecera GPT ", "wrong CRC of GPT header (") + which + L.t(" incorrecto", ")"));
		}
	}

	static byte[] sectors(String disk, int ss, long lba, int n) throws IOException {
		byte[] d = Root.read("dd if=" + disk + " bs=" + ss + " skip=" + lba + " count=" + n + " 2>/dev/null");
		if (d.length != ss * n) {
			throw new IOException(L.t("lectura corta de ", "short read of ") + disk);
		}
		return d;
	}

	static int crc(byte[] d, int off, int len) {
		CRC32 c = new CRC32();
		c.update(d, off, len);
		return (int) c.getValue();
	}

	static ByteBuffer le(byte[] d) {
		return ByteBuffer.wrap(d).order(ByteOrder.LITTLE_ENDIAN);
	}

	/** Everything needed to undo a change: both headers and entry arrays, as they were read. */
	byte[] backup() {
		ByteBuffer b = ByteBuffer.allocate(16 + header.length * 2 + entries.length * 2).order(ByteOrder.LITTLE_ENDIAN);
		b.put("SAPPHGPT".getBytes(StandardCharsets.US_ASCII)).putInt(sector).putInt(entries.length);
		b.put(header).put(entries).put(backupHeader).put(backupEntries);
		return b.array();
	}
}
