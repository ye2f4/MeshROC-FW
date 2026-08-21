#include "kernel/net/rap/LinkQuality.h"

namespace meshroc::net::rap {

void LinkQuality::markPeerMeshRoc(uint32_t nodeNum, uint32_t nowMs)
{
    if (nodeNum == 0 || nodeNum == 0xFFFFFFFFu)
        return; // 非法/广播地址不记录
    for (uint8_t i = 0; i < MAX_PEERS; i++) {
        if (peers_[i].nodeNum == nodeNum) {
            peers_[i].isMeshRoc = true;
            peers_[i].lastSeen  = nowMs;
            return;
        }
    }
    uint8_t slot = peerIdx_;
    peers_[slot] = PeerLink{nodeNum, true, nowMs, 0};
    peerIdx_ = static_cast<uint8_t>((peerIdx_ + 1) % MAX_PEERS);
}

void LinkQuality::recordLinkSnr(uint32_t fromNode, int8_t snr, uint32_t nowMs)
{
    if (fromNode == 0 || fromNode == 0xFFFFFFFFu)
        return;
    for (uint8_t i = 0; i < MAX_PEERS; i++) {
        if (peers_[i].nodeNum == fromNode) {
            if (snr > peers_[i].bestSnr)
                peers_[i].bestSnr = snr; // 仅记录最佳（链路质量上界）
            peers_[i].lastSeen = nowMs;
            return;
        }
    }
    uint8_t slot = peerIdx_;
    peers_[slot] = PeerLink{fromNode, true, nowMs, snr};
    peerIdx_ = static_cast<uint8_t>((peerIdx_ + 1) % MAX_PEERS);
}

int8_t LinkQuality::getPeerSnr(uint32_t nodeNum) const
{
    for (uint8_t i = 0; i < MAX_PEERS; i++)
        if (peers_[i].nodeNum == nodeNum)
            return peers_[i].bestSnr;
    return 0;
}

uint8_t LinkQuality::countMeshRocPeers(uint32_t nowMs) const
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_PEERS; i++) {
        if (peers_[i].isMeshRoc && (nowMs - peers_[i].lastSeen < PEER_CAP_TTL_MS))
            n++;
    }
    return n;
}

bool LinkQuality::isPeerMeshRoc(uint32_t nodeNum, uint32_t nowMs) const
{
    for (uint8_t i = 0; i < MAX_PEERS; i++) {
        if (peers_[i].nodeNum == nodeNum && peers_[i].isMeshRoc) {
            if (nowMs - peers_[i].lastSeen < PEER_CAP_TTL_MS)
                return true;
        }
    }
    return false; // 未见其发私有帧，或已老化 → 视为普通节点
}

}  // namespace meshroc::net::rap
