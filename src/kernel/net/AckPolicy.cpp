#include "net/AckPolicy.h"

namespace meshroc::net {

// 分级策略：仅给出相对原版基准的「调整量」，不替代原版重传调度。
// 借鉴原版 ReliableRouter：原版对 want_ack 包用 NUM_RELIABLE_RETX 次重传，
// 超时由 RadioInterface::getRetransmissionMsec(p) 基于 airtime+CW 算出。
// 这里 CRITICAL 加快重传、增加次数；LOWEST 放宽以省信道。
AckPlan AckPolicy::planFor(MeshRocPriority prio, bool priorityLevelsEnabled)
{
    if (!priorityLevelsEnabled) {
        return AckPlan{1.0f, 0}; // 中性：完全交给原版
    }
    switch (prio) {
    case MeshRocPriority::CRITICAL:
        return AckPlan{0.6f, +2}; // 更快重传、多 2 次
    case MeshRocPriority::HIGH:
        return AckPlan{0.85f, +1};
    case MeshRocPriority::LOW:
        return AckPlan{1.1f, 0};
    case MeshRocPriority::LOWEST:
    default:
        return AckPlan{1.3f, -1}; // 放宽、少 1 次（不低于 0）
    }
}

AckPlan AckPolicy::planForPacket(const MeshRocPacket& pkt, bool priorityLevelsEnabled)
{
    // 本栈设计：求救/告警通过 priority()==CRITICAL 表达（见 MeshRocPacketType 注释），
    // 故直接按包内优先级取策略即可；wantAck 标志位决定原版是否启用重传。
    return planFor(pkt.priority(), priorityLevelsEnabled);
}

uint32_t AckPolicy::resolveTimeoutMs(uint32_t baseTimeoutMs, const AckPlan& plan)
{
    // 不低于原版基准的 50%，避免过激
    uint32_t t = static_cast<uint32_t>(baseTimeoutMs * plan.timeoutFactor);
    uint32_t floor = (baseTimeoutMs / 2) > 0 ? (baseTimeoutMs / 2) : 1;
    return (t < floor) ? floor : t;
}

uint8_t AckPolicy::resolveRetries(uint8_t baseRetries, const AckPlan& plan)
{
    int r = static_cast<int>(baseRetries) + plan.retryDelta;
    if (r < 0) r = 0;
    if (r > 15) r = 15; // 防止耗尽信道
    return static_cast<uint8_t>(r);
}

}  // namespace meshroc::net
