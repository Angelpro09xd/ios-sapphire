#!/usr/bin/env python3
"""scrub.py IN.tar OUT.tar.gz: the overlay as it may be given to anyone.

- /var/jb/etc/master.passwd keeps every account but its password hash becomes '*' (locked; set one on the
  phone with `passwd`); sshd's host keys go (each phone makes its own).
- Third-party jailbreak packages are not ours to redistribute: their files (dpkg's lists) and dpkg records
  go; testers install them from Sileo (the repos stay). Also the ones the user did not want (AIM, BoldersReborn).
- Per-phone data goes or becomes a placeholder the recovery fills in for the phone being installed:
  the sensors' factory calibration (/var/root/sensfiles/mnt: copied from that phone's persist, read-only)
  and the Bluetooth address (@BDADDR@, made from that phone's serial number).
Members are copied byte for byte otherwise (numeric owners, modes, links).
"""
import io, re, sys, tarfile

SECRET = 'private/var/jb/etc/master.passwd'
DEVICE_BDADDR = b'f8:38:8f:07:bc:af'           # the phone the overlay was read from
DROP_PREFIXES = (
    'private/var/jb/etc/ssh/ssh_host_',
    'private/var/root/sensfiles/mnt',
    'private/var/jb/Library/dpkg/status-old',
)
# old builds kept next to the live ones on the developer's phone (not ccbridge.dylib.pre-naspoll: CommCenter's)
BACKUP = re.compile(r'\.(prev|bak|orig|lto1|nolto|oct22|server|stats|tc384|731db34|o2|new|old)$|\.pre-(?!naspoll$)|\.pre[0-9a-z]+$|/SapphireSampler\.')


def scrub_passwd(data):
    out = []
    for line in data.decode('utf-8', 'replace').splitlines():
        f = line.split(':')
        if len(f) > 2 and not line.startswith('#'):
            f[1] = '*'
        out.append(':'.join(f))
    return ('\n'.join(out) + '\n').encode()


REMOVE_PKGS = {
    # not wanted by the user
    'com.ali.aim', 'com.nightwind.boldersreborn',
    # third-party, closed or not licensed for redistribution: from Sileo instead
    'com.alphastream.linearforsb', 'com.fiore.calculatorconverter', 'com.ichitaso.iconorder',
    'com.ryannair05.cylinder', 'com.serena.appindex', 'com.spark.libsparkcolourpicker', 'com.spark.snowboard',
    'com.spark.snowboard.statusbar', 'com.spark.snowboardapplication', 'com.sreerag.duofree',
    'com.tigisoftware.filza',
}
DPKG = 'private/var/jb/Library/dpkg/'


def jb_name(path):
    # dpkg lists /var/jb/...; in the tar /var is private/var
    p = path.lstrip('/')
    return 'private/' + p if p.startswith('var/') else p


def drop_from_status(data):
    out = []
    for stanza in data.decode('utf-8', 'replace').split('\n\n'):
        pkg = next((l.split(':', 1)[1].strip() for l in stanza.splitlines() if l.startswith('Package:')), None)
        if pkg not in REMOVE_PKGS:
            out.append(stanza)
    return '\n\n'.join(out).encode()


src = tarfile.open(sys.argv[1], 'r:*')
members = src.getmembers()
listed = set()
for m in members:
    for pkg in REMOVE_PKGS:
        if m.name == DPKG + 'info/' + pkg + '.list':
            for line in src.extractfile(m).read().decode('utf-8', 'replace').splitlines():
                name = jb_name(line.strip())
                if name.startswith('private/var/jb/') and name != 'private/var/jb':
                    listed.add(name)
                    # MobileSubstrate/DynamicLibraries is a link to usr/lib/TweakInject
                    ms = 'private/var/jb/Library/MobileSubstrate/DynamicLibraries/'
                    if name.startswith(ms):
                        listed.add('private/var/jb/usr/lib/TweakInject/' + name[len(ms):])
dirs = {m.name.rstrip('/') for m in members if m.isdir()}
dropped = listed - dirs
# the package's folders go too once nothing else is left in them (shared ones stay)
names = [m.name.rstrip('/') for m in members]
for d in sorted(listed & dirs, key=len, reverse=True):
    if not any(x.startswith(d + '/') and x not in dropped for x in names):
        dropped.add(d)
dst = tarfile.open(sys.argv[2], 'w:gz', format=tarfile.GNU_FORMAT, compresslevel=9)
n = gone = 0
for m in members:
    name = m.name.rstrip('/')
    if name in dropped or name.startswith(DROP_PREFIXES) or (not m.isdir() and BACKUP.search(name)) or \
            any(m.name.startswith(DPKG + 'info/' + p + '.') for p in REMOVE_PKGS):
        gone += 1
        continue
    if m.isfile():
        data = src.extractfile(m).read()
        if m.name == SECRET:
            data = scrub_passwd(data)
            n += 1
        elif m.name == DPKG + 'status':
            data = drop_from_status(data)
        if DEVICE_BDADDR in data:
            data = data.replace(DEVICE_BDADDR, b'@BDADDR@')
            n += 1
        m.size = len(data)
        dst.addfile(m, io.BytesIO(data))
    else:
        if m.issym() and not m.linkname:
            # Hfs.head reads 1-byte symlinks as empty (to fix in the app's reader). The only ones are ncurses'
            # terminfo hex folders on a case-insensitive layout: 31 -> 1, 6a -> j
            base = m.name.rsplit('/', 1)
            if len(base) == 2 and base[0].endswith('usr/share/terminfo') and len(base[1]) == 2:
                m.linkname = chr(int(base[1], 16))
                n += 1
            else:
                sys.exit('empty symlink target: ' + m.name)
        dst.addfile(m)
dst.close()
print('scrubbed', n, 'file(s);', gone, 'entries left out')
