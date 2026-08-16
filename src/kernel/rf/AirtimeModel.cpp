#include "rf/AirtimeModel.h"

namespace meshroc::rf {

bool AirtimeModel::presetToParams(int presetValue, bool wideLora, LoraParams& out)
{
    // 映射直接复制 src/mesh/MeshRadio.h:205 modemPresetToParams。
    // 整数枚举值与 meshtastic_Config_LoRaConfig_ModemPreset 一致。
    // CN region wideLora=false，走窄带分支；wideLora=true 为 US/AU 等宽带区域。
    float bwKHz = 0;
    uint8_t sf = 0;
    uint8_t cr = 0;
    switch (presetValue) {
        case static_cast<int>(ModemPreset::SHORT_TURBO):
            bwKHz = wideLora ? 1625.0f : 500.0f;  cr = 5; sf = 7;  break;
        case static_cast<int>(ModemPreset::SHORT_FAST):
            bwKHz = wideLora ? 812.5f : 250.0f;  cr = 5; sf = 7;  break;
        case static_cast<int>(ModemPreset::SHORT_SLOW):
            bwKHz = wideLora ? 812.5f : 250.0f;  cr = 5; sf = 8;  break;
        case static_cast<int>(ModemPreset::MEDIUM_FAST):
            bwKHz = wideLora ? 812.5f : 250.0f;  cr = 5; sf = 9;  break;
        case static_cast<int>(ModemPreset::MEDIUM_SLOW):
            bwKHz = wideLora ? 812.5f : 250.0f;  cr = 5; sf = 10; break;
        case static_cast<int>(ModemPreset::MEDIUM_TURBO):
            bwKHz = wideLora ? 1625.0f : 500.0f; cr = 5; sf = 9;  break;
        case static_cast<int>(ModemPreset::LONG_TURBO):
            bwKHz = wideLora ? 1625.0f : 500.0f; cr = 8; sf = 11; break;
        case static_cast<int>(ModemPreset::LONG_MODERATE):
            bwKHz = wideLora ? 406.25f : 125.0f; cr = 8; sf = 11; break;
        case static_cast<int>(ModemPreset::LONG_SLOW):
            bwKHz = wideLora ? 406.25f : 125.0f; cr = 8; sf = 12; break;
        case static_cast<int>(ModemPreset::LITE_FAST):
            bwKHz = 125.0f; cr = 5; sf = 9;  break;
        case static_cast<int>(ModemPreset::LITE_SLOW):
            bwKHz = 125.0f; cr = 5; sf = 10; break;
        case static_cast<int>(ModemPreset::NARROW_FAST):
            bwKHz = 62.5f;  cr = 6; sf = 7;  break;
        case static_cast<int>(ModemPreset::NARROW_SLOW):
            bwKHz = 62.5f;  cr = 6; sf = 8;  break;
        case static_cast<int>(ModemPreset::TINY_FAST):
            bwKHz = 15.6f;  cr = 5; sf = 7;  break;
        case static_cast<int>(ModemPreset::TINY_SLOW):
            bwKHz = 15.6f;  cr = 6; sf = 8;  break;
        case static_cast<int>(ModemPreset::LONG_FAST):
        default:  // LONG_FAST (or illegal)
            bwKHz = wideLora ? 812.5f : 250.0f; cr = 5; sf = 11; break;
    }
    out.sf = sf;
    out.bwHz = static_cast<uint32_t>(bwKHz * 1000.0f);
    out.crDenom = cr;
    return true;  // 全部枚举（含未知）都 fallback 到 LONG_FAST，故恒成功
}

double AirtimeModel::loraOnAirMs(uint16_t payloadBytes, const LoraParams& p, uint16_t preambleSymbols)
{
    if (p.bwHz == 0 || p.sf == 0) return 0.0;
    const double tSym = static_cast<double>(1u << p.sf) / static_cast<double>(p.bwHz);  // seconds
    const double tPreamble = (preambleSymbols + 4.25) * tSym;

    // 复刻原版 getPacketTime 的 LoRa 符号数公式（implicitHeader=false -> ih=0, crc=1, lowDataRateOpt de=0）
    const double de = 0.0;
    const double ih = 0.0;
    const double crc = 1.0;
    const double numerator = 8.0 * payloadBytes - 4.0 * p.sf + 28.0 + 16.0 * crc - 20.0 * ih;
    const double denom = 4.0 * (static_cast<double>(p.sf) - 2.0 * de);
    double payloadSym = 8.0 + std::ceil(numerator / denom) * (p.crDenom + 4.0);
    if (payloadSym < 0.0) payloadSym = 0.0;  // 防御：极小包不应为负

    const double airtimeSec = tPreamble + payloadSym * tSym;
    return airtimeSec * 1000.0;
}

uint32_t AirtimeModel::retransmissionMsec(uint16_t payloadBytes, const LoraParams& p,
                                          uint8_t channelUtilPct, uint32_t slotTimeMsec)
{
    const double airtimeMs = loraOnAirMs(payloadBytes, p);

    // CWsize = map(channelUtil, 0, 100, CWmin, CWmax)
    uint8_t cw = CW_MIN;
    if (channelUtilPct >= 100) {
        cw = CW_MAX;
    } else {
        cw = static_cast<uint8_t>(CW_MIN + (static_cast<uint32_t>(channelUtilPct) * (CW_MAX - CW_MIN)) / 100);
    }

    const uint32_t cwTerm =
        (1u << cw) + (2u * CW_MAX) + (1u << ((CW_MAX + CW_MIN) / 2));

    const uint32_t ret = static_cast<uint32_t>(2.0 * airtimeMs) +
                         cwTerm * slotTimeMsec +
                         PROCESSING_TIME_MSEC;
    return ret;
}

}  // namespace meshroc::rf
