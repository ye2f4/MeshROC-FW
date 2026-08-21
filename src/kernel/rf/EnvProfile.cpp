#include "kernel/rf/EnvProfile.h"

namespace meshroc::rf {

// 地形 → 期望 ModemPreset（仅表达设计意图，未经 region 校验）。
// 取值语义对齐原版 meshtastic_Config_LoRaConfig_ModemPreset：
//   LONG_SLOW     : 最大链路预算（最远距离）
//   LONG_FAST     : 距离/速率折中（原版默认，最通用）
//   LONG_MODERATE : 中距离
//   SHORT_FAST    : 较短距离、较高速率
//   SHORT_TURBO   : 短距离、最高速率（宽 BW，CN region 合法但需 region 允许）
// 仅当调用方用 region 预设表校验通过后才采用，否则沿用当前配置。
meshtastic_Config_LoRaConfig_ModemPreset EnvProfile::intendedPreset(EnvProfile::Terrain t)
{
    switch (t) {
    case Terrain::GOBI:            return meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW;
    case Terrain::MOUNTAIN_FOREST: return meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW;
    case Terrain::VALLEY:          return meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE;
    case Terrain::PLATEAU:         return meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    case Terrain::URBAN:           return meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    case Terrain::COASTAL:         return meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO;
    default:                       return meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    }
}

EnvProfile::Terrain EnvProfile::classify(const LinkSample& s) const
{
    // FIXED 模式：不做自适应，保持中性城镇预设
    if (cfg_.rf.envProfile == config::RfOptimization::EnvProfile::FIXED) {
        return Terrain::URBAN;
    }

    // 丢包率（ppm 千分比，0..1000）
    uint32_t lossPPM = (s.rxTot > 0) ? (1000 * (s.rxTot - s.rxOk) / s.rxTot) : 0;

    // 阈值决策：优先看丢包（最直接影响可达性），其次 SNR，再次噪声底。
    if (lossPPM >= 200 || s.snrDb <= 2) {
        // 高损耗/极低 SNR：最大化距离 → 戈壁式最高链路预算
        return Terrain::GOBI;
    }
    if (s.snrDb <= 6) {
        // 弱信号但有吞吐：高山密林（高 SF）
        return Terrain::MOUNTAIN_FOREST;
    }
    if (s.noiseFloorDbm >= -95) {
        // 噪声高（城镇/密集干扰）：稳健通用预设
        return Terrain::URBAN;
    }
    if (s.snrDb >= 18 && s.rssiDbm >= -90) {
        // 极好链路（视距沿海）：高速率
        return Terrain::COASTAL;
    }
    if (s.rssiDbm <= -110) {
        // 远距离弱收：河谷中距离
        return Terrain::VALLEY;
    }
    // 默认：高原中等
    return Terrain::PLATEAU;
}

bool EnvProfile::presetFor(Terrain t, meshtastic_Config_LoRaConfig_ModemPreset& out) const
{
    meshtastic_Config_LoRaConfig_ModemPreset p = intendedPreset(t);
    // region 合法性校验交给调用方（参考原版 supportsPreset / checkOrClampConfigLora）。
    // 这里仅保证：在 FIXED 模式下不产出变更意图，沿用当前配置交由调用方决定。
    if (cfg_.rf.envProfile == config::RfOptimization::EnvProfile::FIXED) {
        return false; // 固定模式：不建议切换
    }
    out = p;
    return true;
}

void EnvProfile::feedSample(const LinkSample& s)
{
    cur_ = classify(s);
    meshtastic_Config_LoRaConfig_ModemPreset p = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    hasIntent_ = presetFor(cur_, p);
    if (hasIntent_) {
        intendedPreset_ = p;
    }
}

}  // namespace meshroc::rf
