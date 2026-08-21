#pragma once
#include <cstdint>
#include <cstddef>
#include "kernel/config/MeshROCConfig.h"
#include "mesh/generated/meshtastic/config.pb.h"

/**
 * EnvProfile：多地貌射频模板引擎（原创优化 O1，对应网站承诺 #6）
 *
 * 设计原则（关键改动）：本类**绝不**直接写裸 SF/BW/功率到芯片或 LoraConfig 标量字段。
 * 原版 Meshtastic 的射频配置有唯一正确入口：
 *     config.lora.modem_preset ──► RadioInterface::reconfigure()
 *                                  └─ applyModemConfig()  （按 region 做 supportsPreset / powerLimit clamp）
 *                                     └─ RadioLibInterface::reconfigure()  （芯片差异由 RadioLib 抹平）
 * 因此 EnvProfile 只负责「地形分类 → 选出一个对当前 region 合法的 ModemPreset」，
 * 由调用方把该预设写入 config.lora.modem_preset 并调用 radio->reconfigure() 完成下发。
 * SF/BW/功率的具体数值、region 合法性、芯片寄存器映射全部交给原版管线，
 * 本类不重复实现，也不假设任何"写 dBm 直接生效"的抽象层。
 *
 * 预设集约束：地形 → 预设 的映射只从当前 region 的合法预设表（如 CN 的 PRESETS_STD）
 * 中选取，绝不产出 region 不支持的预设（例如 CN 下 SHORT_TURBO/MEDIUM_TURBO 是合法的，
 * 但本类只在整个 region 允许的前提下才选它们）。
 *
 * 支持芯片：SX1268 / LLCC68 / SX1262（全部走原版 SX126xInterface + RadioLib SX126x 驱动）。
 * 这三颗的 SF/BW/频率由 RadioLib 统一封装，功率差异（板级 SX126X_MAX_POWER：22/8/29dBm 等）
 * 与 region 上限（CN=19dBm）已由原版 reconfigure() 内的 powerLimit + SX126X_MAX_POWER 两层 clamp 处理，
 * 本类**无需也不得**自行写功率——否则会绕过 clamp，导致超法规或烧毁 PA。
 * 注：LLCC68 是 SX126x 减配版（无 SF5/6，功率上限更紧），但本类只选 Meshtastic 预设（SF7-12，
 * 无 SF5/6），且功率由上述 clamp 兜底，故在 LLCC68 上同样安全。
 *
 * 参考契约 §15.2-O1 / §15.3 rf.envProfile，及对原版 RadioInterface/SX126xInterface 的借鉴。
 */
namespace meshroc::rf {

// 链路采样（由 PHY 层周期性回填，来源即原版 RadioInterface 的 RSSI/SNR 统计）
struct LinkSample {
    int8_t  snrDb;          // 接收 SNR
    int16_t rssiDbm;        // 接收 RSSI
    int16_t noiseFloorDbm;  // 噪声底估计
    uint32_t rxOk;          // 窗口内成功收包
    uint32_t rxTot;         // 窗口内应收包（含丢失）
};

class EnvProfile {
public:
    explicit EnvProfile(const config::MeshROCConfig& cfg) : cfg_(cfg) {}

    // 地貌枚举（用于分类，不直接绑定任何 SF/BW 数值）
    enum class Terrain : uint8_t {
        MOUNTAIN_FOREST = 0,  // 高山密林（需最高链路预算）
        VALLEY          = 1,  // 河谷（中距离）
        COASTAL         = 2,  // 沿海（视距好、可高速率）
        GOBI            = 3,  // 戈壁（最大距离）
        PLATEAU         = 4,  // 高原（中等）
        URBAN           = 5,  // 城镇（高干扰，需稳健）
        COUNT           = 6,
    };

    // 依据最近一次采样做地形分类，返回地形。
    // 当 config.rf.envProfile == FIXED 时，返回 URBAN（中性预设）不自适应。
    Terrain classify(const LinkSample& s) const;

    // 将地形映射为「对当前 region 合法」的 ModemPreset。
    // 调用方需自行用 RadioInterface::checkOrClampConfigLora / region 预设表校验后再下发，
    // 本函数只保证：在 regionSupports(preset) 为真的前提下输出（不凭空造预设）。
    // 返回 false 表示该地形在当前 region 无可用预设（应沿用当前配置，不切）。
    bool presetFor(Terrain t, meshtastic_Config_LoRaConfig_ModemPreset& out) const;

    // 纯地形 -> 预设意图（不经 FIXED 模式判断、不做 region 校验），供外部地理映射壳复用，
    // 使"地形 -> 预设"定义收口到本类一处（避免 MeshROC.cpp 地理模板另立预设表漂移）。
    static meshtastic_Config_LoRaConfig_ModemPreset presetForTerrain(Terrain t)
    {
        return intendedPreset(t);
    }

    Terrain current() const { return cur_; }

    // ---- 预设意图暂存（供引擎/MeshRocStack 周期性回填后读取）----
    // 引擎在 tick 中周期喂入链路采样，本类据此更新当前地形并缓存"期望预设意图"。
    // 射频层（后续接入）读取 hasPresetIntent()/intendedPreset() 决定是否下调 reconfigure()。
    void feedSample(const LinkSample& s);
    // 自上次 feedSample 以来是否产生了"需要切换预设"的意图（FIXED 模式恒为 false）。
    bool hasPresetIntent() const { return hasIntent_; }
    // 期望切换到的 ModemPreset（仅当 hasPresetIntent() 为真时有意义）。
    meshtastic_Config_LoRaConfig_ModemPreset intendedPreset() const { return intendedPreset_; }

private:
    // 给定 region，返回该地形应选的预设（未做 region 合法性校验，仅表达设计意图）。
    static meshtastic_Config_LoRaConfig_ModemPreset intendedPreset(Terrain t);

    const config::MeshROCConfig& cfg_;
    Terrain cur_ = Terrain::URBAN;
    bool     hasIntent_ = false;
    meshtastic_Config_LoRaConfig_ModemPreset intendedPreset_ =
        meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
};

}  // namespace meshroc::rf
