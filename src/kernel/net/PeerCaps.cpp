#include "net/PeerCaps.h"
#include <algorithm>

namespace meshroc::net {

void PeerCaps::recordLinkSnr(uint16_t from, int8_t snr, uint32_t nowMs)
{
    PeerCap* p = findOrAdd(from);
    if (!p) return;
    p->addr = from;
    p->snr = snr;
    p->lastSeen = nowMs;
    // 指数滑动平均 SNR（×10 存储）
    int16_t v = static_cast<int16_t>(snr) * 10;
    if (p->avgSnrQ10 == 0) p->avgSnrQ10 = static_cast<uint16_t>(v);
    else p->avgSnrQ10 = static_cast<uint16_t>((p->avgSnrQ10 * 7 + v * 3) / 10);
}

bool PeerCaps::getPeerSnr(uint16_t addr, int8_t& outSnr) const
{
    for (uint8_t i = 0; i < count_; ++i) {
        if (peers_[i].addr == addr) {
            outSnr = peers_[i].snr;
            return true;
        }
    }
    return false;
}

uint16_t PeerCaps::routeMetric(uint16_t addr, uint32_t nowMs, uint32_t ttlMs) const
{
    for (uint8_t i = 0; i < count_; ++i) {
        const PeerCap& p = peers_[i];
        if (p.addr != addr) continue;
        if (nowMs - p.lastSeen > ttlMs) return 0;  // 样本过期
        uint32_t rate = p.deliverTot ? (p.deliverOk * 1000 / p.deliverTot) : 0;
        int32_t snrPart = static_cast<int32_t>(p.avgSnrQ10) * 70 / 10;  // 0.7*avg_snr
        int32_t ratePart = static_cast<int32_t>(rate) * 3 / 10;        // 0.3*rate
        int32_t m = snrPart + ratePart;
        return static_cast<uint16_t>(std::max<int32_t>(0, std::min<int32_t>(m, 1000)));
    }
    return 0;
}

PeerCap* PeerCaps::findOrAdd(uint16_t addr)
{
    for (uint8_t i = 0; i < count_; ++i)
        if (peers_[i].addr == addr) return &peers_[i];
    if (count_ < MAX_PEERS) return &peers_[count_++];
    return nullptr;  // 满，丢弃（反转后可换 LRU）
}

}  // namespace meshroc::net
