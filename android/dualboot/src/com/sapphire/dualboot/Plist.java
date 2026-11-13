package com.sapphire.dualboot;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Property lists, binary (bplist00) and XML, read into plain Java objects: Map<String,Object> for dict,
 * List<Object> for array/set, String, Long, Double, Boolean, byte[] (data). Dates come as Double (seconds
 * since 2001) in binary lists and as String in XML ones: nothing here needs them. Plain Java (no Android
 * classes) so it is tested on the PC; the parser trusts nothing it reads (bounds, depth, cycles).
 */
final class Plist {
	private Plist() {
	}

	static Object parse(byte[] b) throws IOException {
		if (b.length >= 8 && new String(b, 0, 6, StandardCharsets.US_ASCII).equals("bplist")) {
			Binary bin = new Binary(b);
			return bin.object(bin.top, 0);
		}
		/* text .strings files are often UTF-16 with a byte order mark */
		boolean utf16 = b.length >= 2 && ((b[0] & 0xff) == 0xFF && (b[1] & 0xff) == 0xFE || (b[0] & 0xff) == 0xFE && (b[1] & 0xff) == 0xFF);
		String s = new String(b, utf16 ? StandardCharsets.UTF_16 : StandardCharsets.UTF_8);
		if (s.startsWith("\uFEFF")) {
			s = s.substring(1);
		}
		if (s.trim().startsWith("<")) {
			return new Xml(s).root();
		}
		return strings(s);
	}

	@SuppressWarnings("unchecked")
	static Map<String, Object> dict(Object o) {
		return o instanceof Map ? (Map<String, Object>) o : null;
	}

	@SuppressWarnings("unchecked")
	static List<Object> list(Object o) {
		return o instanceof List ? (List<Object>) o : null;
	}

	static String string(Map<String, Object> d, String key) {
		Object o = d == null ? null : d.get(key);
		return o instanceof String ? (String) o : null;
	}

	/* ---------------- binary ---------------- */

	private static final class Binary {
		final byte[] b;
		final int offSize, refSize;
		final long count, top, table;

		Binary(byte[] b) throws IOException {
			this.b = b;
			if (b.length < 40) {
				throw new IOException(L.t("plist binario truncado", "truncated binary plist"));
			}
			int t = b.length - 32;
			offSize = b[t + 6] & 0xff;
			refSize = b[t + 7] & 0xff;
			count = Hfs.be64(b, t + 8);
			top = Hfs.be64(b, t + 16);
			table = Hfs.be64(b, t + 24);
			if (offSize < 1 || offSize > 8 || refSize < 1 || refSize > 8 || count <= 0 || count > b.length
			    || top >= count || table < 8 || table + count * offSize > t) {
				throw new IOException(L.t("plist binario con cola incorrecta", "binary plist with a bad trailer"));
			}
		}

		long uint(int off, int n) throws IOException {
			if (off < 0 || off + n > b.length) {
				throw new IOException(L.t("plist binario: fuera de rango", "binary plist: out of range"));
			}
			long v = 0;
			for (int i = 0; i < n; i++) {
				v = v << 8 | (b[off + i] & 0xff);
			}
			return v;
		}

		int offset(long obj) throws IOException {
			if (obj < 0 || obj >= count) {
				throw new IOException(L.t("plist binario: referencia incorrecta", "binary plist: bad reference"));
			}
			long o = uint((int) (table + obj * offSize), offSize);
			if (o < 8 || o >= b.length - 32) {
				throw new IOException(L.t("plist binario: desplazamiento incorrecto", "binary plist: bad offset"));
			}
			return (int) o;
		}

		/** The length in a marker's low nibble, or the int that follows it when it is 0xF; LEN[1] = where data starts. */
		long length(int off, int[] start) throws IOException {
			int n = b[off] & 0x0f;
			if (n != 0x0f) {
				start[0] = off + 1;
				return n;
			}
			int m = b[off + 1] & 0xff;
			if ((m & 0xf0) != 0x10) {
				throw new IOException(L.t("plist binario: longitud incorrecta", "binary plist: bad length"));
			}
			int sz = 1 << (m & 0x0f);
			start[0] = off + 2 + sz;
			long v = uint(off + 2, sz);
			if (v < 0 || v > b.length * 2L) {
				throw new IOException(L.t("plist binario: longitud excesiva", "binary plist: length too large"));
			}
			return v;
		}

		Object object(long obj, int depth) throws IOException {
			if (depth > 64) {
				throw new IOException(L.t("plist binario demasiado profundo", "binary plist nested too deep"));
			}
			int off = offset(obj);
			int marker = b[off] & 0xff, type = marker >> 4, info = marker & 0x0f;
			int[] st = new int[1];
			switch (type) {
			case 0x0:
				return info == 8 ? Boolean.FALSE : info == 9 ? Boolean.TRUE : null;
			case 0x1: {
				int n = 1 << info;
				if (n > 16) {
					throw new IOException(L.t("plist binario: entero raro", "binary plist: odd integer"));
				}
				return n == 16 ? uint(off + 9, 8) : n == 8 ? uint(off + 1, 8) : uint(off + 1, n);
			}
			case 0x2: {
				int n = 1 << info;
				long bits = uint(off + 1, n);
				return n == 4 ? (double) Float.intBitsToFloat((int) bits) : Double.longBitsToDouble(bits);
			}
			case 0x3:
				return Double.longBitsToDouble(uint(off + 1, 8));
			case 0x4: {
				long n = length(off, st);
				check(st[0], n);
				byte[] d = new byte[(int) n];
				System.arraycopy(b, st[0], d, 0, (int) n);
				return d;
			}
			case 0x5: {
				long n = length(off, st);
				check(st[0], n);
				return new String(b, st[0], (int) n, StandardCharsets.ISO_8859_1);
			}
			case 0x6: {
				long n = length(off, st);
				check(st[0], n * 2);
				return new String(b, st[0], (int) n * 2, StandardCharsets.UTF_16BE);
			}
			case 0x7: {
				long n = length(off, st);
				check(st[0], n);
				return new String(b, st[0], (int) n, StandardCharsets.UTF_8);
			}
			case 0x8:
				return uint(off + 1, info + 1);
			case 0xA:
			case 0xC: {
				long n = length(off, st);
				check(st[0], n * refSize);
				List<Object> l = new ArrayList<>();
				for (int i = 0; i < n; i++) {
					l.add(object(uint(st[0] + i * refSize, refSize), depth + 1));
				}
				return l;
			}
			case 0xD: {
				long n = length(off, st);
				check(st[0], n * refSize * 2);
				Map<String, Object> m = new LinkedHashMap<>();
				for (int i = 0; i < n; i++) {
					Object k = object(uint(st[0] + i * refSize, refSize), depth + 1);
					Object v = object(uint(st[0] + (int) (n + i) * refSize, refSize), depth + 1);
					m.put(String.valueOf(k), v);
				}
				return m;
			}
			default:
				throw new IOException(L.t("plist binario: tipo ", "binary plist: unknown type ") + Integer.toHexString(marker) + L.t(" desconocido", ""));
			}
		}

		void check(int start, long n) throws IOException {
			if (n < 0 || start + n > b.length) {
				throw new IOException(L.t("plist binario: dato fuera de rango", "binary plist: data out of range"));
			}
		}
	}

	/* ---------------- XML ---------------- */

	private static final class Xml {
		final String s;
		int i;

		Xml(String s) {
			this.s = s;
		}

		Object root() throws IOException {
			String t = tag();
			while (t != null && !isValueTag(t)) {
				t = tag();     /* <plist version="1.0"> */
			}
			if (t == null) {
				throw new IOException(L.t("plist XML vacío", "empty XML plist"));
			}
			return value(t, 0);
		}

		static boolean isValueTag(String t) {
			String n = name(t);
			return n.equals("dict") || n.equals("array") || n.equals("string") || n.equals("integer") || n.equals("real")
			    || n.equals("true") || n.equals("false") || n.equals("data") || n.equals("date");
		}

		static String name(String t) {
			int e = 0;
			while (e < t.length() && " \t\r\n/".indexOf(t.charAt(e)) < 0) {
				e++;
			}
			return t.substring(0, e);
		}

		/** The next tag's inside ("dict", "/dict", "true/"), skipping comments, <?..?> and <!DOCTYPE>; null at the end. */
		String tag() {
			while (true) {
				int a = s.indexOf('<', i);
				if (a < 0) {
					return null;
				}
				if (s.startsWith("<!--", a)) {
					int e = s.indexOf("-->", a);
					i = e < 0 ? s.length() : e + 3;
					continue;
				}
				int e = s.indexOf('>', a);
				if (e < 0) {
					return null;
				}
				i = e + 1;
				char c = s.charAt(a + 1);
				if (c == '?' || c == '!') {
					continue;
				}
				return s.substring(a + 1, e).trim();
			}
		}

		String text(String end) throws IOException {
			int e = s.indexOf("</" + end, i);
			if (e < 0) {
				throw new IOException(L.t("plist XML: falta </", "XML plist: missing </") + end + ">");
			}
			String t = s.substring(i, e);
			i = s.indexOf('>', e) + 1;
			return unescape(t);
		}

		Object value(String t, int depth) throws IOException {
			if (depth > 64) {
				throw new IOException(L.t("plist XML demasiado profundo", "XML plist nested too deep"));
			}
			boolean empty = t.endsWith("/");
			String n = name(t);
			switch (n) {
			case "true":
				return Boolean.TRUE;
			case "false":
				return Boolean.FALSE;
			case "string":
			case "date":
				return empty ? "" : text(n);
			case "integer": {
				String v = empty ? "0" : text(n).trim();
				try {
					return v.startsWith("0x") ? Long.parseLong(v.substring(2), 16) : Long.parseLong(v);
				} catch (NumberFormatException x) {
					return 0L;
				}
			}
			case "real":
				try {
					return empty ? 0.0 : Double.parseDouble(text(n).trim());
				} catch (NumberFormatException x) {
					return 0.0;
				}
			case "data":
				try {
					return empty ? new byte[0] : Base64.getMimeDecoder().decode(text(n).replaceAll("\\s", ""));
				} catch (IllegalArgumentException x) {
					return new byte[0];
				}
			case "array": {
				List<Object> l = new ArrayList<>();
				if (empty) {
					return l;
				}
				for (String c; (c = tag()) != null && !c.equals("/array"); ) {
					l.add(value(c, depth + 1));
				}
				return l;
			}
			case "dict": {
				Map<String, Object> m = new LinkedHashMap<>();
				if (empty) {
					return m;
				}
				for (String c; (c = tag()) != null && !c.equals("/dict"); ) {
					if (!name(c).equals("key")) {
						throw new IOException(L.t("plist XML: se esperaba <key>, no <", "XML plist: expected <key>, not <") + c + ">");
					}
					String k = c.endsWith("/") ? "" : text("key");
					String v = tag();
					if (v == null) {
						throw new IOException(L.t("plist XML truncado", "truncated XML plist"));
					}
					m.put(k, value(v, depth + 1));
				}
				return m;
			}
			default:
				throw new IOException(L.t("plist XML: etiqueta <", "XML plist: unknown tag <") + n + L.t("> desconocida", ">"));
			}
		}
	}

	static String unescape(String t) {
		if (t.indexOf('&') < 0) {
			return t;
		}
		StringBuilder o = new StringBuilder();
		for (int i = 0; i < t.length(); i++) {
			char c = t.charAt(i);
			int semi = c == '&' ? t.indexOf(';', i) : -1;
			if (semi < 0 || semi - i > 10) {
				o.append(c);
				continue;
			}
			String e = t.substring(i + 1, semi);
			try {
				if (e.startsWith("#x") || e.startsWith("#X")) {
					o.appendCodePoint(Integer.parseInt(e.substring(2), 16));
				} else if (e.startsWith("#")) {
					o.appendCodePoint(Integer.parseInt(e.substring(1)));
				} else {
					String r = e.equals("amp") ? "&" : e.equals("lt") ? "<" : e.equals("gt") ? ">" : e.equals("quot") ? "\""
					    : e.equals("apos") ? "'" : null;
					if (r == null) {
						o.append(c);
						continue;
					}
					o.append(r);
				}
				i = semi;
			} catch (IllegalArgumentException x) {
				o.append(c);
			}
		}
		return o.toString();
	}

	/* ---------------- old-style .strings ("key" = "value";) ---------------- */

	private static Map<String, Object> strings(String s) {
		Map<String, Object> m = new LinkedHashMap<>();
		java.util.regex.Matcher r = java.util.regex.Pattern.compile("\"((?:[^\"\\\\]|\\\\.)*)\"\\s*=\\s*\"((?:[^\"\\\\]|\\\\.)*)\"\\s*;")
		    .matcher(s);
		while (r.find()) {
			m.put(r.group(1).replace("\\\"", "\""), r.group(2).replace("\\\"", "\"").replace("\\n", "\n"));
		}
		return m;
	}
}
