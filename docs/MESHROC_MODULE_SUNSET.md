# MeshRocModule 日落清单（Superposition Legacy → 第一公民融合前奏）

> 状态：已敲定，按 **B 路线** 执行（先停用 MeshRocModule，让第一公民栈独占射频跑通）。
> 生成日期：2026-08-19。关联文档：`MESHROC_INVERSION.md`、`2026-08-17.md`。

---

## 0. 一句话定位（先对齐认知，避免再次记忆错乱）

- **第一公民栈** = `src/kernel/` 的 `MeshRocStack`（10字节包头 + TLV + CRC16，经 `RadioMeshRocBridge::startSendRaw` 直连射频）。已拥有：路由(`HopPlanner`)、RAP(`RapStateMachine`)、分片(`Reassembler`)。与 Meshtastic **零依赖解耦**（kernel/ 禁止 include 原版头）。
- **MeshRocModule** = `src/modules/MeshRocModule.{h,cpp}`（约1586行）。当初是「以 Meshtastic 为宿主」的互通测试程序，自己平行实现了一套：混合路由状态机 + RAP + 屏幕 UI + 数据包格式，同时兼容 meshtastic。它和 kernel 是**叠加态的并行冗余产物**。
- **真实意图（用户 2026-08-19 确认）**：把 MeshRocModule 有价值的部分融合进第一公民（其中路由/RAP 其实 kernel 已有，主要缺的是**屏幕 UI**），然后删掉 MeshRocModule；与 meshtastic 的互通全权交给 meshtastic 自己。

> MeshRocModule = 「叠加态旧产物」。本清单任务 = 先解除叠加（停用），再谈融合（后续阶段）。

---

## 0.1 真实意图（终于清楚，2026-08-19 用户权威论述）

**MeshRocModule = 叠加态旧产物，要被吸收/消灭。**

它当初是「以 Meshtastic 为宿主的互通测试程序」——自己实现了一套 `MeshRocPacket` 解析 + 混合路由状态机 + RAP + 屏幕 UI + 数据包格式，又兼容 Meshtastic（portnum 300 / portnum 1）。

现在**第一公民栈（kernel）已经独立实现了同样的东西**：
- 10字节协议
- 路由（`HopPlanner`）
- 分片（`Reassembler`）
- RAP（`RapStateMachine`）
- ACK（`AckPolicy`）
- 屏幕无关
- 直驱射频

所以 **MeshRocModule 是旧版平行实现，和 kernel 重复了**。

**目标**：把 MeshRocModule 里「有价值的部分」（混合路由 / RAP / UI / 数据格式）融合进第一公民栈，然后 **MeshRocModule 整个退出**。今后与 Meshtastic 的互通，**全权交给 Meshtastic 自己**（即原版栈作为翻译桥，不再是 MeshRocModule 的职责）。

> ⚠️ **认知纠偏**：之前按文档 `MESHROC_INVERSION.md` §4.3 把 MeshRocModule 理解为「翻译官适配器」是**错的**。§4.3 写的是 A 路线演化描述，但真实意图比那更彻底：**MeshRocModule 这个叠加态产物要解体**，不是改造成翻译官。

### 这彻底改变了 Phase 2 的做法
之前纠结的「B1 移除 `SinglePortModule` 还是保留 `service` 回显」——现在有了干净答案：

- MeshRocModule **不再需要回显给普通 Meshtastic 节点**（互通全权交给 Meshtastic 本体）。所以：
  - `service->sendToMesh` / `nodeDB` / `channels` 这些依赖 → **全部移除**，因为 MeshRocModule 不再直接和 Meshtastic 网络对话。
  - 它的「混合路由状态机 + RAP + 数据包格式」→ 已存在于 kernel，确认能否直接复用，能则删 MeshRocModule 的重复实现。
  - 它的「屏幕 UI」→ 这是**唯一 kernel 里没有的东西**，需决定迁去哪（kernel 是零依赖、无 UI；UI 属于 meshtastic 端，应挂在原版 UI 框架上）。

### 必须停下的硬事实（融合前的协议决策，需用户拍板）
「把 MeshRocModule 融合进第一公民并删除」是比之前任何一步都大的重构，且要求先做一件**不能猜**的事：**查清 kernel 自研栈和 MeshRocModule 的协议实现是否真的一致/可合并**。

文档提到**字节序冲突**（kernel 小端 vs MeshRocModule 大端），RAP/路由的状态机两边是「平行实现」但细节未必相同。要「融合」，必须先 diff 两边：

1. **协议头字段布局**：是否都是 10 字节、字段顺序。
2. **RAP 状态机语义**：HELLO / ATTACH / Keepalive 时序、TTL 值。
3. **路由算法**：洪泛 vs 源路由 vs RAP 定向。
4. **屏幕 UI** 要显示什么。

这些不一致，融合时哪个为准、要不要保留 MeshRocModule 的某些特有逻辑，是**需要用户拍板的协议决策，不是我能替用户定的**。且用户明确「等不起编译」（每次 30 分钟），本次重构会同时碰 `kernel/` + `MeshRocModule` + `main.cpp` + `Modules.cpp`，一旦有错又是一轮 30 分钟。

### 建议的两步走（用户定）
- **第一步（只读差异分析，安全）**：把 kernel 自研栈和 MeshRocModule 在协议头/RAP/路由/字节序上的实现逐条 diff，产出「融合清单」：哪些 kernel 已覆盖可删、哪些 MeshRocModule 有而 kernel 缺（需迁）、字节序怎么统一。清单给用户审，**不碰代码**。
- **第二步（用户确认清单后）**：按清单改 kernel（迁缺的部分、统一字节序），然后删除/清空 MeshRocModule 的重复逻辑，改 `main.cpp`/`Modules.cpp` 去掉它的实例化、把 UI 挂到正确位置、把 Meshtastic 互通交给原版桥。

> 本清单（B 路线）是上述「两步走」之前的**解除叠加态前置动作**：先停用让第一公民独占跑通，融合细节后面再说。

---

## 1. 执行路线：B（先停用，让第一公民独占）

### 1.1 B 路线目标
在**不改变现有默认编译行为**的前提下，让 MeshRocModule 不再被实例化，从而：
1. 解除「第一公民栈」与「MeshRocModule 平行双栈」的叠加态；
2. 让启用 `MESHROC_NATIVE_STACK` 时，第一公民栈能干净、独占地驱动射频（`startSendRaw` / `rawSinkAdapter` / `ingestRaw` 全链路验证）；
3. 不删除任何 MeshRocModule 代码（仅停用实例化），保留后续融合阶段的可参考实现。

### 1.2 为什么选 B 而非 A（只读 diff）
- A 只产出「融合清单」，不解决「第一公民现在能不能独立跑通」这个更紧迫地基问题。
- B 直接解除叠加态、验证射频独占链路，是融合阶段的前提。融合清单可作为 B 跑通后的输入，不矛盾。

### 1.3 字节序前置问题（无论 A/B 都绕不开，本阶段只「记录」，不改）
- kernel = **小端**（`MeshRocStack.cpp:19-20`、`RapStateMachine.cpp:7-12`）。
- MeshRocModule = **大端**（匹配真实 datapack 空中字节）。
- 融合前必须裁定：第一公民统一改大端？还是保持小端另做转换层？→ 列入后续「融合阶段」待决议项，**本阶段不动字节序**。

---

## 2. 具体改动清单（B 路线落地动作）

### 改动 1：`src/modules/Modules.cpp` —— 停用实例化
- 第 211-212 行：
  ```cpp
  // Mesh-ROC 定制协议模块（datapack.txt）：始终启用...
  meshRocModule = new MeshRocModule();
  ```
- 改为（加 `#ifdef` 守卫，默认不实例化；仅当显式定义 `MESHROC_LEGACY_MODULE` 时保留旧行为，方便回退）：
  ```cpp
  // Mesh-ROC 旧模块（叠加态遗留）：默认停用，让第一公民栈独占射频。
  // 如需回退到双栈并行，取消下行注释并定义 -DMESHROC_LEGACY_MODULE。
  #ifdef MESHROC_LEGACY_MODULE
      meshRocModule = new MeshRocModule();
  #endif
  ```
- 顶部 include（`#include "modules/MeshRocModule.h"`，第 104 行）保留不动（头文件仍被其它地方 `extern` 引用，且守卫下不实例化不会引入未用符号问题；若编译器报 unused，可一并包进 `#ifdef`，但优先最小改动）。

### 改动 2：`platformio.ini` —— 新增默认关闭的开关锚点
- 在第 45-46 行附近追加（注释态，默认关闭）：
  ```
  ;	-DMESHROC_LEGACY_MODULE   ; 取消注释可回退：重新启用旧 MeshRocModule 双栈并行
  ```

### 改动 3：`src/main.cpp` —— 确认第一公民栈启用路径不受影响
- `MESHROC_NATIVE_STACK` 块（第 1282-1299 行）与 `loop()` 驱动（第 1405-1410 行）**不变**，本阶段不触碰。
- 确认：停用 MeshRocModule 后，`MESHROC_NATIVE_STACK` 开启时不会因缺少 `meshRocModule` 全局而链接失败。`meshRocModule` 是 `*MeshRocModule` 指针（定义于 `MeshRocModule.cpp`），守卫下不实例化只是保持 `nullptr`；其它引用点（如 `MeshService::handleToRadio` 调 `sendTextSmart`）需确认有空指针保护（列入验证项）。

### 改动 4（验证项，非改代码）：全局引用点空指针保护核查
- grep 所有 `meshRocModule->` 调用点，确认在 `nullptr` 时安全降级（不崩溃、不调用）。
- 若发现无条件 `meshRocModule->xxx()` 调用，补 `if (meshRocModule)` 守卫——但**本阶段默认不实例化即 nullptr，必须保证不崩**。

---

## 3. 验证标准（用户环境编译，本机无法编译）

1. 默认构建（不开任何 MESHROC 宏）：MeshRocModule 不实例化，原版 meshtastic 行为完全不变（零回归）。
2. 开启 `-DMESHROC_NATIVE_STACK`（不开 `-DMESHTASTIC_AS_BRIDGE`）：第一公民栈经 `RadioMeshRocBridge` 独占射频，无 `meshRocModule` 干扰；`loop()` 正常 `tick`。
3. 开启 `-DMESHROC_LEGACY_MODULE`（回退开关）：恢复双栈并行旧行为，验证开关可逆。
4. 全局 `meshRocModule` 引用点在 `nullptr` 时不崩溃。

---

## 4. 后续阶段（融合，非本清单范围，仅记录待办）

- **F1 屏幕 UI 融合**：把 `MeshRocModule::drawFrame`（`.cpp:601`）的屏幕渲染逻辑搬入第一公民栈的 UI 层（kernel 当前无 UI，需挂在宿主 UI 框架）。
- **F2 字节序裁定**：统一 kernel 为「大端」或加转换层，匹配 datapack 真实空中字节。
- **F3 能力对账**：逐一对账 MeshRocModule 的 混合路由 / RAP / 数据包格式 与 kernel 同名实现，去重，仅保留 kernel 缺的（主要是 UI）。
- **F4 删除 MeshRocModule**：融合完成后，删除 `MeshRocModule.{h,cpp}`、`Modules.cpp` 守卫、`platformio.ini` 锚点。
- **F5 Phase 3（A 路线）**：若启用 `MESHTASTIC_AS_BRIDGE`，将 OTA / NodeDB / 位置 / MQTT 等原版依赖特性挂回第一公民架构，或标记不支持。

---

## 5. 决策记录（Decision Log）

| 项 | 裁定 |
|----|------|
| MeshRocModule 本质 | 叠加态旧产物，**待融合后删除**（不是改造成翻译官） |
| 与 meshtastic 互通 | 全权由 meshtastic 自身负责（原版栈作翻译桥） |
| 认知纠偏 | §4.3「翻译官适配器」理解错误；真实意图=MeshRocModule 解体，能力迁进 kernel |
| Phase 2 真实含义 | 不是「改 MeshRocModule 为翻译官」，而是「能力迁进 kernel + 删 MeshRocModule」 |
| 依赖清理 | `service->sendToMesh`/`nodeDB`/`channels` 等 meshtastic 网络依赖 → 全部移除 |
| 唯一需迁物 | 屏幕 UI（kernel 零依赖无 UI，应挂原版 UI 框架） |
| 本阶段路线 | B：停用实例化，保留代码 |
| 字节序 | 本阶段不改，列入融合协议决策（需用户拍板） |
| 回退开关 | `MESHROC_LEGACY_MODULE`（默认关） |
| 删除时机 | 融合阶段 F4，确认 kernel 已覆盖后 |
