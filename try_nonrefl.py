#!/usr/bin/env python3
# 尝试非反射 MODBUS: poly=0x8005, init=0xFFFF, xorout=0, ref_in/out=FALSE
def crc_norm(data, poly, init, xorout):
    crc = init
    for b in data:
        crc ^= (b << 8); crc &= 0xFFFF
        for _ in range(8):
            crc = (((crc << 1) ^ poly) & 0xFFFF) if (crc & 0x8000) else ((crc << 1) & 0xFFFF)
    return (crc ^ xorout) & 0xFFFF

vectors = {
    "TestVec-0 文本消息":  "25 00 12 00 08 00 64 10 38 00 06 21 E8 A1 8C E5 88 B0 E6 B0 B4 E7 A9 B7 E5 A4 84 EF BC 8C E5 9D 90 E7 9C 8B E4 BA 91 E8 B5 B7 E6 97 B6 AE 71",
    "TestVec-1 心跳":      "50 00 0A FF FF 00 20 08 2C 01 05 08 52 4F 4F 46 2D 30 41 0A 6F A3",
    "TestVec-2 群消息":    "16 00 0B FF FF 00 35 0A 30 01 06 18 E7 BE A4 E7 BB 84 E6 B5 8B E8 AF 95 EF BC 9A E6 B3 A8 E6 84 8F E4 BF A1 E7 94 A8 07 4C",
    "TestVec-3 ACK(空载荷)":"04 00 08 00 12 00 64 04 33 00 28 D9",
    "TestVec-4 探测(空载荷)":"6B 00 0C FF FF 00 41 0F 2A 01 DA 05",
    "TestVec-5 遥测":      "44 00 0D FF FF 00 28 06 34 01 01 02 0E E6 02 02 14 00 03 02 09 DD 04 02 11 A4 C4 1B",
    "TestVec-6 源路由":    "25 00 08 00 12 00 70 10 32 00 10 06 00 0A 00 0C 00 12 06 05 E6 B5 8B E8 AF 95 41 EB 24",
}
allok=True
for name,h in vectors.items():
    f=bytes.fromhex(h.replace(" ","")); d=f[:-2]; w=(f[-2]<<8)|f[-1]
    g=crc_norm(d,0x8005,0xFFFF,0)
    allok &= (g==w)
    print("%-22s want=0x%04X nonrefl=0x%04X %s"%(name,w,g,"OK" if g==w else "FAIL"))
print("ALL NONREFL OK:",allok)
