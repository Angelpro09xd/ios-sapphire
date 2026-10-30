#!/usr/bin/env python3
"""bt-launchd.py: Bluetooth jobs in launchd's cache (/System/Library/xpc/launchd.plist).

  bt-launchd.py install IN OUT --orig DIR --job com.sapphire.btshim.plist --addr AA:BB:.. [--fake] [--jobs-out DIR]
  bt-launchd.py uninstall IN OUT --orig DIR

install:
  com.sapphire.btshim   added/replaced (ProgramArguments "daemon", or "daemon -F -v -w 0" with --fake;
                        BT_DEVICE_ADDRESS)
  com.apple.bluetoothd  put back (tools/sapphire-ssh-install.sh deletes it from the cache) from Apple's plist in
                        --orig, with EnvironmentVariables BT_HCI_TRANSPORT=VIRT:VIRT:/var/run/btshim/hci and
                        BTBRIDGE_ADDRESS (for btbridge.dylib); _PanicOnCrash keys renamed _PanicOnCrasX as in the image build
  com.apple.BlueTool    ProgramArguments -> /redwood/bin/btshim bluetool (MachServices, UserName kept)
  (bluetoothd's ProgramArguments -> /redwood/bin/btshim bdstart, which execs it with btbridge.dylib inserted)
uninstall: btshim job removed, bluetoothd job removed again (the state before), BlueTool back to Apple's plist.
Every other job is left as it is. --jobs-out writes the three job dicts as plists (for launchctl bootstrap now).
--orig DIR holds Apple's com.apple.bluetoothd.plist and com.apple.BlueTool.plist (20H392 OS dmg,
~/ios-build/bt/System/Library/LaunchDaemons).
"""
import argparse
import os
import plistlib
import re
import sys

LD = '/System/Library/LaunchDaemons/%s.plist'
TRANSPORT = 'VIRT:VIRT:/var/run/btshim/hci'


def find_key(d, label):
    for k, v in d.items():
        if isinstance(v, dict) and v.get('Label') == label:
            return k
    return None


def depanic(job):
    for k in list(job):
        if k == '_PanicOnCrash':
            job['_PanicOnCrasX'] = job.pop(k)
    return job


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what', choices=('install', 'uninstall'))
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--orig', required=True)
    ap.add_argument('--job')
    ap.add_argument('--addr')
    ap.add_argument('--fake', action='store_true')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--jobs-out')
    a = ap.parse_args()

    raw = open(a.src, 'rb').read()
    fmt = plistlib.FMT_BINARY if raw[:8] == b'bplist00' else plistlib.FMT_XML
    p = plistlib.loads(raw)
    d = p['LaunchDaemons']
    before = {k for k in d}
    orig_bd = plistlib.load(open(os.path.join(a.orig, 'com.apple.bluetoothd.plist'), 'rb'))
    orig_bt = plistlib.load(open(os.path.join(a.orig, 'com.apple.BlueTool.plist'), 'rb'))
    touched = set()

    k_bt = find_key(d, 'com.apple.BlueTool') or LD % 'com.apple.BlueTool'
    k_bd = find_key(d, 'com.apple.bluetoothd') or LD % 'com.apple.bluetoothd'
    k_sh = find_key(d, 'com.sapphire.btshim') or LD % 'com.sapphire.btshim'
    touched |= {k_bt, k_bd, k_sh}

    if a.what == 'install':
        if not a.job or not a.addr or not re.fullmatch(r'([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', a.addr):
            sys.exit('install needs --job and --addr aa:bb:cc:dd:ee:ff')
        sh = plistlib.load(open(a.job, 'rb'))
        sh['ProgramArguments'] = ['/redwood/bin/btshim', 'daemon'] + (['-F', '-v', '-w', '0'] if a.fake else (['-v'] if a.verbose else []))
        sh.setdefault('EnvironmentVariables', {})['BT_DEVICE_ADDRESS'] = a.addr.lower()
        d[k_sh] = sh

        bd = depanic(dict(d.get(k_bd) or orig_bd))
        env = dict(bd.get('EnvironmentVariables', {}))
        env['BT_HCI_TRANSPORT'] = TRANSPORT
        # not BT_DEVICE_ADDRESS: bluetoothd zeroes it and then skips the registry lookup btbridge answers
        env.pop('BT_DEVICE_ADDRESS', None)
        env['BTBRIDGE_ADDRESS'] = a.addr.lower()
        bd['EnvironmentVariables'] = env
        # launchd drops DYLD_*: btshim bdstart sets DYLD_INSERT_LIBRARIES=/redwood/lib/btbridge.dylib and execs
        # /usr/sbin/bluetoothd (its BT_DEVICE_ADDRESS is zeroed in release builds; btbridge supplies the address)
        bd['ProgramArguments'] = ['/redwood/bin/btshim', 'bdstart']
        bd.pop('Program', None)
        d[k_bd] = bd

        bt = dict(d.get(k_bt) or orig_bt)
        bt['ProgramArguments'] = ['/redwood/bin/btshim', 'bluetool']
        bt['StandardOutPath'] = bt['StandardErrorPath'] = '/var/tmp/btshim-bluetool.out'
        d[k_bt] = bt
        jobs = {'com.sapphire.btshim': sh, 'com.apple.bluetoothd': bd, 'com.apple.BlueTool': bt}
    else:
        d.pop(k_sh, None)
        d.pop(k_bd, None)
        d[k_bt] = orig_bt
        jobs = {}

    with open(a.dst, 'wb') as f:
        plistlib.dump(p, f, fmt=fmt)
    q = plistlib.load(open(a.dst, 'rb'))['LaunchDaemons']
    for k in (before | set(q)) - touched:
        assert q.get(k) == plistlib.loads(raw)['LaunchDaemons'].get(k), 'other job changed: ' + k
    if a.what == 'install':
        assert q[k_bd]['EnvironmentVariables']['BT_HCI_TRANSPORT'] == TRANSPORT
        assert q[k_bt]['ProgramArguments'][0] == '/redwood/bin/btshim'
    else:
        assert k_sh not in q and k_bd not in q and q[k_bt]['ProgramArguments'][0] == '/usr/sbin/BlueTool'
    if a.jobs_out:
        os.makedirs(a.jobs_out, exist_ok=True)
        for label, job in jobs.items():
            with open(os.path.join(a.jobs_out, label + '.plist'), 'wb') as f:
                plistlib.dump(job, f, fmt=plistlib.FMT_XML)
    print('bluetooth jobs', a.what, ('(recording mode)' if a.fake else ''), 'jobs', len(before), '->', len(q), 'ok')


if __name__ == '__main__':
    main()
