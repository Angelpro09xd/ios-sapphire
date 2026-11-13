/*
 * SapphireHEIF: HEIC / HEIF pictures decoded in software, in the process that asks for them.
 *
 * iOS decodes HEIF through CMPhoto: the HEVC picture by AppleAVD (the hardware decoder) into 4:2:0 surfaces,
 * then AppleM2ScalerCSC (the hardware scaler) into RGB. Neither exists here: ImageIO still reads the file
 * (type, size, properties) but every picture it makes is all zero -- iOS 16's layered wallpapers (their .ca
 * assets are HEIC), photos, thumbnails, any HEIC an app shows. VideoToolbox's own software HEVC decoder runs
 * in mediaserverd and fails there too (-6661).
 *
 * Here CGImageSourceCreateImageAtIndex and CGImageSourceCreateThumbnailAtIndex, for a HEIF source, decode the
 * picture with libheif + libde265 (built in, userland/heif) into an RGBA CGImage with the file's colour space
 * (its ICC profile; else Display P3 or sRGB as its nclx says). The source's bytes are remembered when it is
 * made (CreateWithURL / WithData / WithDataProvider), as an associated object, so they go with it.
 * Everything else -- other types, properties, incremental sources -- stays ImageIO's.
 * Injected wherever ImageIO is loaded (ElleKit, Filter: com.apple.ImageIO). /var/jb/var/sapphire-heif-off: off.
 */
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <libheif/heif.h>

static char data_key, url_key;
static os_log_t lg;

static CGImageSourceRef (*orig_with_url)(CFURLRef, CFDictionaryRef);
static CGImageSourceRef (*orig_with_data)(CFDataRef, CFDictionaryRef);
static CGImageSourceRef (*orig_with_provider)(CGDataProviderRef, CFDictionaryRef);
static CGImageRef (*orig_image)(CGImageSourceRef, size_t, CFDictionaryRef);
static CGImageRef (*orig_thumb)(CGImageSourceRef, size_t, CFDictionaryRef);

static bool
is_heif(CGImageSourceRef src)
{
	CFStringRef t = src ? CGImageSourceGetType(src) : NULL;
	return t != NULL && (CFEqual(t, CFSTR("public.heic")) || CFEqual(t, CFSTR("public.heif")) ||
	    CFEqual(t, CFSTR("public.heics")) || CFEqual(t, CFSTR("public.avci")));
}

/* the bytes the source was made of */
static NSData *
source_bytes(CGImageSourceRef src)
{
	id o = (__bridge id)src;
	NSData *d = objc_getAssociatedObject(o, &data_key);
	if (d != nil) {
		return d;
	}
	NSURL *u = objc_getAssociatedObject(o, &url_key);
	if (u != nil) {
		d = [NSData dataWithContentsOfURL:u options:NSDataReadingMappedIfSafe error:nil];
		if (d != nil) {
			objc_setAssociatedObject(o, &data_key, d, OBJC_ASSOCIATION_RETAIN);
		}
	}
	return d;
}

static void
release_pixels(void *info, const void *data, size_t size)
{
	(void)info;
	(void)size;
	free((void *)data);
}

static CGColorSpaceRef
colour_space(struct heif_image_handle *h)
{
	size_t n = heif_image_handle_get_raw_color_profile_size(h);
	if (n > 0) {
		void *icc = malloc(n);
		if (icc != NULL && heif_image_handle_get_raw_color_profile(h, icc).code == heif_error_Ok) {
			CFDataRef d = CFDataCreate(NULL, icc, (CFIndex)n);
			CGColorSpaceRef cs = CGColorSpaceCreateWithICCData(d);
			CFRelease(d);
			free(icc);
			if (cs != NULL) {
				return cs;
			}
		} else {
			free(icc);
		}
	}
	struct heif_color_profile_nclx *nclx = NULL;
	CGColorSpaceRef cs = NULL;
	if (heif_image_handle_get_nclx_color_profile(h, &nclx).code == heif_error_Ok && nclx != NULL) {
		if (nclx->color_primaries == heif_color_primaries_SMPTE_EG_432_1) {
			cs = CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
		} else if (nclx->color_primaries == heif_color_primaries_ITU_R_BT_2020_2_and_2100_0) {
			cs = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2020);
		}
		heif_nclx_color_profile_free(nclx);
	}
	return cs != NULL ? cs : CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
}

/* picture INDEX of the file (0: the primary one, as ImageIO counts), decoded; NULL if libheif cannot */
static CGImageRef
heif_decode(NSData *bytes, size_t index)
{
	struct heif_context *ctx = heif_context_alloc();
	struct heif_image_handle *h = NULL;
	struct heif_image *img = NULL;
	CGImageRef out = NULL;
	CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();

	if (heif_context_read_from_memory_without_copy(ctx, bytes.bytes, bytes.length, NULL).code != heif_error_Ok) {
		goto done;
	}
	if (index == 0) {
		if (heif_context_get_primary_image_handle(ctx, &h).code != heif_error_Ok) {
			goto done;
		}
	} else {
		int n = heif_context_get_number_of_top_level_images(ctx);
		heif_item_id ids[64], primary = 0;
		heif_context_get_primary_image_ID(ctx, &primary);
		n = heif_context_get_list_of_top_level_image_IDs(ctx, ids, n < 64 ? n : 64);
		size_t k = 0;
		heif_item_id want = 0;
		for (int i = 0; i < n; i++) {
			if (ids[i] == primary) {
				continue;
			}
			if (++k == index) {
				want = ids[i];
			}
		}
		if (want == 0 || heif_context_get_image_handle(ctx, want, &h).code != heif_error_Ok) {
			goto done;
		}
	}
	bool alpha = heif_image_handle_has_alpha_channel(h);
	struct heif_decoding_options *opt = heif_decoding_options_alloc();
	struct heif_error e = heif_decode_image(h, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, opt);
	heif_decoding_options_free(opt);
	if (e.code != heif_error_Ok) {
		os_log_error(lg, "SapphireHEIF: decode: %{public}s", e.message);
		goto done;
	}
	int w = heif_image_get_width(img, heif_channel_interleaved), ht = heif_image_get_height(img, heif_channel_interleaved);
	int stride = 0;
	const uint8_t *p = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
	if (p == NULL || w <= 0 || ht <= 0) {
		goto done;
	}
	size_t row = (size_t)w * 4;
	uint8_t *px = malloc(row * (size_t)ht);
	if (px == NULL) {
		goto done;
	}
	for (int y = 0; y < ht; y++) {
		const uint8_t *s = p + (size_t)y * (size_t)stride;
		uint8_t *d = px + (size_t)y * row;
		/*
		 * BGRA, premultiplied: CoreAnimation's own layout (kCGImageAlphaPremultipliedFirst, 32-bit little
		 * endian), as ImageIO's decoders give it; in any other it converts the picture with the processor
		 * each time it hands it to the window server again -- a wallpaper's four large layers at every wake.
		 */
		for (int x = 0; x < w; x++, s += 4, d += 4) {
			unsigned a = alpha ? s[3] : 255;
			d[0] = (uint8_t)((s[2] * a + 127) / 255);
			d[1] = (uint8_t)((s[1] * a + 127) / 255);
			d[2] = (uint8_t)((s[0] * a + 127) / 255);
			d[3] = (uint8_t)a;
		}
	}
	CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, row * (size_t)ht, release_pixels);
	CGColorSpaceRef cs = colour_space(h);
	out = CGImageCreate((size_t)w, (size_t)ht, 8, 32, row, cs,
	    (alpha ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little, dp, NULL, true,
	    kCGRenderingIntentDefault);
	CGColorSpaceRelease(cs);
	CGDataProviderRelease(dp);
	os_log(lg, "SapphireHEIF: %dx%d%s decoded in %.0f ms", w, ht, alpha ? " (alpha)" : "",
	    (CFAbsoluteTimeGetCurrent() - t0) * 1000);
done:
	if (img != NULL) {
		heif_image_release(img);
	}
	if (h != NULL) {
		heif_image_handle_release(h);
	}
	heif_context_free(ctx);
	return out;
}

static CGImageSourceRef
my_with_url(CFURLRef url, CFDictionaryRef options)
{
	CGImageSourceRef s = orig_with_url(url, options);
	if (s != NULL && url != NULL && is_heif(s)) {
		objc_setAssociatedObject((__bridge id)s, &url_key, (__bridge NSURL *)url, OBJC_ASSOCIATION_RETAIN);
	}
	return s;
}

static CGImageSourceRef
my_with_data(CFDataRef data, CFDictionaryRef options)
{
	CGImageSourceRef s = orig_with_data(data, options);
	if (s != NULL && data != NULL && is_heif(s)) {
		objc_setAssociatedObject((__bridge id)s, &data_key, [(__bridge NSData *)data copy], OBJC_ASSOCIATION_RETAIN);
	}
	return s;
}

static CGImageSourceRef
my_with_provider(CGDataProviderRef provider, CFDictionaryRef options)
{
	CGImageSourceRef s = orig_with_provider(provider, options);
	if (s != NULL && provider != NULL && is_heif(s)) {
		CFDataRef d = CGDataProviderCopyData(provider);
		if (d != NULL) {
			objc_setAssociatedObject((__bridge id)s, &data_key, (__bridge_transfer NSData *)d, OBJC_ASSOCIATION_RETAIN);
		}
	}
	return s;
}

static CGImageRef
my_image(CGImageSourceRef src, size_t index, CFDictionaryRef options)
{
	if (is_heif(src)) {
		NSData *b = source_bytes(src);
		CGImageRef img = b != nil ? heif_decode(b, index) : NULL;
		if (img != NULL) {
			return img;
		}
	}
	return orig_image(src, index, options);
}

/* a thumbnail: the picture, made no larger than kCGImageSourceThumbnailMaxPixelSize */
static CGImageRef
my_thumb(CGImageSourceRef src, size_t index, CFDictionaryRef options)
{
	if (!is_heif(src)) {
		return orig_thumb(src, index, options);
	}
	NSData *b = source_bytes(src);
	CGImageRef full = b != nil ? heif_decode(b, index) : NULL;
	if (full == NULL) {
		return orig_thumb(src, index, options);
	}
	NSNumber *max = options ? ((__bridge NSDictionary *)options)[(id)kCGImageSourceThumbnailMaxPixelSize] : nil;
	size_t w = CGImageGetWidth(full), h = CGImageGetHeight(full), m = w > h ? w : h;
	if (max == nil || max.unsignedLongValue == 0 || max.unsignedLongValue >= m) {
		return full;
	}
	double k = max.doubleValue / (double)m;
	size_t tw = (size_t)(w * k + 0.5), th = (size_t)(h * k + 0.5);
	tw = tw ? tw : 1;
	th = th ? th : 1;
	CGColorSpaceRef cs = CGImageGetColorSpace(full);
	CGContextRef c = CGBitmapContextCreate(NULL, tw, th, 8, 0, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGImageRef out = NULL;
	if (c != NULL) {
		CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
		CGContextDrawImage(c, CGRectMake(0, 0, tw, th), full);
		out = CGBitmapContextCreateImage(c);
		CGContextRelease(c);
	}
	CGImageRelease(full);
	return out;
}

__attribute__((constructor)) static void
init(void)
{
	lg = os_log_create("com.sapphire.heif", "heif");
	if (access("/var/jb/var/sapphire-heif-off", F_OK) == 0) {
		return;
	}
	void *sub = dlopen("/var/jb/usr/lib/libsubstrate.dylib", RTLD_NOW);
	void (*hook)(void *, void *, void **) = sub ? dlsym(sub, "MSHookFunction") : NULL;
	if (hook == NULL) {
		os_log_error(lg, "SapphireHEIF: no MSHookFunction");
		return;
	}
	hook((void *)CGImageSourceCreateWithURL, (void *)my_with_url, (void **)&orig_with_url);
	hook((void *)CGImageSourceCreateWithData, (void *)my_with_data, (void **)&orig_with_data);
	hook((void *)CGImageSourceCreateWithDataProvider, (void *)my_with_provider, (void **)&orig_with_provider);
	hook((void *)CGImageSourceCreateImageAtIndex, (void *)my_image, (void **)&orig_image);
	hook((void *)CGImageSourceCreateThumbnailAtIndex, (void *)my_thumb, (void **)&orig_thumb);
}
