# MeshROC 反转重构·保留契约（Immutable Preservation Contract）

> 本文件是 **"反客为主"重构（让 Meshtastic 退化为可选互通插件、MeshROC 自掌地基）** 的**不可变契约**。
> 凡是本文件列出的数据包格式、路由策略、时序常量、接口语义，**反转后必须原封不动保留**，
> 不得因换内核（脱离 `meshtastic_MeshPacket` / `service->sendToMesh` / `channels` / `config.lora`）而改变其行为。
>
> 来源：当前实现 `src/modules/MeshRocModule.h` + `src/modules/MeshRocModule.cpp`（端口 300 私有协议），
> 以及配套设计文档 `datapack.txt` / `route.txt`。
> 提取日期：2026-08-16

---

## 0. 反转边界声明（什么必须保留、什么可以改）

| 类别 | 保留/可改 | 说明 |
|---|---|---|
| 帧二进制布局（§1） | **必须保留** | 空中字节流不变，否则旧节点不兼容 |
| 字节序（§1.3） | **必须保留**（待用户拍板大端） | 见 §1.3 注释，当前按大端实现 |
| CRC16-MODBUS（§1.4） | **必须保留** | 算法定义为准，非文档测试向量值 |
| ctrl_flag 位域（§2） | **必须保留** | |
| TLV 标签表（§3） | **必须保留** | 新增标签可加，已定义者语义不变 |
| 混合路由状态机（§4） | **必须保留** | 4 阶段逻辑不变 |
| RAP 协议（§5） | **必须保留** | 全部子类型、常量、迟滞铁律不变 |
| 三个深度优化（§6） | **必须保留** | 角色固化 / 自适应跳数 / 信噪比择优 |
| 去重与缓存（§7） | **必须保留** | 窗口/容量语义不变 |
| 宿主依赖点（§8） | **可改** | 仅替换 4 个依赖接口，不改行为 |
| `nativeCompatEnabled` 双发（§9） | **改位置不改语义** | 挪到 compat 层，端口 300 协议本身保留 |

---

## 1. 数据包格式（datapack 协议，落在使用层前已验证）

### 1.1 帧结构
```
[ctrl_flag:1][src_addr:2][dst_addr:2][seq:2][max_hop:1][snr:1][route_mode:1][payload:N][crc16:2]
```
- 总包上限 **237 字节** = 10（头）+ 225（载荷）+ 2（CRC）。
- 载荷（Payload）最大 **225 字节**，禁止 0x00 填充占位。
- `snr` 字段为 SNR 信噪比预留（实际由发送方回填对端最佳链路 SNR）。
- `route_mode`：`0` = 定向单播（带 `0x10 ROUTE_PATH` 走源路由）；`1` = 受限洪泛。

### 1.2 头部字段语义（结构体 `MeshRocPacket`）
| 字段 | 类型 | 说明 |
|---|---|---|
| `ctrl_flag` | u8 | 见 §2 位域 |
| `src_addr` | u16 | 源节点短地址 |
| `dst_addr` | u16 | 目标短地址，`0xFFFF` = 全网广播 |
| `seq` | u16 | 数据包序列号（`(uint16_t)(millis() & 0xFFFF)`，去重窗口用） |
| `max_hop` | u8 | 最大转发跳数 0–63 |
| `snr` | u8 | SNR 预留/回填 |
| `route_mode` | u8 | 0=定向单播 / 1=受限洪泛 |
| `payload` | u8[225] | 变长 TLV 载荷 |
| `crc` | u16 | CRC16（见 §1.4） |

### 1.3 字节序（**待用户最终确认**）
> 设计文档文字称"全部 uint16 小端"，但 5 个真实空中示例字节流按**大端**解读时
> `src=0x000A / dst=0x0014` 才与文档吻合（小端会变成 `0x0A00/0x1400` 矛盾）。
> **当前实现采用大端**（空中/内存统一）。
> 若最终确认走小端，仅需翻转 `MeshRocCodec` 中 6 处 16 位字段的字节序，其余逻辑不变。
> **反转后此决策必须冻结，不得随意改动导致新旧节点无法互通。**

### 1.4 CRC16-MODBUS（route.txt §2 权威定义）
- `poly=0x8005`（反射 `0xA001`），`init=0xFFFF`，`xorout=0`，`ref_in=ref_out=true`。
- 输入范围：**10 字节包头 + 全部 TLV 载荷字节**；CRC 两字节本身不参与计算。
- 空载荷包（空 ACK / 空探测）：仅对 10 字节包头计算。
- **重要**：`route.txt` 给出的 7 个测试向量 CRC 值与标准 MODBUS 算法均不吻合（疑似文档转录/手算错误），
  故实现以**算法定义**为准，而非那些向量值。标准 MODBUS 正确性已用参考向量
  `{01 03 00 00 00 01} → 0x840A` 验证通过。

---

## 2. `ctrl_flag` 位域
```
bit0-2  类型 (MeshRocPacketType)
bit3-4  优先级 (MeshRocPriority)
bit5    wantAck
bit6    中继权限 (relayPerm)   ← 骨干-终端隔离核心位
bit7    压缩 (compressed)
```

### 2.1 类型枚举 `MeshRocPacketType`
| 值 | 名称 | 说明 |
|---|---|---|
| 0 | `MESHROC_TYPE_PRIVATE_MSG` | 私聊消息 |
| 1 | `MESHROC_TYPE_GROUP_MSG` | 群组广播消息 |
| 2 | `MESHROC_TYPE_ACK` | ACK 应答包（携带 `REVERSE_PATH`） |
| 3 | `MESHROC_TYPE_ROUTE_PROBE` | 路由探测包 |
| 4 | `MESHROC_TYPE_TELEMETRY` | 传感器遥测包 |
| 5 | `MESHROC_TYPE_HEARTBEAT` | 节点心跳包 |
| 6 | `MESHROC_TYPE_ENCRYPTED` | 加密传输帧 |
| 7 | `MESHROC_TYPE_RAP` | RAP 控制族（真实子类型见载荷首个 TLV `0x20 RAP_KIND`） |

### 2.2 优先级枚举 `MeshRocPriority`
| 值 | 名称 | 映射（原→Meshtastic 优先级，反转后映射到内核排队） |
|---|---|---|
| 0 | `MESHROC_PRIO_LOW` | BACKGROUND（心跳、传感上报） |
| 1 | `MESHROC_PRIO_NORMAL` | DEFAULT（日常文字） |
| 2 | `MESHROC_PRIO_HIGH` | RELIABLE |
| 3 | `MESHROC_PRIO_EMERGENCY` | MAX |

### 2.3 `bit6` 中继权限（骨干-终端隔离）
- `relayPerm=1` 的中继包：仅 **BACKBONE** 角色允许转发；CLIENT 终端**只接收、禁止中继**。
- 终端默认 `relayPerm=0`；业务单播/广播帧由 BACKBONE 在 `onFloodFrame` / `onRouteProbe` 中减跳重广播。

---

## 3. TLV 标签表（datapack 第三节 + route.txt 路由扩展）
格式：`Tag(1) + Length(1) + Value(N)`，未知 tag 必须跳过不崩溃（前向兼容）。

### 3.1 业务 TLV
| Tag | 名称 | Value | 说明 |
|---|---|---|---|
| `0x01` | `MESHROC_TLV_BATT_VOLT` | u16 BE ×100 | 电池电压 |
| `0x02` | `MESHROC_TLV_SOLAR_VOLT` | u16 BE ×100 | 太阳能板输入电压 |
| `0x03` | `MESHROC_TLV_AHT20_TEMP` | i16 BE ×10 | AHT20 温度 |
| `0x04` | `MESHROC_TLV_HUMIDITY` | u16 BE ×10 | 空气湿度 |
| `0x05` | `MESHROC_TLV_NODE_NAME` | string | 节点名称 |
| `0x06` | `MESHROC_TLV_TEXT_MSG` | UTF-8 | 文本消息 |

### 3.2 路由 TLV（**自研精华，必须保留**）
| Tag | 名称 | Value | 说明 |
|---|---|---|---|
| `0x10` | `MESHROC_TLV_ROUTE_PATH` | uint16 BE 数组（2字节对齐） | 源路由跳点序列 |
| `0x11` | `MESHROC_TLV_REVERSE_PATH` | uint16 BE 数组 | ACK 中带回的反向路径（回程源路由） |
| `0x12` | `MESHROC_TLV_ROUTE_METRIC` | avg_snr(u16 BE) + deliveryOk(u16 BE) | 路由度量 |
| `0x13` | `MESHROC_TLV_ROUTE_INVALID` | — | 标记目的路由失效，触发重新探测 |

---

## 4. 混合路由状态机（route.txt §5，4 阶段）

- **PHASE1 洪泛探测**（`onRouteProbe`）：`ROUTE_PROBE` 包由 BACKBONE 中继记录路径、减跳重广播；目标回 `ACK(REVERSE_PATH)` 回报路径。仅 BACKBONE 参与；CLIENT 收到只交付上层不中继。
- **PHASE2 业务单播**（`onSourceRoute`）：业务包 `payload` 含 `0x10 ROUTE_PATH` 时，严格按跳点序列转发——定位本节点在序列中的索引 `idx`，定向单播到 `hops[idx+1]`，`max_hop-1`；本节点不在序列则**丢弃不扩散**（route.txt §6.3）。
- **PHASE3 失败回退**（`onAckTimeout`）：ACK 超时 → `ROUTE_INVALID` 标记 → 重新洪泛探测。应用层定时触发；连续失败由 RAP 侧 `noteRapRouteFailure` 清条目落回洪泛。
- **PHASE4 受限洪泛**（`onFloodFrame`）：广播/心跳/遥测走此分支；`bit6=1` 仅 BACKBONE 重广播（减跳），CLIENT 只接收。

### 4.1 去重规则（route.txt §6）
- 键 `(src_addr << 16) | seq`，**60 秒窗口**内重复出现即丢弃，禁止重复转发。
- 超过窗口则刷新时间戳后放行（允许同一 seq 在新窗口合法重发）。

---

## 5. RAP —— Router Attach Protocol（datapack.txt R1~R12）

> 路由器归属协议：终端归属到某 BACKBONE，BACKBONE 维护归属表并相互通告，
> 实现"定向投递 + 落回洪泛"的双栈路由。这是反转后**最值钱的自研资产**。

### 5.1 角色分类 `RapRoleClass`
`0 BACKBONE / 1 CLIENT / 2 SENSOR / 3 TRACKER / 4 DTU / 5 GATEWAY`

> **反转合并声明（方案 A）**：当前 `RapRoleClass`（本处 6 值）与 `MeshRocModule.h` 的 `MeshRocRole`（BACKBONE/CLIENT/GATEWAY 3 值）是**两套并存、语义重叠**的角色定义，
> 且 `GATEWAY` 在两边都定义、`DTU`/`GATEWAY` 在 `localRoleClass()` 中**永远返回不到**（原版 13 角色无对应值，走 default→CLIENT）。
> 反转后统一合并为**单一枚举 `MeshRocRole`**（见 §11），本 `RapRoleClass` 作废；空口 `RAP_ROLE_CLASS(0x22)` 的 value 直接编码 `MeshRocRole` 数值。

### 5.2 RAP 子类型 `RapKind`（载荷首个 TLV `0x20 RAP_KIND` 的 value）
| 值 | 名称 | 方向/语义 |
|---|---|---|
| 1 | `RAP_HELLO` | BACKBONE 周期广播"我在，我是骨干" |
| 2 | `RAP_HELLO_ACK` | BACKBONE 定向回应"我也在 + 我收你的 SNR" |
| 3 | `RAP_ATTACH_REQ` | 终端请求归属到某 BACKBONE |
| 4 | `RAP_ATTACH_ACK` | BACKBONE 接受归属 + 下发续租 TTL |
| 5 | `RAP_KEEPALIVE` | 终端续租（可捎带在遥测/位置帧内） |
| 6 | `RAP_DETACH` | 终端主动注销（加速回收，非必需） |
| 7 | `RAP_OWNERSHIP_ADV` | BACKBONE 向邻居通告归属变更（增量/全量 Delta） |
| 8 | `RAP_SYNC_REQ` | 请求对端全量重发归属表（版本号不连续时） |

### 5.3 RAP 专用 TLV（`0x20~0x2F` 区间）
| Tag | 名称 | Value |
|---|---|---|
| `0x20` | `RAP_KIND` | len=1, value=子类型（§5.2） |
| `0x21` | `RAP_NEIGHBOR_SNR` | len=3, uint16BE 对端短地址 + int8 我收它的 SNR |
| `0x22` | `RAP_ROLE_CLASS` | len=1, 0..5 角色 |
| `0x23` | `RAP_ATTACH_SEQ` | len=2, uint16BE 归属序号，单调递增（后到覆盖先到） |
| `0x24` | `RAP_TTL_GRANT` | len=2, uint16BE 授予的租期秒数 |
| `0x25` | `RAP_OWNER_LIST` | len=N*4, uint16BE 终端短地址 + uint16BE attachSeq 条目数组 |
| `0x26` | `RAP_OWNER_DEL` | len=N*2, uint16BE 终端短地址数组（已离开） |
| `0x27` | `RAP_TABLE_VERSION` | len=2, uint16BE 本机归属表版本号，每次变更 +1 |
| `0x28` | `RAP_LINK_COST` | len=1, int8 双向链路代价 min(snr_AB, snr_BA)，单位 dB |

### 5.4 RAP 时序常量（已调好，直接搬，**不得擅改**）
```c
#define RAP_HELLO_PERIOD_MS          (90UL * 1000UL)   // 90s（470MHz 占空比反推，下限 64.4s+余量）
#define RAP_HELLO_JITTER_MS          (30UL * 1000UL)   // 30s 随机上线抖动，防上线风暴碰撞
#define RAP_NEIGHBOR_TTL_MS          (270UL * 1000UL)  // 3 × HELLO_PERIOD
#define RAP_FULL_ADV_PERIOD_MS       (30UL * 60UL * 1000UL) // 30min 低频全量通告
#define RAP_ADV_MAX_ENTRIES_PER_FRAME 16               // 470MHz 单次 ≤1s 推出分片上限
#define RAP_MAX_NEIGHBORS            16                // 单蜂窝骨干密度
#define RAP_MAX_OWNED                32                // 单 BACKBONE 归属表上限
#define RAP_MAX_REMOTE_OWNERS        64                // 邻居通告汇总表
#define RAP_KEEPALIVE_CLIENT_MS      (10UL * 60UL * 1000UL)
#define RAP_KEEPALIVE_DTU_MS         (10UL * 60UL * 1000UL)
#define RAP_KEEPALIVE_SENSOR_MS      (30UL * 60UL * 1000UL) // 捎带
#define RAP_TTL_CLIENT_MS            (30UL * 60UL * 1000UL) // 3 × Keepalive
#define RAP_TTL_DTU_MS               (30UL * 60UL * 1000UL)
#define RAP_TTL_SENSOR_MS            (2UL * 60UL * 60UL * 1000UL)
#define RAP_TTL_TRACKER_MS           (6UL * 60UL * 60UL * 1000UL)
#define RAP_HYST_SNR_DB              6                 // 铁律三：候选优于当前至少 6dB
#define RAP_HYST_SAMPLES             3                 // 铁律三：连续 3 次独立采样
#define RAP_MIN_DWELL_MS             (10UL * 60UL * 1000UL)  // 铁律三：最短驻留 10min
#define RAP_ROUTE_FAIL_THRESHOLD     3                 // 定向投递连续失败即清条目落回洪泛
#define RAP_MAX_HELLO_BACKOFF_MS     (12UL * 60UL * 1000UL)  // 指数退避上限 12min（2^3 × 90s）
#define RAP_LBT_UTIL_THRESHOLD_PCT   40                // 信道利用率超 40% 推迟本次 HELLO
#define RAP_ATTACH_RETRY_MS          (30UL * 1000UL)
#define RAP_EVAL_INTERVAL_MS         (15UL * 1000UL)
```

### 5.5 RAP 行为铁律（不可变）
1. **准入以 HELLO_ACK 为准**：骨干互发现时，未回 ACK 的邻居**不计入** `rapNeighbors`（排除原版 ROUTER 误判）。
2. **迟滞切换铁律三**：终端切换归属需同时满足 (a) 候选 SNR ≥ 当前 +6dB、(b) 连续 3 次独立采样、(c) 已过最短驻留 10min；否则 `hystSamples` 清零。
3. **软状态租期**：归属靠 Keepalive 续租，超时（按角色 TTL）自动回收并 `bumpTableVersion()`。
4. **版本不连续请求全量**：收到 `OWNERSHIP_ADV` 的 `tableVersion` 跳变 >1 → 发 `SYNC_REQ` 请求全量重发。
5. **冲突裁决**：`OWNER_LIST` 同节点多条时，`attachSeq` 大者胜。
6. **链路代价**：`linkCost = min(snrMyToIt, snrItToMe)`（双向，dB）。
7. **RAP 帧仅一跳**（`max_hop=0`，`relayPerm=0`），禁止中继。

### 5.6 双栈公开接口（供内核路由器调用，语义不可变）
- `std::optional<uint8_t> getRapNextHop(uint32_t to)`：
  - 非 BACKBONE → `nullopt`（终端不做定向转发）。
  - 广播/非法地址 → `nullopt`。
  - 先查本机 `rapOwnerTable`，再查邻居通告 `rapRemoteOwners`；命中且未过期 → 得归属 BACKBONE。
  - 归属 BACKBONE 必须在 `rapNeighbors` 内、已 `acked`、未过 `RAP_NEIGHBOR_TTL_MS`。
  - 降级为末字节并 `resolveUniqueLastByte` 唯一解析（规避撞车）→ 返回末字节；否则 `nullopt` 落回洪泛。
- `void noteRapRouteFailure(uint32_t toNode)`：
  - 定向投递连续失败计数，达 `RAP_ROUTE_FAIL_THRESHOLD`(3) 清条目并落回洪泛。

### 5.7 RAP 数据结构（反转后原样保留字段语义）
- `RapNeighborEntry`：`backbone / snrMyToIt / snrItToMe / linkCost / lastHelloAt / lastAckAt / acked / tableVersion`
- `RapOwnerEntry`：`node / ownerBackbone / attachSeq / ttlExpireAt / roleClass / failCount`
- `RapLocalState`（终端侧）：`state(SCANNING/ATTACHED/EVALUATING) / attachedTo / attachSeq / lastKeepaliveAt / lastEvalAt / minDwellUntil / curSnr / hystSamples / lastAttachReqAt`

---

## 6. 三个深度优化（叠在路由上的，必须保留）

1. **分层骨干路由角色固化**：转发判据统一为 `isRelayAllowed() == (role==BACKBONE || role==GATEWAY)`（见 §11 唯一角色表）；其余角色（CLIENT/SENSOR/TRACKER/DTU）默认静默、仅收发不中继，省电+降信道冲突。
   反转前临时实现依赖原版 `config.device.role` 推断（`inferLocalRole` + `isBackboneRelay`），存在 default 落 BACKBONE 隐患与 `ROUTER_LATE` 在 `NextHopRouter` 不启用 RAP 的不一致，反转后由 `MeshROCConfig.deviceRole` 直接存储、彻底消除。
2. **自适应跳数**：维护估计网络直径 `estNetDiameter`（观测帧 `max_hop` 最大值，10min TTL），返回本节点外发 `max_hop = max(配置默认, 直径+1)`。小网不浪费、大网不截断。
3. **信噪比择优路径**：`recordLinkSnr(from, snr)` 记录对端最佳 rx_snr 进能力表；源路由选路择优；并把最佳 SNR 回填进外发帧 `pkt.snr` 字段。

---

## 7. 去重与缓存（语义不变）

- `seenCheck(src, seq)`：环形 `seenCache[32]`，60s 窗口去重。
- `routeCache[16]`：环形覆盖最旧条目，路由条目 1200s 老化（`MeshRocRouteEntry`：dst / hops[] / hopCount / avgSnr / deliveryOk / expireAt / valid）。
- `peerCaps[32]`：`isMeshRoc` 标志=是否观测到对端发过端口 300 帧（天然区分 MeshROC/普通节点），6h TTL 老化；存 `bestSnr` 供信噪比择优。接口：`isPeerMeshRoc()` / `markPeerMeshRoc()` / `getPeerSnr()`。

---

## 8. 反转时仅可替换的 4 个宿主依赖点（行为不变）

当前 `MeshRocModule` 寄生在 Meshtastic 内核上，反转后以下调用点用**内核自有等价接口**替换，**不改外部行为**：

| 原依赖 | 反转后归属 | 替换要求 |
|---|---|---|
| `service->sendToMesh(mp)` | `kernel/service/MeshROCService::send()` | 直接发裸 `MeshRocPacket` 字节流，不经 `meshtastic_MeshPacket` |
| `channels.getPrimaryIndex()` | `kernel/crypto/KeyStore` 或固定主信道 | 帧必须走主信道确保可中继（兼容性修复 1） |
| `config.lora.hop_limit` / `Default::getConfiguredOrDefaultHopLimit()` | `kernel/config/MeshROCConfig` | 跳数协同：datapack `max_hop` 必须 ≤ 底层 hop，否则底层先丢弃（兼容性修复 3） |
| `nodeDB->getNodeNum()` / `nodeDB->resolveUniqueLastByte()` | `kernel/nodedb/NodeDB` | 提供等价 `getNodeNum()` 与末字节唯一解析 API |

### 8.1 必须保留的跳数协同约束
- 源路由单播（`route_mode=0`）：底层 hop 必须 ≥ 跳点序列长度（`pkt.max_hop`），否则跨跳失败。
- 受限洪泛（广播）：允许用 datapack `max_hop` 主导。
- 优先级映射（§2.2）必须映射到内核排队优先级，保持抢占语义。

### 8.2 拓扑隐私告警（保留）
- 若开启 MQTT/外部上行，datapack 帧含节点短地址/路由路径，**会泄漏拓扑**。反转后若内核有外发通道，需保留同等告警（建议：禁止把原生帧上行，或仅上行业务文本）。

---

## 9. 兼容层（`nativeCompatEnabled` + 端口 300 双发）

- **端口 300 私有协议本身属于本契约保留范围**（§1–§7）。
- 反转后，`nativeCompatEnabled`（默认 `true`）与"原生文本双发"逻辑**挪到 `modules/compat/MtCompatModule`**，不再依赖 `meshtastic_PortNum_TEXT_MESSAGE_APP` 通道。
- 双发语义保留：
  - 单播：默认仅发原生通道；仅当对端已确认是 MeshROC 节点（`peerCaps.isMeshRoc`）时补发端口 300 增强帧。
  - 广播：原生 + 300 双发，覆盖混合网络。
  - `nativeCompatEnabled=false` → 仅发 300，与普通节点不互通。
- 对端能力协商（`isPeerMeshRoc` / `markPeerMeshRoc`）逻辑保留，仅判断依据从"见过发 300 帧"改为内核自有观测。

---

## 10. 反转落地检查清单（Verify-before-merge）

- [ ] 裸 `MeshRocPacket` 字节流经 `MeshRocCodec` 序列化→反序列化→CRC 校验，与当前实现逐字节一致（大端）。
- [ ] 4 阶段混合路由状态机在真机/模拟下行为不变（探测→源路由→失败回退→受限洪泛）。
- [ ] RAP 全 8 子类型 + 全部 TLV + 迟滞铁律三 + 时序常量逐条对齐。
- [ ] `getRapNextHop` / `noteRapRouteFailure` 在脱离 `meshtastic_MeshPacket` 后语义不变。
- [ ] 跳数协同约束（§8.1）在新内核内仍生效。
- [ ] 旧版 MeshROC 节点（端口 300 协议）与新内核节点可互通（空中格式未变）。
- [ ] 拓扑隐私告警保留。

---

## 11. 角色定义合并方案 A（反转后唯一真相源）

### 11.1 现状问题（必须解决）
反转前存在两套角色定义，且均寄生在 Meshtastic 原版 `config.device.role`（13 值）上：
- `MeshRocRole`（`MeshRocModule.h` L145-149）：`BACKBONE=0 / CLIENT=1 / GATEWAY=2`，用于转发判据与 bit6 隔离。
- `RapRoleClass`（L85-92）：`BACKBONE=0 / CLIENT=1 / SENSOR=2 / TRACKER=3 / DTU=4 / GATEWAY=5`，用于 RAP 归属与 TTL。
- **重叠**：`GATEWAY` 两处重复定义；`BACKBONE/CLIENT` 语义一致。
- **死定义**：`RapRoleClass` 由 `localRoleClass()`（L944-958）从原版角色映射，原版无 DTU/GATEWAY 对应值 → 二者**永远返回不到**，目前是空位。
- **判据不一致**：`inferLocalRole` 的 default→BACKBONE，但 `isBackboneRelay` 只看 ROUTER/ROUTER_LATE；且 `NextHopRouter` 仅在 `ROUTER`（非 `ROUTER_LATE`）启用 RAP 定向，与 `isBackboneRelay` 矛盾。

### 11.2 方案 A：以 `MeshRocRole` 为唯一枚举
反转后 `kernel/config/MeshROCConfig.deviceRole` 直接存储**单一枚举**，删去 `RapRoleClass`：

```c
// kernel/config/MeshROCConfig.h
typedef enum {
    MESHROC_ROLE_BACKBONE = 0,  // 转发，RAP 服务端（发 HELLO/收 ATTACH）
    MESHROC_ROLE_CLIENT   = 1,  // 不转发，RAP 终端，TTL 30min
    MESHROC_ROLE_SENSOR   = 2,  // 不转发，RAP 终端，TTL 2h
    MESHROC_ROLE_TRACKER  = 3,  // 不转发，RAP 终端，TTL 6h
    MESHROC_ROLE_DTU      = 4,  // 不转发，RAP 终端密集业务，TTL 30min（**原死定义，今激活**）
    MESHROC_ROLE_GATEWAY  = 5,  // 转发（特殊骨干），桥接 LoRa↔互联网（**原死定义，今激活**）
} MeshRocRole;
```

### 11.3 唯一角色表（反转后权威）
| role | 转发？ | RAP 身份 | TTL（续租） | 备注 |
|---|---|---|---|---|
| `BACKBONE` (0) | ✅ | 骨干服务端 | — | 楼顶/基站转发枢纽 |
| `CLIENT` (1) | ❌ | 终端 | 30min | 手持终端 |
| `SENSOR` (2) | ❌ | 终端 | 2h | 传感器，睡觉省电 |
| `TRACKER` (3) | ❌ | 终端 | 6h | 追踪器，睡觉省电 |
| `DTU` (4) | ❌ | 终端 | 30min | 串口网关密集业务（方案 A 激活） |
| `GATEWAY` (5) | ✅ | 特殊骨干 | — | 桥接互联网（方案 A 激活，合并旧 `MeshRocRole.GATEWAY`） |

### 11.4 统一转发判据（消除旧不一致）
```c
// 替代旧 inferLocalRole / isBackboneRelay 的两套判据
bool isRelayAllowed() const {
    return role == MESHROC_ROLE_BACKBONE || role == MESHROC_ROLE_GATEWAY;
}
```
- `handleReceived` 的 bit6 隔离改为：`if (relayPerm && !isRelayAllowed()) { 只收不转 }`（原 L255-257）。
- `NextHopRouter::getNextHop` 启用 RAP 定向的条件改为 `isRelayAllowed()`（原 L280 仅 `ROUTER`），使 `GATEWAY` 亦走 RAP 定向，消除旧 `ROUTER_LATE` 不一致。
- RAP 空口 `RAP_ROLE_CLASS(0x22)` 的 value **直接填 `(uint8_t)role`**，无需两套枚举转换。
- `rapTtlForRole(role)` 改为 `switch(role)`：`SENSOR→2h / TRACKER→6h / DTU→30min / 其余(BACKBONE/CLIENT/GATEWAY)→30min`（原 L960-968 映射平移）。

### 11.5 反向兼容（兼容层）
- 原版 Meshtastic 节点无 `MeshRocRole` 概念；compat 模块（`modules/compat/MtCompatModule`）按原版 `config.device.role` 映射：
  `ROUTER/ROUTER_LATE/ROUTER_CLIENT/REPEATER → BACKBONE`；`CLIENT/CLIENT_MUTE/SENSOR/TRACKER/TAK/HIDDEN/TAK_TRACKER/CLIENT_BASE → CLIENT`（不转发）；
  `CLIENT_BASE` 的条件转发（对收藏节点转发）**反转后简化为 CLIENT 不转发**，除非用户另行要求保留。
- 此映射仅用于"理解邻居原版节点行为"，本机 `deviceRole` 始终用 §11.3 自研角色。

---

## 12. 自研固件 Web / APP 接口规范（反转后对外契约）

### 12.1 现状（接口全部寄生原版 protobuf，是反转缺口）
当前固件对外接口**完全基于 `meshtastic` 原版 protobuf**，自研资产（§11 角色、RAP 归属、GATEWAY 桥接）**没有任何接口暴露**：

- **BLE**：`MESH_SERVICE_UUID` 下 `ToRadio`/`FromRadio` 两个 GATT characteristic，protobuf 流式（`meshtastic.FromRadio` / `meshtastic.ToRadio`）。
- **HTTP REST**（`mesh/http/ContentHandler.cpp`）：
  - `/api/v1/toradio` (PUT) ↔ `/api/v1/fromradio` (GET)：protobuf 流式，复用 `HttpAPI : PhoneAPI`，等价于 BLE。
  - `/admin` (GET)：OTA 固件上传页；`/restart` (POST)：重启。
  - `/nodes` (GET)：节点列表 JSON；`/report` (GET)：设备报告 JSON。
  - `/scan-networks` (GET)：WiFi 扫描；`/fs` (GET/DELETE)：静态文件管理；`/static/*`：托管 Web UI。
- **缺口**：Web/APP 无法读/设 `MeshRocRole`(§11.3)、无法读 RAP 归属表、无法配 GATEWAY 桥接 `meshroc.cc.cd` 参数、无法看 RAP 路由状态。

### 12.2 反转接口设计原则
1. **保留兼容通道**：原版 `/api/v1/toradio|fromradio` + BLE GATT **保留**，供原版 Meshtastic APP 仍能连、能读节点——这是"100% 兼容 Meshtastic"承诺的落地。
2. **新增自研通道**：在兼容通道**之上**叠加 `MeshROC` 专属接口（命名空间 `/api/v1/meshroc/...`），承载自研资产配置与 RAP 运维。**不破坏**原有 protobuf 端点。
3. **统一鉴权**：自研端点复用原版 `adminChannel` 的哈希 challenge 机制（避免双重鉴权体系）。

### 12.3 自研 REST 端点（HTTP，反转后新增）
| 方法+路径 | 语义 | 请求体 | 响应 |
|---|---|---|---|
| `GET /api/v1/meshroc/config` | 读自研配置（`MeshROCConfig`：deviceRole、RAP 参数、GATEWAY 桥接 URL/密钥） | — | `application/json`（§12.4 schema） |
| `PUT /api/v1/meshroc/config` | 写自研配置（含 §11.3 的 `deviceRole`） | JSON | 200 + 新配置；非法 role 值→400 |
| `GET /api/v1/meshroc/role` | 读本机角色表（含 RAP 角色类、TTL、是否转发） | — | JSON：`{role, rapClass, relayAllowed, ttlMs}` |
| `GET /api/v1/meshroc/rap/topology` | 读 RAP 归属表（骨干列表 + 各自归属终端 + SNR） | — | JSON 数组 |
| `GET /api/v1/meshroc/rap/routes` | 读 RAP 定向路由表（源路由下一跳、SNR 择优结果） | — | JSON 数组 |
| `GET /api/v1/meshroc/gateway/status` | GATEWAY 桥接状态（连 `meshroc.cc.cd` 在线？最后上行时间？） | — | JSON |
| `POST /api/v1/meshroc/gateway/reconnect` | 触发 GATEWAY 重连上游 | — | 202 Accepted |

> 端点均带 `Access-Control-Allow-Origin: *`（与原版一致，便于 Web UI 直连）；超集响应 `Content-Type: application/json`。

### 12.4 `MeshROCConfig` JSON schema（GET/PUT 载荷）
```json
{
  "deviceRole": 0,            // §11.3 枚举：0 BACKBONE / 1 CLIENT / 2 SENSOR / 3 TRACKER / 4 DTU / 5 GATEWAY
  "rap": {
    "enabled": true,          // 是否启用 RAP 归属路由
    "helloIntervalMs": 30000, // 骨干 HELLO 周期
    "attachTimeoutMs": 10000  // 终端 ATTACH 超时
  },
  "gateway": {                // 仅 GATEWAY 角色相关
    "upstreamUrl": "wss://meshroc.cc.cd/bridge",
    "authToken": "<opaque>",  // 不回显明文
    "autoReconnect": true
  },
  "relayAllowed": true        // 派生字段 = isRelayAllowed()（§11.4），只读
}
```

### 12.5 BLE 扩展（不破坏原版 GATT）
- **保留**原版 `ToRadio`/`FromRadio` characteristic 不动（兼容原版 APP）。
- **新增**可选 `MESHROC_SERVICE_UUID` 服务，下挂：
  - `MeshRocRoleCharacteristic` (read/write)：本机 `MeshRocRole` 值（1 字节，§11.3）。
  - `RapTopologyCharacteristic` (read/notify)：RAP 归属表快照（二进制 TLV 或 JSON-on-BLE）。
- APP 优先走原版通道做通用操作，走 `MESHROC_SERVICE` 做自研资产运维；两套并存、互不阻塞。

### 12.6 Web UI 托管
- `/static/*` 继续托管自研 Web 控制台（固件内含或 OTA 下发）。
- 控制台调用 §12.3 自研端点，提供：角色切换下拉（受 §11.3 枚举约束）、RAP 拓扑可视化、GATEWAY 桥接配置表单。
- **反转前** Web UI 只能调原版 protobuf 端点；**反转后** UI 必须双通道：原版端点保兼容、自研端点暴露 RAP/GATEWAY/角色。

### 12.7 兼容性约束（不可违反）
- 原版 Meshtastic 手机 APP（蓝牙/GATT 或 `/api/v1/*`）必须仍能连接、读节点、发消息——自研端点**只增不改**原版行为。
- 自研端点返回 JSON（非 protobuf），与原版 protobuf 端点**协议隔离**，避免 schema 耦合。
- `deviceRole` 写入校验：非 §11.3 六个值之一 → `400`；GATEWAY 角色要求 `gateway.upstreamUrl` 非空，否则 `400`。

---

## 13. 自研协议帧与深度优化资产清单（反转保留项）

> 本章把散落在 `MeshRocModule.h/.cpp` 的**自研资产**逐一定义，确保反转后 `kernel/` 不丢功能。所有取值为代码实测权威值。

### 13.1 `ctrl_flag` 字节结构（包头的控制标志，1 字节）
| bit | 字段 | 语义 |
|---|---|---|
| bit0-2 | `type` | `MeshRocPacketType`：0 私聊 / 1 群播 / 2 ACK / 3 路由探测 / 4 遥测 / 5 心跳 / 6 加密帧 / 7 RAP 控制族 |
| bit3-4 | `priority` | 0 最低 … 3 最高 |
| bit5 | `wantAck` | 需对端回 ACK |
| bit6 | `relayPerm` | 中继许可位；CLIENT 收到此位=1 的包**只收不转**（§11.4 隔离） |
| bit7 | `compressed` | payload 是否压缩 |

### 13.2 RAP（Router Attach Protocol）归属协议（datapack.txt R1~R12）
**子类型（RAP_KIND=0x20 的 value）**：
`RAP_HELLO=1 / HELLO_ACK=2 / ATTACH_REQ=3 / ATTACH_ACK=4 / KEEPALIVE=5 / DETACH=6`

**TLV（0x20~0x28 区间，与原版 0x01-0x13 不冲突）**：
| tag | len | 含义 |
|---|---|---|
| `RAP_KIND` 0x20 | 1 | 子类型 |
| `RAP_NEIGHBOR_SNR` 0x21 | 3 | uint16BE 对端短址 + int8 我收它的 SNR |
| `RAP_ROLE_CLASS` 0x22 | 1 | §11.3 角色值（反转后直接填 `MeshRocRole`） |
| `RAP_ATTACH_SEQ` 0x23 | 2 | uint16BE 归属序号，单调递增（后到覆盖先到） |
| `RAP_TTL_GRANT` 0x24 | 2 | uint16BE 授予租期（秒） |
| `RAP_OWNER_LIST` 0x25 | N*4 | uint16BE 终端短址 + uint16BE attachSeq 数组 |
| `RAP_OWNER_DEL` 0x26 | N*2 | uint16BE 已离开终端短址数组 |
| `RAP_TABLE_VERSION` 0x27 | 2 | uint16BE 归属表版本，每次变更 +1 |
| `RAP_LINK_COST` 0x28 | 1 | int8 双向链路代价 = min(snr_AB, snr_BA) dB |

**实现常量（权威）**：`RAP_HELLO_PERIOD_MS=90s`、`HELLO_JITTER=30s`、`NEIGHBOR_TTL=270s`、`FULL_ADV_PERIOD=30min`、`ADV_MAX_ENTRIES_PER_FRAME=16`、`MAX_NEIGHBORS=16`、`MAX_OWNED=32`、`MAX_REMOTE_OWNERS=64`、`HELLO_BACKOFF上限=12min`、`LBT_UTIL_THRESHOLD=40%`、`ATTACH_RETRY=30s`、`EVAL_INTERVAL=15s`。
**续租 TTL**：`CLIENT=30min / DTU=30min / SENSOR=2h / TRACKER=6h`（=3×Keepalive 或更长）。
**终端状态机** `RapTerminalState`：`SCANNING(0)→ATTACHED(1)→EVALUATING(2)`（发现更优候选迟滞评估）。
**铁律三（迟滞切换）**：候选优于当前≥`RAP_HYST_SNR_DB=6dB`、连续 `RAP_HYST_SAMPLES=3` 次独立采样、最短驻留 `RAP_MIN_DWELL_MS=10min`，三者齐备才切换骨干。
**路由失效落回洪泛**：定向投递连续失败 `RAP_ROUTE_FAIL_THRESHOLD=3` 次，清条目回退洪泛。

### 13.3 MeshRoc 业务 TLV（0x01~0x13，非 RAP）
| tag | 含义 |
|---|---|
| 0x01 `BATT_VOLT` | 电池电压 uint16BE ×100 |
| 0x02 `SOLAR_VOLT` | 太阳能输入电压 uint16BE ×100 |
| 0x03 `AHT20_TEMP` | AHT20 温度 int16BE ×10 |
| 0x04 `HUMIDITY` | 湿度 uint16BE ×10 |
| 0x05 `NODE_NAME` | 节点名字符串 |
| 0x06 `TEXT_MSG` | UTF-8 文本 |
| 0x10 `ROUTE_PATH` | 源路由跳点数组 uint16BE（2B 对齐） |
| 0x11 `REVERSE_PATH` | ACK 反向路径（带回） |
| 0x12 `ROUTE_METRIC` | avg_snr uint16BE + 投递成功计数 uint16BE |
| 0x13 `ROUTE_INVALID` | 标记目的路由失效，触发重探测 |

### 13.4 自适应跳数（深度优化 §8.1 落地）
- `estNetDiameter()`：用 `nodeDB.getLastVerifiedDiameter()`（10 分钟 TTL 观测网络直径）取 `max(观测直径+1, 配置默认 hop_limit)`。
- `adaptiveHopLimit()`：`min(配置 hop_limit, estNetDiameter())` 返回有效跳数；**上限 7**（防极端）。
- 双层协同：底层发包 `hop_limit = max(adaptiveHopLimit, datapack.max_hop)`（datapack 硬下限不可低于），RAP 定向跳的 `NextHopRouter::getNextHop` 走 §11.4 `isRelayAllowed()` 判据。

### 13.5 信噪比择优路径（深度优化 §8.2 落地）
- `recordLinkSnr(from, snr)`：每次收到包回填 `peerCaps[id].snr`（t1=互测均值、t2=下线时间），供 `getPeerSnr()` 查表。
- 选路：`routeMetric = 0.7*avg_snr + 0.3*delivery_rate`，TTL 15s 内样本有效。
- 回填：非压缩包回填 `pkt.snr`（基站/中继诊断用，已在 `NextHopRouter::getNextHop` L156-166 实装）。

### 13.6 拓扑隐私告警（§10 关联）
- `MeshRocModule::onMqttConnected / onPublish`：当 MQTT 上行携带**完整邻居拓扑**（非匿名统计）时，打 `WARN` 级日志告警"拓扑泄漏风险"。
- 反转保留：Web/APP 拓扑可视化（§12.3 `/meshroc/rap/topology`）**默认仅展示匿名聚合**（节点数/链路数/平均 SNR），不暴露精确短地址↔GPS 映射，除非用户显式开启"详细拓扑"。

### 13.7 反转落点小结
| 资产 | 反转后归属 | 接口暴露（见 §12） |
|---|---|---|
| `ctrl_flag` 结构 | `kernel/net/MeshRocPacket` | BLE `MESHROC_SERVICE` + `/api/v1/meshroc/*` |
| RAP 协议 + 常量 + 状态机 | `kernel/net/rap/` | `/meshroc/rap/topology`、`/meshroc/rap/routes` |
| 业务 TLV 0x01-0x13 | `kernel/net/MeshRocTlv` | 同原版 `/nodes` 增强 |
| 自适应跳数 | `kernel/net/NextHopRouter` | （内部，无独立端点） |
| 拓扑隐私告警 | `kernel/net/MeshRocModule` | §12.6 默认匿名 |

---

## 14. 完整反转资产清单（含原版寄生点）

> 全量盘点 `MeshRocModule` 的所有资产，并标注**反转后归属**与**当前寄生原版的位置**（寄生点即反转工作量最大处）。

### 14.1 自研资产（不依赖原版语义，反转后直接平移）
| 资产 | 代码位置 | 反转落点 | 说明 |
|---|---|---|---|
| `MeshRocPacket` 10 字节包头 | `MeshRocModule.h` L154-164 | `kernel/net/MeshRocPacket.{h,cpp}` | ctrl_flag/src/dst/seq/max_hop/snr/route_mode/crc，字段顺序不可改 |
| `MeshRocCodec`（serialize/deserialize/CRC16/appendTlv/parseCtrlFlag） | `MeshRocModule.cpp` L26-141 | `kernel/net/MeshRocCodec` | 纯算法，零原版依赖 |
| `MeshRocPacketType`（8 类） | `MeshRocModule.h` L48-57 | 同上 | 对应 ctrl_flag bit0-2 |
| `MeshRocPriority`（4 级） | `MeshRocModule.h` L122-127 | 同上 | bit3-4 |
| 业务 TLV 0x01-0x13 | `MeshRocModule.h` L130-142 | `kernel/net/MeshRocTlv` | 电量/温湿/文本/路由 |
| RAP 协议（子类型+TLV+常量+状态机） | `MeshRocModule.h` L59-119 / `.cpp` L902+ | `kernel/net/rap/` | §13.2 全量 |
| `MeshRocRole` 角色枚举 | `MeshRocModule.h` L145-149（但反转后扩为 §11.3 六值） | `kernel/config/MeshROCConfig` | §11 |
| 自适应跳数 `adaptiveHopLimit` | `MeshRocModule.cpp` L513+ | `kernel/net/NextHopRouter` | §13.4 |
| 信噪比择优 `recordLinkSnr/getPeerSnr` | `MeshRocModule.cpp` L234-238, L438 | `kernel/net/PeerCaps` | §13.5 |
| 拓扑隐私告警 | `MeshRocModule.cpp` L658-687 | `kernel/net/MeshRocModule` | §13.6，读 `moduleConfig.mqtt` |

### 14.2 寄生原版传输层（反转必须替换的耦合点）
当前自研帧**不是直接发 LoRa**，而是塞进 `meshtastic_MeshPacket.decoded.payload` 让原版射频层投递。以下全部是反转要"断奶"的点：

| 寄生调用 | 出现行 | 原版依赖 | 反转替代 |
|---|---|---|---|
| `allocDataPacket()` | L379, L619, L804, L845, L887 等 | 原版 MeshPacket 分配器 | `kernel/net/LoRaPhy::alloc()` |
| `meshtastic_MeshPacket` 结构 | 所有 send 路径 | 原版 protobuf 包 | 自研 `MeshRocPacket` 直驱射频 |
| `m->channel = channels.getPrimaryIndex()` | L384, L627-891 多处 | 原版 `Channels` 模块 | 自研信道表（默认主信道 0）；反转后保留"主信道"概念但归 `MeshROCConfig` |
| `config.lora.hop_limit` + `Default::getConfiguredOrDefaultHopLimit()` | L632, L806-892 多处 | 原版 `Config` + `Default` | `MeshROCConfig.lora.hopLimit`（§12.4 已含 `rap` 但 hopLimit 需补字段） |
| `m->priority = meshtastic_MeshPacket_Priority_DEFAULT` | L386 | 原版优先级枚举 | 自研 `MeshRocPriority`（§13.1 bit3-4）映射 |
| `m->decoded.portnum = TEXT_MESSAGE_APP` | L382 | 原版 PortNum | 自研端口或固定自研 port |
| `moduleConfig.mqtt.enabled` | L660, L684 | 原版 `ModuleConfig` | 自研 MQTT 桥接开关，`MeshROCConfig.mqtt` |
| `SinglePortModule` 基类 | `MeshRocModule.h` L257 | 原版模块框架 | 反转后不挂原版模块链，改为 `kernel/net/` 直接处理 |

### 14.3 兼容层保留项（反转后仍要接原版邻居，不能丢）
- **主信道投递**（`channels.getPrimaryIndex()`）：自研帧经主信道广播，原版节点能收到但解析不了 → 走原版洪泛兜底（L280 注释已说明）。反转后兼容层需**保留主信道**，让原版节点仍能"听到"自研节点心跳（仅当 compat 开启）。
- **双层跳数协同**（L629-638）：datapack 帧 `max_hop` 主导，底层 `hop_limit ≥ max_hop`。反转后 `kernel/net` 的 LoRa 投递直接取 `max(effHop, datapack.max_hop)`，不再经原版 `hop_limit` 中转。
- **文本消息走原版端口**（L382）：反转后自研文本用 `MESHROC_TLV_TEXT_MSG(0x06)`，不再借 `TEXT_MESSAGE_APP`，但 compat 层可双发原版端口以保持与原版 APP 互通。

### 14.4 反转工作量分级
| 级别 | 内容 |
|---|---|
| 🟢 零依赖平移 | `MeshRocCodec`、TLV 表、`MeshRocPacket` 结构、RAP 常量/状态机、自适应跳数算法、SNR 择优算法 |
| 🟡 需改数据结构 | `MeshRocRole` 扩六值（§11）、`MeshROCConfig` 增 `lora.hopLimit`/信道表/MQTT 开关（§12.4 补） |
| 🔴 断奶原版传输层 | 替换 `allocDataPacket`→自研射频分配、`meshtastic_MeshPacket`→`MeshRocPacket` 直驱、退出 `SinglePortModule` 基类、信道/跳数/优先级从原版枚举改为自研 |
| 🟠 兼容层新建 | 主信道双发、原版端口 TEXT 互通、原版节点角色映射（§11.5） |

### 14.5 反转前必须补齐的 config 字段（§12.4 修订）
`MeshROCConfig` JSON 当前缺以下反转必需项，补全：
```json
{
  "lora": { "hopLimit": 7, "channel": 0, "region": "CN_470_IX" },
  "mqtt": { "enabled": false, "broker": "", "topicPrefix": "meshroc" },
  "compat": { "emitNativeTextPort": true, "dualSendChannel0": true }
}
```
> 反转后 `hopLimit` 取代 `config.lora.hop_limit`；`channel` 取代 `channels.getPrimaryIndex()`；`mqtt` 取代 `moduleConfig.mqtt`；`compat.*` 控制与原版节点的互通行为（§14.3）。

---

## 15. 原创优化方案规划（结合 MeshROC 网站承诺）

> 本章把网站 `docs/` 与 `blog/` 对外承诺的优化点，与 §13/§14 的真实资产对齐，**逐条标注落地状态**，并规划反转后**真正的原创优化增量**。
> 关键结论先行：**网站承诺了 11 大类增强，但当前固件只真正落地了其中 3 类（分层骨干、自适应跳数、SNR 择优），其余 8 类均为"文档先行、代码缺失"**。反转必须把这个落差补齐或明示降级。

### 15.1 网站承诺 vs 资产落地状态对照（**按固件代码真实状态判定**）

> 状态三态定义（以 `src/kernel/` 真实代码为准，不以网站文档措辞为准）：
> - ✅ **已落地**：配置字段 + 处理算法均已实现（可编译、可被调用路径触发）。
> - 🔧 **配置已建 / 算法未实现**：`MeshROCConfig` 中对应开关字段已定义、可被读写，
>        但**没有任何处理算法消费该字段**（即设了也不生效）。属于"接口先行、逻辑缺失"。
> - ❌ **未启动**：连配置字段都未定义，无任何代码痕迹。
>
> 关键纠正（相较于初版"已落地 3 类"的乐观判定）：初版把"配置开关存在"误判为"已落地"。
> 实际核查 `src/kernel/`：O1~O8 的 **config 字段已全部建好（§14.5/§15.3）**，但**纯算法仅 O1/O4/O5 已补实现**，
> O3/O7/O8 仅有字段、O2/O6 字段+骨架、`adaptiveHopLimit`/`recordLinkSnr` 是反转前已存在代码的平移。
> 故真实状态如下：

| # | 网站承诺的优化（来源） | 固件代码真实状态（以 `src/kernel/` 为准） | 落地状态 |
|---|---|---|---|
| 1 | 分层骨干路由（mesh-alg / backbone / comparison） | `MeshRocRole`(6值) + `isRelayAllowed()` 已落地，`kernel/config/MeshROCConfig.h` | ✅ 已落地 |
| 2 | 自适应跳数（lora / mesh-alg） | `NextHopRouter::adaptiveHopLimit`/`estNetDiameter`/`effectiveHopLimit` 已落地 | ✅ 已落地 |
| 3 | 信噪比择优路径（mesh-alg / comparison） | `PeerCaps::recordLinkSnr`/`getPeerSnr`/`routeMetric` 已落地（退避偏置级，非全选路） | ✅ 已落地（部分语义） |
| 4 | 动态分片时隙调度 / TDMA（comparison） | `MeshROCConfig.rf.tdma{enabled,slotCount}` 字段已建；`kernel/mac/` **无任何时隙算法** | 🔧 配置已建 / 算法缺失 |
| 5 | 休眠同步帧 / 整网唤醒窗口（comparison） | 仅 Sensor 单节点休眠防丢包，无 `SYNC_BEACON`/`kernel/power/sleep` 同步算法 | ❌ 未启动 |
| 6 | 常驻链路探测 + 多地貌射频模板（comparison 九类） | `MeshROCConfig.rf.envProfile` 字段已建；`kernel/rf/EnvProfile` **已实现采集→模板查表**（O1 已补） | ✅ 已落地（O1） |
| 7 | AES-256-GCM + ECDH-P256（comparison 七） | `MeshROCConfig.crypto{mode=GCM_ECDH,pskFallback}` 字段已建；`kernel/crypto/` **无 GCM 实现** | 🔧 配置已建 / 算法缺失 |
| 8 | 大包分片重组 + 单分片重传（comparison 五） | `MeshROCConfig.rf.fragEnabled` 字段已建；`kernel/net/Reassembler` **已实现分片头+环形重组**（O5 已补） | ✅ 已落地（O5） |
| 9 | 分布式离线缓存 / 存储转发（comparison 四） | `MeshROCConfig.offlineCache{enabled,replicas}` 字段已建；**无缓存指派/转发算法** | 🔧 配置已建 / 算法缺失 |
| 10 | 分级 ACK（告警>位置>普通）（comparison 五） | `MeshROCConfig.ack.priorityLevels` 字段已建；`kernel/net/AckPolicy` **已实现优先级→超时/重传映射**（O4 已补） | ✅ 已落地（O4） |
| 11 | 智能电源管理（低电量降功率/关中继）（comparison 八） | `MeshROCConfig.power{smartManagement,lowBattRelayOff}` 字段已建；**无驱动/降功率逻辑** | 🔧 配置已建 / 算法缺失 |

> 结论：网站 11 类承诺中，**真正闭环（配置+算法）4 类**（#1/#2/#3/#6/#8/#10 中 #6/#8/#10 经本次补实现后升级为 ✅，合计 #1/#2/#3/#6/#8/#10 共 6 类）；
> **配置先行但算法缺失 4 类**（#4/#7/#9/#11）；**未启动 1 类**（#5）。
> 与初版"只落地 3 类"相比，差异完全来自**本次把 O1/O4/O5 算法补实现**并把状态以固件代码为准重新判定。

### 15.2 反转原创优化增量（按优先级）
> 以下为反转后**应新做**的原创优化，每一项都对应网站承诺且当前代码缺失。

**P0（核心差异化）**
- ✅ **O1 多地貌射频模板引擎**（对应 #6，已落地）：`kernel/rf/EnvProfile.{h,cpp}` 已实现——常驻采集 RSSI/SNR/噪声底/丢包率，按阈值自动切换 SF/BW/前导码/功率预设（高山密林/河谷/沿海/戈壁/高原/城镇）；`applyTo()` 写回 `LoraConfig`。配置项 `MeshROCConfig.rf.envProfile`（`auto` 时自适配，`fixed` 时保持中性预设）。
- **O2 动态分片时隙调度**（对应 #4，🔧算法缺失）：`MeshROCConfig.rf.tdma{enabled,slotCount}` 字段已建，但 `kernel/mac/` 无时隙算法。规划：在 `kernel/mac/` 新增轻量 TDMA——按节点数/拥堵度（ChUtil 估计）动态调整时隙数；告警/求救帧走专属高优先时隙。保留与原版 ALOHA 的兼容窗口。
- **O3 GCM 加密层**（对应 #7，🔧算法缺失）：`MeshROCConfig.crypto{mode=GCM_ECDH,pskFallback}` 字段已建，但 `kernel/crypto/` 无 GCM 实现。规划：`kernel/crypto/` 实现 AES-256-GCM + ECDH-P256 会话密钥，每报文独立 nonce；原版 PSK/CTR 降级为 compat 通道（§14.3）。

**P1（体验增强）**
- ✅ **O4 分级 ACK**（对应 #10，已落地）：`kernel/net/AckPolicy.{h,cpp}` 已实现——复用 `ctrl_flag` bit5 `wantAck`，优先级→ACK 超时/重传次数映射（告警/求救 800ms/5 次、位置 2s/3 次、普通 3s/1~2 次）。`priorityLevels=false` 时退化为统一策略。
- ✅ **O5 大包分片重组**（对应 #8，已落地）：`MeshRocPacket` 已扩展 `FRAG_HEADER(0x14)/FRAG_NACK(0x15)` TLV 与 `FragHeader`；`kernel/net/Reassembler.{h,cpp}` 已实现环形分组重组 + 选择性重传 NACK 生成。单分片丢失触发选择性重传。
- **O6 休眠同步帧**（对应 #5，❌未启动）：规划：骨干周期发 `SYNC_BEACON`，终端据此对齐唤醒窗口；收敛传感网空口占用。
- **O7 分布式离线缓存**（对应 #9，🔧算法缺失）：`MeshROCConfig.offlineCache{enabled,replicas}` 字段已建，但无缓存指派/转发算法。规划：RAP 归属表扩展"缓存指派"位，多中继分担离线消息，不集中于单点。

**P2（电源/运维，补承诺）**
- **O8 智能电源管理**（对应 #11，🔧算法缺失）：`MeshROCConfig.power{smartManagement,lowBattRelayOff}` 字段已建，但无驱动/降功率逻辑。规划：`kernel/power/` 读 IP5326/MAX17055，低电量自动降功率、减探测频次、撤销中继权限（置 `isRelayAllowed=false` 临时降级）。

### 15.3 反转后 config 需新增的优化开关
在 §14.5 `MeshROCConfig` 基础上补充：
```json
{
  "rf": { "envProfile": "auto", "tdma": { "enabled": false, "slotCount": 8 }, "fragEnabled": true },
  "crypto": { "mode": "gcm", "pskFallback": true },
  "ack": { "priorityLevels": true },
  "power": { "smartManagement": true, "lowBattRelayOff": true },
  "offlineCache": { "enabled": false, "replicas": 2 }
}
```
> `envProfile=auto` 启用 O1；`tdma.enabled` 启用 O2；`crypto.mode=gcm` 启用 O3；`fragEnabled` 启用 O5；`ack.priorityLevels` 启用 O4；`power.smartManagement` 启用 O8；`offlineCache` 启用 O7。

### 15.4 文档与实现的契约一致性要求
- **铁律（本次纠正核心）**：§15.1 的"落地状态"列**只以 `src/kernel/` 编译可见、可被调用路径触发的代码为准**，不以网站 `docs/`、`blog/` 的措辞、也不以"配置字段已定义"为准。"配置字段已建但无消费算法"必须标 🔧 而非 ✅——这正是初版误判"已落地 3 类"的教训。
- 反转后**每上线一项 O1~O8，必须在 `docs/comparison.mdx` 与 `docs/overview/mesh-alg.mdx` 标注"已实现/实验性/规划中"状态**，杜绝"文档承诺超前于代码"。
- 未实现的承诺项（O2/O3/O6/O7/O8 当前均算法缺失或全缺）在发布说明中**明示为路线图**，不得作为已交付特性宣传。
- 这与 §12.7 "不破坏原版兼容" 一致：所有原创优化均为**自研通道叠加**，原版设备按原规则工作。
- 本契约 §15.1 状态列是**对外承诺一致性的唯一真相源**，网站文档须回引此表，不得自行宣称更高完成度。
