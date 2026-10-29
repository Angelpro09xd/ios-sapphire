#!/usr/bin/env python3
# fpd-launchd.py add|remove IN OUT: launchd's cache (/System/Library/xpc/launchd.plist) with / without the
# com.sapphire.fpd job (the dict of com.sapphire.fpd.plist next to this script). Every other job is left as is.
import os, plistlib, sys

KEY = '/System/Library/LaunchDaemons/com.sapphire.fpd.plist'


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ('add', 'remove'):
        sys.exit('usage: fpd-launchd.py add|remove IN OUT')
    what, src, dst = sys.argv[1:]
    raw = open(src, 'rb').read()
    fmt = plistlib.FMT_BINARY if raw[:8] == b'bplist00' else plistlib.FMT_XML
    p = plistlib.loads(raw)
    d = p['LaunchDaemons']
    before = len(d)
    if what == 'add':
        job = plistlib.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'com.sapphire.fpd.plist'), 'rb'))
        assert job['Label'] == 'com.sapphire.fpd'
        d[KEY] = job
    else:
        d.pop(KEY, None)
    with open(dst, 'wb') as f:
        plistlib.dump(p, f, fmt=fmt)
    q = plistlib.load(open(dst, 'rb'))['LaunchDaemons']
    assert (KEY in q) == (what == 'add')
    assert set(q) - {KEY} == set(d) - {KEY}, 'other jobs changed'
    print('jobs', before, '->', len(q), 'format', 'binary' if fmt == plistlib.FMT_BINARY else 'xml', 'ok')


if __name__ == '__main__':
    main()
