package com.sapphire.dualboot;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** iOS's side of the phone: its boot image in the slot Android is not running from, its root in iosroot. */
final class Ios {
	private static char slot;

	/** The slot iOS boots from: the one Android is not running from (ro.boot.slot_suffix). */
	static char slot() throws IOException {
		if (slot == 0) {
			String now = Root.text("getprop ro.boot.slot_suffix");
			if (now.equals("_a")) {
				slot = 'b';
			} else if (now.equals("_b")) {
				slot = 'a';
			} else {
				throw new IOException(L.t("ranura actual desconocida: «", "unknown current slot: “") + now + L.t("»", "”"));
			}
		}
		return slot;
	}

	static String boot() throws IOException {
		return "/dev/block/by-name/boot_" + slot();
	}
	static final String ROOT = "/dev/block/by-name/iosroot";
	static final int LOG_BLOCKS = 1025;     /* the kernel's console ring at the end of iosroot (sweet_ufs.c) */

	private Ios() {
	}

	/** Why IMAGE must not go into iOS's boot partition, or null when it is an iOS image that looks for its root in iosroot. */
	static String refuse(byte[] image, long partition) {
		if (image.length < 4096 || !new String(image, 0, 8, StandardCharsets.US_ASCII).equals("ANDROID!")) {
			return L.t("no es una imagen de arranque (falta «ANDROID!»)", "not a boot image (no “ANDROID!”)");
		}
		if (image.length > partition) {
			return L.t("la imagen (", "the image (") + image.length + L.t(" bytes) no cabe en la partición boot de iOS (", " bytes) does not fit in the iOS boot partition (") + partition + ")";
		}
		if (indexOf(image, "rd=ufsroot0") < 0) {
			return L.t("no parece una imagen de iOS (sin «rd=ufsroot0»)", "does not look like an iOS image (no “rd=ufsroot0”)");
		}
		if (indexOf(image, "ufsroot=iosroot") < 0) {
			return L.t("imagen de iOS ANTIGUA: le falta «ufsroot=iosroot» y tomaría la userdata de Android por la raíz de iOS", "OLD iOS image: it lacks “ufsroot=iosroot” and would take Android's userdata as the iOS root");
		}
		return null;
	}

	/** What iOS's boot partition holds now. */
	static String bootState() throws IOException {
		byte[] head = Root.read("head -c 33554432 " + boot());
		if (head.length < 8 || !new String(head, 0, 8, StandardCharsets.US_ASCII).equals("ANDROID!")) {
			return L.t("vacía o desconocida", "empty or unknown");
		}
		if (indexOf(head, "rd=ufsroot0") < 0) {
			return L.t("una imagen de Android (no iOS)", "an Android image (not iOS)");
		}
		String sched = indexOf(head, "sweetsched=13") >= 0 ? L.t(", planificador 13", ", scheduler 13") : "";
		return indexOf(head, "ufsroot=iosroot") >= 0 ? L.t("iOS (raíz en iosroot)", "iOS (root in iosroot)") + sched : L.t("iOS ANTIGUA (sin ufsroot=iosroot)", "OLD iOS (no ufsroot=iosroot)");
	}

	/*
	 * Whether the iOS image in its boot partition can hand the phone back and keeps itself booted: kernel 0306
	 * or later (kern.sweet_boot_android with the boot LUN, its slot marked good after a boot). Its kernel
	 * carries the name of the boot-arg 0306 added.
	 */
	static boolean canReturn() throws IOException {
		return indexOf(Root.read("head -c 33554432 " + boot()), "sweetmarkgood") >= 0;
	}

	/** "Darwin Kernel Version 22.6.0: <date>; <builder>" of the image in iOS's boot partition, shortened. */
	static String kernelInfo() throws IOException {
		byte[] head = Root.read("head -c 33554432 " + boot());
		int i = indexOf(head, "Darwin Kernel Version ");
		if (i < 0) {
			return L.t("kernel: sin identificar", "kernel: unidentified");
		}
		int e = i;
		while (e < head.length && head[e] != 0 && head[e] != '\n' && e - i < 160) {
			e++;
		}
		String s = new String(head, i, e - i, StandardCharsets.US_ASCII);
		String ver = s.replace("Darwin Kernel Version ", "");
		int colon = ver.indexOf(':'), semi = ver.indexOf(';');
		String num = colon > 0 ? ver.substring(0, colon) : ver;
		String date = colon > 0 && semi > colon ? ver.substring(colon + 1, semi).trim() : "";
		return "Darwin " + num + (date.isEmpty() ? "" : " · " + date);
	}

	/** The UFS boot LUN (1 = slot a's XBL, 2 = slot b's), as Linux shows it. */
	static String bootLun() {
		try {
			String v = Root.text("cat /sys/devices/platform/soc/4804000.ufshc/attributes/boot_lun_enabled");
			return v.endsWith("1") ? L.t("1 (ranura a)", "1 (slot a)") : v.endsWith("2") ? L.t("2 (ranura b)", "2 (slot b)") : v;
		} catch (IOException e) {
			return "?";
		}
	}

	static void install(byte[] image) throws IOException {
		long part = Long.parseLong(Root.text("blockdev --getsize64 " + boot()));
		String why = refuse(image, part);
		if (why != null) {
			throw new IOException(why);
		}
		Root.run("dd of=" + boot() + " bs=1048576 conv=fsync 2>/dev/null", image);
		/*
		 * The rest of the partition zeroed: what an earlier, larger image left at its end includes its AVB
		 * footer, which the bootloader reads at the partition's end; pointing into data overwritten, it made
		 * the bootloader refuse the slot and stop in fastboot (without even counting a failed boot).
		 */
		long from = (image.length + 4095L) / 4096L, blocks = part / 4096L - from;
		if (image.length % 4096 != 0) {
			byte[] pad = new byte[(int) (from * 4096L - image.length)];
			Root.run("dd of=" + boot() + " bs=1 seek=" + image.length + " count=" + pad.length + " conv=notrunc,fsync 2>/dev/null", pad);
		}
		if (blocks > 0) {
			Root.text("dd if=/dev/zero of=" + boot() + " bs=4096 seek=" + from + " count=" + blocks + " conv=notrunc,fsync 2>/dev/null");
		}
		Root.text("sync");
		byte[] back = Root.read("head -c " + image.length + " " + boot());
		if (!Arrays.equals(back, image)) {
			throw new IOException(L.t("lo leído de la partición boot de iOS no coincide con la imagen", "what was read back from the iOS boot partition does not match the image"));
		}
	}

	/** "40.1 GiB, volumen HFS+ de 40.0 GiB, 19.8 GiB usados" from the partition and the HFS+ volume header. */
	static String rootState() throws IOException {
		long part = Long.parseLong(Root.text("blockdev --getsize64 " + ROOT));
		byte[] vh = Root.read("dd if=" + ROOT + " bs=4096 count=1 2>/dev/null");
		String s = String.format(L.t("partición %.1f GiB", "partition %.1f GiB"), part / 1073741824.0);
		if (vh.length >= 1024 + 52 && vh[1024] == 'H' && (vh[1025] == '+' || vh[1025] == 'X')) {
			ByteBuffer b = ByteBuffer.wrap(vh, 1024, 512).order(ByteOrder.BIG_ENDIAN);
			long bs = b.getInt(1024 + 40) & 0xffffffffL, total = b.getInt(1024 + 44) & 0xffffffffL;
			long free = b.getInt(1024 + 48) & 0xffffffffL;
			s += String.format(L.t(", volumen HFS+ de %.1f GiB, %.1f GiB usados", ", HFS+ volume of %.1f GiB, %.1f GiB used"), total * bs / 1073741824.0,
			    (total - free) * bs / 1073741824.0);
		} else {
			s += L.t(", SIN volumen HFS+ reconocible", ", NO recognizable HFS+ volume");
		}
		return s;
	}

	/** The kernel's console log as iOS last wrote it (tools/sweet-log.py). */
	static String log() throws IOException {
		long part = Long.parseLong(Root.text("blockdev --getsize64 " + ROOT));
		long skip = part / 4096 - LOG_BLOCKS;
		byte[] area = Root.read("dd if=" + ROOT + " bs=4096 skip=" + skip + " count=" + LOG_BLOCKS + " 2>/dev/null");
		if (area.length < 4096 || !new String(area, 0, 8, StandardCharsets.US_ASCII).equals("SWEETLOG")) {
			return L.t("(iOS todavía no ha escrito su log en esta partición)", "(iOS has not written its log to this partition yet)");
		}
		ByteBuffer b = ByteBuffer.wrap(area).order(ByteOrder.LITTLE_ENDIAN);
		int size = b.getInt(12);
		long total = b.getLong(16), seq = b.getLong(24);
		if (size <= 0 || 4096 + size > area.length) {
			return L.t("(cabecera del log incorrecta)", "(bad log header)");
		}
		byte[] text;
		if (total <= size) {
			text = Arrays.copyOfRange(area, 4096, 4096 + (int) total);
		} else {
			int start = (int) (total % size);
			text = new byte[size];
			System.arraycopy(area, 4096 + start, text, 0, size - start);
			System.arraycopy(area, 4096, text, size - start, start);
		}
		String head = seq == -1 ? L.t("# volcado por PÁNICO\n", "# PANIC dump\n") : L.t("# escritura n.º ", "# write no. ") + seq + "\n";
		return head + new String(text, StandardCharsets.UTF_8).replace("\r", "");
	}

	static int indexOf(byte[] hay, String needle) {
		byte[] n = needle.getBytes(StandardCharsets.US_ASCII);
		outer:
		for (int i = 0; i + n.length <= hay.length; i++) {
			for (int j = 0; j < n.length; j++) {
				if (hay[i + j] != n[j]) {
					continue outer;
				}
			}
			return i;
		}
		return -1;
	}
}
