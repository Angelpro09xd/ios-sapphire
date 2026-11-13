#!/var/jb/bin/bash
# sapphire-appwatch: run by launchd (com.sapphire.appwatch, WatchPaths /Applications and /var/jb/Applications)
# when those folders change. uicache only when the list of app bundles really changed: uicache itself touches
# the folders, and running it on every trigger looped every ThrottleInterval (SpringBoard reloading its apps
# each time, which closed the SIM PIN prompt).
export PATH=/var/jb/usr/bin:/var/jb/bin:/usr/bin:/bin:/usr/sbin:/sbin
STATE=/var/tmp/sapphire-appwatch.state
sleep 3
killall -0 SpringBoard 2>/dev/null || exit 0
now=$(ls -1d /Applications/*.app /var/jb/Applications/*.app 2>/dev/null | md5sum | cut -d' ' -f1)
if [ ! -f $STATE ]; then
	echo "$now" > $STATE        # the first look after a boot: what is there now is registered already
	exit 0
fi
[ "$(cat $STATE)" = "$now" ] && exit 0
echo "$now" > $STATE
echo "$(date '+%F %T') apps changed: uicache"
uicache -a
