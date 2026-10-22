#!/bin/sh
# Builds the turnip capture program. Needs glslangValidator in PATH and a Mesa tree.
set -e
cd "$(dirname "$0")"
MESA=${MESA:-/serverhive/angelpro09devv/mesa-src}
mkdir -p out
glslangValidator -V --vn quad_vert -o out/quad.vert.h quad.vert >/dev/null
glslangValidator -V --vn quad_frag -o out/quad.frag.h quad.frag >/dev/null
cc -O1 -g -I"$MESA/include" -Iout -o out/cap cap.c -ldl
echo built out/cap
