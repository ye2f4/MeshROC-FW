#pragma once
#include <cstdint>

/**
 * MeshROC 反转内核配置（唯一真相源）
 * 对应契约 §11.3 / §14.5 / §15.3。
 * 反转后本结构体取代原版 meshtastic_Config / ChannelStruct / ModuleConfig 中
 * 与 MeshROC 自研资产相关的字段，直接由 FlashKV 存储，不再寄生原版 config。
 */

namespace meshroc::config {

// ---- §11.3 唯一角色枚举（反转后权威） ----
enum class MeshRocRole : uint8_t {
    BACKBONE = 0,  // 转发，RAP 服务端（发 HELLO/收 ATTACH）
    CLIENT   = 1,  // 不转发，RAP 终端，TTL 30min
    SENSOR   = 2,  // 不转发，RAP 终端，TTL 2h
    TRACKER  = 3,  // 不转发，RAP 终端，TTL 6h
    DTU      = 4,  // 不转发，RAP 终端密集业务，TTL 30min（方案 A 激活）
    GATEWAY  = 5,  // 转发（特殊骨干），桥接 LoRa↔互联网（方案 A 激活）
    COUNT    = 6,  // 枚举上界，用于校验
};

// 统一转发判据（§11.4）：替代旧 inferLocalRole / isBackboneRelay 两套判据
inline bool isRelayAllowed(MeshRocRole role)
{
    return role == MeshRocRole::BACKBONE || role == MeshRocRole::GATEWAY;
}

// RAP 续租 TTL（毫秒），§11.3 / §13.2
constexpr uint32_t rapTtlMs(MeshRocRole role)
{
    switch (role) {
        case MeshRocRole::SENSOR:  return 2u * 60 * 60 * 1000;  // 2h
        case MeshRocRole::TRACKER: return 6u * 60 * 60 * 1000;  // 6h
        case MeshRocRole::CLIENT:
        case MeshRocRole::DTU:
        case MeshRocRole::BACKBONE:
        case MeshRocRole::GATEWAY:
        default:                   return 30u * 60 * 1000;      // 30min
    }
}

// ---- LoRa 物理层（§14.5 取代 config.lora.hop_limit / channels 主信道） ----
struct LoraConfig {
    uint8_t  hopLimit = 7;            // 取代原版 config.lora.hop_limit
    uint8_t  channel  = 0;            // 取代 channels.getPrimaryIndex() 主信道
    uint8_t  region   = 0;            // 0=CN_470_IX，详见 platform 区域表
    // 以下两字段为「自研栈 RF 意图提示」：EnvProfile 现已改为输出对 region 合法的
    // modem_preset 并经原版 RadioInterface::reconfigure() 下发，故这两个字段当前**未被写入**，
    // 仅保留作未来「自研射频层」的参考值（0=auto）。请勿把它们当 SF/BW 的直写落点。
    uint8_t  spreadFactor = 0;        // 0=auto（保留，EnvProfile 不再直写）
    uint32_t airBandwidthHz = 0;      // 0=auto（保留，EnvProfile 不再直写）
};

// ---- MQTT 桥接（§14.5 取代 moduleConfig.mqtt） ----
struct MqttConfig {
    bool   enabled     = false;
    char   broker[128] = {0};
    char   topicPrefix[32] = "meshroc";
};

// ---- 兼容层（§14.3，控制与原版节点互通） ----
struct CompatConfig {
    bool emitNativeTextPort = true;   // 自研文本同时走原版 TEXT_MESSAGE_APP 端口
    bool dualSendChannel0   = true;   // 主信道双发，让原版节点能"听到"自研心跳
};

// ---- §15.3 原创优化开关 ----
struct RfOptimization {
    enum class EnvProfile : uint8_t { FIXED = 0, AUTO = 1 };
    EnvProfile envProfile = EnvProfile::AUTO;  // O1 多地貌射频模板

    struct Tdma {
        bool  enabled  = false;
        uint8_t slotCount = 8;                  // O2 动态分片时隙
    } tdma;

    bool fragEnabled = true;                     // O5 大包分片重组
};

struct CryptoConfig {
    enum class Mode : uint8_t { PSK_CTR = 0, GCM_ECDH = 1 };
    Mode  mode = Mode::GCM_ECDH;                 // O3 AES-256-GCM + ECDH-P256
    bool  pskFallback = true;                    // 与原版 PSK 节点互通
};

struct AckConfig {
    bool priorityLevels = true;                  // O4 分级 ACK（告警>位置>普通）
};

struct PowerConfig {
    bool smartManagement = true;                 // O8 智能电源管理
    bool lowBattRelayOff = true;                 // 低电量撤销中继权限
};

struct OfflineCacheConfig {
    bool  enabled  = false;                      // O7 分布式离线缓存
    uint8_t replicas = 2;                        // 缓存副本数
};

// ---- 顶层配置（MeshROCConfig，§12.4 扩展版） ----
struct MeshROCConfig {
    MeshRocRole       deviceRole = MeshRocRole::CLIENT;

    LoraConfig        lora;
    MqttConfig        mqtt;
    CompatConfig      compat;

    // RAP 归属协议参数（§13.2 常量可调）
    struct {
        bool     enabled          = true;
        uint32_t helloIntervalMs  = 90000;       // RAP_HELLO_PERIOD_MS
        uint32_t attachTimeoutMs  = 10000;
    } rap;

    // GATEWAY 桥接（仅 GATEWAY 角色相关，§12.4）
    struct {
        char upstreamUrl[160] = {0};             // wss://meshroc.cc.cd/bridge
        char authToken[128]   = {0};             // 不回显明文
        bool autoReconnect    = true;
    } gateway;

    // 原创优化开关（§15.3）
    RfOptimization   rf;
    CryptoConfig     crypto;
    AckConfig        ack;
    PowerConfig      power;
    OfflineCacheConfig offlineCache;
};

}  // namespace meshroc::config
