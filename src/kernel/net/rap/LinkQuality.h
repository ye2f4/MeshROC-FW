#pragma once
#include <cstdint>
#include <cstddef>

namespace meshroc::net::rap {

// 链路质量表（平移自 MeshRocModule::peerCaps，去掉 Meshtastic 依赖）。
// 记录对端最佳接收 SNR 与 MeshROC 能力标志，供 RAP 选路/归属择优。
// 时间源由调用方传入（nowMs），不依赖 millis()。
struct PeerLink {
    uint32_t nodeNum   = 0;   // 对端全节点号（高 16 位=短址）
    bool     isMeshRoc = false;
    uint32_t lastSeen  = 0;   // 最近见到其私有帧的时间戳（ms）
    int8_t   bestSnr   = 0;   // 历史最佳接收 SNR（链路质量上界）
};

class LinkQuality {
public:
    static constexpr uint8_t MAX_PEERS = 32;
    static constexpr uint32_t PEER_CAP_TTL_MS = 6UL * 60 * 60 * 1000; // 6h，见 MeshRocModule MR_PEER_CAP_TTL

    // 观测到该节点发过私有帧 → 标记为 MeshROC 节点（天然区分普通 Meshtastic 节点）。
    void markPeerMeshRoc(uint32_t nodeNum, uint32_t nowMs);

    // 记录对端 rx_snr 进链路质量表（仅记最佳）。
    void recordLinkSnr(uint32_t fromNode, int8_t snr, uint32_t nowMs);

    // 取对端最佳链路 SNR（未记录返回 0）。
    int8_t getPeerSnr(uint32_t nodeNum) const;

    // 已确认 MeshROC 对端数（能力协商结果，有效期内）。
    uint8_t countMeshRocPeers(uint32_t nowMs) const;

    bool isPeerMeshRoc(uint32_t nodeNum, uint32_t nowMs) const;

private:
    PeerLink peers_[MAX_PEERS];
    uint8_t  peerIdx_ = 0;
};

}  // namespace meshroc::net::rap
