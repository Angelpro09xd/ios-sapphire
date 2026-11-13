# recipes.d/10-sileo-wallpaper.sh: sourced by recovery/sapphire.sh's recipes() (OS, S, mod_copy, plistop, MOVES,
# $S/excl are there).

# Sileo also in /Applications (as on the phone the installer reproduces): a system app iOS lists at its first
# boot; the jailbreak's own copy in /var/jb/Applications stays
s=$S/ovl/private/var/jb/Applications/Sileo.app
if [ -d "$s" ] && [ ! -e "$OS/Applications/Sileo.app" ]; then
	MOVES="$MOVES -g /Applications/Sileo.app=$s"
fi

# The default wallpaper (7400 "WWDC 2022", a layered animation): its pictures are HEIC, which iOS decodes here
# with SapphireHEIF (the tweak: libheif + libde265; no hardware HEVC). The @3x variant stays as Apple made it;
# an @2x one is added, as the screen runs at 375x812 @2x and the IPSW only has @3x ones: the same files.
W=/Library/Wallpaper/Collections/iOS_16~iphone.wallpaperCollection/Wallpapers
w3=$W/7400.WWDC_2022-375w-812h@3x~iphone.wallpaper
w2=$W/7400.WWDC_2022-375w-812h@2x~iphone.wallpaper
if [ -d "$OS$w3" ]; then
	step "$(t "Fondo de pantalla" "Wallpaper")"
	mkdir -p "$S/mod$w2"
	cp -p "$OS$w3/Wallpaper.plist" "$S/mod$w2/Wallpaper.plist"
	plistop "$S/mod$w2/Wallpaper.plist" set logicalScreenClass string "375w-812h@2x~iphone" || fail "Wallpaper.plist"
	for ca in "$OS$w3"/*.ca; do
		b=${ca##*/}
		mkdir -p "$S/mod$w2/$b/assets"
		for f in "$ca"/*; do
			[ -f "$f" ] && cp -p "$f" "$S/mod$w2/$b/"
		done
		cp -p "$ca"/assets/* "$S/mod$w2/$b/assets/"
	done
	echo "$(t "fondo 7400: variante @2x (HEIC)" "wallpaper 7400: @2x variant (HEIC)")"
fi
