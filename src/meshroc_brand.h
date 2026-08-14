#pragma once

/**
 * MeshROC 品牌常量（集中对齐 E:/MESHROC 官网资料）
 * =====================================================
 * 两层含义（务必区分，源自建站要求 + siteData.json）：
 *   - 系统名 System  = "Mesh Radio-Optimized Communications"（固件/硬件，本仓库产物）
 *   - 社区名 Community = "Mesh Realm Of Connection"（互联之域，官网/社区）
 * 两者不是一个概念；固件属于"系统"，对外展示应区分。
 *
 * 许可（GitHub Pages 审核硬性要求，避免误导）：
 *   - 固件（本仓库）：GPL-3.0（源自 Meshtastic，二者不兼容 MIT，勿标 MIT）
 *   - 硬件：开源硬件（立创开源/星火计划）
 *   - 文档与官网：MIT
 * 详见 docs/meshroc-app-dev-kit.md。
 *
 * 视觉规范（建站要求 §7）：
 *   主色 深空蓝 #0F1C2D / 辅色 科技青 #22D1C8 / 点缀 信号橙 #FF8C29
 */

// ---- 名称 ----
static constexpr const char *MESHROC_BRAND_SHORT = "MeshROC";
static constexpr const char *MESHROC_SYSTEM_NAME = "Mesh Radio-Optimized Communications";
static constexpr const char *MESHROC_COMMUNITY_NAME = "Mesh Realm Of Connection";
static constexpr const char *MESHROC_COMMUNITY_NAME_CN = "互联之域";

// ---- 对外链接（永久绑定，勿 404）----
static constexpr const char *MESHROC_SITE_URL = "https://meshroc.cc.cd";
static constexpr const char *MESHROC_GITHUB_URL = "https://github.com/ye2f4/MeshROC";

// ---- 许可 ----
static constexpr const char *MESHROC_FW_LICENSE = "GPL-3.0"; // 固件（本仓库）许可，非 MIT
static constexpr const char *MESHROC_HW_LICENSE = "Open Hardware (立创开源/星火计划)";
static constexpr const char *MESHROC_DOC_LICENSE = "MIT";     // 文档/官网许可

// ---- 品牌色（RGB565，供屏幕绘制）----
static constexpr uint16_t MESHROC_COLOR_DEEPBLUE = 0x08E5; // #0F1C2D 深空蓝（主色）
static constexpr uint16_t MESHROC_COLOR_CYAN     = 0x8699; // #22D1C8 科技青（辅色）
static constexpr uint16_t MESHROC_COLOR_ORANGE   = 0xFC65; // #FF8C29 信号橙（点缀）

// ---- 产品线（建站要求 §3，对应 MeshRocRole 语义映射）----
//   BACKBONE(0) → MeshROC Backbone 山顶太阳能骨干节点
//   CLIENT(1)   → MeshROC Walk 手持终端（主力）
//   GATEWAY(2)  → MeshROC Gateway POE 城市基站
//   （Sensor 低功耗传感终端为独立 SKU，不在路由角色内）
static constexpr const char *meshRocProductName(int role)
{
    switch (role) {
    case 0: return "Backbone"; // 山顶太阳能骨干中继
    case 2: return "Gateway";  // POE 城市基站
    case 1:
    default: return "Walk";    // 手持终端 / 通用
    }
}
