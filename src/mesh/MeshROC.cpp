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

// 固件全局配置对象（与 RadioInterface / NodeDB 中定义一致）。
extern struct meshtastic_Config config;

void MeshROC_applyEnvTemplate(int id)
{
    // 统一国内 CN 频段（470MHz）——MeshROC 的核心合规约束。
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_CN;

    switch (id) {
        case 0: // 华北山地
            config.lora.use_preset = true;
            config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW;
            config.lora.hop_limit = 5;
            config.lora.tx_power = 30;
            break;
        case 1: // 东北林区
        case 2: // 南方多雨山林
        case 3: // 东南沿海丘陵
        case 6: // 盆地河谷
            config.lora.use_preset = true;
            config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
            config.lora.hop_limit = 4;
            config.lora.tx_power = 27;
            break;
        case 4: // 西北荒漠戈壁
            config.lora.use_preset = true;
            config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW;
            config.lora.hop_limit = 7;
            config.lora.tx_power = 30;
            break;
        case 5: // 青藏高原
            config.lora.use_preset = true;
            config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW;
            config.lora.hop_limit = 5;
            config.lora.tx_power = 30;
            break;
        case 7: // 城中村高楼遮挡
        case 8: // 工业区电磁复杂
            config.lora.use_preset = true;
            config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST;
            config.lora.hop_limit = 3;
            config.lora.tx_power = 20;
            break;
        default:
            break;
    }

    // TODO(firmware): 在此调用固件配置持久化 + 射频重配置 API（如 service->reloadConfig()），
    // 使本次修改立即生效并写入 Flash。未编译验证前暂不自动调用。
}
