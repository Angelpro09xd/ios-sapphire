#!/usr/bin/env python3
"""
arconv.py LOG OUT.arp [--rx-intf N] [--rx-mask M]: a GSL packet log (the AudioReach harness, ar-harness
out/*_full.log) as a script for arplay, the phone's replayer (/dev/redwood-gpr).

The log is every packet Android's GSL sends for a use case, with the out-of-band payloads it put in
shared memory. Here:
  - the shared memory maps and unmaps are left out: the kernel has one map of its own (192 KB), and
    each out-of-band payload gets an offset in it. Calibration tables (APM_CMD_REGISTER_CFG), which
    the DSP reads for as long as the graph is open, each get their own; DEREGISTER_CFG names the same
    table again (same address in the log), so it gets the same offset. Everything else shares a
    scratch area at the start.
  - the codec DMA interface of the RX graph's endpoint can be changed (--rx-intf, --rx-mask): Android's
    earpiece is RX-0 (index 1), but the codec here is set up for RX-1 (index 2), the speaker path
    iOS's sound plays on.
  - GRAPH_CLOSE's payload is not in the log (GSL had freed its shared memory): the graph's subgraph
    list from its prepare goes in band instead.
  - the script is split in two sections: "start" (to the last GRAPH_START) and "stop" (the rest);
    the start closes iOS's playback graph first (it has the same interface) and the stop opens it again.

With --record (a record use case, record_*.log) the sections are "open" (open, configure, prepare;
then the kernel is told how to start the graph and read it, for /dev/redwood-mic) and "close".

Script: "ARP1", then entries: u32 type, u32 length of what follows, then
  1 WRITE    u32 offset, bytes
  2 CMD      u32 src, dst, opcode, rsp_opcode, flags, oob_offset, oob_len, then the in-band payload
  3 PLAYBACK u32 on
  6 SECTION  name, NUL-terminated
  7 MIC      u32 src, RD_SHARED_MEM_EP port, buffers' offset, then GRAPH_START's in-band payload
"""
import re
import struct
import sys

APM_CMD_GRAPH_PREPARE = 0x01001001
APM_CMD_GRAPH_START = 0x01001002
APM_CMD_GRAPH_CLOSE = 0x01001004
APM_CMD_REGISTER_CFG = 0x01001008
APM_CMD_DEREGISTER_CFG = 0x01001009
SKIP = {0x0100100C, 0x0100100D}         # SHARED_MEM_MAP_REGIONS / UNMAP
PARAM_ID_CODEC_DMA_INTF_CFG = 0x08001063

DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2 = 0x0400100B

SCRATCH = 0x6000
REGION = 0x30000
MIC_BUFFERS = 0x2C000                   # the record graph's 4 buffers (the kernel's mic_service())


def parse(path):
    pkts = []
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"#(\d+) SEND \S+ opcode=0x([0-9a-f]+) src=\d+:0x([0-9a-f]+) dst=\d+:0x([0-9a-f]+)", line)
        if m:
            cur = {"n": int(m.group(1)), "opcode": int(m.group(2), 16), "src": int(m.group(3), 16),
                   "dst": int(m.group(4), 16), "p": [], "o": [], "oob": None}
            pkts.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r"\s+P [0-9a-f]+: (.*)", line)
        if m:
            cur["p"] += [int(w, 16) for w in m.group(1).split()]
            continue
        m = re.match(r"\s+OOB addr=0x([0-9a-f]+) mem_map_handle=0x[0-9a-f]+ size=(\d+)", line)
        if m:
            cur["oob"] = (int(m.group(1), 16), int(m.group(2)))
            continue
        m = re.match(r"\s+O [0-9a-f]+: (.*)", line)
        if m:
            cur["o"] += [int(w, 16) for w in m.group(1).split()]
    return pkts


def patch_intf(words, iid, intf, mask, what):
    n = 0
    for i in range(len(words) - 6):
        if words[i] == iid and words[i + 1] == PARAM_ID_CODEC_DMA_INTF_CFG:
            old = words[i + 4:i + 7]
            words[i + 5] = intf
            words[i + 6] = mask
            print(f"  {what}: endpoint {iid:#x} codec DMA {old} -> {words[i + 4:i + 7]}")
            n += 1
    return n


def main():
    args = sys.argv[1:]
    rx_intf, rx_mask, record = None, 1, False
    while "--rx-intf" in args:
        i = args.index("--rx-intf"); rx_intf = int(args[i + 1], 0); del args[i:i + 2]
    while "--rx-mask" in args:
        i = args.index("--rx-mask"); rx_mask = int(args[i + 1], 0); del args[i:i + 2]
    if "--record" in args:
        args.remove("--record"); record = True
    src, out = args
    pkts = [p for p in parse(src) if p["opcode"] not in SKIP]
    ents = []

    def ent(t, body):
        ents.append(struct.pack("<II", t, len(body)) + body)

    tables = {}
    prepared = {}
    nxt = SCRATCH

    def emit(p):
        nonlocal nxt
        flags = off = olen = 0
        if p["oob"] is not None:
            addr, size = p["oob"]
            data = p["o"][:(size + 3) // 4]
            if rx_intf is not None:
                patch_intf(data, 0x43B3, rx_intf, rx_mask, f"#{p['n']} oob")
            if p["opcode"] in (APM_CMD_REGISTER_CFG, APM_CMD_DEREGISTER_CFG):
                if addr not in tables:
                    tables[addr] = nxt
                    nxt = (nxt + size + 63) & ~63
                off = tables[addr]
            else:
                if size > SCRATCH:
                    sys.exit(f"#{p['n']}: {size} bytes out of band, more than the scratch area")
                off = 0
            raw = struct.pack(f"<{len(data)}I", *data)[:size]
            ent(1, struct.pack("<I", off) + raw)
            flags, olen = 1, size
        words = list(p["p"])
        if p["opcode"] == APM_CMD_GRAPH_PREPARE:
            prepared[p["src"]] = words
        if p["opcode"] == APM_CMD_GRAPH_CLOSE and p["oob"] is None and words[2:3] != [0]:
            # its payload was in shared memory GSL had freed before the log was written: the
            # graph's subgraph list, in band, as its prepare had it (status 1 otherwise)
            words = list(prepared[p["src"]])
        if rx_intf is not None and p["dst"] in (0x1, 0x43B3):
            patch_intf(words, 0x43B3, rx_intf, rx_mask, f"#{p['n']} in band")
        inband = struct.pack(f"<{len(words)}I", *words)
        if len(inband) + 24 > 128 and not flags:
            print(f"  warning: #{p['n']} {p['opcode']:#x}: {len(inband)} bytes in band (the DSP's buffers are 128)")
        ent(2, struct.pack("<7I", p["src"], p["dst"], p["opcode"], 0, flags, off, olen) + inband)

    if record:
        first_start = min(i for i, p in enumerate(pkts) if p["opcode"] == APM_CMD_GRAPH_START)
        reads = [p for p in pkts if p["opcode"] == DATA_CMD_RD_SH_MEM_EP_DATA_BUFFER_V2]
        ent(6, b"open\0")
        for p in pkts[:first_start]:
            emit(p)
        start = pkts[first_start]
        ent(7, struct.pack("<3I", start["src"], reads[0]["dst"], MIC_BUFFERS) +
            struct.pack(f"<{len(start['p'])}I", *start["p"]))
        ent(6, b"close\0")
        for p in pkts:
            if p["opcode"] == APM_CMD_GRAPH_CLOSE:
                emit(p)
    else:
        last_start = max(i for i, p in enumerate(pkts) if p["opcode"] == APM_CMD_GRAPH_START)
        ent(6, b"start\0")
        ent(3, struct.pack("<I", 0))
        for i, p in enumerate(pkts):
            if i == last_start + 1:
                ent(6, b"stop\0")
            emit(p)
        ent(3, struct.pack("<I", 1))
    if nxt > (MIC_BUFFERS if record else REGION):
        sys.exit(f"tables need {nxt:#x} bytes, more than there is")
    with open(out, "wb") as f:
        f.write(b"ARP1" + b"".join(ents))
    print(f"{out}: {len(pkts)} packets, tables {len(tables)} up to {nxt:#x} of {REGION:#x}")


main()
