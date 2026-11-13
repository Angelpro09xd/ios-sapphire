package com.sapphire.dualboot;

import java.io.ByteArrayOutputStream;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/**
 * Apple's "crushed" PNGs (Xcode's pngcrush -iphone, the CgBI chunk first): the image data is a raw deflate
 * stream (no zlib header), and the pixels are BGRA with premultiplied alpha. Android's decoder rejects them
 * or shows them in the wrong colours, so app icons taken from iOS bundles and IPAs are decoded here.
 * Plain Java: returns ARGB pixels, the caller makes the Bitmap.
 */
final class Png {
	private Png() {
	}

	static boolean isCgBI(byte[] p) {
		return p != null && p.length > 16 && (p[0] & 0xff) == 0x89 && p[1] == 'P' && p[12] == 'C' && p[13] == 'g' && p[14] == 'B'
		    && p[15] == 'I';
	}

	/** ARGB pixels of a CgBI PNG, with WH[0..1] set to its width and height; null when it is not one this can read. */
	static int[] decodeCgBI(byte[] p, int[] wh) {
		if (!isCgBI(p)) {
			return null;
		}
		int w = 0, h = 0, depth = 0, color = 0, interlace = 0;
		ByteArrayOutputStream idat = new ByteArrayOutputStream();
		for (int o = 8; o + 12 <= p.length; ) {
			int len = Hfs.be32(p, o);
			if (len < 0 || o + 12 + len > p.length) {
				return null;
			}
			String type = new String(p, o + 4, 4, java.nio.charset.StandardCharsets.US_ASCII);
			if (type.equals("IHDR") && len >= 13) {
				w = Hfs.be32(p, o + 8);
				h = Hfs.be32(p, o + 12);
				depth = p[o + 16] & 0xff;
				color = p[o + 17] & 0xff;
				interlace = p[o + 20] & 0xff;
			} else if (type.equals("IDAT")) {
				idat.write(p, o + 8, len);
			} else if (type.equals("IEND")) {
				break;
			}
			o += 12 + len;
		}
		/* icons: small, 8-bit RGBA (as Xcode writes them), not interlaced */
		if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || depth != 8 || (color != 6 && color != 2) || interlace != 0) {
			return null;
		}
		int bpp = color == 6 ? 4 : 3, stride = w * bpp;
		byte[] raw = new byte[(stride + 1) * h];
		Inflater inf = new Inflater(true);
		try {
			inf.setInput(idat.toByteArray());
			int n = 0;
			while (n < raw.length) {
				int k = inf.inflate(raw, n, raw.length - n);
				if (k == 0) {
					if (inf.finished() || inf.needsInput() || inf.needsDictionary()) {
						break;
					}
				}
				n += k;
			}
			if (n < raw.length) {
				return null;
			}
		} catch (DataFormatException e) {
			return null;
		} finally {
			inf.end();
		}
		byte[] prev = new byte[stride], cur = new byte[stride];
		int[] out = new int[w * h];
		for (int y = 0; y < h; y++) {
			int f = raw[y * (stride + 1)] & 0xff;
			System.arraycopy(raw, y * (stride + 1) + 1, cur, 0, stride);
			for (int x = 0; x < stride; x++) {
				int a = x >= bpp ? cur[x - bpp] & 0xff : 0, b = prev[x] & 0xff, c = x >= bpp ? prev[x - bpp] & 0xff : 0;
				int v = cur[x] & 0xff;
				switch (f) {
				case 1:
					v += a;
					break;
				case 2:
					v += b;
					break;
				case 3:
					v += (a + b) >> 1;
					break;
				case 4: {
					int pp = a + b - c, pa = Math.abs(pp - a), pb = Math.abs(pp - b), pc = Math.abs(pp - c);
					v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
					break;
				}
				default:
				}
				cur[x] = (byte) v;
			}
			for (int x = 0; x < w; x++) {
				int bl = cur[x * bpp] & 0xff, gr = cur[x * bpp + 1] & 0xff, rd = cur[x * bpp + 2] & 0xff;
				int al = bpp == 4 ? cur[x * bpp + 3] & 0xff : 255;
				if (al != 0 && al != 255) {         /* undo the premultiplication */
					rd = Math.min(255, rd * 255 / al);
					gr = Math.min(255, gr * 255 / al);
					bl = Math.min(255, bl * 255 / al);
				}
				out[y * w + x] = al << 24 | rd << 16 | gr << 8 | bl;
			}
			byte[] t = prev;
			prev = cur;
			cur = t;
		}
		wh[0] = w;
		wh[1] = h;
		return out;
	}
}
