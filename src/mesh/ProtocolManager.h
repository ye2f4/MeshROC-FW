#pragma once
#include <cstdint>
#include "kernel/config/MeshROCConfig.h"

// 前向声明：自研 MeshRoc 协议栈（固件主体 / 第一公民）
namespace meshroc {
class MeshRocStack;
}

/**
 * ProtocolManager：多协议共存调度器（MESHROC 固件主体）
 *
 * 重要定位（用户 2026-08-17 更正）：
 *   MESHROC 固件（src/kernel/ 自研栈）才是主体。Meshtastic 与 MeshCore 都是「已知第三方固件」，
 *   MESHROC 固件兼容它们的方式 = 侦听其信道、解析其帧、桥接消息，而非把它们的全栈内置运行。
 *   - E:\firmware   = Meshtastic 原版固件（独立仓库，参考用）
 *   - E:\MeshCore   = MeshCore   原版固件（独立仓库，参考用）
 *   - E:\meshroc-fw = MESHROC   自研固件（本仓库，主体）
 *   若删除 meshroc-fw 中 MESHROC 相关文件，它就退化回独立 Meshtastic 固件副本。
 *
 * 因此本管理器的职责是：
 *   1) 驱动自研 MeshRocStack（原生、第一公民）；
 *   2) 挂载两个「兼容桥接器」Bridge —— 它们在独立信道上侦听第三方帧并桥接进 MESHROC 网络，
 *      不把第三方全栈搬进本固件（避免造车轮、保持轻量）。
 *
 * 关键约束：
 *   - 射频层（SX126xInterface）只保留一份，三家协议都向它借收发；
 *     第三方帧经桥接器收到后转交 MESHROC 原生栈统一路由。
 *   - kernel/ 内部仍保持零依赖原版头文件的铁律；本管理器位于 src/mesh/ 层，允许依赖两侧。
 *   - 不照搬 Meshtastic(protobuf) / MeshCore(自有) 的线格式进原生栈，仅做「解析→语义桥接」。
 */
namespace meshroc {

enum class ProtocolId : uint8_t {
    MESHROC    = 0,  ///< 自研原生栈（第一公民）
    MESHTASTIC = 1,  ///< 第三方兼容：侦听信道 + 解析 protobuf + 桥接
    MESHCORE   = 2,  ///< 第三方兼容：侦听信道 + 解析其帧 + 桥接
    COUNT      = 3,
};

class ProtocolManager {
public:
    /**
     * @param cfg      MeshROC 反转配置（§11~§15）
     * @param myAddr   本节点在 MeshRoc 栈中的 16-bit 地址
     * @param isBackbone 是否骨干中继（影响 RAP/转发）
     */
    ProtocolManager(const config::MeshROCConfig& cfg,
                    uint16_t myAddr,
                    bool isBackbone = false);
    ~ProtocolManager();

    // 周期性驱动：推进自研栈 tick（RAP/O1环境采样/O2 TDMA/O4重传/O5重组超时）
    void tick(uint32_t nowMs);

    // 第三方桥接器挂载点（未来实现 MeshtasticBridge / MeshCoreBridge 后填充）
    void attachMeshtasticBridge(void* bridge) { meshtasticBridge_ = bridge; }
    void attachMeshCoreBridge(    void* bridge) { meshcoreBridge_   = bridge; }

    // 状态查询
    bool meshtasticCompatible() const { return meshtasticBridge_ != nullptr; }
    bool meshcoreCompatible()    const { return meshcoreBridge_   != nullptr; }

    // 取原生栈（供应用层/模块调用 sendPayload / onPacket 派生）
    MeshRocStack* meshrocStack() { return stack_; }

private:
    const config::MeshROCConfig& cfg_;
    MeshRocStack*       stack_;          ///< 自研原生栈（本管理器内 new/delete）

    // 第三方兼容桥接器（未来实现后填充；当前为 nullptr，不内置第三方全栈）
    void* meshtasticBridge_ = nullptr;   ///< MeshtasticBridge*（解析 protobuf → 桥接）
    void* meshcoreBridge_   = nullptr;   ///< MeshCoreBridge*（解析其帧 → 桥接）
};

}  // namespace meshroc
