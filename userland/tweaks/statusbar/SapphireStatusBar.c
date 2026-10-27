/*
 * SapphireStatusBar: the iPhone X's status bar for iOS drawn at 2x (xnu 0239/0240, sweetfb_scale=2).
 *
 * UIKit picks a status bar layout by the screen -- +[_UIStatusBarVisualProvider_iOS
 * visualProviderSubclassForScreen:visualProviderInfo:] -- and has one only for real iPhone screens
 * (pixel width, scale, corner radius: _UIStatusBarVisualProvider_Split1125 for the X's 375 points at
 * 3x). The X's 375 x 812 points at 2x is none of them: it logged "No status bar visual provider for
 * screen ... (width: 375.00, scale: 2.00, idiom: 0, cornerRadius: 39.00)" and showed the red
 * "rdar://45025538" placeholder bar. This tweak (ElleKit's TweakLoader brings it into every UIKit
 * process) gives such a screen the X's layout, which is in points.
 */
#include <objc/runtime.h>
#include <objc/message.h>
#include <string.h>

typedef Class (*subclass_fn)(id, SEL, id, id);
static subclass_fn original;

static Class
subclass_for_screen(id self, SEL selector, id screen, id info)
{
	Class chosen = original(self, selector, screen, info);
	const char *name = chosen != Nil ? class_getName(chosen) : NULL;

	if (chosen == Nil || (name != NULL && strstr(name, "Fallback") != NULL)) {
		Class x = objc_getClass("_UIStatusBarVisualProvider_Split1125");
		if (x != Nil) {
			return x;
		}
	}
	return chosen;
}

__attribute__((constructor)) static void
install(void)
{
	Class provider = objc_getClass("_UIStatusBarVisualProvider_iOS");
	SEL selector = sel_registerName("visualProviderSubclassForScreen:visualProviderInfo:");
	Method method = provider != Nil ? class_getClassMethod(provider, selector) : NULL;

	if (method != NULL) {
		original = (subclass_fn)method_setImplementation(method, (IMP)subclass_for_screen);
	}
}
