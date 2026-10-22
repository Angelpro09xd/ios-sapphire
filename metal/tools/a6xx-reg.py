#!/usr/bin/env python3
"""Offset and bitfields of a6xx registers or packet fields, from Mesa's a6xx.xml / adreno_pm4.xml.

    a6xx-reg.py [--xml DIR] NAME [NAME ...]

The A6XX variant is the one printed when a register has several (A6XX, A7XX-, ...). Bitsets
referenced by type= are expanded. Enum values named in a NAME= form (e.g. RM6_COMPUTE) are
looked up in every enum.
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

args = sys.argv[1:]
xdir = '/serverhive/angelpro09devv/infinity/external/mesa3d/src/freedreno/registers/adreno'
if args[:1] == ['--xml']:
    xdir = args[1]
    args = args[2:]

def load(f):
    s = open(os.path.join(xdir, f)).read()
    s = re.sub(r'xmlns="[^"]+"', '', s, count=1)
    return ET.fromstring(s)

roots = [load(f) for f in ('a6xx.xml', 'adreno_pm4.xml', 'a6xx_enums.xml', 'adreno_common.xml')
         if os.path.exists(os.path.join(xdir, f))]
bitsets, enums = {}, {}
for r in roots:
    for b in r.iter('bitset'):
        bitsets[b.get('name')] = b
    for e in r.iter('enum'):
        for v in e.iter('value'):
            enums.setdefault(v.get('name'), []).append((e.get('name'), v.get('value'), v.get('variants')))

def a6(v):
    if v is None:
        return True
    for part in v.split(','):
        part = part.strip()
        m = re.match(r'A(\d)XX(?:-(?:A(\d)XX)?)?$', part)
        if not m:
            continue
        lo = int(m.group(1))
        hi = int(m.group(2)) if m.group(2) else (99 if part.endswith('-') else lo)
        if lo <= 6 <= hi:
            return True
    return False

def fields(node):
    out = []
    t = node.get('type')
    if t in bitsets:
        out += fields(bitsets[t])
    for f in node.findall('bitfield'):
        if not a6(f.get('variants')):
            continue
        lo, hi, pos = f.get('low'), f.get('high'), f.get('pos')
        rng = f'{pos}' if pos else f'{lo}..{hi}'
        extra = ' '.join(f'{k}={f.get(k)}' for k in ('type', 'shr') if f.get(k))
        out.append(f'    {rng:7s} {f.get("name")} {extra}')
    return out

for name in args:
    found = False
    for r in roots:
        for tag in ('reg32', 'reg64'):
            for n in r.iter(tag):
                if n.get('name') == name and a6(n.get('variants')):
                    print(f'{name}: {tag} offset {n.get("offset")} type={n.get("type")} {n.get("low") and ("low=" + n.get("low")) or ""}')
                    print('\n'.join(fields(n)))
                    found = True
                    break
        for d in r.iter('domain'):
            if d.get('name') == name:
                for n in d:
                    if n.tag in ('reg32', 'reg64') and a6(n.get('variants')):
                        print(f'{name}.{n.get("name")}: {n.tag} offset {n.get("offset")}')
                        print('\n'.join(fields(n)))
                found = True
    if name in enums:
        for en, val, var in enums[name]:
            print(f'{name} = {val}   (enum {en}{", " + var if var else ""})')
        found = True
    if not found:
        print(f'{name}: not found')
