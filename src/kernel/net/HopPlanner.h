#pragma once
#include <cstdint>
#include "kernel/config/MeshROCConfig.h"
#include "kernel/net/MeshRocPacket.h"

/**
 * HopPlanner：自适应跳数规划器（§13.4 / §11.4）
 * 仅负责「算出发包跳数上限」，不做真正的下一跳路由。
 * 注意：与 src/mesh/NextHopRouter（原版 meshtastic 的完整路由器，继承 FloodingRouter、
 *       处理重传与路由健康）是**两个不同职责的类**，仅名字相似。
 *       此处重命名以彻底消除命名冲突，避免后续维护混淆。
 * 转发判据统一为 isRelayAllowed()（§11.4），
 *   消除原版 "仅 ROUTER 启用 RAP 定向" 与 isBackboneRelay 的不一致。
 */
namespace meshroc::net {

class HopPlanner {
public:
    explicit HopPlanner(const config::MeshROCConfig& cfg) : cfg_(cfg) {}

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
