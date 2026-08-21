#include "kernel/net/Router.h"
#include "kernel/net/MeshRocCodec.h"
#include <algorithm>
#include <cstring>

namespace meshroc::net {

// 大端头偏移（与 MeshRocStack.cpp 一致，D3 大端）
static constexpr size_t HDR_OFF_SRC    = 1;  // 源地址（u16 BE）
static constexpr size_t HDR_OFF_MAXHOP = 7;
static constexpr size_t HDR_OFF_SNR    = 8;
static constexpr size_t HDR_OFF_ROUTE  = 9;
static constexpr size_t TLV_OFF        = 10;

uint8_t Router::parsePathTlv(const uint8_t* val, uint8_t len, uint16_t* outHops, uint8_t maxHops) const
{
    uint8_t n = 0;
    uint8_t i = 0;
    while (i + 2 <= len && n < maxHops) {
        outHops[n] = static_cast<uint16_t>((static_cast<uint16_t>(val[i]) << 8) | val[i + 1]);
        n++; i += 2;
    }
    return n;
}

RouteEntry* Router::routeLookup(uint16_t dst)
{
    for (uint8_t i = 0; i < kMaxRoutes; i++)
        if (routeCache_[i].valid && routeCache_[i].dst == dst)
            return &routeCache_[i];
    return nullptr;
}

void Router::routeStore(uint16_t dst, const uint16_t* hops, uint8_t hopCount, uint16_t avgSnr, uint16_t ok, uint32_t nowMs)
{
    RouteEntry* e = routeLookup(dst);
    if (!e) {
        e = &routeCache_[routeCacheIdx_];
        routeCacheIdx_ = static_cast<uint8_t>((routeCacheIdx_ + 1) % kMaxRoutes);
    }
    e->dst = dst;
    e->hopCount = hopCount > (kMaxRoutes * 4) ? static_cast<uint8_t>(kMaxRoutes * 4) : hopCount;
    for (uint8_t i = 0; i < e->hopCount && i < 64; i++)
        e->hops[i] = hops[i];
    e->avgSnr = avgSnr;
    e->deliveryOk = ok;
    e->expireAt = nowMs + kRouteTtlMs;
    e->valid = true;
}

void Router::routeInvalidate(uint16_t dst)
{
    RouteEntry* e = routeLookup(dst);
    if (e) e->valid = false;
}

bool Router::seenCheck(uint16_t src, uint16_t seq, uint32_t nowMs)
{
    uint32_t key = (static_cast<uint32_t>(src) << 16) | seq;
    for (uint8_t i = 0; i < kMaxSeen; i++) {
        if (seenCache_[i].key == key) {
            if (nowMs - seenCache_[i].seenAt < kSeenWindowMs) return true;
            seenCache_[i].seenAt = nowMs;
            return false;
        }
    }
    seenCache_[seenCacheIdx_] = SeenEntry{key, nowMs};
    seenCacheIdx_ = static_cast<uint8_t>((seenCacheIdx_ + 1) % kMaxSeen);
    return false;
}

// 重广播/定向转发：复制帧、改 max_hop、重写 CRC16、发出
void Router::relayDecrement(const uint8_t* frame, uint16_t frameLen, uint32_t /*nowMs*/, uint16_t nextHop)
{
    if (frameLen < TLV_OFF + 2) return;
    uint8_t buf[300];
    if (frameLen > sizeof(buf)) return;
    ::memcpy(buf, frame, frameLen);
    // 减跳（大端头 max_hop 在偏移 7）
    if (buf[HDR_OFF_MAXHOP] > 0) buf[HDR_OFF_MAXHOP]--;
    // 若指定下一跳（定向单播），改 dst 字段（大端，偏移 3）
    if (nextHop != 0xFFFF) {
        buf[3] = static_cast<uint8_t>((nextHop >> 8) & 0xFF);
        buf[4] = static_cast<uint8_t>(nextHop & 0xFF);
    }
    // 重写帧尾 CRC16（覆盖到 TLV 末尾，不含 2B CRC）
    uint16_t crc = MeshRocCodec::crc16(buf, frameLen - 2);
    buf[frameLen - 2] = static_cast<uint8_t>(crc & 0xFF);
    buf[frameLen - 1] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    if (sendFn_) sendFn_(ctx_, buf, frameLen);
}

void Router::floodRelay(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs)
{
    (void)nowMs;
    relayDecrement(frame, frameLen, nowMs, 0xFFFF); // 广播（dst 不变）
}

void Router::onRouteProbe(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs)
{
    // 原样复刻 MeshRocModule::onRouteProbe：
    // 非骨干：把帧当普通帧处理（解析 TLV，含 REVERSE_PATH/ROUTE_INVALID），不中继。
    // 骨干：仅骨干中继参与洪泛探测，减跳重广播。
    if (!config::isRelayAllowed(myRole_)) {
        onMeshRocFrame(frame, frameLen, nowMs);
        return;
    }
    if (frameLen < TLV_OFF + 2) return;
    if (frame[HDR_OFF_MAXHOP] == 0) return; // max_hop==0 丢弃
    floodRelay(frame, frameLen, nowMs);
}

void Router::onSourceRoute(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs)
{
    if (frameLen < TLV_OFF + 2) return;
    const uint8_t* tlv = frame + TLV_OFF;
    uint16_t tlvLen = static_cast<uint16_t>(frameLen - TLV_OFF - 2);
    // 在 TLV 中定位 ROUTE_PATH（0x10）
    size_t i = 0;
    while (i + 2 <= tlvLen) {
        uint8_t tag = tlv[i], l = tlv[i + 1];
        const uint8_t* v = tlv + i + 2;
        if (tag == static_cast<uint8_t>(MeshRocTlv::ROUTE_PATH)) {
            uint16_t hops[64];
            uint8_t n = parsePathTlv(v, l, hops, 64);
            int idx = -1;
            for (uint8_t k = 0; k < n; k++)
                if (hops[k] == myAddr_) idx = k;
            if (idx >= 0 && idx + 1 < n) {
                relayDecrement(frame, frameLen, nowMs, hops[idx + 1]); // 定向到下一跳
            }
            // 本节点不在跳点序列则丢弃（不洪泛扩散）
            return;
        }
        i += 2 + l;
    }
}

void Router::onFloodFrame(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs)
{
    if (frameLen < TLV_OFF + 2) return;
    // 原样复刻 MeshRocModule::onFloodFrame：先处理 TLV（REVERSE_PATH/ROUTE_INVALID），
    // 再骨干中继（减跳重广播）；CLIENT 只接收不中继。
    onMeshRocFrame(frame, frameLen, nowMs);
    bool relayPerm = (frame[HDR_OFF_ROUTE] & 0x40) != 0; // 受限洪泛 bit6=1 仅骨干中继
    if (relayPerm && !config::isRelayAllowed(myRole_)) return; // 非骨干不中继受限洪泛
    if (config::isRelayAllowed(myRole_) && frame[HDR_OFF_MAXHOP] > 0) {
        floodRelay(frame, frameLen, nowMs);
    }
}

// 原样复刻 MeshRocModule::onMeshRocFrame：解析 TLV 载荷，
// REVERSE_PATH → 反向路径即回程源路由，缓存 src→路径反向；ROUTE_INVALID → 失效。
void Router::onMeshRocFrame(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs)
{
    (void)nowMs;
    if (frameLen < TLV_OFF + 2) return;
    const uint8_t* tlv = frame + TLV_OFF;
    uint16_t tlvLen = static_cast<uint16_t>(frameLen - TLV_OFF - 2);
    // src 在 10B 头偏移 1（大端，D3）
    uint16_t src = static_cast<uint16_t>((static_cast<uint16_t>(frame[HDR_OFF_SRC]) << 8) | frame[HDR_OFF_SRC + 1]);
    uint8_t snr = frame[HDR_OFF_SNR];

    size_t i = 0;
    while (i + 2 <= tlvLen) {
        uint8_t tag = tlv[i], l = tlv[i + 1];
        const uint8_t* v = tlv + i + 2;
        if (i + 2 + l > tlvLen) break; // 截断保护

        if (tag == static_cast<uint8_t>(MeshRocTlv::REVERSE_PATH)) {
            uint16_t hops[64];
            uint8_t n = parsePathTlv(v, l, hops, 64);
            if (n > 0) {
                // 反向路径即回程源路由：缓存 src→本包路径反向（原版 routeStore(pkt.src_addr, hops, n, ...)）
                routeStore(src, hops, n, snr, 1, nowMs);
            }
        } else if (tag == static_cast<uint8_t>(MeshRocTlv::ROUTE_INVALID)) {
            routeInvalidate(src);
        }
        i += 2 + l;
    }
}

void Router::onAckTimeout(uint16_t dst)
{
    // 原样复刻 MeshRocModule::onAckTimeout：标记失效。
    // 注：原版注释"应用层据此重新发起 PHASE1 探测"，但原代码仅标记失效，无实际重探触发。
    // 此处保持原样半残行为，不额外补重探。
    routeInvalidate(dst);
}

void Router::observeDiameter(uint8_t observedMaxHop, uint32_t nowMs)
{
    // 原样复刻 MeshRocModule::adaptiveHopLimit 的观测更新：
    // 取历史观测最大值作为网络直径估计。
    if (observedMaxHop > estNetDiameter_) {
        estNetDiameter_ = observedMaxHop;
        diameterSampleAt_ = nowMs;
    } else if (nowMs - diameterSampleAt_ > kDiameterTtlMs) {
        // 老化后重置，下次重新估计
        estNetDiameter_ = observedMaxHop;
        diameterSampleAt_ = nowMs;
    }
}

uint8_t Router::adaptiveHopLimit() const
{
    // 原样复刻 MeshRocModule::adaptiveHopLimit：
    // 以观测到的网络直径估计值为基础（取观测直径 + 1 余量），至少取配置默认。
    uint8_t base = defaultHopLimit_;
    if (estNetDiameter_ == 0) return base;
    uint8_t est = static_cast<uint8_t>(estNetDiameter_ + 1);
    return (est > base) ? est : base;
}

}  // namespace meshroc::net
