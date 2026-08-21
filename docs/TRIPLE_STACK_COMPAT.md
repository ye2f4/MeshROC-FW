# 三栈兼容架构（MeshRoc 主体 + Meshtastic/MeshCore 第三方兼容）

> 基线日期：2026-08-17（修正版）
> **核心定位更正**：MESHROC 固件（src/kernel/ 自研栈）才是主体。Meshtastic 与 MeshCore 都是
> 「已知第三方固件」，MESHROC 兼容它们 = 侦听其信道、解析其帧、桥接消息，而非把它们的全栈内置运行。

## 0. 三个仓库的真实关系（重要）

| 路径 | 性质 | git remote |
|---|---|---|
| `E:\firmware`     | **Meshtastic 原版固件**（独立，参考用） | `origin=meshtastic/firmware` |
| `E:\MeshCore`     | **MeshCore 原版固件**（独立，参考用）   | `git@github.com:meshcore-dev/MeshCore` |
| `E:\meshroc-fw`   | **MESHROC 自研固件**（本仓库，主体）     | `github=ye2f4/MeshROC-FW` + `origin=meshtastic/firmware` |

- `meshroc-fw` 是从 meshtastic 改出的 fork，**主题是 MESHROC**，不是 meshtastic 分支。
- 若删除 meshroc-fw 中 MESHROC 相关文件，它就退化回一个独立的 Meshtastic 固件副本。
- 因此：**Meshtastic / MeshCore 都不是本固件的内置栈**，而是「第三方兼容对象」。

## 1. 三方现状比对

| 维度 | Meshtastic（第三方，E:\firmware） | MeshCore（第三方，E:\MeshCore） | MeshRoc（主体，src/kernel/） |
|---|---|---|---|
| 包格式 | protobuf `MeshPacket` | 自有紧凑二进制 | 10B 头 + TLV（§13） |
| 路由 | Flooding / ReliableRouter | 邻居表 + 路径转发（`Mesh.cpp`） | HopPlanner（仅跳数规划） |
| 分片 | 原版无 | `StaticPoolPacketManager` | Reassembler + FRAG_HEADER 0x14 |
| 加密 | AES-128/256 PSK | ed25519 签名 + 对称 | O3 AES-256-GCM + ECDH（stub） |
| ACK/重传 | `RadioInterface::getRetransmissionMsec` | `Mesh.h` 内建重传 | O4 委托原版同构公式 |
| 射频驱动 | `SX126xInterface`/RadioLib（深度集成） | 独立 `RadioLibWrappers` | 未接（sendRaw 空 TODO） |
| 在本固件角色 | 第三方兼容桥接对象 | 第三方兼容桥接对象 | 原生第一公民 |

**核心事实**：三种空中格式互不兼容，故「兼容」= 按信道/角色隔离 + 第三方帧解析桥接，而非同信道混跑或内置第三方全栈。

## 2. 架构：MESHROC 主体 + 第三方桥接（ProtocolManager）

```
MESHROC 固件（主体）
  ├─ 原生：MeshRocStack（10B头+TLV，第一公民，由 ProtocolManager 驱动）
  │
  ├─ 兼容：MeshtasticBridge（第三方）── 独立信道侦听 → 解析 protobuf → 桥接进 MESHROC
  └─ 兼容：MeshCoreBridge（第三方）── 独立信道侦听 → 解析其帧 → 桥接进 MESHROC
                        │
        统一射频(SX126x) 作唯一字节出入口，三家协议都向其借收发
```

三条铁律：
1. **射频层只一份** → 继续用 `SX126xInterface` 作唯一字节出入口；第三方帧经桥接器收到后转交 MESHROC 原生栈统一路由。
2. **MESHROC 自研栈是第一公民** → `ProtocolManager` 直接持有并驱动 `MeshRocStack`；不把任何第三方全栈搬进本固件。
3. **第三方仅作桥接** → Meshtastic/MeshCore 的兼容 = 侦听信道 + 解析帧 + 桥接语义，不照搬其线格式进原生栈（避免造车轮）。

## 3. 从 MeshCore 借鉴（不抄线格式，抄思路）

| 痛点 | MeshCore 方案 | 借鉴动作 |
|---|---|---|
| Reassembler 超时手写 | `StaticPoolPacketManager` 对象池 + 统一 expire | 改成对象池范式 |
| 路由表双份混乱 | 单一路由表 + 邻居质量评分 | 合并 HopPlanner 与 mesh 层路由 |
| RAP 状态机自创 | 更简洁 HELLO/ATTACH/KEEPALIVE | 对齐状态命名 |
| TDMA 时隙纯理论 | slot 调度已实跑 | 校准 SLOT_MS=50 |
| CryptoHook 空 stub | `lib/ed25519` 成熟实现 | 复用其 ed25519，不重造 ECDH |

## 4. 已落地修正（2026-08-17）

- 自研 `NextHopRouter` 重命名为 `HopPlanner`（`src/kernel/net/`），消除与
  `src/mesh/NextHopRouter`（原版完整路由器）的命名冲突。
- 新增 `src/mesh/ProtocolManager.{h,cpp}`：MESHROC 主体调度器地基，持有原生
  `MeshRocStack*`（第一公民），预留 `meshtasticBridge_` / `meshcoreBridge_` 第三方兼容桥接接入点
  （不内置任何第三方全栈）。
- MeshCore 仓库已克隆至 `E:\MeshCore`（v1.17.1），作为思路参考与未来桥接源。

## 5. 后续待办

- [ ] 实现 `MeshRocStack::sendRaw()` 委托原版 `RadioInterface::sendPacket`（接射频）。
- [ ] 实现 `MeshCoreBridge`：侦听 MeshCore 信道、解析其帧、桥接消息到 MeshRoc/Meshtastic。
- [ ] ProtocolManager 接入真实 RadioInterface 字节回调（非侵入式包装）。
- [ ] 配置项：信道分配 / 双发模式（CompatConfig.dualSendChannel0）。
