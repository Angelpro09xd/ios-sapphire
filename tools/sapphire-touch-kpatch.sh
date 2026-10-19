#!/bin/bash
# Binary-patch the Focaltech touch scaling of a CI kernel for sapphire,
# when no CI build is possible:
#
#   tools/sapphire-touch-kpatch.sh KERNEL OUT
#
# Measured on sapphire (FT3519T, raw dumps 2026-10-19): X raw 0..2159
# (2 per pixel), Y raw 0..2399 (1 per pixel). The topaz parser drops
# X > 1349 / Y > 2999 and divides both by 1.25. touch_focaltech is inlined
# into touch_thread and unrolled once per record; in each copy this sets the
# limits to 2159/2399, the shared 1/5 multiplier to 1 and the shifts to
# >>3 (x = (raw*4+frac)/8) and >>2 (y = (raw*4+frac)/4).
set -euo pipefail
in=$1 out=$2
here=$(dirname "$(realpath "$0")")
start=$(llvm-nm "$in" | awk '$3=="_touch_thread"{print $1}')
asm=$(mktemp)
trap 'rm -f "$asm"' EXIT
llvm-objdump -d --no-show-raw-insn --start-address=0x$start --stop-address=$(printf '0x%x' $((0x$start + 0x2000))) "$in" > "$asm"
args=$(python3 - "$asm" <<'PY'
import re, sys
P = [(int(l.split(':')[0], 16), l.split(':', 1)[1].strip())
     for l in open(sys.argv[1]) if re.match(r'\s*[0-9a-f]+:', l)]
cmp_imm = lambda rn, imm: 0x7100001f | imm << 10 | rn << 5
movz = lambda rd, imm: 0x52800000 | imm << 5 | rd
lsr = lambda rd, rn, sh: 0x53000000 | sh << 16 | 31 << 10 | rn << 5 | rd
args, blocks = [], 0
for i, (va, ins) in enumerate(P):
    m = re.fullmatch(r'cmp\s+w(\d+), #0x545', ins)
    if not m:
        continue
    win = P[i:i + 22]
    def first(rx):
        for j, (v, s) in enumerate(win):
            mm = re.fullmatch(rx, s)
            if mm:
                return j, v, mm
    a = first(r'cmp\s+w(\d+), #0xbb7')
    b = first(r'bfi\s+w(\d+), w\d+, #2, #12')
    c = first(r'bfxil\s+w(\d+), w\d+, #4, #2')
    d = first(r'mov\s+w(\d+), #0xcccd\s*;.*')
    assert all((a, b, c, d)), hex(va)
    fx, fy, rc = int(b[2].group(1)), int(c[2].group(1)), int(d[2].group(1))
    muls, outs = {}, []
    for v, s in win[d[0] + 1:d[0] + 8]:
        m2 = re.fullmatch(r'mul\s+w(\d+), w(\d+), w(\d+)', s)
        if m2 and int(m2.group(3)) == rc and int(m2.group(2)) in (fx, fy):
            muls[int(m2.group(1))] = int(m2.group(2))
        m3 = re.fullmatch(r'lsr\s+w(\d+), w(\d+), #18', s)
        if m3 and int(m3.group(2)) in muls:
            outs.append((v, int(m3.group(1)), int(m3.group(2))))
    assert len(muls) == 2 and len(outs) == 2, hex(va)
    args += [f'{va:x}={cmp_imm(int(m.group(1)), 2159):08x}',
             f'{a[1]:x}={cmp_imm(int(a[2].group(1)), 2399):08x}',
             f'{d[1]:x}={movz(rc, 1):08x}']
    args += [f'{v:x}={lsr(rd, rn, 3 if muls[rn] == fx else 2):08x}' for v, rd, rn in outs]
    blocks += 1
assert blocks == 10, f'{blocks} parser copies (expected 10): different kernel?'
print(' '.join(args))
PY
)
python3 "$here/kpatch.py" "$in" "$out" $args | wc -l | xargs echo "patched instructions:"
