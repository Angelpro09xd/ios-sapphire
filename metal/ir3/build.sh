#!/bin/sh
# Build sapphire-ir3as against a Mesa build tree (meson, see GPU-ROADMAP.md layer 3):
#   metal/ir3/build.sh MESA_SRC MESA_BUILD
set -eu
src=$1 build=$2 here=$(cd "$(dirname "$0")" && pwd)
flags=$(python3 -I - "$build" <<'PY'
import json, shlex, sys
cc = json.load(open(sys.argv[1] + '/compile_commands.json'))
e = next(e for e in cc if e['file'].endswith('freedreno/ir3/tests/disasm.c'))
args = [a for a in shlex.split(e['command']) if a not in ('/usr/bin/ccache', 'cc', 'ccache')]
keep, skip = [], False
for a in args:
    if skip:
        skip = False
        continue
    if a in ('-o', '-c', '-MF', '-MQ'):
        skip = True
        continue
    if a.endswith('.c') or a in ('-MD',):
        continue
    keep.append(a)
print(' '.join(shlex.quote(a) for a in keep))
PY
)
cd "$build"
eval cc $flags -c "$here/sapphire-ir3as.c" -o "$here/sapphire-ir3as.o"
cc -o "$here/sapphire-ir3as" "$here/sapphire-ir3as.o" -Wl,--start-group \
    src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/isa/libir3encode.a src/util/libmesa_util.a \
    src/util/libmesa_util_clflush.a src/util/libmesa_util_clflushopt.a src/util/libmesa_util_simd.a \
    src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a src/compiler/nir/libnir.a \
    src/compiler/libcompiler.a src/freedreno/common/libfreedreno_common.a src/freedreno/isa/libir3decode.a \
    src/compiler/isaspec/libisaspec.a -pthread -lm -lz -ldl -Wl,--end-group
echo "$here/sapphire-ir3as"
