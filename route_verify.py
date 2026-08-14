#!/usr/bin/env python3
# 直接读取 route.txt，解析每个 TestVec 的 Header+Payload 与文档 CRC 值，
# 用标准 CRC16-MODBUS 重算并比对，确认文档现已自洽。
import re
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

txt=open(r"E:/route.txt",encoding="utf-8").read()
# 抓每个 TestVec 块
blocks=re.split(r"TestVec‑\d+:", txt)
# 用名字匹配
names=re.findall(r"TestVec‑(\d+):", txt)
# 对每个 vec，提取 Header: 行、Payload: 行（或 "Payload length=0"）、CRC 行
# 逐块解析：每个 TestVec 块内提取 Header / Payload / 空载荷 / CRC
blocks=re.split(r"(TestVec‑\d+:)", txt)
allok=True
i=1
while i < len(blocks):
    title=blocks[i]; body=blocks[i+1] if i+1<len(blocks) else ""
    i+=2
    vid=re.search(r"TestVec‑(\d+):", title).group(1)
    # 字节：拼接所有 Header: 和 Payload: 行
    hexparts=[]
    for m in re.finditer(r"(?:Header|Payload):\s*([0-9A-Fa-fx, ]+)", body):
        hexparts.append(m.group(1))
    empty=("Payload length=0" in body)
    if not hexparts and not empty:
        print("TestVec-%s: 无法解析字节" % vid); allok=False; continue
    if empty:
        b=bytes.fromhex(re.sub(r"0x|,|\s","",hexparts[0])) if hexparts else b""
        # 空载荷：字节只来自 Header（Payload length=0，无 payload 字节）
        # header 已在 hexparts[0]
    if hexparts:
        b=bytes.fromhex(re.sub(r"0x|,|\s","", " ".join(hexparts)))
    else:
        print("TestVec-%s: 空载荷但无 header" % vid); allok=False; continue
    mc=re.search(r"CRC16[^\n]*?0x([0-9A-Fa-f]{4})", body)
    if not mc:
        print("TestVec-%s: 无 CRC" % vid); allok=False; continue
    calc=crc16(b); doc=int(mc.group(1),16)
    ok=(calc==doc); allok&=ok
    print("TestVec-%s byteLen=%d calcCRC=0x%04X docCRC=0x%04X %s" % (vid,len(b),calc,doc,"OK" if ok else "FAIL"))
print("\nroute.txt 全部向量 CRC 自洽:", allok)
