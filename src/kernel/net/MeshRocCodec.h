#pragma once
#include <cstdint>
#include <cstddef>
#include "kernel/net/MeshRocPacket.h"

/**
 * MeshRocCodec：包头与 TLV 的纯算法编解码（§14.1 零依赖平移）
 * 不引用任何原版 meshtastic 类型，可独立单测。
 */
namespace meshroc::net {

class MeshRocCodec {
public:
    // CRC8（包头校验，poly 0x07，初值 0x00）
    static uint8_t crc8(const uint8_t* data, size_t len);

    // 计算并填入包头 CRC
    static void finalizeHeader(MeshRocPacket& pkt);

    // 校验包头 CRC
    static bool verifyHeader(const MeshRocPacket& pkt);

    // 构造 ctrl_flag 字节
    static uint8_t makeCtrlFlag(MeshRocPacketType type,
                                MeshRocPriority prio,
                                bool wantAck,
                                bool relayPerm,
                                bool compressed);

    // ---- TLV 工具（§13.3 / §13.2） ----
    // 往 buffer 追加一个 TLV；返回写入字节数，空间不足返回 0
    static size_t appendTlv(uint8_t* buf, size_t cap, size_t offset,
                            uint8_t tag, const uint8_t* val, uint8_t vlen);

    // 追加 uint16BE 便捷封装
    static size_t appendTlvU16(uint8_t* buf, size_t cap, size_t offset,
                               uint8_t tag, uint16_t v);

    // 在 buffer 中查找指定 tag 的 TLV，返回 value 指针与长度（未找到返回 nullptr）
    static const uint8_t* findTlv(const uint8_t* buf, size_t len,
                                  uint8_t tag, uint8_t& outVlen);

    // CRC16（payload 校验，poly 0x1021，初值 0xFFFF）
    static uint16_t crc16(const uint8_t* data, size_t len);
};

}  // namespace meshroc::net
