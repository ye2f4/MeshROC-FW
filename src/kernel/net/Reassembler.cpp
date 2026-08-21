#include "kernel/net/Reassembler.h"
#include <cstring>

namespace meshroc::net {

static inline void setBit(uint8_t* mask, uint8_t idx)
{ mask[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7)); }
static inline bool getBit(const uint8_t* mask, uint8_t idx)
{ return (mask[idx >> 3] >> (idx & 7)) & 1u; }

Reassembler::FragGroup* Reassembler::findOrAdd(uint16_t src, uint16_t fragId, uint32_t nowMs)
{
    FragGroup* freeSlot = nullptr;
    for (auto& g : groups_) {
        if (g.active && g.src == src && g.fragId == fragId) return &g;
        if (!g.active && !freeSlot) freeSlot = &g;
    }
    if (!freeSlot) {
        // 回收最旧的活跃组
        FragGroup* oldest = &groups_[0];
        for (auto& g : groups_) {
            if (g.active && g.lastAt < oldest->lastAt) oldest = &g;
        }
        freeSlot = oldest;
    }
    FragGroup& g = *freeSlot;
    g = FragGroup{};
    g.src = src;
    g.fragId = fragId;
    g.active = true;
    g.lastAt = nowMs;
    return &g;
}

bool Reassembler::feed(uint16_t src, const FragHeader& fh, const uint8_t* chunk,
                       uint16_t chunkLen, uint8_t* outBuf, uint16_t& outLen, uint32_t nowMs)
{
    if (fh.total == 0 || fh.total > MAX_FRAG_TOTAL || fh.seq >= fh.total) return false;
    FragGroup* g = findOrAdd(src, fh.fragId, nowMs);
    if (g->total == 0) g->total = fh.total;
    if (g->total != fh.total) return false;   // 同组总片数突变，丢弃

    if (!getBit(g->haveMask, fh.seq)) {
        uint16_t off = 0;
        for (uint8_t i = 0; i < fh.seq; ++i) off += g->chunkLen[i];
        if (off + chunkLen <= MAX_PAYLOAD) {
            ::memcpy(g->buf + off, chunk, chunkLen);
            g->chunkLen[fh.seq] = chunkLen;
            g->bufLen = (off + chunkLen > g->bufLen) ? (off + chunkLen) : g->bufLen;
            setBit(g->haveMask, fh.seq);
        }
    }
    g->lastAt = nowMs;

    // 是否收齐
    for (uint8_t i = 0; i < g->total; ++i) {
        if (!getBit(g->haveMask, i)) return false;
    }
    outLen = g->bufLen;
    ::memcpy(outBuf, g->buf, g->bufLen);
    g->active = false;   // 重组完成，释放槽位
    return true;
}

uint8_t Reassembler::buildNack(uint16_t src, uint16_t fragId,
                               uint8_t* missingSeqs, uint8_t maxSeqs) const
{
    for (const auto& g : groups_) {
        if (!g.active || g.src != src || g.fragId != fragId) continue;
        uint8_t n = 0;
        for (uint8_t i = 0; i < g.total && n < maxSeqs; ++i) {
            if (!getBit(g.haveMask, i)) missingSeqs[n++] = i;
        }
        return n;
    }
    return 0;
}

void Reassembler::expire(uint32_t nowMs)
{
    for (auto& g : groups_) {
        if (g.active && (nowMs - g.lastAt) > FRAG_TTL_MS) g.active = false;
    }
}

}  // namespace meshroc::net
