N=/home/angel/ios-sapphire/installer/tools-arm64/heic/wallpaper-notes
cd $N/raw
find . -type f ! -iname '*.heic' | sort | while read -r f; do echo "##### $f"; file -b "$f"; done
python3 -I - <<'EOF'
import plistlib, glob, os, json
os.chdir('/home/angel/ios-sapphire/installer/tools-arm64/heic/wallpaper-notes/raw')
for f in sorted(glob.glob('**/*.plist', recursive=True)) + sorted(glob.glob('**/*.loctable', recursive=True)):
    with open(f,'rb') as fh: d = plistlib.load(fh)
    out = '/home/angel/ios-sapphire/installer/tools-arm64/heic/wallpaper-notes/' + f.replace('/','__') + '.xml.txt'
    with open(out,'wb') as o: plistlib.dump(d, o, fmt=plistlib.FMT_XML)
    print('#####', f); print(open(out).read())
EOF
