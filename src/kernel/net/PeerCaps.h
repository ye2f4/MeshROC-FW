#pragma once
#include <cstdint>
#include <cstddef>
#include "config/MeshROCConfig.h"

/**
 * PeerCaps：邻近节点链路质量表（§13.5 信噪比择优）
 * recordLinkSnr 回填，getPeerSnr 供源路由选路。
 */
namespace meshroc::net {

struct PeerCap {
    uint16_t addr    = 0;     // 对端短地址
    int8_t   snr     = 0;     // 我收它的 SNR（t1 = 互测均值）
    uint32_t lastSeen = 0;    // 最后见时间（t2 = 下线时间判定）
    uint16_t avgSnrQ10 = 0;   // 平均 SNR ×10（选路用）
    uint16_t deliverOk = 0;   // 投递成功计数
    uint16_t deliverTot = 0;  // 投递总计数
};

class PeerCaps {
public:
    static constexpr uint8_t MAX_PEERS = 64;

    // 记录一次收包 SNR（回填，§13.5）
    void recordLinkSnr(uint16_t from, int8_t snr, uint32_t nowMs);

    // 查询某对端 SNR（查不到返回 false）
    bool getPeerSnr(uint16_t addr, int8_t& outSnr) const;

    // 选路度量：0.7*avg_snr + 0.3*delivery_rate（§13.5），返回 0..1000
    // ttlMs 内样本有效（默认 15s）
    uint16_t routeMetric(uint16_t addr, uint32_t nowMs, uint32_t ttlMs = 15000) const;

    const PeerCap* begin() const { return peers_; }
    const PeerCap* end() const { return peers_ + count_; }

private:
    PeerCap peers_[MAX_PEERS];
    uint8_t count_ = 0;

    PeerCap* findOrAdd(uint16_t addr);
};

}  // namespace meshroc::net
