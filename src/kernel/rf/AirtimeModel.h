#pragma once
#include <cstdint>
#include <cmath>

/**
 * AirtimeModel：LoRa 空中时间与重传基准超时（原创优化 O4 的基准来源）
 *
 * 设计要点（关键）：本模块**刻意零依赖**——不 include 任何原版 meshtastic / RadioLib 头，
 * 也不引用 meshtastic_MeshPacket。原因是本自研栈（kernel/）在「先不碰射频」阶段
 * 必须与原版栈解耦；同时 O4 AckPolicy 需要"与原版 getRetransmissionMsec 同构"的重传基准，
 * 以便后续接入原版射频/重传时行为完全对齐（不另立超时死表，符合用户铁律）。
 *
 * 公式严格复刻原版 RadioInterface::getRetransmissionMsec / getPacketTime（见 src/mesh/RadioInterface.cpp:766
 * 与 src/platform/portduino/SimRadio.cpp:407），常量取自 RadioInterface.h：
 *   - PROCESSING_TIME_MSEC = 4500
 *   - CWmin = 3, CWmax = 8 （信道利用率 -> 竞争窗口尺寸映射上下限）
 *   - preambleLength = 16 （Meshtastic 实际用 16，非 8）
 *   - slotTimeMsec 由引擎从当前 radio 配置传入（默认 13，与原版一致）
 *
 * 预设 -> (SF, BW, CR) 映射直接复制原版 modemPresetToParams（src/mesh/MeshRadio.h:205），
 * 整数枚举值与 meshtastic_Config_LoRaConfig_ModemPreset 完全一致，保证选定的 ModemPreset
 * 在两边都对应同一组物理参数。
 *
 * 参考契约 §15.2-O4 / §15.3 ack.priorityLevels，及对原版 RadioInterface 的借鉴。
 */
namespace meshroc::rf {

// 与原版 meshtastic_Config_LoRaConfig_ModemPreset 整数枚举一一对应（仅取本栈可能用到的预设）。
enum class ModemPreset : int {
    LONG_FAST     = 0,
    LONG_SLOW     = 1,
    LONG_MODERATE = 2,
    MEDIUM_SLOW   = 3,
    MEDIUM_FAST   = 4,
    SHORT_SLOW    = 5,
    SHORT_FAST    = 6,
    SHORT_TURBO   = 8,
    LONG_TURBO    = 9,
    MEDIUM_TURBO  = 16,
    // 以下供完整映射使用（与 protobuf 枚举一致）
    LITE_FAST     = 7,
    LITE_SLOW     = 10,
    NARROW_FAST   = 11,
    NARROW_SLOW   = 12,
    TINY_FAST     = 13,
    TINY_SLOW     = 14,
};

// 一组物理层参数（与原版 modemPresetToParams 输出同构）
struct LoraParams {
    uint8_t  sf;       // spreading factor, 5..12
    uint32_t bwHz;     // bandwidth in Hz
    uint8_t  crDenom;  // coding rate denominator (4/CR), CR=5 -> 4/5
};

class AirtimeModel {
public:
    static constexpr uint8_t  CW_MIN = 3;        // 来自 RadioInterface.h
    static constexpr uint8_t  CW_MAX = 8;
    static constexpr uint32_t PROCESSING_TIME_MSEC = 4500;
    static constexpr uint16_t PREAMBLE_SYMBOLS = 16;
    static constexpr uint32_t DEFAULT_SLOT_TIME_MSEC = 13;

    // 预设枚举值 -> (sf, bwHz, crDenom)。
    // wideLora 对应原版 RegionInfo::wideLora（CN 为 false）。
    // 返回 false 表示枚举未识别（调用方应沿用当前配置不切换）。
    // 映射直接复制 src/mesh/MeshRadio.h:205 modemPresetToParams，整数枚举值一致。
    static bool presetToParams(int presetValue, bool wideLora, LoraParams& out);

    // 便捷：直接传 ModemPreset 枚举。
    static bool presetToParams(ModemPreset preset, bool wideLora, LoraParams& out)
    {
        return presetToParams(static_cast<int>(preset), wideLora, out);
    }

    // 纯 LoRa on-air 时间（毫秒，double），含 preAmble。
    // payloadBytes = 整个帧在空中传输的总字节数（含本栈 10 字节包头 + TLV + 载荷）。
    // 复刻原版 getPacketTime：tSym=(1<<sf)/bw；tPreamble=(preamble+4.25)*tSym；
    // payloadSym = 8 + max(ceil((8*pl - 4*sf + 28 + 16*crc - 20*ih)/(4*(sf-2*de)))*(cr+4), 0)。
    // 本栈固定 implicitHeader=false(ih=0), crc=1, de=0。
    static double loraOnAirMs(uint16_t payloadBytes, const LoraParams& p,
                              uint16_t preambleSymbols = PREAMBLE_SYMBOLS);

    // 复刻原版 RadioInterface::getRetransmissionMsec 公式：
    //   2*airtime + (2^CWsize + 2*CWmax + 2^((CWmax+CWmin)/2)) * slotTimeMsec + PROCESSING_TIME_MSEC
    // 其中 CWsize = map(channelUtilPct, 0, 100, CWmin, CWmax)。
    // 这是 O4 AckPolicy 的"基准超时"来源（被 AckPolicy 的 priority 倍率微调）。
    static uint32_t retransmissionMsec(uint16_t payloadBytes, const LoraParams& p,
                                       uint8_t channelUtilPct,
                                       uint32_t slotTimeMsec = DEFAULT_SLOT_TIME_MSEC);
};

}  // namespace meshroc::rf
