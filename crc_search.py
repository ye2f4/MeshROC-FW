#!/usr/bin/env python3
# 暴力搜索统一 CRC16 参数，匹配用户补充的 5 个真实数据包
# 字段均为小端；CRC 值为用户给定的权威值（比较 CRC 数值即可，与字节序无关）
# CRC 输入 = 10字节包头 + 全部TLV载荷字节（末2字节CRC不参与计算）

# 用户补充文档 5 个包的完整字节流（含末尾 CRC 两字节），按文档字段精确还原
raw = {
    # 1. 心跳 HEARTBEAT  CRC=0x3BD2
    "hb":  "50 00 0A 00 14 00 5A 08 1E 01 "
           "01 02 3A 00 "          # bat volt 0x003A
           "02 02 4B 00 "          # solar   0x004B
           "03 02 1C 00 "          # temp    0x001C
           "04 02 2D 00 "          # hum     0x002D
           "05 07 31 38 E5 8F B7 " # node name "18楼"
           "06 04 E4 BD A0 E4 BD 9C "  # text "作者"
           "3B D2",
    # 2. 群消息 GROUP_MSG  CRC=0x4D67
    "grp": "16 00 14 00 0A 00 3C 08 1E 00 "
           "06 0B E7 94 A8 E4 BD 9C E8 BF 98 E6 98 AF E5 A5 BD E7 9A 84 "  # text "信用作还是好的"
           "4D 67",
    # 3. ACK  CRC=0x1248
    "ack": "6B 00 0E 00 14 00 7B 08 1E 00 "
           "02 01 00 "             # relay? len1 val0
           "04 01 2A "             # hum len1 val0x2A
           "12 48",
    # 4. 路由探测 ROUTE_PROBE  CRC=0x80F4
    "probe":"04 00 08 00 14 00 99 08 1E 00 "
           "03 01 01 "             # route tag len1 val1
           "80 F4",
    # 5. 遥测 TELEMETRY  CRC=0xC78E
    "tele":"44 00 0C 00 14 00 2A 08 1E 00 "
           "01 01 05 "             # bat len1 val5
           "05 07 31 38 E5 8F B7 " # node name "18楼"
           "C7 8E",
}

packets = []
for name, h in raw.items():
    full = bytes.fromhex(h.replace(" ", ""))
    data = full[:-2]          # header + TLV
    val  = (full[-2] << 8) | full[-1]   # CRC 权威值（大端存储）
    packets.append((name, data, val))
    print(name, "datalen=%d crc=0x%04X" % (len(data), val))

def crc_reflected(data, poly, init, xorout):
    crc = init
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ poly if (crc & 1) else (crc >> 1)
    return (crc ^ xorout) & 0xFFFF

def crc_normal(data, poly, init, xorout):
    crc = init
    for b in data:
        crc ^= (b << 8); crc &= 0xFFFF
        for _ in range(8):
            crc = (((crc << 1) ^ poly) & 0xFFFF) if (crc & 0x8000) else ((crc << 1) & 0xFFFF)
    return (crc ^ xorout) & 0xFFFF

hits = []
for poly in range(1, 0x10000):
    for init in (0x0000, 0xFFFF):
        for xorout in (0x0000, 0xFFFF):
            for refl in (False, True):
                ok = True
                for name, data, val in packets:
                    c = crc_reflected(data, poly, init, xorout) if refl else crc_normal(data, poly, init, xorout)
                    if c != val:
                        ok = False; break
                if ok:
                    hits.append((poly, init, xorout, refl))
                    print("HIT poly=0x%04X init=0x%04X xorout=0x%04X reflected=%s" % (poly, init, xorout, refl))

print("TOTAL HITS:", len(hits))
