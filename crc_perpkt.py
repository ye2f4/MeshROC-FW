
#!/usr/bin/env python3
raw = {
    "hb":   "50 00 0A 00 14 00 5A 08 1E 01 "
            "01 02 3A 00 02 02 4B 00 03 02 1C 00 04 02 2D 00 "
            "05 07 31 38 E5 8F B7 06 04 E4 BD A0 E4 BD 9C 3B D2",
    "grp":  "16 00 14 00 0A 00 3C 08 1E 00 "
            "06 0B E7 94 A8 E4 BD 9C E8 BF 98 E6 98 AF E5 A5 BD E7 9A 84 4D 67",
    "ack":  "6B 00 0E 00 14 00 7B 08 1E 00 "
            "02 01 00 04 01 2A 12 48",
    "probe": "04 00 08 00 14 00 99 08 1E 00 "
            "03 01 01 80 F4",
    "tele": "44 00 0C 00 14 00 2A 08 1E 00 "
            "01 01 05 05 07 31 38 E5 8F B7 C7 8E",
}
pkts = {n: bytes.fromhex(h.replace(" ", "")) for n, h in raw.items()}

def crc_refl(data, poly, init, xorout):
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ poly if (crc & 1) else (crc >> 1)
    return (crc ^ xorout) & 0xFFFF

def crc_norm(data, poly, init, xorout):
    crc = init
    for b in data:
        crc ^= (b << 8); crc &= 0xFFFF
        for _ in range(8):
            crc = (((crc << 1) ^ poly) & 0xFFFF) if (crc & 0x8000) else ((crc << 1) & 0xFFFF)
    return (crc ^ xorout) & 0xFFFF

# A) 旧算法算真实包
print("=== A) datapack-doc CRC(0xD508,0x8005,refl) on real packets ===")
for n, full in pkts.items():
    data, val = full[:-2], (full[-2] << 8) | full[-1]
    c = crc_refl(data, 0xD508, 0x8005, 0x0000)
    print("  %-5s want=0x%04X got=0x%04X %s" % (n, val, c, "OK" if c == val else "MISMATCH"))

# B) 单包暴力搜索，统计每包命中参数（取前若干 + 交集）
print("\n=== B) per-packet brute force (poly,init,xorout,refl), find matched ===")
common = None
for n, full in pkts.items():
    data, val = full[:-2], (full[-2] << 8) | full[-1]
    s = set()
    for poly in range(1, 0x10000):
        for init in (0x0000, 0xFFFF):
            for xorout in (0x0000, 0xFFFF):
                for refl in (False, True):
                    c = crc_refl(data, poly, init, xorout) if refl else crc_norm(data, poly, init, xorout)
                    if c == val:
                        s.add((poly, init, xorout, refl))
    print("  %-5s hits=%d  sample=%s" % (n, len(s), list(sorted(s))[:5]))
    common = s if common is None else (common & s)
print("  COMMON across all 5:", common)
