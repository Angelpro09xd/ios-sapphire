#!/usr/bin/env python3
# launchd-job.py add|remove JOB.plist IN OUT: launchd's cache (/System/Library/xpc/launchd.plist) with / without the
# job JOB.plist (its dict, under /System/Library/LaunchDaemons/<Label>.plist). Every other job is left as is.
import plistlib, sys


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in ('add', 'remove'):
        sys.exit('usage: launchd-job.py add|remove JOB.plist IN OUT')
    what, jobfile, src, dst = sys.argv[1:]
    job = plistlib.load(open(jobfile, 'rb'))
    key = '/System/Library/LaunchDaemons/%s.plist' % job['Label']
    raw = open(src, 'rb').read()
    fmt = plistlib.FMT_BINARY if raw[:8] == b'bplist00' else plistlib.FMT_XML
    p = plistlib.loads(raw)
    d = p['LaunchDaemons']
    before = len(d)
    if what == 'add':
        d[key] = job
    else:
        d.pop(key, None)
    with open(dst, 'wb') as f:
        plistlib.dump(p, f, fmt=fmt)
    q = plistlib.load(open(dst, 'rb'))['LaunchDaemons']
    assert (key in q) == (what == 'add')
    assert set(q) - {key} == set(d) - {key}, 'other jobs changed'
    print(job['Label'], what, 'jobs', before, '->', len(q), 'ok')


if __name__ == '__main__':
    main()
