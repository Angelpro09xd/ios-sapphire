# 0.1.8 (testers' bugs, 2026-11-13). Each change keeps a copy of what it replaces in /var/root (once).
P=/redwood/bin/plistop
# qmitool (mobile data on the modem's default APN profile first, bug 23) is setuid root, as installed
chown 0:0 /redwood/bin/qmitool; chmod 4755 /redwood/bin/qmitool
# Wi-Fi ~5 min late at boot (bug 6): configd's InterfaceNamer waits for an IONetworkStack this kernel does not
# publish until WaitStackTimeout runs out (~300 s), and wifid waits for InterfaceNamer
N=/System/Library/SystemConfiguration/InterfaceNamer.bundle/Info.plist
[ -f /var/root/InterfaceNamer.Info.plist.orig ] || cp -p $N /var/root/InterfaceNamer.Info.plist.orig
$P $N set WaitStackTimeout int 2 set WaitQuietTimeout int 10
# launchd's cache, read at boot: fairplayd (exits before it checks in, so locationd and the Settings panes that
# ask it hung, bugs 8/27/30) replaced by fpstub, which answers its services with an error; com.sapphire.wifidkick
# restarts wifid only if it has not opened the device 25 s into the boot (kernel 0322's kern.sweet_wifid_opened)
C=/System/Library/xpc/launchd.plist
[ -f /var/root/launchd.plist.pre-0.1.8 ] || cp -p $C /var/root/launchd.plist.pre-0.1.8
F='LaunchDaemons:/System/Library/LaunchDaemons/com.apple.fairplayd.H2.plist'
if [ -n "$($P $C get "$F:Label" 2>/dev/null)" ]; then
	$P $C set "$F:Program" string /redwood/bin/fpstub del "$F:ProgramArguments"
fi
W='LaunchDaemons:/System/Library/LaunchDaemons/com.sapphire.wifidkick.plist'
if [ -n "$($P $C get "$W:Label" 2>/dev/null)" ]; then
	$P $C put "$W" /redwood/share/com.sapphire.wifidkick.cache-job.plist
fi
echo "0.1.8: fairplayd -> $($P $C get "$F:Program"), InterfaceNamer WaitStackTimeout $($P $N get WaitStackTimeout)"
