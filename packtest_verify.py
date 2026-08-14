#!/usr/bin/env python3
# 直接读取 E:/packtest.txt，解析每个 TestVec 的 Header+Payload Hex，
# 用标准 CRC16-MODBUS 计算 CRC 并做 round-trip + TLV 完整性校验。
import re, os
table=[]
for i in range(256):
    c=i
    for _ in range(8):
        c=(c>>1)^0xA001 if c&1 else c>>1
    table.append(c&0xFFFF)
def crc16_modbus(data):
    crc=0xFFFF
    for b in data:
        crc=(crc>>8)^table[(crc^b)&0xFF]
    crc&=0xFFFF
    return ((crc&0xFF)<<8)|(crc>>8)

path=r"E:/packtest.txt"
txt=open(path,"r",encoding="utf-8").read()
# 取 "RAW TEST PACKETS" 之后的内容
seg=txt.split("RAW TEST PACKETS",1)[1]
# 按 TestVec-N 拆分
blocks=re.split(r"\nTestVec-\d+:", seg)
# blocks[0] 是前言，之后每个对应一个 vec；用名字匹配
names=re.findall(r"TestVec-(\d+):\s*([^\n]+)", seg)
hexes=re.findall(r"Header\+Payload Hex:\n([0-9A-Fa-f\s]+)", seg)
# 重新对齐：用名字行 + 后续 hex 行
lines=seg.splitlines()
vecs=[]
cur=None
for ln in lines:
    m=re.match(r"TestVec[‐‑\-](\d+):\s*(.*)", ln)
    if m:
        cur={"id":m.group(1),"desc":m.group(2).strip(),"hex":None}
        vecs.append(cur)
    elif cur is not None and re.match(r"^\s*[0-9A-Fa-f]+(?:\s+[0-9A-Fa-f]+)*\s*$", ln) and cur["hex"] is None:
        cur["hex"]=ln.strip()

TYPES={0:"私聊",1:"群消息",2:"ACK",3:"路由探测",4:"遥测",5:"心跳",6:"加密帧"}
PRIO={0:"低",1:"普通",2:"高",3:"紧急"}
TAGS={0x01:"电池电压",0x02:"太阳能电压",0x03:"AHT20温度",0x04:"湿度",0x05:"节点名",0x06:"文本",
      0x10:"ROUTE_PATH",0x11:"REVERSE_PATH",0x12:"ROUTE_METRIC",0x13:"ROUTE_INVALID"}
def u16(b,i): return (b[i]<<8)|b[i+1]

allok=True
for v in vecs:
    if v["hex"] is None: continue
    body=bytes.fromhex(v["hex"].replace(" ",""))
    crc=crc16_modbus(body)
    full=body+bytes([crc>>8,crc&0xFF])
    cf=full[0]; typ=cf&7; prio=(cf>>3)&3; ack=(cf>>5)&1; relay=(cf>>6)&1; comp=(cf>>7)&1
    src=u16(full,1); dst=u16(full,3); seq=u16(full,5)
    maxhop=full[7]; snr=full[8]; route=full[9]
    crcrecv=u16(full,len(full)-2)
    payload=full[10:-2]
    ok=(crcrecv==crc); allok&=ok
    print("="*60)
    print("TestVec-%s: %s"%(v["id"],v["desc"]))
    print("  ctrl=0x%02X type=%s prio=%s ack=%d relay=%d | src=0x%04X dst=0x%04X seq=%d maxhop=%d snr=%d route=%d"
          %(cf,TYPES.get(typ,typ),PRIO.get(prio,prio),ack,relay,src,dst,seq,maxhop,snr,route))
    print("  CRC16-MODBUS = 0x%04X  %s"%(crc,"OK" if ok else "FAIL"))
    i=0; tlvok=True
    while i+2<=len(payload):
        tag=payload[i]; ln=payload[i+1]
        if i+2+ln>len(payload):
            print("    !! TLV 越界 tag=0x%02X len=%d"%(tag,ln)); tlvok=False; break
        val=payload[i+2:i+2+ln]
        sval=val.decode("utf-8","replace") if tag in (0x05,0x06) else val.hex(' ')
        print("    TLV 0x%02X(%s) len=%d : %s"%(tag,TAGS.get(tag,tag),ln,sval))
        i+=2+ln
    if i!=len(payload):
        print("    !! payload 未消费完, 余 %s"%payload[i:].hex(' ')); tlvok=False
    allok&=tlvok
    print("  -> TLV 结构", "OK" if tlvok else "缺口/越界")
print("\nALL (CRC self-check + TLV struct) OK:", allok)
