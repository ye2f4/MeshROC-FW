#pragma once
#include <cstdint>
#include <cstddef>
#include "kernel/net/MeshRocPacket.h"

/**
 * Reassembler：大包分片重组（原创优化 O5，对应网站承诺 #8）
 *
 * 重要定位说明（避免误用）：
 * 本类属于 meshroc::net 的**自研 MeshRocPacket 协议栈**（见 MeshRocPacket.h），
 * 与 Meshtastic 的 meshtastic_MeshPacket **不互操作**。它**不是**复刻原版的分片机制——
 * 原版 Meshtastic 没有 FRAG_HEADER 这种 TLV，大包处理走 protobuf 层自身路径。
 * 因此 FRAG_HEADER(0x14)/FRAG_NACK(0x15) 是 MeshRoc 私有 TLV，仅在同一 MeshROC 固件间有效。
 *
 * 当前状态：本类实现**接收端**重组逻辑；**发送端分片器已由 MeshRocStack::sendFragmented
 * 实现**（> MAX_PAYLOAD 的大包会被切成带 FRAG_HEADER 的片并发送），并配套 FRAG_NACK
 * 选择性重传闭环（发送端缓存分片 + 接收端回 NACK + tick 超时兜底重发，上限 3 次）。
 * FRAG_HEADER(0x14)/FRAG_NACK(0x15) 是 MeshRoc 私有 TLV，仅在同一 MeshROC 固件间有效。
 *
 * 借鉴原版调度思路：选择性重传（FRAG_NACK）的超时/退避应复用 O4 AckPolicy 的
 * airtime+CW 推导策略，而非另立超时表。
 *
 * 接收端按 (src, fragId) 聚合分片，收齐后输出完整载荷；单分片丢失时生成
 * FRAG_NACK 触发选择性重传。环形缓存、零堆分配。
 * 参考契约 §15.2-O5 / §15.3 rf.fragEnabled。
 */
namespace meshroc::net {

class Reassembler {
public:
    // 单大包最大分片数：32 片 × ~1KB 单片 ≈ 32KB 重组上限，足够文本/传感大包且省 RAM
    static constexpr uint8_t  MAX_FRAG_TOTAL = 32;
    static constexpr uint16_t MAX_PAYLOAD    = 1024;   // 重组后最大载荷
    static constexpr uint32_t FRAG_TTL_MS    = 30000;  // 分片组存活窗口

    // 喂入一个分片（已解析出 FragHeader + 本片载荷）。返回 true 表示整包已收齐。
    // outBuf/outLen 仅在返回 true 时填充完整载荷。
    bool feed(uint16_t src, const FragHeader& fh, const uint8_t* chunk, uint16_t chunkLen,
              uint8_t* outBuf, uint16_t& outLen, uint32_t nowMs);

    // 生成针对 (src,fragId) 的选择性重传 NACK（缺失分片序号列表）。
    // 无缺失或不存在返回 0。
    uint8_t buildNack(uint16_t src, uint16_t fragId,
                      uint8_t* missingSeqs, uint8_t maxSeqs) const;

    // 周期清理超时分片组
    void expire(uint32_t nowMs);

private:
    struct FragGroup {
        uint16_t src    = 0;
        uint16_t fragId = 0;
        uint8_t  total  = 0;
        uint32_t lastAt = 0;
        uint8_t  haveMask[(MAX_FRAG_TOTAL + 7) / 8] = {0};  // 位示图
        uint16_t chunkLen[MAX_FRAG_TOTAL] = {0};
        uint16_t bufLen = 0;                                // 当前已收总长
        uint8_t  buf[MAX_PAYLOAD] = {0};                    // 重组缓冲
        bool     active = false;
    };

    static constexpr uint8_t MAX_GROUPS = 4;                 // 并发重组组数（压 RAM：原 8）
    FragGroup groups_[MAX_GROUPS];

    FragGroup* findOrAdd(uint16_t src, uint16_t fragId, uint32_t nowMs);
};

}  // namespace meshroc::net
