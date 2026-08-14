#!/usr/bin/env python3
# 严格验证 packtest.txt 每个包的 TLV 解析（按真实字节流长度，不臆测）
# 并定位 TestVec-2 / TestVec-6 的缺口
raw = {
 "TestVec-0":  "25 00 12 00 08 00 64 10 38 00 06 21 E8 A1 8C E5 88 B0 E6 B0 B4 E7 A9 B7 E5 A4 84 EF BC 8C E5 9D 90 E7 9C 8B E4 BA 91 E8 B5 B7 E6 97 B6",
 "TestVec-1":  "50 00 0A FF FF 00 20 08 2C 01 05 08 52 4F 4F 46 2D 30 41 0A",
 "TestVec-2":  "16 00 0B FF FF 00 35 0A 30 01 06 18 E7 BE A4 E7 BB 84 E6 B5 8B E8 AF 95 EF BC 9A E6 B3 A8 E6 84 8F E4 BF A1 E7 94 A8",
 "TestVec-3":  "04 00 08 00 12 00 64 04 33 00",
 "TestVec-4":  "6B 00 0C FF FF 00 41 0F 2A 01",
 "TestVec-5":  "44 00 0D FF FF 00 28 06 34 01 01 02 0E E6 02 02 14 00 03 02 09 DD 04 02 11 A4",
 "TestVec-6":  "25 00 08 00 12 00 70 10 32 00 10 06 00 0A 00 0C 00 12 06 05 E6 B5 8B E8 AF 95 41",
}
TAGS={0x01:"电池",0x02:"太阳能",0x03:"温度",0x04:"湿度",0x05:"节点名",0x06:"文本",
      0x10:"ROUTE_PATH",0x11:"REVERSE_PATH",0x12:"ROUTE_METRIC",0x13:"ROUTE_INVALID"}

for name,h in raw.items():
    body=bytes.fromhex(h.replace(" ",""))
    payload=body[10:]  # 10字节头之后全是 payload
    print("="*60); print(name, "total=%d  payload=%d"%(len(body),len(payload)))
    i=0; ok=True
    while i+2<=len(payload):
        tag=payload[i]; ln=payload[i+1]
        if i+2+ln>len(payload):
            print("  !! TLV 越界 tag=0x%02X len=%d (剩 %d)"%(tag,ln,len(payload)-(i+2))); ok=False; break
        val=payload[i+2:i+2+ln]
        sval=val.decode("utf-8","replace") if tag in (0x05,0x06) else val.hex(' ')
        print("  TLV 0x%02X(%s) len=%d : %s"%(tag,TAGS.get(tag,tag),ln,sval))
        i+=2+ln
    if i!=len(payload):
        print("  !! payload 未消费完: 已解析 %d / %d 字节 (余 %s)"%(i,len(payload),payload[i:].hex(' ')))
        ok=False
    print("  -> TLV 结构", "OK" if ok else "有缺口/越界")
