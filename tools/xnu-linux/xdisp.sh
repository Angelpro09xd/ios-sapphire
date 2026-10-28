#!/bin/bash
# xdisp.sh: libdispatch and libplatform of macOS 13.5 (libfirehose_kernel's sources and headers)
set -e
L=~/xnu-local
for proj in libdispatch libplatform; do
	tag=$(python3 -c 'import json,sys; print([p["tag"] for p in json.load(open(sys.argv[1]))["projects"] if p["project"]==sys.argv[2]][0])' $L/release.json $proj)
	echo "$proj $tag"
	[ -d $L/$proj ] || git clone -q --depth 1 --branch "$tag" https://github.com/apple-oss-distributions/$proj.git $L/$proj 2>&1 | grep -v -i 'detached\|advice\|switch\|^$\|git\|branch\|commit' || true
done
cat $L/libdispatch/xcodeconfig/libfirehose_kernel.xcconfig
ls $L/libdispatch/src/firehose
