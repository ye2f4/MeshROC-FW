#pragma once
#include <cstdint>
#include "config/MeshROCConfig.h"
#include "net/MeshRocPacket.h"

/**
 * NextHopRouter：自适应跳数 + 转发判据（§13.4 / §11.4）
 * 反转后取代原版 NextHopRouter 对 meshtastic_Config 的依赖。
 * 注：RAP 定向下一跳的启用条件统一为 isRelayAllowed()（§11.4），
 *      消除原版 "仅 ROUTER 启用 RAP 定向" 与 isBackboneRelay 的不一致。
 */
namespace meshroc::net {

class NextHopRouter {
public:
    explicit NextHopRouter(const config::MeshROCConfig& cfg) : cfg_(cfg) {}

    // 网络直径观测（10min TTL），由 NodeDB 注入；0 表示未知
    void setObservedDiameter(uint8_t diameter) { observedDiameter_ = diameter; }

    // estNetDiameter：max(观测直径+1, 配置默认 hopLimit)
    uint8_t estNetDiameter() const;

    // adaptiveHopLimit：min(配置 hopLimit, estNetDiameter)，上限 7（§13.4）
    uint8_t adaptiveHopLimit() const;

    // 实际发包跳数（双层协同）：max(adaptiveHopLimit, datapack.max_hop 硬下限)
    uint8_t effectiveHopLimit(uint8_t datapackMaxHop) const;

    // 本节点是否允许中继（统一判据 §11.4）
    bool isRelayAllowed() const { return config::isRelayAllowed(cfg_.deviceRole); }

    // RAP 定向下一跳是否启用（统一为 isRelayAllowed，消除旧 ROUTER_LATE 不一致）
    bool rapDirectedEnabled() const { return isRelayAllowed(); }

private:
    const config::MeshROCConfig& cfg_;
    uint8_t observedDiameter_ = 0;  // 0 = 未知
};

}  // namespace meshroc::net
