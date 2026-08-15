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

**MeshROC（Mesh Radio-Optimized Communications，互联之域）** 是一套面向国产硬件与本地化场景优化的开源 LoRa  mesh 组网固件。它基于 [Meshtastic](https://meshtastic.org) 固件深度增强，在**完整保留与上游 Meshtastic 互联互通能力**的前提下，针对中国本土频段合规、复杂地形射频与骨干路由做了增强。

固件支持多种硬件平台：ESP32、nRF52、RP2040 / RP2350，以及基于 Linux 的 Portduino 设备。

### 与 Meshtastic 的兼容性

| 维度 | 兼容性 |
| --- | --- |
| 空中协议（MeshPacket / protobuf） | ✅ 100% 互通 |
| 与普通 Meshtastic 节点混网 | ✅ 默认互通（文本走原生端口） |
| Python CLI / 手机 App | ✅ 兼容 |
| 信道加密 | ✅ 不变 |
| 构建体系（PlatformIO / protobufs） | ✅ 沿用 |

> MeshROC 是 Meshtastic 的兼容增强分支，而非另起炉灶。默认配置下与普通 Meshtastic 节点可无缝混网。

### 核心特性

- **本土合规**：默认以中国 CN 470MHz 频段、合规发射功率与标准调制预设运行。
- **地形射频模板**：内置全国九类地形到标准 LoRa 参数的映射辅助（`src/mesh/MeshROC.cpp`）。
- **增强骨干路由**：在保留上游混合路由/洪泛的基础上，叠加路由器归属（RAP）与分层源路由增强（`src/modules/MeshRocModule.cpp`）。
- **双栈协议协商**：增强帧封装于私有端口（portnum 300），并通过原生文本端口（portnum 1）与普通节点保持互通，自动协商。

## 快速开始

- 🔧 **[构建固件](https://docs.meshtastic.org/development/firmware/build)** — 从源码编译（基于上游 PlatformIO 流程，命令一致）。
- ⚡ **刷写固件** — 使用官方刷写工具或本仓库文档所述流程烧录到设备。

## 许可证

固件代码基于 Meshtastic 固件，以 **GPL-3.0** 许可证发布。硬件以开源硬件形式提供，站点与文档以 MIT 许可证发布。

## 社区

- 社区站点：互联之域 MeshROC（https://github.com/ye2f4/meshroc-fw）
- 问题反馈与新功能建议请通过 GitHub Issues。

---

*MeshROC 派生自 Meshtastic firmware（GPL-3.0），在兼容基础上进行本土化增强。*
