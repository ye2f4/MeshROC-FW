#pragma once
#include <cstdint>
#include <cstddef>
#include "kernel/net/MeshRocPacket.h"

/**
 * RAP（Router Attach Protocol）归属协议定义（§13.2）
 * 子类型、TLV tag、实现常量、终端状态机。
 * 注意：RAP 帧的 type 固定为 MeshRocPacketType::RAP，子类型编码在 RAP_KIND(0x20) TLV。
 */
namespace meshroc::net::rap {

// ---- RAP 子类型（RAP_KIND=0x20 的 value） ----
enum class RapKind : uint8_t {
    HELLO      = 1,
    HELLO_ACK  = 2,
    ATTACH_REQ = 3,
    ATTACH_ACK = 4,
    KEEPALIVE  = 5,
    DETACH     = 6,
    OWNERSHIP_ADV = 7,  // 骨干向终端广播归属表（OWNER_LIST / OWNER_DEL）
    SYNC_REQ      = 8,  // 终端向骨干请求全量归属表同步
};

// ---- RAP TLV（0x20-0x28，与原版 0x01-0x13 不冲突，§13.2） ----
enum class RapTlv : uint8_t {
    KIND          = 0x20,  // 1B 子类型
    NEIGHBOR_SNR  = 0x21,  // 3B uint16BE 对端短址 + int8 我收它的 SNR
    ROLE_CLASS    = 0x22,  // 1B §11.3 角色值（反转后直接填 MeshRocRole）
    ATTACH_SEQ    = 0x23,  // 2B uint16BE 归属序号（单调递增，后到覆盖先到）
    TTL_GRANT     = 0x24,  // 2B uint16BE 授予租期（秒）
    OWNER_LIST    = 0x25,  // N*4 uint16BE 终端短址 + uint16BE attachSeq
    OWNER_DEL     = 0x26,  // N*2 uint16BE 已离开终端短址
    TABLE_VERSION = 0x27,  // 2B uint16BE 归属表版本，每次变更 +1
    LINK_COST     = 0x28,  // 1B int8 双向链路代价 = min(snr_AB, snr_BA) dB
};

// ---- 实现常量（§13.2 权威值） ----
constexpr uint32_t HELLO_PERIOD_MS        = 90'000;   // 90s
constexpr uint32_t HELLO_JITTER_MS        = 30'000;   // 30s
constexpr uint32_t NEIGHBOR_TTL_MS        = 270'000;  // 270s
constexpr uint32_t FULL_ADV_PERIOD_MS     = 30 * 60 * 1000;  // 30min
constexpr uint8_t  ADV_MAX_ENTRIES_PER_FRAME = 16;
constexpr uint8_t  MAX_NEIGHBORS          = 16;
constexpr uint8_t  MAX_OWNED              = 32;
constexpr uint8_t  MAX_REMOTE_OWNERS      = 64;
constexpr uint32_t HELLO_BACKOFF_MAX_MS   = 12 * 60 * 1000;  // 12min
constexpr uint8_t  LBT_UTIL_THRESHOLD_PCT = 40;     // 信道利用率阈值
constexpr uint32_t ATTACH_RETRY_MS        = 30'000;  // 30s
constexpr uint32_t EVAL_INTERVAL_MS       = 15'000;  // 15s
// 归属失联判定（ATTACHED 状态下超过该时长无活动 → 回 SCANNING 重发现）
constexpr uint32_t ATTACH_TIMEOUT_MS      = 270'000; // 270s（等同 NEIGHBOR_TTL）
// 归属续租周期（ATTACHED 状态下周期性发 KEEPALIVE 续租）
constexpr uint32_t KEEPALIVE_PERIOD_MS    = 60'000;  // 60s

// 续租 TTL（秒）：CLIENT/DTU=30min SENSOR=2h TRACKER=6h（§13.2 / §11.3）
constexpr uint32_t TTL_CLIENT_S  = 30 * 60;
constexpr uint32_t TTL_DTU_S     = 30 * 60;
constexpr uint32_t TTL_SENSOR_S  = 2 * 60 * 60;
constexpr uint32_t TTL_TRACKER_S = 6 * 60 * 60;
// 其余（BACKBONE/GATEWAY）不续租（服务端）

// ---- 铁律三（迟滞切换，§13.2） ----
constexpr int8_t  RAP_HYST_SNR_DB    = 6;     // 候选需优出 6dB
constexpr uint8_t RAP_HYST_SAMPLES   = 3;     // 连续 3 次独立采样
constexpr uint32_t RAP_MIN_DWELL_MS  = 10 * 60 * 1000;  // 最短驻留 10min

// 路由失效落回洪泛阈值
constexpr uint8_t RAP_ROUTE_FAIL_THRESHOLD = 3;

// ---- 终端状态机（§13.2） ----
enum class RapTerminalState : uint8_t {
    SCANNING   = 0,  // 监听 HELLO，收集候选
    ATTACHED   = 1,  // 已归属某骨干
    EVALUATING = 2,  // 发现更优候选，迟滞评估中
};

}  // namespace meshroc::net::rap
