# MeshROC 互操作性 & 部署清单

Status: 已落地（固件分支 `feat/meshroc`）
Audience: 部署运维 / 固件评审 / 想混用 MeshROC 与普通 Meshtastic 节点的用户

## 1. 一句话结论

MeshROC 节点 = 刷了 MeshROC 固件的 Meshtastic 节点。
**它能和普通 Meshtastic 节点共存于同一网络、共用信道，并且已实现最基本的文本消息双向互通。**
MeshROC 的专属增强协议（传感器 TLV、源路由等）仍封装在私有端口 300 的 datapack 二进制帧里；
普通节点不认识端口 300，直接丢弃——但**文本消息已通过原生端口 1 (TEXT_MESSAGE_APP) 实现向下兼容**。

## 2. 协议分层兼容性矩阵

| 层级 | 是否共用 | 说明 |
|------|----------|------|
| LoRa 物理层 / 调制参数 | ✅ 完全一致 | 同区域、同预设、同信道即可在同一空口 |
| Meshtastic MAC / 加密传输层 | ✅ 完全一致 | 信道 PSK 相同则包可被同网节点中继/解密 |
| 端口路由 (portnum) | ✅ 双监听 | 本模块同时监听端口 300(私有) 与端口 1(原生文本) |
| 应用语义 | ⚠️ 部分 | 文本消息走原生端口互通；传感器/源路由等增强功能仅 MeshROC 节点间互通 |

## 3. 消息互通矩阵（已实现）

| 方向 | 文本 | 遥测/传感器 | 心跳 | 位置/原生消息 |
|------|------|-------------|------|---------------|
| MeshROC → 普通节点 | ✅ 原生端口1 | ❌ (端口300被丢) | ❌ | ✅ 原生通道通用 |
| 普通节点 → MeshROC | ✅ 原生端口1 (CONTINUE 交给 TextMessageModule 显示) | ❌ | — | ✅ |
| MeshROC → MeshROC | ✅ (端口300增强通道) | ✅ | ✅ | ✅ |
| 普通节点 → 普通节点 | ✅ | ✅ | ✅ | ✅ |

结论：
- **文本消息：完全向下兼容**，普通 Meshtastic 节点用官方 App/CLI 即可与 MeshROC 节点互发文本。
- 想用 MeshROC 专属功能（传感器组网、源路由优化），网络内**所有节点都得是 MeshROC**。
- 普通 Meshtastic 用户仍可正常互聊，只是看不到 MeshROC 节点的传感器/心跳等增强数据（反之亦然）。

## 4. 向下兼容实现说明（代码）

文件：`src/modules/MeshRocModule.cpp` / `MeshRocModule.h`

- `wantPacket()` 重写为同时监听 `ourPortNum(300)` 与 `meshtastic_PortNum_TEXT_MESSAGE_APP(1)`。
- `handleReceived()` 按 portnum 分派：
  - 端口 300 → 原有混合路由状态机（datapack 解析/源路由/洪泛）。
  - 端口 1 → `onNativeText()`，记录日志后返回 `CONTINUE`，让官方 `TextMessageModule` 继续显示消息。
- `sendTextSmart(const char *text, uint32_t toNode)`（**推荐默认入口**）：按对端能力协商的智能文本发送。
  - 默认走原生 `portnum=1`，普通节点直接显示，最省带宽。
  - 仅当对端已被观测到发过端口 300 私有帧（确认是 MeshROC 节点）时，才额外补发一份端口 300 增强帧；
    广播因无法预知听众类型，自动双发（端口 1 + 端口 300）覆盖混合网络。
  - 能力表 `peerCaps[32]` 在 `handleReceived` 收到任意端口 300 帧时自动 `markPeerMeshRoc()` 打标，6 小时老化。
- `sendNativeText(const char *text, uint32_t toNode)`（**底层 API**，仅在"强制只发原生、不要 300 增强帧"时调用）：
  用原生 `portnum=1` 发送 UTF-8 文本，普通节点直接显示。这是"MeshROC 节点 → 普通 Meshtastic 节点"最基本的兼容路径。
- 开关 `nativeCompatEnabled`（默认 `true`）：置 `false` 则 `sendTextSmart` 退化为仅发私有端口 300（不互通普通节点）。

> 注意：`sendTextSmart` / `sendNativeText` 走标准 Meshtastic 文本通道，因此也支持官方 App/Python 库的 `sendText()`，
> 即 MeshROC 节点作为普通 Meshtastic 节点被官方客户端完全管理。业务层应统一调用 `sendTextSmart()`，
> 由框架按对端能力自动决定通道，无需业务代码判断"对方是不是普通节点"。

## 5. 部署前置条件

### 5.1 骨干/中继节点 rebroadcast_mode = ALL（仅影响 MeshROC 增强通道跨跳）
代码位置：`logDeploymentWarnings()`
- 端口 1 的原生文本消息由 Meshtastic 原生机制中继，**不受此限制**。
- 端口 300 的私有帧要跨多跳，骨干节点须 `device.rebroadcast_mode = ALL`。
- 仅 ROUTER / ROUTER_LATE 角色参与 MeshROC 路由转发。

### 5.2 拓扑隐私：关 MQTT 或走私有 broker（仅影响端口 300 增强数据）
- datapack 帧含节点短地址/路由路径；若 `moduleConfig.mqtt.enabled`，拓扑会泄漏到公共 broker。

### 5.3 双层跳数协同（仅影响端口 300 增强数据）
- `sendFrame` 已自动取 `max(datapack.max_hop, 配置hop_limit)`。

## 6. 自检清单（开机日志）

节点启动后查看日志，端口 1 互通无需特殊配置；端口 300 增强通道应无以下 WARN：
- [ ] "rebroadcast_mode=... is NOT 'ALL'" → 设为 ALL（若只用文本互通可忽略）
- [ ] "MQTT enabled on a Mesh-ROC node" → 关 MQTT 或私有 broker

收到帧应见：
- [ ] "MeshRoc: native text from=0x.. (Meshtastic-compatible): ..." 表示收到普通节点文本
- [ ] "MeshRoc: send native text (port=1) to=0x..: ..." 表示已发送兼容文本
- [ ] "MeshRoc: sendTextSmart unicast/BROADCAST (port=300) to=0x.. peerMeshRoc=N" 表示已按能力协商补发/双发增强帧
- [ ] "MeshRoc: recv type=... src=0x.. dst=0x.." 表示端口300帧被正确解析

## 7. 已知限制（非阻塞）

- 未做真实 ESP32 多节点烧录验证（无工具链）。
- route.txt §7 上层逻辑（定时器/ACK 超时触发、网关下行）未实现，当前 onAckTimeout 仅标记失效。
- 字节序采用大端（与 route.txt 权威定义及 5 个真实示例吻合）；若上游文档改小端，需翻转 MeshRocCodec 中 6 处 16 位字段。
- 端口 1 文本消息走原生通道，不享受 MeshROC 源路由优化（按需使用端口 300 增强通道）。
