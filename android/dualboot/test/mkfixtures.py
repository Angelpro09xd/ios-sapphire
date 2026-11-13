#!/usr/bin/env python3
# mkfixtures.py OUTDIR: inputs for FormatsTest -- a binary and an XML Info.plist, a CgBI ("crushed", as Xcode
# writes iOS icons) PNG of known pixels, and a small IPA (zip: Payload/Demo.app/Info.plist + icons).
import os, plistlib, struct, sys, zipfile, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
info = {
	'CFBundleDisplayName': 'Demo & Co',
	'CFBundleName': 'Demo',
	'CFBundleIdentifier': 'com.example.demo',
	'CFBundleShortVersionString': '1.2.3',
	'CFBundleVersion': '45',
	'CFBundleExecutable': 'Demo',
	'MinimumOSVersion': '14.0',
	'UIDeviceFamily': [1, 2],
	'Big': 2 ** 40,
	'Ratio': 0.5,
	'Flag': True,
	'Blob': b'\x00\x01\x02',
	'Unicode': 'Configuración ✓',
	'CFBundleIcons': {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon60x60'], 'CFBundleIconName': 'AppIcon'}},
}
open(os.path.join(out, 'info.bplist'), 'wb').write(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
open(os.path.join(out, 'info.xml'), 'wb').write(plistlib.dumps(info, fmt=plistlib.FMT_XML))


def chunk(t, d):
	return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)


# 3x2 pixels, RGBA, one of them half transparent
px = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 255, 128), (10, 20, 30, 255), (0, 0, 0, 0)]
w, h = 3, 2
# crushed: BGRA, premultiplied, rows with filter type 1 (sub) to exercise the unfiltering, raw deflate
rows = b''
for y in range(h):
	line = []
	for x in range(w):
		r, g, b, a = px[y * w + x]
		line += [b * a // 255, g * a // 255, r * a // 255, a]
	filt = [line[i] - (line[i - 4] if i >= 4 else 0) & 0xff for i in range(len(line))]
	rows += bytes([1] + filt)
co = zlib.compressobj(9, zlib.DEFLATED, -15)
idat = co.compress(rows) + co.flush()
png = b'\x89PNG\r\n\x1a\n' + chunk(b'CgBI', b'\x50\x00\x20\x06') + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) \
	+ chunk(b'IDAT', idat) + chunk(b'IEND', b'')
open(os.path.join(out, 'icon-cgbi.png'), 'wb').write(png)
# the expected ARGB after un-premultiplying (integer maths as Png.java does it)
exp = []
for r, g, b, a in px:
	pr, pg, pb = r * a // 255, g * a // 255, b * a // 255
	if a not in (0, 255):
		pr, pg, pb = min(255, pr * 255 // a), min(255, pg * 255 // a), min(255, pb * 255 // a)
	exp.append('%08x' % (a << 24 | pr << 16 | pg << 8 | pb))
open(os.path.join(out, 'icon-cgbi.txt'), 'w').write(' '.join(exp))

with zipfile.ZipFile(os.path.join(out, 'demo.ipa'), 'w') as z:
	z.writestr('Payload/Demo.app/Info.plist', plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
	z.writestr('Payload/Demo.app/AppIcon60x60@2x.png', png)
	z.writestr('Payload/Demo.app/AppIcon60x60@3x.png', png + b'x')
	z.writestr('Payload/Demo.app/AppIcon76x76@2x~ipad.png', png + b'xx')
	z.writestr('Payload/Demo.app/Demo', b'\xcf\xfa\xed\xfe' + b'\0' * 1000)
	z.writestr('Payload/Demo.app/Frameworks/X.framework/Info.plist', b'not this one')
	z.writestr('Payload/Demo.app/es.lproj/InfoPlist.strings', '"CFBundleDisplayName" = "Demostración";\n'.encode('utf-16'))
