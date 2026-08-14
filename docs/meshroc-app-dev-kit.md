# MeshROC 自研 App 开发素材清单（Dev Kit Reference）

> 本文档汇总把 MeshROC 固件接入**自有客户端 App**（替代/补充官方 Meshtastic App）所需的全部素材、接口、协议要点与已改造的品牌标识。
> 目标：让后续 App 开发直接照此文落地，无需再翻固件源码。
> 固件定位：**Meshtastic 兼容基础上的国产增强层**（hw_model / protobuf / 端口 1 原生通道完全保留，官方 App 仍能识别管理设备）。

---

## 0. 一句话架构

```
自有 App  ←(BLE / Serial / WiFi HTTP API / MQTT)→  MeshROC 设备
                                              └→ 端口 1 TEXT_MESSAGE_APP（原生文本，与官方 App 互通）
                                              └→ 端口 300 Mesh-ROC 私有帧（增强层：TLV/源路由/传感器/心跳）
```

自有 App 走**标准 Meshtastic 协议通道**即可与设备通信；增强功能（MeshROC 专属）通过端口 300 私有帧 + 本章节定义的 TLV 实现。

---

## 1. 通信通道（App 连设备的三种方式）

| 通道 | 协议 | 用途 | 固件入口 |
|------|------|------|---------|
| **BLE（蓝牙）** | Meshtastic PhoneAPI（protobuf over GATT） | 手机 App 主通道 | `NimbleBluetooth` / `NRF52Bluetooth`：`getDeviceName()` 作广播名 |
| **Serial / USB** | PhoneAPI over UART | 桌面/调试 | `StreamAPI` / `PhoneAPI` |
| **WiFi HTTP API** | REST + protobuf（`/api/v1/toradio`、`/api/v1/fromradio`） | 同网 Web/App | `src/mesh/http/ContentHandler.cpp` |
| **MQTT** | 上行到 broker | 跨网/互联网 | `src/mqtt/MQTT.cpp`（注意拓扑隐私，见 §6） |

### 设备发现标识（已改为 MeshROC 品牌）
- **BLE 广播名**：默认 `MeshROC_%02x%02x`（末 2 字节 MAC），自定义 short_name 时为 `短名_%02x%02x` —— 来源 `src/main.cpp:getDeviceName()`
- **mDNS 服务名**：`_meshroc._tcp.local`（主机 `MeshROC.local`）—— 来源 `src/mesh/wifi/WiFiAPClient.cpp`
- **默认长名（App 显示的设备名）**：`MeshROC %04x` —— 来源 `src/mesh/NodeDB.cpp`
- **HTTP 主页标题**：`<h1>MeshROC</h1>` —— 来源 `src/mesh/http/ContentHandler.cpp`
- **自签证书组织名**：`O=MeshROC,C=US`，`CN=meshroc.local` —— 来源 `WebServer.cpp` / `PiWebServer.cpp`

> ⚠️ 旧设备 NVS 命名空间仍为 `"meshtastic"`（故意保留，避免清空旧配置），App 无需关心。

---

## 2. 核心协议（直接复用 Meshtastic 协议栈）

自研 App **不需要重写协议**，直接复用官方 protobuf 定义：

| 素材 | 位置 / 获取方式 |
|------|----------------|
| **Protobuf 定义（.proto）** | 上游 `github.com/meshtastic/protobufs`（固件 `src/mesh/generated/meshtastic/*.pb.cpp` 即由它生成） |
| **端口号枚举 `PortNum`** | `meshtastic/portnums.proto`：`TEXT_MESSAGE_APP=1`、各种 ` *_APP` |
| **FromRadio / ToRadio 消息** | `meshtastic/mesh.proto`：`meshtastic_ToRadio`（App→设备，含 `send_text`、`want_config`）、`meshtastic_FromRadio`（设备→App，含 `my_info`、`node_info`、`text`、`log`） |
| **Config / ModuleConfig / Channel / User** | `meshtastic/config.proto`、`module_config.proto`、`channel.proto`、`meshtastic/User` |
| **Position / Telemetry / RouteDiscovery** | `position.proto`、`telemetry.proto`、`mesh.proto` |
| **加密/信道 PSK** | `channel.pto` + AES/PSK 派生（与官方一致，自研 App 须实现相同 PSK 派生才能解密加密信道） |

### 文本消息标准路径（自研 App 必实现）
- App 发文本：构造 `ToRadio{ send_text: { payload, channel, dest } }` → 设备 `MeshService::handleToRadio` 接收 → **自动改走 `meshRocModule->sendTextSmart()`**（见 §5 增强层）协商发送。
- 设备收文本：任意节点（含普通 Meshtastic 节点）的端口 1 文本 → `TextMessageModule` → `FromRadio{ text: {...} }` 推给 App。

---

## 3. MeshROC 增强层（端口 300 私有帧）

这是 MeshROC 原创部分，自研 App 若要展现"增强能力"需自行解析。

### 3.1 帧结构（`src/modules/MeshRocModule.h`）
```
Packet = Header[10] + Payload(N≤225) + CRC16[2]   // 总 ≤237 字节
Header: ctrl_flag(1) | src_addr(2,BE) | dst_addr(2,BE) | seq(2,BE) | max_hop(1) | snr(1) | route_mode(1) | payload[...]
```
- 字节序：**大端**（与 datapack.txt 真实空中字节流吻合，注释中已标注待最终确认）
- CRC16：**MODBUS**（poly=0x8005, init=0xFFFF, xorout=0, ref_in/out=true）
- 封装方式：整个帧作为 `Meshtastic MeshPacket.decoded.payload` 的二进制 blob，portnum=**300**

### 3.2 类型 / 优先级（`ctrl_flag` bit0-2 / bit3-4）
| type | 含义 |
|------|------|
| 0 PRIVATE_MSG | 私聊 |
| 1 GROUP_MSG | 群组广播 |
| 2 ACK | 应答（含 REVERSE_PATH） |
| 3 ROUTE_PROBE | 路由探测 |
| 4 TELEMETRY | 传感器遥测 |
| 5 HEARTBEAT | 心跳 |
| 6 ENCRYPTED | 加密传输帧 |

### 3.3 TLV Tag 清单（`MeshRocTlvTag`）
| tag | 含义 | 编码 |
|-----|------|------|
| 0x01 | 电池电压 | uint16 BE, ×100 |
| 0x02 | 太阳能电压 | uint16 BE, ×100 |
| 0x03 | AHT20 温度 | int16 BE, ×10 |
| 0x04 | 湿度 | uint16 BE, ×10 |
| 0x05 | 节点名称 | UTF-8 字符串 |
| 0x06 | 文本消息 | UTF-8 字符串 |
| 0x10 | 源路由跳点 | uint16 BE 数组, 2字节对齐 |
| 0x11 | 反向路径(ACK带回) | uint16 BE 数组 |
| 0x12 | 路由度量 | avg_snr(uint16 BE)+成功计数(uint16 BE) |
| 0x13 | 路由失效标记 | — |

### 3.4 节点角色（`MeshRocRole`）
- 0 BACKBONE（楼顶骨干中继，可执行完整路由/洪泛/源路由）
- 1 CLIENT（手持终端，收 bit6=1 包但不中继）
- 2 GATEWAY（桥接 LoRa↔互联网）

---

## 4. 对端能力协商（App 可展示"对端是否为 MeshROC 节点"）

机制（无需 App 参与，设备侧自动）：
- 设备观测到某节点发过端口 300 帧 → 标记其为 MeshROC 节点（`peerCaps[]`，6h 老化）。
- `sendTextSmart` 单播时：对端是普通节点只发端口 1；对端是 MeshROC 节点额外补端口 300 增强帧；广播双发。

**App 可做的事**：
- 在节点列表里，根据"是否收到过该节点端口 300 帧"标记为"MeshROC 节点"（监听端口 300 的 FromRadio 原始帧即可判断）。
- 展示增强数据：解析端口 300 帧的 TLV（电压/温度/湿度/源路由路径）渲染到节点详情页。

---

## 5. 已落地的固件改造（本轮）

| 改动 | 文件:行 | 说明 |
|------|--------|------|
| BLE 默认名前缀 `Meshtastic_` → `MeshROC_` | `src/main.cpp:getDeviceName()` | 广播名 |
| 默认长名 `Meshtastic %04x` → `MeshROC %04x` | `src/mesh/NodeDB.cpp` | App 设备名 |
| 启动屏 splash `meshtastic.org` → `MeshROC` | `src/graphics/draw/UIRenderer.cpp` | OLED 启动 |
| InkHUD Logo 标题 `Meshtastic` → `MeshROC` | `LogoApplet.cpp` | E-Ink 启动 |
| HTTP 标题 `<h1>Meshtastic</h1>` → `<h1>MeshROC</h1>` | `ContentHandler.cpp` (×2) | Web |
| mDNS 服务 `_meshtastic._tcp` → `_meshroc._tcp` | `WiFiAPClient.cpp` | 网络发现 |
| 自签证书 `O=Meshtastic` → `O=MeshROC` | `WebServer.cpp` / `PiWebServer.cpp` | TLS |
| WiFi STA 主机名 `Meshtastic-` → `MeshROC-` | `WiFiAPClient.cpp` | 网络标识 |
| syslog appName `Meshtastic` → `MeshROC` | `WiFiAPClient.cpp` / `ethClient.cpp` | 日志 |
| Tips/通知页链接 `meshtastic.org` → `meshroc.org` | `TipsApplet.cpp` / `NotificationRenderer.cpp` | 屏幕文案 |
| 版本线 `2.8.0` → `1.0.0`（MeshROC 自有） | `version.properties` | 版本号 |
| **文本自动协商发送** | `MeshService::handleToRadio` 挂接 `sendTextSmart` | App 发文本自动走 MeshROC 增强层 |

### 故意保留（不破坏互操作）
- `hw_model` 枚举、`meshtastic_*` protobuf 类型、端口 1 原生通道
- NVS 命名空间 `"meshtastic"`（改了会清空旧设备配置）
- `KEK_DOMAIN "meshtastic-tak-kek-v2"`（加密派生域，须与固件一致）
- NTP 池 `meshtastic.pool.ntp.org`、protobuf 生成代码、上游文档链接

---

## 6. 部署前置条件（App 联调须知）

- **骨干/中继节点** `device.rebroadcast_mode = ALL`：否则端口 300 私有帧跨跳失败（端口 1 原生文本不受影响）。
- **拓扑隐私**：端口 300 帧含节点短地址/路由路径，若 `moduleConfig.mqtt.enabled` 会上泄漏到 broker；自研 App 走 MQTT 须用私有 broker。
- **双层跳数协同**：端口 300 帧 `max_hop` 须 ≤ 底层 `hop_limit`，否则底层先丢帧。
- **占空比限制**：设备 `Router::send` 会查占空比，超限中止发送并回 NAK（App 需处理 `DUTY_CYCLE_LIMIT`）。

---

## 7. 自研 App 最小功能清单（checklist）

- [ ] BLE 扫描按 `MeshROC_` / `短名_` 前缀识别设备
- [ ] mDNS 发现 `_meshroc._tcp.local`
- [ ] PhoneAPI protobuf 编解码（FromRadio/ToRadio）
- [ ] 发送文本（`send_text`）→ 验证设备侧自动走 `sendTextSmart`
- [ ] 接收文本（`FromRadio.text`）
- [ ] 节点列表（FromRadio.node_info）+ 标记 MeshROC 节点（监听端口 300）
- [ ] 位置/电量展示（position / 端口 300 TLV 0x01）
- [ ] 配置读写（config / module_config / channel，含 PSK 派生）
- [ ] 端口 300 私有帧解析（TLV 0x01~0x13）渲染增强数据
- [ ] 处理 `DUTY_CYCLE_LIMIT` / ACK 超时
- [ ] （可选）MQTT 上行到私有 broker

---

## 8. 参考源码位置（自研 App 对照固件实现）

| 功能 | 固件源码 |
|------|---------|
| 文本自动协商发送 | `src/mesh/MeshService.cpp:handleToRadio` → `MeshRocModule::sendTextSmart` |
| 端口 300 帧编解码 | `src/modules/MeshRocModule.cpp`（`MeshRocCodec`） |
| 能力表 | `MeshRocModule.h` `peerCaps[]` / `markPeerMeshRoc` |
| 设备名/版本 | `src/main.cpp:getDeviceName`、版本宏 `APP_VERSION` |
| HTTP API | `src/mesh/http/ContentHandler.cpp` |
| BLE | `src/nimble/NimbleBluetooth.cpp` |
| 消息渲染(屏幕) | `src/graphics/draw/MessageRenderer.cpp` |
| 混网部署说明 | `docs/meshroc-interop-deployment.md` |

---
*维护：本文档随固件 MeshROC 化改造同步更新。品牌站点正式域名 `https://meshroc.cc.cd`（永久绑定）；GitHub `https://github.com/ye2f4/MeshROC`（勿 404）。*

## 9. 许可与品牌边界（GitHub Pages 审核硬性要求）

为避免被 GitHub Pages / 官方审核判定为"冒充 Meshtastic 衍生页"，明确区分：

| 层 | 许可 | 说明 |
|----|------|------|
| **固件（本仓库 `MeshROC-fw`）** | **GPL-3.0** | 源自 Meshtastic，GPL3 与 MIT **不兼容**，固件文案/屏幕/文档均**不得标 MIT** |
| **硬件** | 开源硬件（立创开源/星火计划） | 独立自研 PCB |
| **文档与官网（MeshROC 站点）** | **MIT** | 仅文档/官网层 |

**两层含义（务必区分，源自 `E:/MESHROC` 建站资料）：**
- 系统名 System = `Mesh Radio-Optimized Communications`（固件/硬件，本仓库产物）
- 社区名 Community = `Mesh Realm Of Connection`（互联之域，官网/社区）
- 两者不是同一概念；固件属于"系统"，对外展示应区分，避免把系统写成社区名。

**对外定位（官网标准话术）：**
> MeshROC = Meshtastic-Compatible + China-Regional Optimized + Hardware-Tailored Professional Mesh System

固件屏幕 MeshROC 信息帧已按此区分：显示系统名 `Mesh Radio-Optimized Communications` + 固件版本 + `GPL-3.0`，不混淆社区名。
