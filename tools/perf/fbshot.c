/* fbshot PREFIX [W H]: writes every IOSurface of W x H (default 750x1624, the screen's swap chain) among the ids
 * 1..4096 to PREFIX-N.raw, as BGRA rows of W*4 bytes. Link with -framework IOSurface -framework CoreFoundation. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct __IOSurface *IOSurfaceRef;
IOSurfaceRef IOSurfaceLookup(uint32_t id);
size_t IOSurfaceGetWidth(IOSurfaceRef), IOSurfaceGetHeight(IOSurfaceRef), IOSurfaceGetBytesPerRow(IOSurfaceRef);
void *IOSurfaceGetBaseAddress(IOSurfaceRef);
uint32_t IOSurfaceGetSeed(IOSurfaceRef), IOSurfaceGetPixelFormat(IOSurfaceRef);
int IOSurfaceLock(IOSurfaceRef, uint32_t, uint32_t *), IOSurfaceUnlock(IOSurfaceRef, uint32_t, uint32_t *);
void CFRelease(const void *);
int main(int argc, char **argv)
{
	size_t w = argc > 3 ? atoi(argv[2]) : 750, h = argc > 3 ? atoi(argv[3]) : 1624;
	int n = 0;
	if (argc < 2) return 2;
	for (uint32_t id = 1; id < 4096; id++) {
		IOSurfaceRef s = IOSurfaceLookup(id);
		if (!s) continue;
		if (IOSurfaceGetWidth(s) == w && IOSurfaceGetHeight(s) == h) {
			char path[256]; snprintf(path, sizeof path, "%s-%d.raw", argv[1], n++);
			FILE *o = fopen(path, "wb");
			IOSurfaceLock(s, 1, NULL);
			const uint8_t *p = IOSurfaceGetBaseAddress(s); size_t bpr = IOSurfaceGetBytesPerRow(s);
			for (size_t y = 0; y < h; y++) fwrite(p + y * bpr, 1, w * 4, o);
			IOSurfaceUnlock(s, 1, NULL);
			fclose(o);
			printf("id %u -> %s\n", id, path);
		}
		CFRelease(s);
	}
	if (!n) { printf("fbshot: no surface of %zux%zu\n", w, h); return 1; }
	return 0;
}
