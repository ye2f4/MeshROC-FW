#!/usr/bin/env python3
# 用标准 CRC16-MODBUS 计算 route.txt 当前每个 TestVec 的正确 CRC
# (以 route.txt 现有字节流为准，不修改其字节，只给出应填的正确 CRC)
table=[]
for i in range(256):
    c=i
    for _ in range(8):
        c=(c>>1)^0xA001 if c&1 else c>>1
    table.append(c&0xFFFF)
def crc16(data):
    crc=0xFFFF
    for b in data:
        crc=(crc>>8)^table[(crc^b)&0xFF]
    crc&=0xFFFF
    return ((crc&0xFF)<<8)|(crc>>8)

# route.txt 现有字节流（从文件逐字抄录）
vecs = {
 "TestVec-0": "25 00 12 00 08 00 64 10 38 00 06 21 E8 A1 8C E5 88 B0 E6 B0 B4 E7 A9 B7 E5 A4 84 EF BC 8C E5 9D 90 E7 9C 8B E4 BA 91 E8 B5 B7 E6 97 B6",
 "TestVec-1": "50 00 0A FF FF 00 20 08 2C 01 05 08 52 4F 4F 46 2D 30 41 0A",
 "TestVec-2": "16 00 0B FF FF 00 35 0A 30 01 06 18 E7 BE A4 E7 BB 84 E6 B5 8B E8 AF 95 EF BC 9A E6 B3 A8 E6 84 8F E4 BF A1 E7 94 A8",
 "TestVec-3": "04 00 08 00 12 00 64 04 33 00",
 "TestVec-4": "6B 00 0C FF FF 00 41 0F 2A 01",
 "TestVec-5": "44 00 0D FF FF 00 28 06 34 01 01 02 0E E6 02 02 14 00 03 02 09 DD 04 02 11 A4",
 "TestVec-6": "25 00 08 00 12 00 70 10 32 00 10 06 00 0A 00 0C 00 12 06 05 E6 B5 8B E8 AF 95 41",
}
for k,v in vecs.items():
    b=bytes.fromhex(v.replace(" ",""))
    print("%-10s correct CRC16-MODBUS = 0x%04X   (旧文档值见 route.txt)" % (k, crc16(b)))
