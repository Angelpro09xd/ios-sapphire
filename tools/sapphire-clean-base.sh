#!/var/jb/bin/sh
# A clean base on the phone, as root (scp it there and run it, over SSH):
#   sh sapphire-clean-base.sh         turn off what crash-loops or relaunches without end
#   sh sapphire-clean-base.sh undo    put everything back
#
# Since 0156 (AMFI) the system sees the app extensions, and those that need what this phone
# has not (Metal, the modem, Bluetooth, speech assets, a Secure Enclave) crash again and again:
# each crash wakes ReportCrash and launchd, and two to three minutes after a boot the phone
# freezes and SSH cannot get in (2026-10-23: speech synthesizers ~30 crashes each, assetsd 16,
# MusicWidgets 11, AegirPoster 10, wifip2pd relaunched 28 times in one boot).
#
# Extensions are moved to $B (origen.txt keeps where each came from); daemons are disabled with
# launchctl (kept in launchd's disabled list, so across boots). Both are undone by "undo".
# Turn things back on one at a time, when they are needed and can work.
export PATH=/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/usr/bin:/bin:/usr/sbin
B=/private/var/mobile/disabled-extensions
L=/var/jb/usr/bin/launchctl

EXTENSIONS="
/System/Library/CoreServices/AegirProxyApp.app/PlugIns/AegirPoster.appex
/System/Library/PrivateFrameworks/EmojiPoster.framework/PlugIns/EmojiPosterExtension.appex
/System/Library/PrivateFrameworks/MacinTalk.framework/PlugIns/MacinTalkAUSP.appex
/System/Library/PrivateFrameworks/SiriTTSService.framework/PlugIns/SiriTTSSynthesizerAU.appex
/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/PlugIns/KonaSynthesizer.appex
/System/Library/PrivateFrameworks/MobileTimer.framework/PlugIns/MobileTimerIntents.appex
/System/Library/PrivateFrameworks/CalendarWidget.framework/PlugIns/IntentsExtension.appex
/Applications/Music.app/PlugIns/MusicWidgets.appex
/Applications/Home.app/PlugIns/HomeWidget.appex
/Applications/Home.app/PlugIns/HomeWidgetSingleAccessoryIntent.appex
/Applications/Books.app/PlugIns/BooksWidgetExtension.appex
/Applications/Weather.app/PlugIns/WeatherWidget.appex
/Applications/AppleTV.app/PlugIns/TVWidgetExtension.appex
/Applications/Stocks.app/PlugIns/StocksDetailIntents.appex
/Applications/Passbook.app/PlugIns/PassbookWidgetsExtension-iPhone.appex
/Applications/AppStore.app/PlugIns/AppStoreWidgetsExtension.appex
/System/Library/AppPlaceholders/Music.app/PlugIns/MusicWidgets.appex
/System/Library/AppPlaceholders/Home.app/PlugIns/HomeWidget.appex
/System/Library/AppPlaceholders/Home.app/PlugIns/HomeWidgetSingleAccessoryIntent.appex
/System/Library/AppPlaceholders/Books.app/PlugIns/BooksWidgetExtension.appex
/System/Library/AppPlaceholders/Weather.app/PlugIns/WeatherWidget.appex
/System/Library/AppPlaceholders/AppleTV.app/PlugIns/TVWidgetExtension.appex
/System/Library/AppPlaceholders/Stocks.app/PlugIns/StocksDetailIntents.appex
/System/Library/AppPlaceholders/Passbook.app/PlugIns/PassbookWidgetsExtension-iPhone.appex
"
# wifip2pd: AWDL / Wi-Fi Direct, no such radio mode here; assetsd: the photo library daemon,
# crash-looping (Photos needs it: turn it back on when Photos is worked on)
DAEMONS="com.apple.wifip2pd com.apple.assetsd"

if [ "${1:-}" = undo ]; then
	[ -f "$B/origen.txt" ] && while read -r path; do
		name=${path##*/}
		[ -n "$name" ] && [ -d "$B/$name" ] && [ ! -e "$path" ] && mv "$B/$name" "$path" && echo "back: $path"
	done < "$B/origen.txt"
	: > "$B/origen.txt"
	[ -f "$B/more/origen.txt" ] && while read -r file path; do
		[ -n "$file" ] && [ -d "$B/more/$file" ] && [ ! -e "$path" ] && mv "$B/more/$file" "$path" && echo "back: $path"
	done < "$B/more/origen.txt"
	[ -f "$B/more/origen.txt" ] && : > "$B/more/origen.txt"
	for d in $DAEMONS; do
		$L enable "system/$d" && echo "enabled: $d"
	done
	sync
	exit 0
fi

mkdir -p "$B"
for path in $EXTENSIONS; do
	name=${path##*/}
	[ -d "$path" ] || continue
	if [ -e "$B/$name" ]; then
		# the same bundle name from another place (Applications and AppPlaceholders): keep both
		mkdir -p "$B/more"
		n=0
		while [ -e "$B/more/$n-$name" ]; do n=$((n + 1)); done
		mv "$path" "$B/more/$n-$name" && echo "$n-$name $path" >> "$B/more/origen.txt" && echo "off: $path"
	else
		mv "$path" "$B/$name" && echo "$path" >> "$B/origen.txt" && echo "off: $path"
	fi
done
for d in $DAEMONS; do
	$L disable "system/$d" 2>/dev/null
	$L bootout "system/$d" 2>/dev/null
	echo "disabled: $d"
done
sync
