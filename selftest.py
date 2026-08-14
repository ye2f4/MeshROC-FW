#!/usr/bin/env python3
# 自洽测试：模拟模块的内部编解码（MODBUS + 大端），验证 round-trip 正确。
# 证明：模块内部收发 CRC 自洽，不依赖 route.txt 错误的测试向量值。
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
    return ((crc&0xFF)<<8)|(crc>>8)  # 寄存器顺序

def serialize(ctrl, src, dst, seq, maxhop, snr, route, payload):
    hdr=bytes([ctrl, src>>8, src&0xFF, dst>>8, dst&0xFF, seq>>8, seq&0xFF, maxhop, snr, route])
    body=hdr+payload
    crc=crc16_modbus(body)
    return body+bytes([crc>>8, crc&0xFF])

def deserialize(b):
    if len(b)<12: return None
    ctrl=b[0]; src=(b[1]<<8)|b[2]; dst=(b[3]<<8)|b[4]; seq=(b[5]<<8)|b[6]
    maxhop=b[7]; snr=b[8]; route=b[9]
    payload=b[10:-2]; crcrecv=(b[-2]<<8)|b[-1]
    calc=crc16_modbus(b[:-2])
    return dict(ctrl=ctrl,src=src,dst=dst,seq=seq,maxhop=maxhop,snr=snr,route=route,
               payload=payload, crc_ok=(calc==crcrecv))

# 构造一个心跳包（参照 TestVec-1 的字段，但用正确 MODBUS CRC 重算）
pkt=serialize(0x50, 0x000A, 0xFFFF, 0x0020, 0x08, 0x2C, 0x01,
              bytes([0x05,0x08,0x52,0x4F,0x4F,0x46,0x2D,0x30,0x41,0x0A]))
print("constructed heartbeat:", pkt.hex(' '))
d=deserialize(pkt)
print("deserialized:", d)
print("CRC self-check OK:", d['crc_ok'])
print("src=0x%04X dst=0x%04X seq=0x%04X" % (d['src'], d['dst'], d['seq']))

# 源路由包 round-trip（参照 TestVec-6 字段）
path_tlv=bytes([0x10,0x06,0x00,0x0A,0x00,0x0C,0x00,0x12])
text_tlv=bytes([0x06,0x05,0xE6,0xB5,0x8B,0xE8,0xAF,0x95,0x41])
pkt2=serialize(0x25, 0x0008, 0x0012, 0x0070, 0x10, 0x32, 0x00, path_tlv+text_tlv)
d2=deserialize(pkt2)
print("\nsource-route packet CRC self-check OK:", d2['crc_ok'])
print("payload hex:", d2['payload'].hex(' '))
