#pragma once
#include <cstdint>
#include "net/MeshRocPacket.h"

/**
 * AckPolicy：分级 ACK 策略（原创优化 O4，对应网站承诺 #10）
 *
 * 设计原则（关键改动）：本类**不**自造重传调度循环，也不维护独立超时死表。
 * 原版 Meshtastic 的 ACK / 重传机制是唯一正确实现：
 *   - want_ack 是 meshtastic_MeshPacket 的字段；
 *   - ReliableRouter::send() 对 want_ack 包调 startRetransmission(copy, NUM_RELIABLE_RETX)；
 *   - 重传超时由 RadioInterface::getRetransmissionMsec(p) 基于「包 airtime + 信道利用率 CW」算出；
 *   - 收到 ACK（RoutingModule::sendAckNak）后停止重传，超时耗尽发 NAK。
 *
 * 因此 AckPolicy 只做一件事：在原版算出的重传超时/次数之上，按 MeshRoc 优先级施加
 * 倍率微调（CRITICAL 缩短超时、增加重传；LOWEST 放宽），其余一律委托原版。
 * 真正执行重传的是 ReliableRouter / NextHopRouter，本类只提供「建议参数」。
 *
 * 参考契约 §15.2-O4 / §15.3 ack.priorityLevels，及对原版 ReliableRouter 的借鉴。
 */
namespace meshroc::net {

struct AckPlan {
    float timeoutFactor;  // 乘在原版 getRetransmissionMsec 之上的倍率（<1 更快重传）
    int8_t retryDelta;    // 相对原版 NUM_RELIABLE_RETX 的增减（可为负）
};

class AckPolicy {
public:
    // 取某优先级的「建议调整」。priorityLevels=false（配置关闭）时返回中性(1.0/0)，即完全交给原版。
    static AckPlan planFor(MeshRocPriority prio, bool priorityLevelsEnabled);

    // 便捷：从包直接判（求救/告警走 CRITICAL，其余按包内优先级）。
    static AckPlan planForPacket(const MeshRocPacket& pkt, bool priorityLevelsEnabled);

    // 把「建议调整」合成到原版基准超时上，得到最终超时(ms)。
    // baseTimeoutMs 应来自 RadioInterface::getRetransmissionMsec(p)。
    static uint32_t resolveTimeoutMs(uint32_t baseTimeoutMs, const AckPlan& plan);

    // 把「建议调整」合成到原版基准重传次数上，得到最终次数。
    // baseRetries 应来自原版 NUM_RELIABLE_RETX。
    static uint8_t resolveRetries(uint8_t baseRetries, const AckPlan& plan);
};

}  // namespace meshroc::net
