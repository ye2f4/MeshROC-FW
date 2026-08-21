#pragma once
#include <cstdint>
#include <cstddef>
#include "kernel/config/MeshROCConfig.h"

/**
 * Router：四相混合路由（平移自 MeshRocModule 混合路由状态机，route.txt §5）
 *   PHASE1 洪泛探测：骨干中继记录路径、减跳、重广播
 *   PHASE2 业务单播：严格按 ROUTE_PATH 跳点转发
 *   PHASE3 失败回退：ACK 超时 → 标记失效 → 重新洪泛探测
 *   PHASE4 广播/心跳/遥测：受限洪泛
 *
 * 去 Meshtastic 依赖：发帧经注入的 SendFn（→ MeshRocStack::sendRaw），
 * 时间经 nowMs 参数，地址经 myAddr_。角色 = config::MeshRocRole。
 */
namespace meshroc::net {

// 路由缓存条目：目的地址 → 源路由跳点序列 + 度量 + 老化计时
struct RouteEntry {
    uint16_t dst       = 0;
    uint16_t hops[64]  = {0};  // 源路由跳点（uint16 大端, 2字节对齐）
    uint8_t  hopCount  = 0;
    uint16_t avgSnr    = 0;
    uint16_t deliveryOk= 0;
    uint32_t expireAt  = 0;
    bool     valid     = false;
};

// 去重缓存项（60s 窗口）
struct SeenEntry {
    uint32_t key   = 0;
    uint32_t seenAt = 0;
};

// 重广播/转发回调（宿主把字节交射频）
using RouteSendFn = void (*)(void* ctx, const uint8_t* frame, uint16_t len);

class Router {
public:
    static constexpr uint8_t kMaxRoutes = 16;
    static constexpr uint8_t kMaxSeen   = 32;
    static constexpr uint32_t kRouteTtlMs = 1200UL * 1000; // 1200s 老化
    static constexpr uint32_t kSeenWindowMs = 60000;

    Router(uint16_t myAddr, config::MeshRocRole myRole, RouteSendFn sendFn, void* ctx,
           uint8_t defaultHopLimit)
        : myAddr_(myAddr), myRole_(myRole), sendFn_(sendFn), ctx_(ctx),
          defaultHopLimit_(defaultHopLimit) {}

    // 解析含 ROUTE_PATH TLV 的帧并严格按跳点转发（PHASE2）
    // frame 为完整空中帧（10B头+TLV+2B CRC），调用方已校验 CRC。
    void onSourceRoute(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs);

    // 洪泛探测中继（PHASE1）：仅 BACKBONE 参与，减跳重广播
    void onRouteProbe(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs);

    // 受限洪泛（PHASE4）：bit6=1 仅骨干中继
    void onFloodFrame(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs);

    // ACK 超时 → 标记失效（PHASE3）
    void onAckTimeout(uint16_t dst);

    // 路由缓存 CRUD
    RouteEntry* routeLookup(uint16_t dst);
    void routeStore(uint16_t dst, const uint16_t* hops, uint8_t hopCount, uint16_t avgSnr, uint16_t ok, uint32_t nowMs);
    void routeInvalidate(uint16_t dst);

    // 去重检查（返回 true 表示已见过，应丢弃）
    bool seenCheck(uint16_t src, uint16_t seq, uint32_t nowMs);

    // 自适应跳数（原样复刻 MeshRocModule::adaptiveHopLimit）：
    // 维护估计的网络直径 estNetDiameter（基于观测到的帧 max_hop 最大值 + 余量），
    // 返回本节点发出私有帧应使用的 max_hop。小网不浪费跳数，大网不截断。
    void observeDiameter(uint8_t observedMaxHop, uint32_t nowMs);
    uint8_t adaptiveHopLimit() const;

    // 普通数据帧 TLV 处理（原样复刻 MeshRocModule::onMeshRocFrame）：
    // 解析 REVERSE_PATH → routeStore（探测建表的唯一入口），ROUTE_INVALID → routeInvalidate。
    // frame 为完整空中帧（10B头+TLV+2B CRC），调用方已校验 CRC。
    void onMeshRocFrame(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs);

private:
    void relayDecrement(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs, uint16_t nextHop);
    void floodRelay(const uint8_t* frame, uint16_t frameLen, uint32_t nowMs);
    uint8_t parsePathTlv(const uint8_t* val, uint8_t len, uint16_t* outHops, uint8_t maxHops) const;

    uint16_t          myAddr_;
    config::MeshRocRole myRole_;
    RouteSendFn       sendFn_;
    void*             ctx_;
    uint8_t           defaultHopLimit_ = 7;  // 配置默认跳数（原版 config.lora.hop_limit）

    // 自适应跳数状态（原样复刻 MeshRocModule::estNetDiameter / adaptiveHopLimit）
    uint8_t           estNetDiameter_ = 0;   // 观测到的网络直径（max_hop 历史最大值），0=未知
    uint32_t          diameterSampleAt_ = 0; // 上次重算时间戳
    static constexpr uint32_t kDiameterTtlMs = 600000; // 10min 老化（原版 MESHROC_DIAMETER_TTL）

    RouteEntry        routeCache_[kMaxRoutes];
    uint8_t           routeCacheIdx_ = 0;
    SeenEntry         seenCache_[kMaxSeen];
    uint8_t           seenCacheIdx_ = 0;
};

}  // namespace meshroc::net
