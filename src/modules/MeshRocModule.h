#pragma once

#include "SinglePortModule.h"
#include "meshUtils.h" // for portable htons/ntohs if available
#include "meshroc_brand.h" // MeshROC 品牌常量（系统名/社区名/配色/链接/许可）

/**
 * Mesh-ROC 定制通信协议模块
 * ===========================
 * 本协议严格遵循 E:/datapack.txt 的《mesh-ROC 定制通信协议完整文档》。
 *
 * 设计原则（与用户确认）：
 *   不改动原生 Meshtastic 的 MeshPacket / protobuf / 加密传输层。
 *   本模块把 datapack 定义的【10 字节固定包头 + TLV 变长载荷 + 2 字节 CRC16】
 *   作为一个完整的二进制 blob，封装进一个 Meshtastic 私有 portnum 的 Data.payload 中，
 *   由 Meshtastic 的既有路由 / 信道加密 / 重传机制负责传输。
 *
 * 这样即可在保持与官方 Meshtastic 网络兼容（可共存、可回退）的同时，
 * 让节点之间按照 Mesh-ROC 专有格式收发数据。
 *
 * 包结构（空中/内存统一）：
 *   Packet = Header[10] + Payload(N) + CRC16[2]
 *   总包最大长度 = 10 + 225 + 2 = 237 字节
 *   载荷（Payload）最大 225 字节，禁止 0x00 填充占位。
 *
 * 字节序（重要 / 待用户最终确认）：
 *   用户补充文档文字声称"全部 uint16 小端"，但 5 个真实示例字节流按大端解读
 *   时 src/dst/seq 才与文档文字吻合（心跳 src=0x000A、dst=0x0014），按小端则
 *   变成 0x0A00/0x1400 与文档矛盾。故本模块当前采用【大端】以匹配真实空中
 *   字节流；若最终确认走小端，只需翻转 MeshRocCodec 中 6 处 16 位字段字节序。
 *
 * 骨干-终端隔离：
 *   ctrl_flag.bit6 = 1 时，仅楼顶骨干中继节点允许转发；手持终端只接收、禁止中继。
 *   本模块在 handleReceived 中依据本节点角色（config.device.role）决定是否转发。
 */

// Mesh-ROC 私有 portnum（落在 portnums.proto 的 PRIVATE_APP=256 ~ MAX=511 区间）
#define MESHTASTIC_PORTNUM_MESHROC ((meshtastic_PortNum)300)

// datapack 协议常量
#define MESHROC_HEADER_LEN 10
#define MESHROC_MAX_PAYLOAD 225
#define MESHROC_MAX_PACKET (MESHROC_HEADER_LEN + MESHROC_MAX_PAYLOAD + 2) // 237
#define MESHROC_CRC_LEN 2
#define MESHROC_ADDR_INVALID 0xFFFF

// 数据包类型（ctrl_flag bit0-2）
typedef enum {
    MESHROC_TYPE_PRIVATE_MSG = 0, // 私聊消息
    MESHROC_TYPE_GROUP_MSG   = 1, // 群组广播消息
    MESHROC_TYPE_ACK         = 2, // ACK 应答包
    MESHROC_TYPE_ROUTE_PROBE = 3, // 路由探测包
    MESHROC_TYPE_TELEMETRY   = 4, // 传感器遥测包
    MESHROC_TYPE_HEARTBEAT   = 5, // 节点心跳包
    MESHROC_TYPE_ENCRYPTED   = 6, // 加密传输帧
    MESHROC_TYPE_RAP         = 7, // RAP 控制族（路由器归属协议，真实子类型见载荷首个 TLV 0x20 RAP_KIND）
} MeshRocPacketType;

// RAP 子类型（RAP_KIND 的 value，1 字节；datapack.txt R3）
typedef enum {
    RAP_HELLO = 1,        // BACKBONE 周期广播：我在，我是骨干
    RAP_HELLO_ACK = 2,    // BACKBONE 定向回应：我也在 + 我收你的 SNR
    RAP_ATTACH_REQ = 3,   // 终端请求归属到某 BACKBONE
    RAP_ATTACH_ACK = 4,   // BACKBONE 接受归属 + 下发续租 TTL
    RAP_KEEPALIVE = 5,    // 终端续租（可捎带在遥测/位置帧内）
    RAP_DETACH = 6,       // 终端主动注销（加速回收，非必需）
    RAP_OWNERSHIP_ADV = 7,// BACKBONE 向邻居通告归属变更（增量/全量 Delta）
    RAP_SYNC_REQ = 8,     // 请求对端全量重发归属表（版本号不连续时）
} RapKind;

// RAP 专用 TLV（0x20~0x2F 区间，与既有 0x01-0x13 不冲突；datapack.txt R4）
typedef enum {
    RAP_KIND = 0x20,            // len=1   value=子类型（见 RapKind）
    RAP_NEIGHBOR_SNR = 0x21,    // len=3   uint16BE 对端短地址 + int8 我收它的 SNR
    RAP_ROLE_CLASS = 0x22,      // len=1   0 BACKBONE/1 CLIENT/2 SENSOR/3 TRACKER/4 DTU/5 GATEWAY
    RAP_ATTACH_SEQ = 0x23,      // len=2   uint16BE 归属序号，单调递增（后到覆盖先到）
    RAP_TTL_GRANT = 0x24,       // len=2   uint16BE 授予的租期秒数
    RAP_OWNER_LIST = 0x25,      // len=N*4 uint16BE 终端短地址 + uint16BE attachSeq（条目数组）
    RAP_OWNER_DEL = 0x26,       // len=N*2 uint16BE 终端短地址数组（已离开我）
    RAP_TABLE_VERSION = 0x27,   // len=2   uint16BE 本机归属表版本号，每次变更 +1
    RAP_LINK_COST = 0x28,       // len=1   int8 双向链路代价 min(snr_AB,snr_BA)，单位 dB
} RapTlv;

// 角色分类（对应 RAP_ROLE_CLASS；datapack.txt R7）
typedef enum {
    RAP_ROLE_BACKBONE = 0,
    RAP_ROLE_CLIENT = 1,
    RAP_ROLE_SENSOR = 2,
    RAP_ROLE_TRACKER = 3,
    RAP_ROLE_DTU = 4,
    RAP_ROLE_GATEWAY = 5,
} RapRoleClass;

// ============================================================================
// RAP 实现常量（来源 datapack.txt R11 / route.txt §10.5，权威取值）
// ============================================================================
#define RAP_HELLO_PERIOD_MS        (90UL * 1000UL) // 90s（470MHz 占空比反推，下限64.4s+余量）
#define RAP_HELLO_JITTER_MS        (30UL * 1000UL) // 30s 随机上线抖动，防上线风暴碰撞
#define RAP_NEIGHBOR_TTL_MS        (270UL * 1000UL) // 3 × HELLO_PERIOD
#define RAP_FULL_ADV_PERIOD_MS     (30UL * 60UL * 1000UL) // 30min 低频全量通告（软状态收敛）
#define RAP_ADV_MAX_ENTRIES_PER_FRAME 16  // 470MHz 单次发射 ≤1s 硬约束推出（全量32条 ToA 超限，须分片）
#define RAP_MAX_NEIGHBORS         16   // 单蜂窝骨干密度
#define RAP_MAX_OWNED             32   // 单 BACKBONE 归属表上限
#define RAP_MAX_REMOTE_OWNERS     64   // 邻居通告汇总表
#define RAP_KEEPALIVE_CLIENT_MS    (10UL * 60UL * 1000UL) // 10min（信道总负载修正后）
#define RAP_KEEPALIVE_DTU_MS       (10UL * 60UL * 1000UL) // 10min
#define RAP_KEEPALIVE_SENSOR_MS    (30UL * 60UL * 1000UL) // 30min（捎带）
#define RAP_TTL_CLIENT_MS          (30UL * 60UL * 1000UL) // 3 × Keepalive
#define RAP_TTL_DTU_MS             (30UL * 60UL * 1000UL) // 3 × Keepalive
#define RAP_TTL_SENSOR_MS          (2UL * 60UL * 60UL * 1000UL) // 2h
#define RAP_TTL_TRACKER_MS         (6UL * 60UL * 60UL * 1000UL) // 6h
#define RAP_HYST_SNR_DB            6    // 铁律三：候选优于当前至少 6dB
#define RAP_HYST_SAMPLES           3    // 铁律三：连续 3 次独立采样
#define RAP_MIN_DWELL_MS            (10UL * 60UL * 1000UL) // 铁律三：最短驻留 10min
#define RAP_ROUTE_FAIL_THRESHOLD   3    // 定向投递连续失败即清条目落回洪泛
#define RAP_MAX_HELLO_BACKOFF_MS    (12UL * 60UL * 1000UL) // 指数退避上限 12min（2^3 × 90s）
#define RAP_LBT_UTIL_THRESHOLD_PCT 40   // 信道利用率超 40% 推迟本次 HELLO
#define RAP_ATTACH_RETRY_MS        (30UL * 1000UL)
#define RAP_EVAL_INTERVAL_MS       (15UL * 1000UL)

// 优先级（ctrl_flag bit3-4）
typedef enum {
    MESHROC_PRIO_LOW      = 0, // 心跳、传感上报
    MESHROC_PRIO_NORMAL   = 1, // 日常文字消息
    MESHROC_PRIO_HIGH     = 2,
    MESHROC_PRIO_EMERGENCY = 3,
} MeshRocPriority;

// TLV Tag 清单（datapack 第三节 + route.txt 路由扩展）
typedef enum {
    MESHROC_TLV_BATT_VOLT     = 0x01, // 电池电压 (uint16 大端, ×100)
    MESHROC_TLV_SOLAR_VOLT    = 0x02, // 太阳能板输入电压 (uint16 大端, ×100)
    MESHROC_TLV_AHT20_TEMP    = 0x03, // AHT20 温度 (int16 大端, ×10)
    MESHROC_TLV_HUMIDITY      = 0x04, // 空气湿度 (uint16 大端, ×10)
    MESHROC_TLV_NODE_NAME     = 0x05, // 节点名称字符串
    MESHROC_TLV_TEXT_MSG      = 0x06, // UTF-8 文本消息
    // 路由相关扩展 TLV
    MESHROC_TLV_ROUTE_PATH    = 0x10, // 源路由跳点数组 (uint16 大端, 2字节对齐)
    MESHROC_TLV_REVERSE_PATH  = 0x11, // 反向路径 (ACK 中带回, uint16 大端数组)
    MESHROC_TLV_ROUTE_METRIC  = 0x12, // 路由度量 (avg_snr uint16 BE + 投递成功计数 uint16 BE)
    MESHROC_TLV_ROUTE_INVALID = 0x13, // 标记目的路由失效, 触发重新探测
} MeshRocTlvTag;

// 节点角色（存本地 flash, 不在包头内）
typedef enum {
    MESHROC_ROLE_BACKBONE = 0, // 楼顶骨干中继: 可执行完整路由/洪泛/源路由解析
    MESHROC_ROLE_CLIENT   = 1, // 手持终端: 收 bit6=1 包但不中继; 默认关闭 bit6=0 中继
    MESHROC_ROLE_GATEWAY  = 2, // 特殊骨干: 桥接 LoRa↔互联网 meshroc.cc.cd
} MeshRocRole;

/**
 * 10 字节固定包头（与 datapack 第四节 C 结构一致，字段顺序不可改动）
 */
typedef struct {
    uint8_t ctrl_flag;              // bit0-2 类型 / bit3-4 优先级 / bit5 ACK / bit6 中继权限 / bit7 压缩
    uint16_t src_addr;              // 源节点短地址
    uint16_t dst_addr;              // 目标节点短地址，0xFFFF=全网广播
    uint16_t seq;                   // 数据包序列号
    uint8_t max_hop;                // 最大转发跳数 0-63
    uint8_t snr;                    // SNR 信噪比预留
    uint8_t route_mode;             // 0=定向单播, 1=受限洪泛
    uint8_t payload[MESHROC_MAX_PAYLOAD]; // 变长 TLV 载荷
    uint16_t crc;                   // CRC16
} MeshRocPacket;

/**
 * 解析/构造辅助类
 */
class MeshRocCodec {
  public:
    // CRC16-MODBUS 校验（route.txt §2 权威定义）：poly=0x8005, init=0xFFFF,
    // xorout=0, ref_in/out=true。输入 = 10字节包头 + 全部TLV；CRC两字节本身不参与。
    // 注：route.txt 给出的 7 个测试向量 CRC 值与标准 MODBUS 算法均不吻合（疑似
    // 文档转录/手算错误），故本实现以"算法定义"为准而非那些向量值。
    static uint16_t crc16(const uint8_t *data, size_t len);

    // 把 ctrl_flag 各字段打包/解包
    static uint8_t makeCtrlFlag(MeshRocPacketType type, MeshRocPriority prio, bool wantAck, bool relayPerm,
                                bool compressed);
    static void parseCtrlFlag(uint8_t flag, MeshRocPacketType &type, MeshRocPriority &prio, bool &wantAck,
                              bool &relayPerm, bool &compressed);

    // 把 MeshRocPacket 序列化为 out（不含 struct padding，严格 10+plen+2 字节）。
    // 返回写入字节数，失败返回 0。
    static size_t serialize(const MeshRocPacket &pkt, uint8_t payloadLen, uint8_t *out, size_t outCap);

    // 从 buf 反序列化（buf 含 CRC）。成功返回 true 且通过 crc 校验。
    static bool deserialize(const uint8_t *buf, size_t bufLen, MeshRocPacket &pkt, uint8_t &payloadLen);

    // 在 payload 尾追加一条 TLV。返回新 payloadLen，超长返回 0。
    static uint8_t appendTlv(uint8_t *payload, uint8_t payloadLen, MeshRocTlvTag tag, const uint8_t *value,
                             uint8_t valueLen);
};

// 路由缓存条目：目的地址 → 源路由跳点序列 + 度量 + 老化计时
struct MeshRocRouteEntry {
    uint16_t dst;                       // 目的节点短地址
    uint16_t hops[MESHROC_MAX_PACKET/2];// 源路由跳点（uint16 大端, 2字节对齐）
    uint8_t  hopCount;                  // 跳数
    uint16_t avgSnr;                    // 平均 SNR（路由度量）
    uint16_t deliveryOk;                // 投递成功计数
    uint32_t expireAt;                  // 到期时间戳(ms), 默认 1200s
    bool     valid;                     // false = 已标记失效(0x13)
};

// 最近见过的包缓存（去重用）：key = (src_addr << 16) | seq
struct MeshRocSeenEntry {
    uint32_t key;
    uint32_t seenAt;
};

// ============================================================================
// RAP 数据结构（datapack.txt R6）
// ============================================================================

// 对端 BACKBONE 邻居表项（仅 BACKBONE 维护）
struct RapNeighborEntry {
    uint32_t backbone;       // 对端完整 NodeNum
    int8_t snrMyToIt;        // 我收它的 SNR
    int8_t snrItToMe;        // 它回报收我的 SNR（来自 HELLO_ACK 的 RAP_NEIGHBOR_SNR）
    int8_t linkCost;         // 双向代价 = min(snrMyToIt, snrItToMe)
    uint32_t lastHelloAt;    // 收到 HELLO 的时间戳（ms）
    uint32_t lastAckAt;      // 收到 HELLO_ACK 的时间戳（ms，准入标志）
    bool acked;              // 是否回过 HELLO_ACK（未回 ACK 不算邻居，排除原版 ROUTER）
    uint16_t tableVersion;   // 最后已知对端归属表版本
};

// 归属表项（BACKBONE 维护下属；终端侧也用同一结构记自己归属）
struct RapOwnerEntry {
    uint32_t node;           // 被归属节点完整 NodeNum（自维护完整，仅上空口降级短地址）
    uint32_t ownerBackbone;  // 归属的 BACKBONE（终端侧为自己当前 attachedTo）
    uint16_t attachSeq;      // 归属序号，单调递增
    uint32_t ttlExpireAt;    // 租期到期时间戳（ms），软状态靠 Keepalive 续租
    uint8_t roleClass;       // RAP_ROLE_CLASS
    uint8_t failCount;       // 定向投递连续失败计数（仅 BACKBONE 侧用于路由失效）
};

// 终端侧本地状态机
typedef enum {
    RAP_STATE_SCANNING = 0,  // 尚无归属
    RAP_STATE_ATTACHED = 1,  // 已归属
    RAP_STATE_EVALUATING = 2,// 发现更优候选，迟滞评估中
} RapTerminalState;

struct RapLocalState {
    RapTerminalState state;  // 终端状态机
    uint32_t attachedTo;     // 当前归属 BACKBONE（0 = 无）
    uint16_t attachSeq;      // 本机归属序号（每次切换 +1）
    uint32_t lastKeepaliveAt;// 上次续租时间戳
    uint32_t lastEvalAt;     // 上次评估时间戳
    uint32_t minDwellUntil;  // 最短驻留到期（铁律三 c）
    int8_t curSnr;           // 当前归属链路 SNR
    int8_t hystSamples;      // 连续满足迟滞(a)的采样计数（铁律三 b）
    uint32_t lastAttachReqAt;// 上次发 ATTACH_REQ 时间戳
};

class MeshRocModule : public SinglePortModule, private concurrency::OSThread {
  public:
    MeshRocModule();

    // 部署兼容性自检：检查会阻断 Mesh-ROC 跨跳收发的节点配置（rebroadcast_mode / MQTT）
    void logDeploymentWarnings();

    // ---- 向下兼容 Meshtastic：原生文本消息通道 ----
    // 用原生 portnum=1(TEXT_MESSAGE_APP) 发送 UTF-8 文本，普通 Meshtastic 节点可直接显示。
    // 这是"MeshROC 节点 ←→ 普通 Meshtastic 节点"最基本的消息收发兼容路径。
    bool sendNativeText(const char *text, uint32_t toNode = UINT32_MAX);

    // 是否启用原生兼容通道（向下兼容）。默认开启，便于混网部署。
    // 关闭后 MeshROC 节点仅使用私有端口 300 协议，与普通节点不再互通文本。
    bool nativeCompatEnabled = true;

    // ---- 按对端能力协商的智能文本发送 ----
    // 设计（与用户确认）：采用"默认走原生、对端是 MeshROC 节点才补 300 增强帧"的变体。
    //   - 单播：默认只发原生端口 1（普通节点直接显示，最省带宽）；
    //           当且仅当对端已被观测到发过端口 300 帧（确认是 MeshROC 节点）时，
    //           额外补发一份端口 300 增强帧（带 TLV / 源路由 / 传感器等增强能力）。
    //   - 广播：无法预知听众类型，故原生端口 1 + 端口 300 双发，保证混合网络全覆盖。
    // 这样纯 Meshtastic 网络里 MeshROC 节点表现=普通节点（零额外开销），
    // MeshROC 集群内才启用增强层，最贴合"兼容基础上的增强"定位。
    // nativeCompatEnabled=false 时退化为仅发端口 300（与普通节点不互通）。
    bool sendTextSmart(const char *text, uint32_t toNode = UINT32_MAX);

    // 查询/标记对端能力：是否观测到该节点发过 Mesh-ROC 私有帧(端口300)。
    // 普通 Meshtastic 节点永远不会发端口 300 帧，故"见过其发 300 帧"天然区分两类节点，
    // 无需在 NodeInfo 里塞自定义能力位（NodeInfo/User 无预留字段，加位会破坏协议兼容）。
    bool isPeerMeshRoc(uint32_t nodeNum) const;
    void markPeerMeshRoc(uint32_t nodeNum);

    // ---- 深度优化（结合官网文档 docs/ 落地）----
    // 1) 分层骨干路由角色固化：依据 config.device.role 把 localRole 推断为骨干/终端。
    void inferLocalRole();
    // 2) 自适应跳数：依据观测帧的 max_hop 估计网络直径，返回本节点应使用的 max_hop。
    uint8_t adaptiveHopLimit();
    // 3) 信噪比择优：把 Meshtastic 层 rx_snr 记录进路由度量/对端表，源路由选路择优。
    void recordLinkSnr(uint32_t fromNode, int8_t snr);
    // 取对端最近记录的最佳 SNR（无记录返回 0）
    int8_t getPeerSnr(uint32_t nodeNum) const;

    // ---- RAP 双栈公开接口（供 NextHopRouter 调用，route.txt §10.3）----
    // 返回目标节点的「唯一末字节」下一跳；若 RAP 无归属信息或不可达则返回 nullopt（落回洪泛）。
    std::optional<uint8_t> getRapNextHop(uint32_t to);
    // 由 NextHopRouter::noteRouteFailure 调用，记录定向投递失败（连续失败清条目落回洪泛）。
    void noteRapRouteFailure(uint32_t toNode);

    // ---- 屏幕 UI 帧（功能丰富：让 MeshROC 在设备上可见）----
    // 注册一个模块帧，显示系统名/固件版本/本机产品线角色/已确认 MeshROC 对端数/端口300活动。
    // 通过 MeshModule::GetMeshModulesWithUIFrames 自动注入屏幕帧列表（见 Screen.cpp）。
#if HAS_SCREEN
    virtual bool wantUIFrame() override { return true; }
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
    // 统计当前 peerCaps 中仍有效的 MeshROC 对端数量
    uint8_t countMeshRocPeers() const;
#endif

  protected:
    // 收到本模块监听的数据包时回调。
    // 本模块同时监听端口 300(Mesh-ROC 私有协议) 与端口 1(原生 TEXT_MESSAGE_APP)：
    //   - 端口 300：payload 即完整 datapack 二进制帧，走原有混合路由状态机。
    //   - 端口 1  ：普通 Meshtastic 节点发来的原生文本，走 onNativeText 兼容处理。
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

    // 重写 wantPacket：同时接收 Mesh-ROC 私有端口(300) 与 原生文本端口(1)。
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;

    // RAP 周期任务（OSThread）：BACKBONE 发 HELLO / 全量通告 / 过期扫描；终端发 Keepalive / 评估。
    virtual int32_t runOnce() override;

  private:
    // ---- 混合路由状态机 (route.txt §5) ----
    // PHASE1 洪泛探测：源发探测包 → 骨干中继记录路径 → 目标回 ACK(REVERSE_PATH)
    void onRouteProbe(const MeshRocPacket &pkt, uint8_t payloadLen);
    // PHASE2 业务单播：源路由包，严格按 0x10 ROUTE_PATH 跳点转发
    void onSourceRoute(const MeshRocPacket &pkt, uint8_t payloadLen);
    // PHASE3 失败回退：ACK 超时 → 标记失效 → 重新洪泛探测
    void onAckTimeout(uint16_t dst);
    // PHASE4 广播/心跳/遥测：受限洪泛, bit6=1 仅骨干中继
    void onFloodFrame(const MeshRocPacket &pkt, uint8_t payloadLen);

    // 解析 0x10/0x11 路径 TLV（uint16 大端数组）到 hops[]; 返回跳数, 0=失败
    uint8_t parsePathTlv(const uint8_t *val, uint8_t len, uint16_t *outHops, uint8_t maxHops);
    // 把 hops[] 写入 0x10 ROUTE_PATH TLV（大端, 2字节对齐）
    uint8_t buildPathTlv(uint8_t *payload, uint8_t payloadLen, const uint16_t *hops, uint8_t hopCount);

    // 路由缓存查询/写入/失效
    MeshRocRouteEntry *routeLookup(uint16_t dst);
    void routeStore(uint16_t dst, const uint16_t *hops, uint8_t hopCount, uint16_t avgSnr, uint16_t ok);
    void routeInvalidate(uint16_t dst);

    // 去重：返回 true 表示已见过(丢弃), false 表示新包(记录)
    bool seenCheck(uint16_t src, uint16_t seq);

    // ---- RAP（Router Attach Protocol，datapack.txt R1~R12）----
    // RAP 帧分发：handleReceived 在 ctrl_flag 类型==MESHROC_TYPE_RAP(7) 时调用。
    void onRapPacket(const MeshRocPacket &pkt, uint8_t payloadLen, const meshtastic_MeshPacket &mp);
    // RAP 发送构造
    void sendRapHello(bool fullAdv = false);
    void sendRapHelloAck(uint32_t dst, int8_t snrToIt);
    void sendRapAttachReq(uint32_t backbone);
    void sendRapAttachAck(uint32_t dst, uint16_t ttlGrant, uint16_t attachSeq);
    void sendRapKeepalive(bool standalone = true);
    void sendRapDetach(uint32_t oldOwner);
    void sendRapOwnershipAdv(const RapOwnerEntry *entries, uint8_t count, bool isDelete = false);
    void sendRapSyncReq(uint32_t dst);
    // RAP 接收处理
    void onRapHello(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from);
    void onRapHelloAck(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from);
    void onRapAttachReq(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from);
    void onRapAttachAck(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from);
    void onRapKeepalive(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from);
    void onRapDetach(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from);
    void onRapOwnershipAdv(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from);
    void onRapSyncReq(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from);
    // RAP 内部工具
    RapNeighborEntry *findNeighbor(uint32_t bb);
    RapOwnerEntry *findOwner(uint32_t node);
    RapOwnerEntry *findRemoteOwner(uint32_t node);
    void pruneExpiredOwners();
    void pruneExpiredNeighbors();
    void bumpTableVersion();
    int8_t computeLinkCost(int8_t a, int8_t b);
    uint32_t rapTtlForRole(uint8_t role);
    uint8_t localRoleClass() const;

  private:
    // 解析 datapack 帧（已校验 CRC），按 TLV 分发处理
    void onMeshRocFrame(const MeshRocPacket &pkt, uint8_t payloadLen, const meshtastic_MeshPacket &mp);

    // 向下兼容：处理普通 Meshtastic 节点发来的原生文本消息(portnum=1)。
    // 返回 CONTINUE 让 TextMessageModule 继续处理并显示，实现双向互通。
    ProcessMessage onNativeText(const meshtastic_MeshPacket &mp);

    // 本节点是否为楼顶骨干中继（决定是否允许转发中继包）
    bool isBackboneRelay() const;

    // 发送一个 datapack 帧（封装进 Meshtastic 包并发往 mesh）
    bool sendFrame(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t toNode);

    // 路由缓存表（容量受限，环型覆盖最旧条目）
    MeshRocRouteEntry routeCache[16];
    uint8_t           routeCacheIdx = 0;
    // 去重缓存
    MeshRocSeenEntry  seenCache[32];
    uint8_t           seenCacheIdx = 0;
    // 本节点角色（分层骨干路由核心：骨干转发 / 终端静默，见建站要求 §4.1）
    // 默认 BACKBONE 兼容旧逻辑；开机时由 inferLocalRole() 依据 config.device.role 固化：
    //   ROUTER/ROUTER_LATE → BACKBONE（山顶骨干/城市基站承担转发）
    //   CLIENT/CLIENT_MUTE → CLIENT（手持终端默认静默，仅收发不中继，省电+降信道冲突）
    MeshRocRole       localRole = MESHROC_ROLE_BACKBONE;
    bool              localRoleInited = false;

    // 自适应跳数（建站要求 §4.1 "自适应跳数"）：维护估计的网络直径，
    // 依据观测到的帧 max_hop 最大值 + 余量动态调整本节点发出的 max_hop，
    // 避免全网约设固定 hop 导致的小网浪费/大网截断。
    uint8_t           estNetDiameter = 0;
    uint32_t          diameterSampleAt = 0;
    static const uint32_t MESHROC_DIAMETER_TTL = 10UL * 60 * 1000;

    // ---- 对端能力表（按对端能力协商，避免无脑双发）----
    // 能力标志 = 是否观测到对端发送过 Mesh-ROC 私有帧(端口300)。
    // 普通 Meshtastic 节点永不发端口 300，故该标志天然区分两类节点。
    // lastSeen 用于老化：超过 MESHROC_PEER_CAP_TTL ms 未再见其发 300 帧则清掉标志，
    // 避免节点重启/固件变更后误判（参考 Meshtastic NodeInfo 6h 缓存思路，此处取较短）。
    static const uint32_t MESHROC_PEER_CAP_TTL = 6UL * 60 * 60 * 1000; // 6 小时
    static const uint8_t  MESHROC_MAX_PEERS   = 32;

    struct PeerCap {
        uint32_t nodeNum;
        bool     isMeshRoc;  // 见过它发的端口300帧
        uint32_t lastSeen;   // 最近观测 tick(ms)
        int8_t   bestSnr;    // 信噪比择优：观测到的最佳链路 SNR（文档"信噪比择优路径"）
    };
    PeerCap            peerCaps[MESHROC_MAX_PEERS];
    uint8_t            peerCapsIdx = 0;

    // ---- RAP 状态（datapack.txt R6）。BACKBONE 判定复用 localRole。----
    RapLocalState      rapLocal;                  // 终端侧状态（仅非 BACKBONE 使用）
    RapNeighborEntry   rapNeighbors[RAP_MAX_NEIGHBORS]; // BACKBONE 互相邻居表
    RapOwnerEntry      rapOwnerTable[RAP_MAX_OWNED];     // 本 BACKBONE 下属表
    RapOwnerEntry      rapRemoteOwners[RAP_MAX_REMOTE_OWNERS]; // 邻居通告的归属汇总
    uint8_t            nNeighbors = 0;
    uint8_t            nOwners = 0;
    uint8_t            nRemoteOwners = 0;
    uint16_t           rapTableVersion = 0;       // 本机归属表版本
    uint16_t           rapNextAttachSeq = 1;      // 本机发出的归属序号分配器
    uint32_t           lastHelloSentAt = 0;       // 上次 HELLO 时间戳
    uint8_t            helloBackoffPow = 0;       // 指数退避指数 k
    uint32_t           lastFullAdvAt = 0;         // 上次全量通告

    // 本机是否 RAP BACKBONE（ROUTER/ROUTER_LATE 角色）
    bool isBackbone() const { return localRole == MESHROC_ROLE_BACKBONE; }
};

// 全局单例（定义于 MeshRocModule.cpp），供其它模块/服务自动调用 MeshROC 能力
// （如 MeshService::handleToRadio 接管文本发送时调用 sendTextSmart）。
extern MeshRocModule *meshRocModule;
