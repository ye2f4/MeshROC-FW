#!/usr/bin/env python3
def crc16_modbus(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8005 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF

# 标准 Modbus 官方例子
print("sanity 01 03 00 00 00 01 ->", hex(crc16_modbus(bytes([0x01,0x03,0x00,0x00,0x00,0x01])), ), "expect 0x840a")

# 暴力：对 TestVec-0 数据，尝试各种 (poly, init, xorout, ref) 找匹配 0xAE71
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

full = bytes.fromhex("25 00 12 00 08 00 64 10 38 00 06 21 E8 A1 8C E5 88 B0 E6 B0 B4 E7 A9 B7 E5 A4 84 EF BC 8C E5 9D 90 E7 9C 8B E4 BA 91 E8 B5 B7 E6 97 B6 AE 71".replace(" ",""))
data, want = full[:-2], (full[-2]<<8)|full[-1]
print("TestVec-0 want", hex(want))
hits=[]
for poly in range(1,0x10000):
    for init in (0,0xFFFF):
        for xorout in (0,0xFFFF):
            for ref in (False,True):
                c = crc_refl(data,poly,init,xorout) if ref else crc_norm(data,poly,init,xorout)
                if c==want: hits.append((poly,init,xorout,ref))
print("hits for TestVec-0:", hits[:20], "total", len(hits))
# 如果找到，用同样参数测其它向量
if hits:
    for (poly,init,xorout,ref) in hits[:5]:
        okall=True
        for name,h in {
            "v1":"50 00 0A FF FF 00 20 08 2C 01 05 08 52 4F 4F 46 2D 30 41 0A 6F A3",
            "v2":"16 00 0B FF FF 00 35 0A 30 01 06 18 E7 BE A4 E7 BB 84 E6 B5 8B E8 AF 95 EF BC 9A E6 B3 A8 E6 84 8F E4 BF A1 E7 94 A8 07 4C",
            "v3":"04 00 08 00 12 00 64 04 33 00 28 D9",
            "v4":"6B 00 0C FF FF 00 41 0F 2A 01 DA 05",
            "v5":"44 00 0D FF FF 00 28 06 34 01 01 02 0E E6 02 02 14 00 03 02 09 DD 04 02 11 A4 C4 1B",
            "v6":"25 00 08 00 12 00 70 10 32 00 10 06 00 0A 00 0C 00 12 06 05 E6 B5 8B E8 AF 95 41 EB 24",
        }.items():
            f=bytes.fromhex(h.replace(" ","")); d=f[:-2]; w=(f[-2]<<8)|f[-1]
            c = crc_refl(d,poly,init,xorout) if ref else crc_norm(d,poly,init,xorout)
            if c!=w: okall=False
        print("  params",poly,init,xorout,ref,"all-others:",okall)
