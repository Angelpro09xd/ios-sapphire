#!/usr/bin/env python3
# sapphire-mg-scale2.py CACHE3X CACHE2X OUT: for iOS drawing at 2x (xnu 0239, sweetfb_scale=2).
# MobileGestalt computes the screen's class from its pixel size and scale, and 750x1624 @2x is no
# iPhone it knows: class -1, and UIKit has no status bar for it (the red "rdar://45025538" bar).
# This takes the cache it wrote at 2x (CACHE2X: com.apple.MobileGestalt.plist in
# /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/
# Caches, after deleting it and booting at 2x) and gives it back the iPhone X class (15) and the
# point-based geometry of the 3x one (CACHE3X), keeping the 2x pixel size and scale. The offsets
# are 20H392's (iOS 16.7.16); a cache of another build needs them checked (mgdiff of the two).
import plistlib, sys
c3 = plistlib.load(open(sys.argv[1], 'rb'))
c2 = plistlib.load(open(sys.argv[2], 'rb'))
data = bytearray(c2['CacheData'])
old = c3['CacheData']
for off in (3016, 5129):                    # the screen class, and a flag that went with it
    print('CacheData[%d]: %02x -> %02x' % (off, data[off], old[off]))
    data[off] = old[off]
c2['CacheData'] = bytes(data)
ex = c2['CacheExtra']
dims = bytearray(ex['oBbtJ8x+s1q0OkaiocPuog'])
dims[20:24] = c3['CacheExtra']['oBbtJ8x+s1q0OkaiocPuog'][20:24]   # screen class 15, not -1
ex['oBbtJ8x+s1q0OkaiocPuog'] = bytes(dims)
for k in ('rJkMAGeVLdhP5+10G5hVcA', 'pMeQxE5szZTjLMk10TisDQ'):      # points, as at 3x
    ex[k] = c3['CacheExtra'][k]
plistlib.dump(c2, open(sys.argv[3], 'wb'), fmt=plistlib.FMT_BINARY)
print('written', sys.argv[3])
