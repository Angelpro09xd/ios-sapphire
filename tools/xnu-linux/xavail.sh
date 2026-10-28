#!/bin/bash
# xavail.sh: the macOS 13.5 tags of the projects the xnu build uses, and AvailabilityVersions installed
# into the fakeroot (availability.pl)
set -e
L=~/xnu-local
curl -s https://raw.githubusercontent.com/apple-oss-distributions/distribution-macOS/macos-135/release.json > $L/release.json
python3 - $L/release.json <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for p in d["projects"]:
    if p["project"] in ("AvailabilityVersions", "libdispatch", "Libsystem", "libplatform", "dtrace", "bootstrap_cmds", "xnu", "libpthread"):
        print(p["project"], p["tag"])
PY
tag=$(python3 -c 'import json,sys; print([p["tag"] for p in json.load(open(sys.argv[1]))["projects"] if p["project"]=="AvailabilityVersions"][0])' $L/release.json)
rm -rf $L/AvailabilityVersions
git clone -q --depth 1 --branch "$tag" https://github.com/apple-oss-distributions/AvailabilityVersions.git $L/AvailabilityVersions
cd $L/AvailabilityVersions
ls
make install OBJROOT=$L/build/av SYMROOT=$L/build/av DSTROOT=$L/fakeroot > $L/av.log 2>&1 || { tail -20 $L/av.log; exit 1; }
find $L/fakeroot -name 'availability*' | head
