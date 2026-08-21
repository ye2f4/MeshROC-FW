#pragma once

#include "MeshModule.h"
#include "meshroc_brand.h" // MeshROC 品牌常量（系统名/品牌/许可/配色）

/**
 * MeshROCStatusModule：第一公民栈的屏幕状态帧（融合自 MeshRocModule::drawFrame）
 * =============================================================================
 * 这是 8.4 融合的产物：原 MeshRoc 叠加态产物的屏幕 UI 能力，迁到原版 UI 框架
 * （MeshModule 的 wantUIFrame/drawFrame 机制，由 Screen.cpp 自动收集）。
 *
 * 关键约束（用户 2026-08-19 拍板）：
 *   - 本模块**不实现任何协议**（无混合路由/RAP/分片/编码），仅做"只读展示"。
 *   - 数据全部来自第一公民栈（g_meshrocBridge，即 RadioMeshRocBridge）的只读接口：
 *       cfg()           → 本机角色(config::MeshRocRole) + 兼容开关
 *       meshRocPeerCount(nowMs) → 已确认 MeshROC 对端数
 *       rapState()/attachedBackbone() → 归属状态（后续可加）
 *   - 不依赖 service->sendToMesh / nodeDB / channels / MeshRocModule 任何残留。
 */
class MeshROCStatusModule : public MeshModule {
public:
    MeshROCStatusModule();

#if HAS_SCREEN
    virtual bool wantUIFrame() override { return true; }
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif
    // 纯只读 UI 展示，不处理任何 Mesh 包 → 恒 false（不接收）
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;

private:
    // 把 config::MeshRocRole 映射到官网产品线名（覆盖 DTU/SENSOR/TRACKER，旧 3 值版已失效）
    static const char *productName(uint8_t role);
};
