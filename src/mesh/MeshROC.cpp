/**
 * MeshROC（Mesh Radio-Optimized Communications）固件适配辅助实现。
 *
 * 设计：与官网「面向全国地形气候」九类环境一一对应的射频模板，
 * 统一国内 CN 频段（470MHz），并套用对应 modem 预设 / 跳数 / 功率。
 * 这些值只用到固件标准 LoRaConfig 字段，与上游 Meshtastic 完全互通。
 *
 * 安全说明：本函数仅写入 config.lora.* 全局，未调用任何未经验证的
 * 重配置 / 持久化 API，且当前未被自动调用，因此不会破坏既有构建与运行行为。
 * 接入方式（待固件维护者编译验证后）：
 *   1) 在合适时机（如启动读取已存偏好、或新增串口/Web 命令）调用 MeshROC_applyEnvTemplate(id)；
 *   2) 调用后触发固件配置持久化 + 射频重配置（如 service->reloadConfig()）。
 */

#include "MeshROC.h"
#include "kernel/rf/EnvProfile.h"  // 统一地形->预设定义，消除双表漂移
#include "mesh/generated/meshtastic/localonly.pb.h"  // meshtastic_LocalConfig（config 全局的真实类型）

// 固件全局配置对象（与 NodeDB.cpp:84 定义一致：meshtastic_LocalConfig config）。
// 注意：meshtastic_Config 是 typedef 名（非 struct 标签），且全局 config 的类型是
// meshtastic_LocalConfig（含 lora 等字段），不能用 `struct meshtastic_Config config;`。
extern meshtastic_LocalConfig config;

// 地理区域 id -> 自研地形枚举（与官网九类环境一一对应）。
// 预设选择统一交由 EnvProfile::presetForTerrain 处理（单一真相），本函数只做地理->地形桥接。
static meshroc::rf::EnvProfile::Terrain geoIdToTerrain(int id)
{
    using T = meshroc::rf::EnvProfile::Terrain;
    switch (id) {
        case 0: // 华北山地
        case 1: // 东北林区
        case 2: // 南方多雨山林
            return T::MOUNTAIN_FOREST;
        case 3: // 东南沿海丘陵（视距好、可高速率）
            return T::COASTAL;
        case 4: // 西北荒漠戈壁（最大距离）
            return T::GOBI;
        case 5: // 青藏高原
            return T::PLATEAU;
        case 6: // 盆地河谷
            return T::VALLEY;
        case 7: // 城中村高楼遮挡
        case 8: // 工业区电磁复杂
            return T::URBAN;
        default:
            return T::URBAN;
    }
}

void MeshROC_applyEnvTemplate(int id)
{
    // 统一国内 CN 频段（470MHz）——MeshROC 的核心合规约束。
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_CN;

    // 地形 -> ModemPreset 由 EnvProfile 单一决定（不在此另立预设表）
    meshtastic_Config_LoRaConfig_ModemPreset preset =
        meshroc::rf::EnvProfile::presetForTerrain(geoIdToTerrain(id));
    config.lora.use_preset = true;
    config.lora.modem_preset = preset;

    // 跳数 / 功率仍按地理场景微调（这些是原版 LoRaConfig 字段，不在 EnvProfile 职责内）。
    // 注：tx_power 最终由 region powerLimit + 板级 SX126X_MAX_POWER 两层 clamp 兜底，
    // 此处仅作"优先请求值"，不直接写芯片（符合射频铁律）。
    switch (id) {
        case 0: case 1: case 2: // 山地/林区：远距、多跳
            config.lora.hop_limit = 5; config.lora.tx_power = 30; break;
        case 3: case 6:          // 沿海/河谷：中距
            config.lora.hop_limit = 4; config.lora.tx_power = 27; break;
        case 4:                   // 戈壁：最远
            config.lora.hop_limit = 7; config.lora.tx_power = 30; break;
        case 5:                   // 高原
            config.lora.hop_limit = 5; config.lora.tx_power = 30; break;
        case 7: case 8:          // 城区/工业区：近距、低功率
            config.lora.hop_limit = 3; config.lora.tx_power = 20; break;
        default:
            break;
    }

    // TODO(firmware): 在此调用固件配置持久化 + 射频重配置 API（如 service->reloadConfig()），
    // 使本次修改立即生效并写入 Flash。未编译验证前暂不自动调用。
}
