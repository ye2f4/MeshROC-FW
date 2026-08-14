#!/usr/bin/env python3
# 同一批字节流，按"大端"解析，验证是否与文档语义一致。
raw = {
    "心跳 HEARTBEAT":  "50 00 0A 00 14 00 5A 08 1E 01 "
                       "01 02 3A 00 02 02 4B 00 03 02 1C 00 04 02 2D 00 "
                       "05 07 31 38 E5 8F B7 06 04 E4 BD A0 E4 BD 9C 3B D2",
    "群消息 GROUP_MSG": "16 00 14 00 0A 00 3C 08 1E 00 "
                       "06 0B E7 94 A8 E4 BD 9C E8 BF 98 E6 98 AF E5 A5 BD E7 9A 84 4D 67",
    "ACK":             "6B 00 0E 00 14 00 7B 08 1E 00 "
                       "02 01 00 04 01 2A 12 48",
    "路由探测 ROUTE_PROBE": "04 00 08 00 14 00 99 08 1E 00 "
                       "03 01 01 80 F4",
    "遥测 TELEMETRY":  "44 00 0C 00 14 00 2A 08 1E 00 "
                       "01 01 05 05 07 31 38 E5 8F B7 C7 8E",
}
TYPES = {0:"私聊",1:"群消息",2:"ACK",3:"路由探测",4:"遥测",5:"心跳",6:"加密帧"}
PRIO  = {0:"低",1:"普通",2:"高",3:"紧急"}
TAGS  = {0x01:"电池电压",0x02:"太阳能电压",0x03:"AHT20温度",0x04:"湿度",
         0x05:"节点名称",0x06:"UTF-8文本"}
def u16be(b, i):
    return (b[i] << 8) | b[i+1]
for name, h in raw.items():
    b = bytes.fromhex(h.replace(" ", ""))
    cf = b[0]
    typ = cf & 0x07; prio = (cf>>3)&0x03; ack=(cf>>5)&1; relay=(cf>>6)&1; comp=(cf>>7)&1
    src = u16be(b,1); dst = u16be(b,3); seq = u16be(b,5)
    maxhop=b[7]; snr=b[8]; route=b[9]
    crcv = u16be(b, len(b)-2)
    payload = b[10:len(b)-2]
    print("="*60); print(name)
    print("  ctrl_flag=0x%02X type=%s prio=%s ack=%d relay=%d comp=%d" %
          (cf, TYPES.get(typ,typ), PRIO.get(prio,prio), ack, relay, comp))
    print("  src=0x%04X dst=0x%04X seq=%d maxhop=%d snr=%d route=%d" %
          (src, dst, seq, maxhop, snr, route))
    print("  CRC(大端读)=0x%04X payload=%d字节" % (crcv, len(payload)))
    i=0
    while i+2 <= len(payload):
        tag=payload[i]; ln=payload[i+1]; val=payload[i+2:i+2+ln]
        sval = val.decode("utf-8","replace") if tag in (0x05,0x06) else val.hex()
        print("    TLV tag=0x%02X(%s) len=%d val=%s" % (tag, TAGS.get(tag,tag), ln, sval))
        i += 2+ln
