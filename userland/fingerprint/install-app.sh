#!/bin/bash
# install-app.sh IPA [PHONE]: the Huella app onto the phone, from WSL.
# With TrollStore: its trollstorehelper (found under /var/containers/Bundle/Application/*/TrollStore.app) installs
# the IPA as root ("install"), keeping the entitlements (no sandbox, platform-application). Without it: the bundle
# goes to /Applications/Huella.app and uicache registers it (as Sileo was installed). The IPA is also left at
# /var/mobile/Documents/Huella.ipa, to install by hand from TrollStore if neither works.
set -e
ipa=$1
P=${2:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
[ -f "$ipa" ] || { echo "usage: install-app.sh IPA [PHONE]"; exit 2; }
scp -q $O "$ipa" $P:/var/mobile/Documents/Huella.ipa
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin
chown 501:501 /var/mobile/Documents/Huella.ipa
h=$(ls -d /var/containers/Bundle/Application/*/TrollStore.app/trollstorehelper 2>/dev/null | head -1)
if [ -n "$h" ]; then
	echo "trollstorehelper: $h"
	if "$h" install /var/mobile/Documents/Huella.ipa; then
		echo "Huella installed with TrollStore"
		exit 0
	fi
	echo "trollstorehelper failed (code $?): trying /Applications"
fi
if ! command -v uicache >/dev/null; then
	echo "no TrollStore helper and no uicache: install /var/mobile/Documents/Huella.ipa from the TrollStore app"
	exit 0
fi
t=$(mktemp -d)
cd $t && unzip -q /var/mobile/Documents/Huella.ipa 2>/dev/null || python3 -c "import zipfile;zipfile.ZipFile('/var/mobile/Documents/Huella.ipa').extractall('.')" 2>/dev/null
if [ ! -f $t/Payload/Huella.app/Huella ]; then echo "could not unpack the IPA on the phone: install it from the TrollStore app"; exit 0; fi
rm -rf /Applications/Huella.app
/var/jb/bin/mv $t/Payload/Huella.app /Applications/Huella.app
chown -R root:wheel /Applications/Huella.app; chmod 755 /Applications/Huella.app/Huella
uicache -p /Applications/Huella.app && echo "Huella installed in /Applications (uicache)"
rm -rf $t
EOF
