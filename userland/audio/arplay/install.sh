#!/bin/bash
# install.sh [PHONE]: arplay and the voice and record scripts onto the phone (root@192.168.7.2, key
# ~/.ssh/sapphire_phone), then the job restarted. Not during a call: it closes the voice graphs.
# The job itself is com.sapphire.arplay.plist's dict in launchd's cache, /System/Library/xpc/launchd.plist
# (LaunchDaemons, key /System/Library/LaunchDaemons/com.sapphire.arplay.plist; the cache before it:
# launchd.plist.pre-arplay): /var/jb/Library/LaunchDaemons is not loaded at boot here.
set -e
cd "$(dirname "$0")"
P=${1:-root@192.168.7.2}
O="-o ConnectTimeout=6 -i $HOME/.ssh/sapphire_phone"
out=$(mktemp -d)
bash build.sh "$out" > /dev/null
ssh $O $P 'mkdir -p /redwood/bin /redwood/share/arplay'
scp -q $O "$out/arplay" $P:/redwood/bin/arplay
scp -q $O "$out/voice_handset.arp" "$out/voice_speaker.arp" "$out/record_mic.arp" $P:/redwood/share/arplay/
ssh $O $P /var/jb/bin/bash -s <<'EOF'
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin
launchctl kickstart -k system/com.sapphire.arplay 2>/dev/null || echo "no job com.sapphire.arplay in launchd's cache"
sleep 5
tail -3 /var/tmp/arplay.log
EOF
rm -rf "$out"
