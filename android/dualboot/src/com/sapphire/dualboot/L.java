package com.sapphire.dualboot;

import java.util.Locale;

// The app's language: Spanish when the device is in Spanish, English otherwise. t(es, en) picks one.
final class L {
	static final boolean ES = "es".equals(Locale.getDefault().getLanguage());

	static String t(String es, String en) { return ES ? es : en; }
}
