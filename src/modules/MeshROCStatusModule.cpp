#include "MeshROCStatusModule.h"

// kernel 头定义 MeshRocPriority::LOW/HIGH，而 Arduino 框架（esp32-hal-gpio.h，经 MeshModule.h
// 链引入）把 LOW/HIGH 定义为 0x0/0x1 宏，会污染 kernel 枚举解析。必须先取消这两个宏再 include kernel。
#ifdef LOW
#undef LOW
#endif
#ifdef HIGH
#undef HIGH
#endif

#include "kernel/MeshRocStack.h"      // meshroc::MeshRocStack, g_meshrocBridge
#include "kernel/config/MeshROCConfig.h"     // config::MeshRocRole
#include "mesh/MeshROCBridge.h"       // meshroc::RadioMeshRocBridge（g_meshrocBridge 的类型，必须可见）
#include "configuration.h"            // HAS_SCREEN
#include "graphics/ScreenFonts.h"     // FONT_SMALL / FONT_HEIGHT_SMALL（定义在此）
#include <cstdio>
#include <cstring>

// 第一公民栈全局实例（main.cpp 中 MESHROC_NATIVE_STACK 块实例化，默认可能为空）
extern meshroc::RadioMeshRocBridge *g_meshrocBridge;

MeshROCStatusModule::MeshROCStatusModule()
    : MeshModule("meshrocStatus", meshtastic_PortNum_UNKNOWN_APP)
{
}

// 纯只读 UI 展示模块，不处理任何 Mesh 包。
bool MeshROCStatusModule::wantPacket(const meshtastic_MeshPacket *p)
{
    (void)p;
    return false;
}

const char *MeshROCStatusModule::productName(uint8_t role)
{
    using meshroc::config::MeshRocRole;
    switch (static_cast<MeshRocRole>(role)) {
    case MeshRocRole::BACKBONE: return "Backbone"; // 山顶太阳能骨干中继
    case MeshRocRole::GATEWAY:   return "Gateway";  // POE 城市基站
    case MeshRocRole::SENSOR:    return "Sensor";   // 低功耗传感终端
    case MeshRocRole::TRACKER:   return "Tracker";  // 追踪终端
    case MeshRocRole::DTU:       return "DTU";      // 密集业务终端
    case MeshRocRole::CLIENT:
    default:                     return "Walk";     // 手持终端 / 通用
    }
}

#if HAS_SCREEN
void MeshROCStatusModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    (void)state;

    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    // 标题行：品牌短名
    display->drawString(x, y, MESHROC_BRAND_SHORT);

    int16_t ly = y + FONT_HEIGHT_SMALL;
    char buf[48];

    // 系统名（固件/硬件层，区别于社区名 Mesh Realm Of Connection）
    snprintf(buf, sizeof(buf), "%s", MESHROC_SYSTEM_NAME);
    display->drawString(x, ly, buf);
    ly += FONT_HEIGHT_SMALL;

    // 固件版本 + 许可（明确 GPL-3.0，避免被误认为 MIT）
    snprintf(buf, sizeof(buf), "FW %s | %s", optstr(APP_VERSION_SHORT), MESHROC_FW_LICENSE);
    display->drawString(x, ly, buf);
    ly += FONT_HEIGHT_SMALL;

    // 本机产品线角色（来自第一公民栈 config::MeshRocRole）
    uint8_t role = 1; // 默认 CLIENT
    bool compat = false;
    uint8_t peers = 0;
    if (g_meshrocBridge) {
        role = static_cast<uint8_t>(g_meshrocBridge->cfg().deviceRole);
        compat = g_meshrocBridge->cfg().compat.emitNativeTextPort;
        peers = g_meshrocBridge->meshRocPeerCount(/*nowMs=*/millis());
    }
    snprintf(buf, sizeof(buf), "Product: MeshROC %s", productName(role));
    display->drawString(x, ly, buf);
    ly += FONT_HEIGHT_SMALL;

    // 已确认 MeshROC 对端数（链路质量表，能力协商结果）
    snprintf(buf, sizeof(buf), "MeshROC peers: %u", peers);
    display->drawString(x, ly, buf);
    ly += FONT_HEIGHT_SMALL;

    // 原生兼容通道状态
    snprintf(buf, sizeof(buf), "Compat: %s", compat ? "ON" : "OFF");
    display->drawString(x, ly, buf);
}
#endif // HAS_SCREEN
