#!/usr/bin/env python3
# sapphire-launchd-glload.py IN OUT: launchd's cache with a job that sets kern.sweet_glload=1 at boot (the app loader:
# the EAGL bridge for OpenGL ES apps and ElleKit's tweaks), if the loader is installed
import plistlib, sys
fmt = plistlib.FMT_BINARY if open(sys.argv[1], 'rb').read(8) == b'bplist00' else plistlib.FMT_XML
p = plistlib.load(open(sys.argv[1], 'rb'))
d = p['LaunchDaemons']
key = '/System/Library/LaunchDaemons/com.sapphire.glload.plist'
d[key] = {
    'Label': 'com.sapphire.glload',
    'ProgramArguments': ['/var/jb/bin/sh', '-c',
                         '[ -f /redwood/lib/libredwood-glload.dylib ] && /var/jb/usr/sbin/sysctl -w kern.sweet_glload=1'],
    'RunAtLoad': True,
    'LaunchOnlyOnce': True,
    'UserName': 'root',
    'POSIXSpawnType': 'Interactive',
}
plistlib.dump(p, open(sys.argv[2], 'wb'), fmt=fmt)
print('jobs', len(d), 'format', 'binary' if fmt == plistlib.FMT_BINARY else 'xml')
# check it reads back
q = plistlib.load(open(sys.argv[2], 'rb'))
assert q['LaunchDaemons'][key]['Label'] == 'com.sapphire.glload'
print('ok')
