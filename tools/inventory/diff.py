#!/usr/bin/env python3
"""diff.py BASE.txt NOW.txt [OUTDIR]: what a running iOS root has beyond the image first flashed.

Both listings are "TYPE MODE UID GID SIZE MTIME PATH [-> TARGET]" (BASE from `find -printf` on the mounted
ios16-root.img, NOW from the phone through the Android app's HFS+ reader, HfsWalk). Prints a summary by area,
and writes OUTDIR/{added,changed,removed}.txt. Areas marked PERSONAL are never packaged by the installer.
"""
import collections, os, sys

# areas, first match wins: (prefix, label, personal)
AREAS = [
    ('/private/var/mobile/Containers/Data', 'datos de apps', True),
    ('/private/var/mobile/Containers/Shared', 'datos compartidos de apps', True),
    ('/private/var/mobile/Media', 'fotos, música, descargas', True),
    ('/private/var/mobile/Library/SMS', 'mensajes', True),
    ('/private/var/mobile/Library', 'ajustes y bases del usuario', True),
    ('/private/var/mobile/Documents', 'documentos', True),
    ('/private/var/containers/Bundle', 'apps instaladas por el usuario', True),
    ('/private/var/containers', 'contenedores', True),
    ('/private/var/keybags', 'llavero/keybag', True),
    ('/private/var/wireless', 'telefonía/SIM', True),
    ('/private/var/preferences', 'preferencias del sistema (Wi-Fi...)', True),
    ('/private/var/Keychains', 'llavero', True),
    ('/private/var/db', 'bases del sistema', True),
    ('/private/var/log', 'registros', True),
    ('/private/var/logs', 'registros', True),
    ('/private/var/mobile', 'otros de mobile', True),
    ('/private/var/root/Library', 'datos de root', True),
    ('/private/var/root', 'herramientas en /var/root', False),
    ('/private/var/tmp', 'temporales', True),
    ('/private/var/folders', 'temporales', True),
    ('/private/var/networkd', 'red', True),
    ('/private/var/installd', 'installd', True),
    ('/private/var/jb', 'jailbreak (/var/jb)', False),
    ('/private/preboot', 'preboot (jailbreak, cryptexes)', False),
    ('/private/var', 'otros de /var', True),
    ('/redwood', 'redwood (GL, Mesa, bridges)', False),
    ('/System/Library/xpc', 'launchd (caché de trabajos)', False),
    ('/System/Library/LaunchDaemons', 'LaunchDaemons', False),
    ('/System/Library/Caches', 'cachés del sistema', False),
    ('/System', 'otros de /System', False),
    ('/Applications', 'apps del sistema', False),
    ('/usr', '/usr', False),
    ('/Library', '/Library', False),
    ('/', 'raíz', False),
]


def area(p):
    for pre, label, personal in AREAS:
        if p == pre or p.startswith(pre.rstrip('/') + '/') or pre == '/':
            return label, personal
    return 'otros', False


def load(path):
    d = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        parts = line.split(' ', 6)
        if len(parts) < 7:
            continue
        t, mode, uid, gid, size, mtime, rest = parts
        target = None
        if ' -> ' in rest:
            rest, target = rest.split(' -> ', 1)
        if t == 'h':
            t = 'f'
        if rest == '' or rest == '/':
            continue
        d[rest] = (t, int(mode, 8), int(uid), int(gid), int(size), int(float(mtime)), target)
    return d


base, now = load(sys.argv[1]), load(sys.argv[2])
out = sys.argv[3] if len(sys.argv) > 3 else None
added = [p for p in now if p not in base]
removed = [p for p in base if p not in now]
changed = []
for p in now:
    if p in base:
        b, n = base[p], now[p]
        if b[0] != n[0] or (n[0] == 'f' and (b[4] != n[4] or b[5] != n[5])) or b[1] != n[1] or b[2:4] != n[2:4] or b[6] != n[6]:
            changed.append(p)

for name, lst in (('añadidos', added), ('modificados', changed), ('borrados', removed)):
    agg = collections.OrderedDict()
    for p in lst:
        label, personal = area(p)
        src = now if p in now else base
        e = agg.setdefault((label, personal), [0, 0])
        e[0] += 1
        e[1] += max(src[p][4], 0) if src[p][0] == 'f' else 0
    print('== %s: %d' % (name, len(lst)))
    for (label, personal), (n, b) in sorted(agg.items(), key=lambda kv: -kv[1][1]):
        print('  %-42s %7d  %9.1f MB%s' % (label, n, b / 1e6, '   [PERSONAL: no se empaqueta]' if personal else ''))
if out:
    os.makedirs(out, exist_ok=True)
    for name, lst in (('added', added), ('changed', changed), ('removed', removed)):
        with open(os.path.join(out, name + '.txt'), 'w') as f:
            for p in sorted(lst):
                label, personal = area(p)
                e = now.get(p) or base.get(p)
                f.write('%s\t%s\t%s\t%d\t%s\n' % ('P' if personal else '-', e[0], label, e[4], p))
