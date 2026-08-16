#include "net/MeshRocCodec.h"

namespace meshroc::net {

uint8_t MeshRocCodec::crc8(const uint8_t* data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x80)
                crc = static_cast<uint8_t>((crc << 1) ^ 0x07);
            else
                crc = static_cast<uint8_t>(crc << 1);
        }
    }
    return crc;
}

void MeshRocCodec::finalizeHeader(MeshRocPacket& pkt)
{
    // CRC 覆盖前 9 字节（不含 crc 自身），与已部署格式一致
    pkt.crc = crc8(reinterpret_cast<const uint8_t*>(&pkt), MeshRocPacket::HEADER_LEN - 1);
}

bool MeshRocCodec::verifyHeader(const MeshRocPacket& pkt)
{
    uint8_t expect = crc8(reinterpret_cast<const uint8_t*>(&pkt), MeshRocPacket::HEADER_LEN - 1);
    return expect == pkt.crc;
}

uint8_t MeshRocCodec::makeCtrlFlag(MeshRocPacketType type,
                                   MeshRocPriority prio,
                                   bool wantAck,
                                   bool relayPerm,
                                   bool compressed)
{
    uint8_t f = 0;
    f |= (static_cast<uint8_t>(type) & 0x07);
    f |= (static_cast<uint8_t>(prio) & 0x03) << 3;
    f |= (wantAck   ? 1u : 0u) << 5;
    f |= (relayPerm ? 1u : 0u) << 6;
    f |= (compressed ? 1u : 0u) << 7;
    return f;
}

size_t MeshRocCodec::appendTlv(uint8_t* buf, size_t cap, size_t offset,
                               uint8_t tag, const uint8_t* val, uint8_t vlen)
{
    // TLV: 1B tag + 1B len + vlen
    if (offset + 2 + vlen > cap) return 0;
    buf[offset] = tag;
    buf[offset + 1] = vlen;
    for (uint8_t i = 0; i < vlen; ++i) buf[offset + 2 + i] = val[i];
    return 2 + vlen;
}

size_t MeshRocCodec::appendTlvU16(uint8_t* buf, size_t cap, size_t offset,
                                  uint8_t tag, uint16_t v)
{
    uint8_t tmp[2] = { static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xFF) };
    return appendTlv(buf, cap, offset, tag, tmp, 2);
}

const uint8_t* MeshRocCodec::findTlv(const uint8_t* buf, size_t len,
                                     uint8_t tag, uint8_t& outVlen)
{
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t t = buf[i];
        uint8_t l = buf[i + 1];
        const uint8_t* v = &buf[i + 2];
        if (t == tag) {
            outVlen = l;
            return v;
        }
        i += 2 + l;
    }
    outVlen = 0;
    return nullptr;
}

uint16_t MeshRocCodec::crc16(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000)
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            else
                crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

}  // namespace meshroc::net
