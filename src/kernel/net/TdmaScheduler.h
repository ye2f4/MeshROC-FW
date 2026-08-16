#pragma once
#include <cstdint>

/**
 * TdmaScheduler：O2 时隙调度骨架（原创优化 O2）
 *
 * 设计定位：本类只提供"最小可运行时隙调度"——把时间轴切成等长槽，节点按地址哈希
 * 占固定槽位，从而错开发送窗口、降低冲突。这是 O2 的**协议层骨架**，不直接操作射频
 * 时序（真正的介质访问控制/槽位同步接入留给后续射频层）。
 *
 * 借鉴原版思路：原版 Meshtastic 也用"时隙化的发送退避"（getTxDelayMsec 基于 CWsize 的
 * 随机 slot 等待）避免冲突；本栈以确定性固定槽位替代随机退避，更适合确定性的归属拓扑。
 *
 * 参考契约 §15.2-O2 / §15.3 rf.tdma。
 */
namespace meshroc::net {

class TdmaScheduler {
public:
    static constexpr uint32_t SLOT_MS = 50;   // 单槽时长（后续射频层可按需调整）
    static constexpr uint32_t FRAME_MS = 1000; // 一个 TDMA 帧=1s，含 20×50ms 槽

    TdmaScheduler(bool enabled, uint8_t slotCount)
        : enabled_(enabled), slotCount_(slotCount < 1 ? 1 : slotCount) {}

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    uint8_t slotCount() const { return slotCount_; }

    // 当前时间落在哪个槽（0..slotCount-1）
    uint8_t currentSlot(uint32_t nowMs) const
    {
        return static_cast<uint8_t>((nowMs / SLOT_MS) % slotCount_);
    }

    // 本节点的固定槽位：地址哈希均匀分布到 [0, slotCount)
    uint8_t mySlot(uint16_t myAddr) const
    {
        // 简单哈希：FNV-1a 低 8 位取模，足以打散
        uint32_t h = 2166136261u;
        h ^= static_cast<uint32_t>(myAddr) & 0xFF;
        h *= 16777619u;
        h ^= (static_cast<uint32_t>(myAddr) >> 8) & 0xFF;
        h *= 16777619u;
        return static_cast<uint8_t>(h % slotCount_);
    }

    // 此刻是否处于本节点发送窗口（仅当 enabled 且当前槽==我的槽）
    bool isMyTxWindow(uint32_t nowMs, uint16_t myAddr) const
    {
        if (!enabled_) return true;  // 关闭时不做时隙限制，回退到无 TDMA（与原版随机退避等价）
        return currentSlot(nowMs) == mySlot(myAddr);
    }

private:
    bool     enabled_;
    uint8_t  slotCount_;
};

}  // namespace meshroc::net
