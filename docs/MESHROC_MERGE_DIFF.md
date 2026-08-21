# MeshRocModule ↔ 第一公民栈（kernel）融合差异清单

> 用途：融合阶段（删除 MeshRocModule 前）的可执行 checklist。只读分析产出，不含代码改动。
> 生成日期：2026-08-19。关联：`MESHROC_MODULE_SUNSET.md`（B 路线已落地）、`MESHROC_INVERSION.md`。
> 状态（**2026-08-21 复核反转**）：本文档生成时标"待拍板/待执行"，但 D1–D4 拍板后，8.1–8.6 融合**已全部落地为代码**。
> 经 `pio run -e kernel-smoke` 编译验证，内核处于融合终态——**本清单已无待合并代码，仅作历史依据留存**。详见第 10 节。

---

## 0. 总览结论（先给结论）

| 维度 | kernel 现状 | MeshRocModule 现状 | 融合判定 |
|------|------------|-------------------|----------|
| 协议头布局 | 10B 头 + 1B CRC8（小端） | 10B 头 + 2B CRC16（大端） | **不兼容**，需裁定统一方向 |
| 字节序 | 小端（putU16/getU16） | 大端（serialize/deserialize） | **冲突**，需拍板 |
| RAP 子类型/TTL | 完整（RapProtocol.h）且与 MeshRocModule 常量一致 | 同源常量（datapack 权威值） | **可复用 kernel**，MeshRocModule 重复定义 |
| RAP 状态机 | 骨架（HELLO/KEEPALIVE/ATTACH_ACK/DETACH 基础迁移） | 完整（含 ATTACH_REQ/ACK、OWNERSHIP_ADV、SYNC_REQ、迟滞切换、TTL 分级） | **MeshRocModule 更全**，需迁进 kernel |
| 路由算法 | 仅 `HopPlanner`（跳数规划，无路由表/无源路由转发/无洪泛探测） | 完整混合路由：routeCache 源路由表 + onRouteProbe 洪泛探测 + onSourceRoute 严格跳点转发 + onAckTimeout 回退 | **MeshRocModule 独有能力**，kernel 缺，需迁 |
| 分片 | `Reassembler`（接收端完整） | 无独立分片实现（依赖 kernel 包头 + 注释说发送端缺失） | kernel 已有，MeshRocModule 无重复 |
| ACK | `AckPolicy`（优先级倍率微调，委托原版） | 应用层 onAckTimeout 标记失效回退洪泛 | 两者职责不同层，可并存 |
| 屏幕 UI | 无（kernel 零依赖无 UI） | `drawFrame` 完整（品牌/系统名/FW 版本/角色/对端数/兼容开关） | **唯一 kernel 绝对缺失**，需迁到原版 UI 框架 |
| Meshtastic 网络耦合 | 无（直驱射频） | 强耦合：`service->sendToMesh`/`nodeDB`/`channels`/`config`/`MeshService` 接管 | 融合后**全部移除**（互通交 Meshtastic 本体） |

**核心判定**：MeshRocModule 在「RAP 完整度 / 路由算法 / 屏幕 UI」三处比 kernel 更全；kernel 在「分片 / ACK 策略 / 零依赖直驱」已覆盖。融合 = **把 MeshRocModule 的 RAP 完整逻辑、混合路由、屏幕 UI 迁进 kernel（及原版 UI 框架），然后删 MeshRocModule**。协议头/字节序必须先统一，否则迁过去也解不开包。

---

## 1. 协议头布局 diff（需拍板）

### kernel 头（`src/kernel/net/MeshRocPacket.h`）
- 字段顺序：`ctrl_flag(1) | src(2) | dst(2) | seq(2) | max_hop(1) | snr(1,int8) | route_mode(1) | crc(1,CRC8)` = **11 字节**（`HEADER_LEN=11`，含 1B CRC8）。
- route_mode 语义：`0=洪泛 1=源路由 2=RAP 定向`（三类）。
- CRC：**包头 CRC8**（单字节，`h.crc = buf[10]`）。

### MeshRocModule 头（`src/modules/MeshRocModule.h:154`）
- 字段顺序：`ctrl_flag(1) | src_addr(2) | dst_addr(2) | seq(2) | max_hop(1) | snr(1) | route_mode(1) | payload[] | crc(2,CRC16)` = **10B 头 + 2B CRC16**。
- route_mode 语义：`0=定向单播 1=受限洪泛`（仅两类，无 RAP 定向位）。
- CRC：**CRC16-MODBUS**（2 字节，`crc16(10B头+TLV)`）。

### 冲突点（必须拍板）
1. **CRC 长度/算法**：kernel 用 1B CRC8（仅包头），MeshRocModule 用 2B CRC16-MODBUS（包头+载荷）。空中格式以谁为准？→ **决策 D1**。
2. **头长度**：kernel 11B（含 CRC8 在头内），MeshRocModule 10B 头 + 外挂 2B CRC。帧边界不同。→ 与 D1 绑定。
3. **route_mode 取值**：kernel 三类（含 RAP 定向）vs MeshRocModule 两类。若统一到 kernel 定义，MeshRocModule 的 `0=定向单播` 需重新映射到 `2=RAP 定向` 或新增。→ **决策 D2**。
4. **字段名**：kernel 用 `src/dst/seq`，MeshRocModule 用 `src_addr/dst_addr/seq`；纯命名差异，迁时统一即可，无协议冲突。

---

## 2. 字节序 diff（需拍板，最关键）

- **kernel 小端**：`MeshRocStack.cpp:19-20`、匿名命名空间 `putU16/getU16`（`p[0]=v&0xFF; p[1]=(v>>8)&0xFF`）；`RapStateMachine.cpp:7-15` 同。
- **MeshRocModule 大端**：`MeshRocCodec::serialize/deserialize`（`.cpp:99-160`）显式 `(v>>8)&0xFF` 先写高位；注释明确「真实空中字节流按大端解读才吻合 src=0x000A/dst=0x0014」。

### 判定
真实 datapack 空中字节是**大端**（MeshRocModule 已按真实样本校准）。kernel 当前小端是**自研栈内部假设**，尚未经过真实空中样本验证。

→ **决策 D3（最高优先级）**：第一公民栈统一改**大端**以匹配真实 datapack 空中字节？还是保持小端、给 MeshRocModule 遗留设备做转换层？
  - 推荐：统一改大端（一锤定音，未来部署一致；改动点 = kernel 所有 `putU16/getU16` 三处 + RAP 消息体 + FragHeader）。
  - 影响范围：`MeshRocStack.cpp` 匿名 `putU16/getU16`、`RapStateMachine.cpp` 匿名 `putU16/getU16`、`Reassembler.cpp` 的 `FragHeader`（若用 putU16 则自动跟改，需核查是否直接用 memcpy）、`MeshRocPacket.h` 内若有位域拼接（无，仅 struct）。
  - 注：kernel 的 TLV tag 值（0x01-0x13 / RAP 0x20-0x28）与 MeshRocModule 的 tag 值（0x01-0x13 / 0x20-0x28，命名 MESHROC_TLV_*）**完全一致**，仅字节序影响多字节字段（uint16BE 的 SNR/度量/跳点数组），tag 本身单字节无冲突。

---

## 3. RAP 状态机 diff

### 常量一致性（已核对，无需改）
`RapProtocol.h` 与 `MeshRocModule.h:95-119` 的权威常量**逐项相同**：
- HELLO_PERIOD 90s / JITTER 30s / NEIGHBOR_TTL 270s / FULL_ADV 30min / MAX_NEIGHBORS 16 / MAX_OWNED 32 / MAX_REMOTE_OWNERS 64 / HYST_SNR 6dB / HYST_SAMPLES 3 / MIN_DWELL 10min / ROUTE_FAIL_THRESHOLD 3 / HELLO_BACKOFF 12min / LBT_UTIL 40% / ATTACH_RETRY 30s / EVAL_INTERVAL 15s。
- TTL 分级（CLIENT/DTU 30min、SENSOR 2h、TRACKER 6h）两边一致。
- RapKind 子类型：kernel（HELLO=1/HELLO_ACK=2/ATTACH_REQ=3/ATTACH_ACK=4/KEEPALIVE=5/DETACH=6）vs MeshRocModule（RAP_HELLO=1/HELLO_ACK=2/ATTACH_REQ=3/ATTACH_ACK=4/KEEPALIVE=5/DETACH=6）——**完全一致**。

### 实现完整度差异（kernel 缺，需迁）
| 能力 | kernel RapStateMachine | MeshRocModule |
|------|------------------------|---------------|
| HELLO 发送 | ✓ | ✓ |
| KEEPALIVE 续租 | ✓（30s 骨架周期） | ✓（按角色 TTL 分级，捎带模式） |
| ATTACH_ACK 处理 | ✓（置 ATTACHED） | ✓ |
| DETACH | ✓（回 SCANNING） | ✓ |
| **ATTACH_REQ 主动发起** | ✗ 骨架无 | ✓ `sendRapAttachReq` |
| **OWNERSHIP_ADV / 归属表** | ✗ | ✓ `sendRapOwnershipAdv` + RapOwnerEntry |
| **SYNC_REQ** | ✗ | ✓ `sendRapSyncReq` |
| **迟滞切换（EVALUATING 多骨干评估）** | ✗（评完直接回 SCANNING） | ✓ `inferLocalRole`+迟滞逻辑 |
| **骨干侧拓扑广播（NODE_SET）** | ✗（注释 TODO） | 部分（OWNERSHIP_ADV） |
| **TTL 分级授予** | ✗（骨架固定 60s ATTACH_TIMEOUT） | ✓ `rapTtlForRole` 按角色 |

### 融合动作
- **删** MeshRocModule 的 RAP 全部发送函数（1070-1190 行区间）与常量定义（保留 RapProtocol.h 为权威）。
- **迁** ATTACH_REQ 主动发起、OWNERSHIP_ADV、SYNC_REQ、迟滞切换、TTL 分级 → 扩写 `RapStateMachine`（或新增 `RapOwnerTable` / `RapBackboneLogic` 类，挂 kernel）。
- kernel 当前 KEEPALIVE_PERIOD 30s / ATTACH_TIMEOUT 60s 是骨架值，需改为按角色 TTL（接 MeshRocModule 的 `rapTtlForRole` 逻辑）。

---

## 4. 路由算法 diff（kernel 缺，需迁）

### kernel 现状
- `HopPlanner`：仅跳数规划（`estNetDiameter` / `adaptiveHopLimit` / `effectiveHopLimit`），**无路由表、无源路由转发、无洪泛探测**。
- `MeshRocStack::sendPayload` 用 `router_.effectiveHopLimit()` 算跳数，但**没有下一跳选择逻辑**（无路由缓存）。

### MeshRocModule 现状（完整混合路由，route.txt §5）
- `routeCache`（`MeshRocRouteEntry[32]`）：dst → 源路由跳点数组 + avgSnr + 成功计数 + 老化。
- `routeLookup` / `routeStore` / `routeInvalidate`：CRUD。
- `onRouteProbe`（PHASE1 洪泛探测）：仅 BACKBONE 中继记录路径、减跳、重广播；目标回 ACK(REVERSE_PATH)。
- `onSourceRoute`（PHASE2 业务单播）：严格按 0x10 ROUTE_PATH 跳点转发。
- `onAckTimeout`（PHASE3 失败回退）：标记失效 → 重新洪泛探测。
- `onFloodFrame`（PHASE4 受限洪泛）：bit6=1 仅骨干中继。
- `adaptiveHopLimit`：本地角色自适应跳数。

### 融合动作（最大工作量）
- 在 kernel 新增 `Router`（扩展现有 HopPlanner 或新建）：源路由缓存表 + 洪泛探测状态机 + 严格跳点转发 + ACK 超时回退。
- 或：判断「第一公民栈当前是否真需要源路由」——若第一阶段只跑 RAP 定向 + 受限洪泛，可先迁 `onFloodFrame`/`onRouteProbe` 简化版，源路由表作为后续迭代。
- **需拍板 D4**：融合后路由模型以哪套为准？（建议：保留 MeshRocModule 的四相混合路由，因已落地验证）。

---

## 5. 屏幕 UI diff（唯一 kernel 绝对缺失，需迁到原版 UI）

### MeshRocModule::drawFrame（`.cpp:601-636`，`#if HAS_SCREEN`）
显示内容：
1. 品牌短名 `MESHROC_BRAND_SHORT`
2. 系统名 `MESHROC_SYSTEM_NAME`（"MeshROC"）
3. 固件版本 + 许可 `FW %s | %s`（GPL-3.0 明示）
4. 本机产品线角色 `MeshROC %s`（Backbone/Gateway/Walk/Sensor）
5. 已确认 MeshROC 对端数 `MeshROC peers: %u`
6. 原生兼容通道状态 `Compat: ON/OFF`

### 挂载点
- `drawFrame` 是 `OLEDDisplay` 回调，需查它在 Screen 框架的注册处（搜索 `drawFrame` 调用 / `Screen::setFrame` / `Graphics` 框架）。GUI 相关文件在 `src/ui/` 或 `src/graphics/`（本次未深查，标记为待办 M1）。

### 融合动作
- kernel 是零依赖、无 UI 层，**UI 不应进 kernel**。
- 正确归宿：把 `drawFrame` 逻辑迁到**原版 UI 框架**（meshtastic 的 Screen/Graphics），作为「MeshROC 信息帧」。
- 角色/对端数等数据来源：融合后由 kernel 暴露只读状态接口（如 `kernel.stats().peerCount()`），UI 层调用，**不反向依赖**。

---

## 6. Meshtastic 网络耦合（融合后全部移除）

MeshRocModule 对 Meshtastic 的直接依赖调用点（融合时删除，互通交 Meshtastic 本体）：
- `service->sendToMesh` / `allocDataPacket`：`.cpp:426/648/694/842/883/925`（sendFrame 走 `allocDataPacket`+`service->sendToMesh`）
- `nodeDB->getNodeNum()`：`.cpp:1073/1087/...`（取自身短地址，融合后改由 kernel `myAddr_`）
- `channels.getPrimaryIndex()`：`.cpp:656`（选主信道，融合后 kernel 直驱射频不经信道）
- `config` / `moduleConfig.mqtt.enabled`：`.cpp:661/689`（双层跳数/MQTT 隐私告警）
- `MeshService::handleToRadio` 接管（`MeshService.cpp:322` `sendTextSmart`）：融合后**移除该接管分支**，文本走原版原生路径（互通交 Meshtastic）。
- `NextHopRouter.cpp:280-286 / 614-615` 的 `meshRocModule->getRapNextHop/noteRapRouteFailure`：融合后改调 kernel 的路由接口（或移除该注入点，因 kernel 自己管路由）。

---

## 7. 协议决策（D1–D4）— 已拍板（2026-08-19）

**总原则：以 MeshRocModule 为主**。它承载了自研系统的逻辑核心，数据包格式也是经不断修正而来。以下四项为其落实细则，均为最终决策。

| 决策 | 问题 | 裁定 | 影响 |
|------|------|------|------|
| **D1** | CRC 以谁为准？ | **MeshRocModule CRC16-MODBUS（2B，头+载）** | kernel 头从 11B(含CRC8) 改为 10B头+2B CRC16；解析全改 |
| **D2** | route_mode 取值统一？ | **kernel 三类（0洪泛 / 1源路由 / 2=RAP定向）** 为权威定义 | MeshRocModule 当前两类(0定向/1洪泛) 迁时重映射；建议新定义：0洪泛/1源路由/2RAP定向，与 kernel 对齐 |
| **D3** | 字节序统一方向？ | **统一大端**（匹配真实 datapack 空中字节） | kernel 三处 putU16/getU16 + FragHeader + RAP 消息体改大端 |
| **D4** | 融合后路由模型以哪套为准？ | **保留 MeshRocModule 四相混合路由（已验证）** | kernel 需新增源路由表 + 洪泛探测 + 严格转发 + ACK 回退 |

> 四者自洽 = 完全对齐真实 datapack 空中格式（大端 + CRC16-MODBUS + kernel 三类 route_mode）。即「第一公民栈向 MeshRocModule 的成熟空中格式收敛」。

---

## 8. 融合执行顺序建议（**2026-08-21 复核：已全部完成**）

> 下列 6 步在 8.1–8.6 融合阶段已落实为代码，下方逐项给出落地位置与验证结论。

1. **统一协议底座（D1–D3）** — ✅ 已完成。
   - `src/kernel/net/MeshRocPacket.h`：10B 头 + 帧尾 2B CRC16-MODBUS（poly=0x1021,init=0xFFFF，大端）。
   - `src/kernel/MeshRocStack.cpp` 匿名 `putU16/getU16` 全大端；`route_mode` 三类（0 洪泛/1 源路由/2 RAP 定向）。
2. **迁 RAP 完整逻辑** — ✅ 已完成。
   - `src/kernel/net/rap/RapStateMachine.{h,cpp}`：含 ATTACH_REQ 主动发起、OWNERSHIP_ADV/OWNER_LIST、SYNC_REQ、迟滞切换（hystSamples/hystSnr/EVALUATING）、TTL 分级（`rapTtlForRole`）。
3. **迁路由（四相混合）** — ✅ 已完成。
   - `src/kernel/net/Router.{h,cpp}`：routeCache 源路由表 + routeLookup/routeStore/routeInvalidate + onRouteProbe(PHASE1) + onSourceRoute(PHASE2, ROUTE_PATH 0x10 严格跳点) + onFloodFrame(PHASE4, bit6 骨干中继) + onAckTimeout(PHASE3 回退)。`MeshRocStack::handleRoute` 按 route_mode 分发。
4. **迁 UI** — ✅ 已落于原版框架（非内核职责）。
   - `src/modules/MeshROCStatusModule.{h,cpp}`：`drawFrame` 继承 MeshModule，`#if HAS_SCREEN` 只读绑定 `g_meshrocBridge`；kernel 经 `cfg()`/`meshRocPeerCount()` 暴露只读状态。UI 不反向依赖 kernel。
5. **解耦 Meshtastic** — ✅ 已完成。
   - `NextHopRouter` 移除 RAP 注入；`MeshService::handleToRadio` 移除 `sendTextSmart` 接管分支；Meshtastic 互通交原版本体。
6. **删除 MeshRocModule** — ✅ 已完成。
   - `src/modules/MeshRocModule.{h,cpp}` 已删；`PeerCaps.{h,cpp}` 已删；`platformio.ini` `MESHROC_LEGACY_MODULE` 锚点移除；`Modules.cpp` 守卫移除。

---

## 9. 待办（本次未深查，标记后续）
- **M1**：定位 `drawFrame` 在 Screen/Graphics 框架的注册点（`src/ui/` 或 `src/graphics/`），确认如何挂「MeshROC 信息帧」。
- **M2**：核查 `Reassembler.cpp` 的 `FragHeader` 是否走 putU16（若直接 memcpy 则 D3 改字节序时需手动翻转）。
- **M3**：确认 kernel `MeshRocStack::ingestRaw` 当前是否已按「小端+CRC8」解析真实包（验证第一公民当前能否解 MeshRocModule 发的包——预期不能，因字节序/CRC 不同，这正是 D1/D3 要解决的）。

---

## 10. 当前代码实况（2026-08-21 复核 · 倒反天罡）

> 本节为**代码权威复核**，反写前 9 节的"待执行"表述。结论：文档生成时（08-19）尚处"计划"，
> 但 8.1–8.6 融合已将计划全部兑现为代码。本清单**不再含任何待合并代码**。

### 10.1 文档表述 vs 代码实况对照
| 前文档表述 | 代码实况（2026-08-21） | 判定 |
|------------|------------------------|------|
| §0 "kernel 小端 11B 头+CRC8" | `MeshRocPacket.h` 已 10B 头 + 2B CRC16-MODBUS 帧尾，`putU16` 全大端 | 文档旧，代码新 |
| §0 "RAP 状态机骨架、缺 ATTACH_REQ/OWNERSHIP/SYNC/迟滞" | `RapStateMachine.cpp` 全部已实现 | 文档旧，代码新 |
| §0 "路由算法仅 HopPlanner，缺源路由/洪泛/回退" | `Router.cpp` 四相混合路由完整 | 文档旧，代码新 |
| §0 "屏幕 UI 唯一 kernel 绝对缺失" | UI 已落 `MeshROCStatusModule`（原版框架），kernel 保持零依赖 | 正确分工 |
| §0 "MeshRocModule 强耦合 meshtastic" | `MeshRocModule.{h,cpp}` 已删除 | 文档旧，代码新 |
| §7 "D1–D4 已拍板" | 拍板已转化为代码（见 §8 各项 ✅） | 一致 |

### 10.2 遗留小瑕疵（不影响编译/功能，仅注释陈旧）
- **M2 实际核查**：`MeshRocStack.cpp` 的 `FragHeader` 经 `putU16(fragVal, fragId)` 写入，**已是字节序大端**，与 D3 一致；但第 196/420 行注释仍写 `fragId u16 LE`——注释误导，建议改为 `u16 BE`。功能正确，非协议 bug。
- **M1 实际核查**：`drawFrame` 已迁至 `src/modules/MeshROCStatusModule.cpp`（继承 `MeshModule`），注册点随 meshtastic `MeshModule` 机制，非独立 Screen 帧——原 M1 待办已自然消解。
- **M3 实际核查**：`MeshRocStack` 已按「大端 + CRC16-MODBUS」解析，与 MeshRocModule（已删）时期的真实 datapack 空中格式对齐；M3 担忧的"字节序/CRC 不同"问题已通过 D1/D3 解决。

### 10.3 验证结论
- `pio run -e kernel-smoke`（隔离编译 env，仅编 `src/kernel/` + 驱动桩）**SUCCESS**，生成 `firmware-kernel-smoke-*.bin`。
- kernel 自研栈（协议/RAP/路由/分片/ACK/射频抽象）编译 + 链接零错误。
- bridge 层（MeshROC.cpp / MeshROCBridge / MeshROCStatusModule / MeshRocHandlers 引用 meshtastic lib）仍需全量 `pio run -e heltec-v3` 验证，不在本 env 覆盖范围内。
- **本 merge diff 文档使命终结**：它曾是融合的执行依据，现已成为"已完成融合的存档说明"。后续改动应直接基于 `src/kernel/` 当前代码，而非本清单。
