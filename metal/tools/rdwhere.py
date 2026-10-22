#!/usr/bin/env python3
"""Which packet of a cffdump listing holds the dword at a GPU address?

  rdwhere.py LISTING.txt ADDR [ADDR...]     (cffdump --no-color output)

Prints, for each address, the description lines cffdump printed for the packet whose hex
dump contains it, and the dword's index inside that packet.
"""
import re, sys

lines = open(sys.argv[1], errors='replace').read().split('\n')
hexre = re.compile(r'^\s*([0-9a-f]{16}):\s+([0-9a-f]{4}):((?: [0-9a-f]{8})+)')
spans = []   # (start_addr, n_dwords, line_index_of_first_hex_line)
i = 0
while i < len(lines):
    m = hexre.match(lines[i])
    if m and m.group(2) == '0000':
        base = int(m.group(1), 16); n = 0; j = i
        while j < len(lines):
            mm = hexre.match(lines[j])
            if not mm or int(mm.group(1), 16) != base:
                if lines[j].strip() == '*':     # repeated lines: cffdump elides them
                    j += 1; continue
                break
            n = int(mm.group(2), 16) // 4 + len(mm.group(3).split())
            j += 1
        spans.append((base, n, i))
        i = j
    else:
        i += 1
for a in sys.argv[2:]:
    addr = int(a, 16)
    hit = [s for s in spans if s[0] <= addr < s[0] + 4 * s[1]]
    if not hit:
        print('%x: not in any dumped packet' % addr); continue
    base, n, li = hit[-1]
    k = li - 1; desc = []
    while k >= 0 and not hexre.match(lines[k]) and len(desc) < 40:
        desc.append(lines[k]); k -= 1
    print('%x: dword %d of the packet at %x (%d dwords)' % (addr, (addr - base) // 4, base, n))
    for d in reversed(desc[:40]):
        if d.strip():
            print('    ' + d.strip()[:150])
