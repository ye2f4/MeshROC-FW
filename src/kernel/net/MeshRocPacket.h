#pragma once
#include <cstdint>
#include <cstddef>
#include "config/MeshROCConfig.h"

/**
 * MeshRocPacket 协议头（§13.1 / §14.1）
 * 自研 10 字节固定包头，直接驱动射频，不寄生原版 meshtastic_MeshPacket。
 * 字段顺序不可改（与已部署固件空中格式兼容）。
 */
namespace meshroc::net {

// ---- 包类型（ctrl_flag bit0-2） ----
enum class MeshRocPacketType : uint8_t {
    PRIVATE   = 0,  // 私聊
    GROUP     = 1,  // 群播
    ACK       = 2,  // 确认
    PROBE     = 3,  // 路由探测
    TELEMETRY = 4,  // 遥测
    HEARTBEAT = 5,  // 心跳
    ENCRYPTED = 6,  // 加密帧
    RAP       = 7,  // RAP 控制族（归属协议）
};

// ---- 优先级（ctrl_flag bit3-4） ----
enum class MeshRocPriority : uint8_t {
    LOWEST  = 0,
    LOW     = 1,
    HIGH    = 2,
    CRITICAL= 3,    // 求救/告警
};

// ---- ctrl_flag 字节结构（§13.1） ----
// bit0-2: type | bit3-4: priority | bit5: wantAck | bit6: relayPerm | bit7: compressed
struct CtrlFlag {
    uint8_t type      : 3;
    uint8_t priority  : 2;
    uint8_t wantAck   : 1;
    uint8_t relayPerm : 1;
    uint8_t compressed: 1;
};
static_assert(sizeof(CtrlFlag) == 1, "CtrlFlag must be 1 byte");

// ---- 10 字节固定包头 ----
struct MeshRocPacket {
    uint8_t   ctrl_flag;     // 见 CtrlFlag 位域
    uint16_t  src;           // 源短地址
    uint16_t  dst;           // 目的短地址
    uint16_t  seq;           // 包序列号
    uint8_t   max_hop;       // 双层跳数硬下限（datapack 主导）
    int8_t    snr;           // 接收 SNR（回填，§13.5）
    uint8_t   route_mode;    // 0=洪泛 1=源路由 2=RAP 定向
    uint8_t   crc;           // 包头 CRC8

    // 包头后接 TLV 段（§13.3 业务 TLV 0x01-0x13 / §13.2 RAP TLV 0x20-0x28）

    constexpr static size_t HEADER_LEN = 10;

    CtrlFlag ctrl() const { return reinterpret_cast<const CtrlFlag&>(ctrl_flag); }

    MeshRocPacketType  type()     const { return static_cast<MeshRocPacketType>(ctrl() & 0x07); }
    MeshRocPriority    priority() const { return static_cast<MeshRocPriority>((ctrl_flag >> 3) & 0x03); }
    bool               wantAck()  const { return (ctrl_flag >> 5) & 0x01; }
    bool               relayPerm()const { return (ctrl_flag >> 6) & 0x01; }
    bool               compressed()const{ return (ctrl_flag >> 7) & 0x01; }
};
static_assert(sizeof(MeshRocPacket) == MeshRocPacket::HEADER_LEN, "header must be 10 bytes");

// ---- 业务 TLV tag（§13.3，0x01-0x13） ----
enum class MeshRocTlv : uint8_t {
    BATT_VOLT     = 0x01,  // uint16BE ×100
    SOLAR_VOLT    = 0x02,  // uint16BE ×100
    AHT20_TEMP    = 0x03,  // int16BE ×10
    HUMIDITY      = 0x04,  // uint16BE ×10
    NODE_NAME     = 0x05,  // 字符串
    TEXT_MSG      = 0x06,  // UTF-8
    ROUTE_PATH    = 0x10,  // 源路由跳点 uint16BE[]
    REVERSE_PATH = 0x11,  // ACK 反向路径
    ROUTE_METRIC  = 0x12,  // avg_snr uint16BE + 成功计数 uint16BE
    ROUTE_INVALID = 0x13,  // 目的路由失效标记
    // ---- O5 大包分片重组（§15.2-O5 / §15.3 rf.fragEnabled） ----
    FRAG_HEADER   = 0x14,  // 分片头：fragId(u16BE) + fragTotal(u8) + fragSeq(u8)，后接本片载荷
    FRAG_NACK     = 0x15,  // 选择性重传请求：fragId(u16BE) + 缺失 fragSeq 列表(u8[])
};

// ---- 分片头（FRAG_HEADER 的 value 布局，非对齐紧凑） ----
struct FragHeader {
    uint16_t fragId;     // 同一大包的分片共享 ID（取原包 seq）
    uint8_t  total;      // 总分片数 1..255
    uint8_t  seq;        // 本片序号 0..total-1
};
static_assert(sizeof(FragHeader) == 4, "FragHeader must be 4 bytes");

}  // namespace meshroc::net
