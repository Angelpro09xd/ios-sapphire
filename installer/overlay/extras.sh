#!/bin/bash
# extras.sh: the overlay's parts built from this repo (not read from a phone), merged into
# out/sapphire-overlay.tar.gz (made by make-overlay.sh) at their iOS paths, owned by root:
#   /System/Library/Extensions/SapphireMetal.bundle       metal/build.sh: Metal's "driver", gives backboardd
#                                                          the GL bridge (without it the screen stays on the logo)
#   /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver userland/audio: iOS's sound output
#   /Applications/Huella.app                               userland/fingerprint: the fingerprint app
# (CommCenter's ccbridge, /usr/appleinternal/lib/liblinkguard.dylib, comes from /redwood/lib in the overlay:
#  recovery/sapphire.sh copies it.)
set -euo pipefail
cd "$(dirname "$0")"
R=$(cd ../.. && pwd)
export PATH=/usr/lib/llvm-18/bin:$PATH
SDK=${SDK:-$HOME/sdks/theos-sdks/iPhoneOS16.5.sdk}
LDID=${LDID:-$HOME/tools-dl/ldid/ldid}
E=out/extras
rm -rf $E
mkdir -p $E/System/Library/Extensions $E/System/Library/Audio/Plug-Ins/HAL $E/Applications

bash $R/metal/build.sh $(pwd)/out/metal >/dev/null
cp -r out/metal/SapphireMetal.bundle $E/System/Library/Extensions/

SDK=$SDK bash $R/userland/audio/build.sh >/dev/null
"$LDID" -S $R/userland/obj/RedwoodAudio.driver/RedwoodAudio
cp -r $R/userland/obj/RedwoodAudio.driver $E/System/Library/Audio/Plug-Ins/HAL/

rm -rf out/fp && mkdir -p out/fp
SDK=$SDK LDID=$LDID bash $R/userland/fingerprint/build.sh $(pwd)/out/fp >/dev/null
(cd out/fp && rm -rf Payload && python3 -I -c "import zipfile,sys; zipfile.ZipFile('Huella.ipa').extractall('.')")
cp -r out/fp/Payload/Huella.app $E/Applications/

# modes as on the phone: executables 755, the rest 644, folders 755
find $E -type d -exec chmod 755 {} +
find $E -type f -exec chmod 644 {} +
chmod 755 $E/System/Library/Extensions/SapphireMetal.bundle/SapphireMetal \
    $E/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver/RedwoodAudio $E/Applications/Huella.app/Huella

python3 -I - out/sapphire-overlay.tar.gz $E <<'PY'
import io, os, sys, tarfile, time
src, extras = sys.argv[1], sys.argv[2]
add = {}
for d, dirs, files in os.walk(extras):
    for n in sorted(dirs) + sorted(files):
        p = os.path.join(d, n)
        add[os.path.relpath(p, extras)] = p
old = tarfile.open(src, 'r:gz')
tmp = src + '.tmp'
new = tarfile.open(tmp, 'w:gz', format=tarfile.GNU_FORMAT, compresslevel=9)
for m in old:
    if m.name.rstrip('/') in add:
        continue                        # replaced by the repo's build
    new.addfile(m, old.extractfile(m) if m.isfile() else None)
for name in sorted(add):
    ti = new.gettarinfo(add[name], name)
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = ''
    if ti.isfile():
        with open(add[name], 'rb') as f:
            new.addfile(ti, f)
    else:
        new.addfile(ti)
new.close()
os.replace(tmp, src)
print('extras merged:', len(add), 'entries')
PY
ls -la out/sapphire-overlay.tar.gz
