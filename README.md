<div align="center" markdown="1">

<img src="branding/logo.svg" alt="MeshROC Logo" width="80"/>

# MeshROC Firmware

**Mesh Radio-Optimized Communications · 互联之域**

![GitHub release](https://img.shields.io/github/v/release/ye2f4/meshroc-fw)
[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](LICENSE)
[![CI](https://img.shields.io/github/actions/workflow/status/ye2f4/meshroc-fw/main_matrix.yml?branch=develop&label=actions&logo=github&color=yellow)](https://github.com/ye2f4/meshroc-fw/actions)

</div>

<div align="center">
	<a href="https://github.com/ye2f4/meshroc-fw">代码仓库</a>
	·
	<a href="https://github.com/ye2f4/meshroc-fw/wiki">文档与维基</a>
	·
	<a href="https://github.com/ye2f4/meshroc-fw/issues">问题反馈</a>
</div>

## 概述

**MeshROC（Mesh Radio-Optimized Communications，互联之域）** 是一套面向国产硬件与本地化场景优化的开源 LoRa mesh 组网固件。它以**自研协议栈 `src/kernel/` 为第一公民**（协议/RAP/路由/射频出口自主掌控），并复用 [Meshtastic](https://meshtastic.org) 固件作为宿主框架，在**保持与原版 Meshtastic 节点互通**的前提下，针对中国本土频段合规、复杂地形射频与骨干路由做了增强。

固件支持多种硬件平台：ESP32、nRF52、RP2040 / RP2350，以及基于 Linux 的 Portduino 设备。

### 架构立场：自研协议栈是第一公民

MeshROC 并非"在 Meshtastic 上外挂一个模块"，而是**以自研协议栈 `src/kernel/` 为运行主体**：协议格式、RAP 归属、四相混合路由、射频字节出口全部由自研栈自主掌控。Meshtastic 固件在本项目中扮演**宿主框架 / 兼容底座**角色——提供平台适配、屏幕、蓝牙、与上游节点的互通能力，但自研栈的帧经 `RadioLibInterface::startSendRaw()` **直驱射频**，不经 `meshtastic_MeshPacket` 管线。

### 与 Meshtastic 的兼容性

| 维度 | 兼容性 |
| --- | --- |
| 与原版 Meshtastic 节点混网 | ✅ 由宿主框架负责，默认互通 |
| Python CLI / 手机 App | ✅ 兼容（经宿主框架） |
| 信道加密 | ✅ 沿用 |
| 构建体系（PlatformIO / protobufs） | ✅ 沿用 |

### 核心特性

- **自研协议栈（第一公民）**：10 字节大端包头 + TLV + CRC16-MODBUS 尾（`src/kernel/net/`），匹配真实 datapack 空中格式；O1 环境感知跳数、O2 TDMA 时隙、O3 加密钩子、O4 ACK 策略、O5 分片重组。
- **RAP 归属协议**：终端节点归属到骨干节点，由 `src/kernel/net/rap/RapStateMachine` 全量实现。
- **四相混合路由**：PHASE1 洪泛探测 / PHASE2 源路由 / PHASE3 ACK 超时失效回退 / PHASE4 受限洪泛，路由权威完全在自研栈内（`src/kernel/net/Router`）。
- **本土合规**：默认以中国 CN 470MHz 频段、合规发射功率与标准调制预设运行。
- **地形射频模板**：内置全国九类地形到标准 LoRa 参数的映射辅助（`src/mesh/MeshROC.cpp`）。

## 快速开始

- 🔧 **[构建固件](https://docs.meshtastic.org/development/firmware/build)** — 从源码编译（基于上游 PlatformIO 流程，命令一致）。
- ⚡ **刷写固件** — 使用官方刷写工具或本仓库文档所述流程烧录到设备。

## 许可证

固件代码以 **GPL-3.0** 许可证发布（自研栈与集成的上游遵循各自许可）。硬件以开源硬件形式提供，站点与文档以 MIT 许可证发布。

## 社区

- 社区站点：互联之域 MeshROC（https://github.com/ye2f4/meshroc-fw）
- 问题反馈与新功能建议请通过 GitHub Issues。

---

*MeshROC 以自研协议栈（src/kernel/）为第一公民，复用 Meshtastic 作为宿主框架保持节点互通；固件以 GPL-3.0 发布。*
