// Kernel 隔离冒烟测试驱动（仅编译验证用，不烧录/不链接固件）。
// 作用：实例化 src/kernel/ 下所有自研模块，触发全部 .cpp 编译，
// 快速发现 kernel 层（net:: 限定、嵌套枚举返回类型、宏污染等）的编译错误，
// 而不必编译整个 meshtastic 固件（200+ 源文件、Arduino 宏链、RadioLib 等）。
//
// 本文件不进真机固件：platformio.ini 的 [env:kernel-smoke] 用 build_src_filter
// 只收 src/kernel/ + 本文件，且 default_envs 不含它，正常 heltec-v3 构建不受影响。

#include "kernel/MeshRocStack.h"
#include "kernel/config/MeshROCConfig.h"
#include "kernel/net/Router.h"
#include "kernel/net/AckPolicy.h"
#include "kernel/net/Reassembler.h"
#include "kernel/net/rap/RapStateMachine.h"
#include "kernel/net/rap/LinkQuality.h"
#include "kernel/rf/EnvProfile.h"
#include "kernel/rf/AirtimeModel.h"
#include "kernel/net/HopPlanner.h"
#include "kernel/net/CryptoHook.h"
#include "kernel/net/MeshRocCodec.h"

// 全局配置实例（与 MeshROCConfig.cpp 中定义一致）。
meshroc::config::MeshROCConfig gMeshRocConfig;

// 最小 RadioMeshRocBridge 桩：仅实例化以触发 MeshRocStack/Router 等编译。
// 不实现射频，sendRaw 留空即可（本 env 不链接、不运行）。
class SmokeBridge : public meshroc::MeshRocStack {
public:
    SmokeBridge()
        : meshroc::MeshRocStack(gMeshRocConfig, 0x1234, true) {}

    void sendRaw(const uint8_t* /*buf*/, uint16_t /*len*/) override {
        // 冒烟测试不发真实射频。
    }
    uint8_t channelUtilization() const override { return 0; }
    uint32_t slotTimeMsec() const override { return 0; }
};

SmokeBridge g_bridge;

// 让链接器保留这些符号，避免被优化掉（纯编译验证用，不影响逻辑）。
volatile unsigned g_keep = 0;
void kernel_smoke_keep() {
    g_keep = (unsigned)(uintptr_t)&g_bridge;
}

#ifndef ARDUINO
// 非 Arduino 环境下提供最小桩，保证能编过（本 env 实际是 arduino 框架，
// 此分支仅为防御性，正常不会进入）。
int main() { kernel_smoke_keep(); return 0; }
#else
// Arduino 框架要求全局 setup()/loop() 入口；冒烟测试不运行、只验证链接，留空。
void setup() {
    kernel_smoke_keep();
    g_bridge.tick(0);  // 触发一次引擎 tick，确保 g_bridge 符号被引用
}
void loop() {}
#endif
