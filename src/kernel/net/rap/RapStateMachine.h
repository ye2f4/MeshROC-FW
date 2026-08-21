#pragma once
#include <cstdint>
#include <cstddef>
#include "kernel/net/rap/RapProtocol.h"  // rap::RapKind / RapTerminalState / RapTlv / 常量
#include "kernel/config/MeshROCConfig.h"  // config::MeshRocRole
#include "kernel/net/rap/LinkQuality.h"

/**
 * RapStateMachine：归属协议完整引擎（原创优化 "O归属" —— MeshROC 私有 RAP 层）
 *
 * 平移自 MeshRocModule 的 RAP 实现（datapack.txt R6 / route.txt §10.5），去除
 * Meshtastic 依赖（nodeDB/config.meshtastic 等），统一以 kernel 为唯一真相源：
 *   - 角色 = config::MeshRocRole（含 DTU=4 / GATEWAY=5，见 MeshROCConfig.h §11.3）
 *   - 短地址 = 构造注入的 myAddr_
 *   - 时间 = tick/onRapFrame 的 nowMs 参数（不依赖 millis()）
 *   - 链路 SNR = 注入的 LinkQuality 引用
 *   - 帧发送 = 注入的 SendFn 回调（引擎经 RadioMeshRocBridge::startSendRaw 直驱射频）
 *
 * 帧封装：本引擎输出"RAP 消息体"（即 RapTlv::KIND=0x20 的 TLV value）：
 *   [rapKind u8][src u16BE][dst u16BE]  （共 5 字节，大端，D3）
 * 引擎负责把它包进 MeshRocPacket(opcode=RAP) 的 KIND TLV 发出。
 *
 * 与原版关系：RAP 是 MeshROC 私有"终端-骨干"归属层，原版 Meshtastic 无对应概念；
 * 故完全独立于原版 Router，仅在字节进出层面通过引擎 sendRaw/ingestRaw 与外界交互。
 * 参考契约 §15.2-O归属 / §13.2 rap。
 */
namespace meshroc::net::rap {

// 对端 BACKBONE 邻居表项（仅 BACKBONE 维护）
struct RapNeighborEntry {
    uint32_t backbone   = 0;   // 对端完整 NodeNum
    int8_t   snrMyToIt  = 0;   // 我收它的 SNR
    int8_t   snrItToMe  = 0;   // 它回报收我的 SNR（来自 HELLO_ACK 的 RAP_NEIGHBOR_SNR）
    int8_t   linkCost   = 0;   // 双向代价 = min(snrMyToIt, snrItToMe)
    uint32_t lastHelloAt = 0;  // 收到 HELLO 的时间戳（ms）
    uint32_t lastAckAt   = 0;  // 收到 HELLO_ACK 的时间戳（ms，准入标志）
    bool     acked       = false; // 是否回过 HELLO_ACK（未回 ACK 不算邻居，排除原版 ROUTER）
    uint16_t tableVersion = 0; // 最后已知对端归属表版本
};

// 归属表项（BACKBONE 维护下属；终端侧也用同一结构记自己归属）
struct RapOwnerEntry {
    uint32_t node           = 0;    // 被归属节点完整 NodeNum
    uint32_t ownerBackbone  = 0;    // 归属的 BACKBONE
    uint16_t attachSeq      = 0;    // 归属序号，单调递增
    uint32_t ttlExpireAt    = 0;    // 租期到期时间戳（ms），软状态靠 Keepalive 续租
    uint8_t  roleClass      = 0;    // RAP_ROLE_CLASS（= config::MeshRocRole 整数值）
    uint8_t  failCount      = 0;    // 定向投递连续失败计数（BACKBONE 侧用于路由失效）
};

// 终端侧本地状态机
struct RapLocalState {
    RapTerminalState state = RapTerminalState::SCANNING; // 终端状态机
    uint32_t attachedTo     = 0;    // 当前归属 BACKBONE（0 = 无）
    uint16_t attachSeq      = 0;    // 本机归属序号（每次切换 +1）
    uint32_t lastKeepaliveAt= 0;    // 上次续租时间戳
    uint32_t lastEvalAt     = 0;    // 上次评估时间戳
    uint32_t minDwellUntil  = 0;    // 最短驻留到期（铁律三 c）
    int8_t   curSnr         = 0;    // 当前归属链路 SNR
    int8_t   hystSamples    = 0;    // 连续满足迟滞(a)的采样计数（铁律三 b）
    uint32_t lastAttachReqAt= 0;    // 上次发 ATTACH_REQ 时间戳
};

// RAP 帧发送回调：引擎构造好 RAP 消息体后，由宿主发包。
// dstShort=0xFFFF 表示广播。ctx 为宿主上下文（通常为 MeshRocStack*）。
using RapSendFn = void (*)(void* ctx, const uint8_t* rapBody, uint16_t len, uint16_t dstShort);

class RapStateMachine {
public:
    // 单个 RAP 消息体最大长度（KIND TLV value 容量）
    static constexpr uint8_t kRapMsgMax = 32;

    RapStateMachine(uint16_t myAddr, config::MeshRocRole myRole, LinkQuality& link, RapSendFn sendFn, void* ctx = nullptr)
        : myAddr_(myAddr), myRole_(myRole),
          isBackbone_(config::isRelayAllowed(myRole)),
          link_(link), sendFn_(sendFn), ctx_(ctx),
          state_(isBackbone_ ? RapTerminalState::ATTACHED : RapTerminalState::SCANNING),
          lastActivityMs_(0), nextSendMs_(0) {}

    config::MeshRocRole myRole() const { return myRole_; }
    bool                isBackbone() const { return isBackbone_; }
    RapTerminalState    state() const { return state_; }
    uint16_t            myAddr() const { return myAddr_; }
    uint16_t            attachedBackbone() const { return static_cast<uint16_t>(rapLocal_.attachedTo & 0xFFFF); }

    // 引擎周期性调用：返回此刻需要发出的 RAP 消息体（写入 outBuf，<=kRapMsgMax）。
    // 返回写入字节数（0=无需发）。
    uint16_t tick(uint32_t nowMs, uint8_t* outBuf, uint16_t bufCap);

    // 处理收到的 RAP 消息体（KIND TLV 的 value 部分）。返回 true 表示状态迁移。
    bool onRapFrame(const uint8_t* msg, uint16_t len, uint32_t nowMs, uint32_t fromNode);

    // 让宿主在收到私有帧时喂 SNR（平移 recordLinkSnr）。
    void noteRx(uint32_t fromNode, int8_t snr, uint32_t nowMs) { link_.recordLinkSnr(fromNode, snr, nowMs); }

private:
    // ---- 内部发送辅助（构造 RAP 消息体并经 sendFn_ 发出） ----
    void sendRapFrame(rap::RapKind kind, uint16_t dstShort, const uint8_t* extra, uint8_t extraLen, uint32_t nowMs);
    void sendHello(bool fullAdv, uint32_t nowMs);
    void sendHelloAck(uint32_t dst, int8_t snrToIt, uint32_t nowMs);
    void sendAttachReq(uint32_t backbone, uint32_t nowMs);
    void sendAttachAck(uint32_t dst, uint16_t ttlGrant, uint16_t attachSeq, uint32_t nowMs);
    void sendKeepalive(bool standalone, uint32_t nowMs);
    void sendDetach(uint32_t oldOwner, uint32_t nowMs);
    void sendOwnershipAdv(const RapOwnerEntry* entries, uint8_t count, bool isDelete, uint32_t nowMs);
    void sendSyncReq(uint32_t dst, uint32_t nowMs);

    // ---- 接收处理 ----
    void onHello(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs);
    void onHelloAck(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs);
    void onAttachReq(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs);
    void onAttachAck(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs);
    void onKeepalive(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs);
    void onDetach(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs);
    void onOwnershipAdv(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs);
    void onSyncReq(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs);

    // ---- 表维护 ----
    RapNeighborEntry* findNeighbor(uint32_t bb);
    RapOwnerEntry*    findOwner(uint32_t node);
    RapOwnerEntry*    findRemoteOwner(uint32_t node);
    void pruneExpiredOwners(uint32_t nowMs);
    void pruneExpiredNeighbors(uint32_t nowMs);
    void bumpTableVersion();
    uint8_t localRoleClass() const;
    uint32_t rapTtlForRole(uint8_t role) const;
    static int8_t computeLinkCost(int8_t a, int8_t b) { return (a < b) ? a : b; }

    // ---- 5 字节 RAP 消息体构造（大端） ----
    uint16_t buildMsg(rap::RapKind kind, uint16_t dst, uint8_t* out, uint8_t& outLen);

    uint16_t          myAddr_;
    config::MeshRocRole myRole_;
    bool              isBackbone_;
    LinkQuality&      link_;
    RapSendFn         sendFn_;
    void*             ctx_ = nullptr;

    RapTerminalState  state_;
    uint32_t          lastActivityMs_;
    uint32_t          nextSendMs_;

    // 终端侧本地状态
    RapLocalState     rapLocal_;

    // 邻居 / 归属表
    RapNeighborEntry  neighbors_[MAX_NEIGHBORS];
    uint8_t           nNeighbors_ = 0;
    RapOwnerEntry     owners_[MAX_OWNED];
    uint8_t           nOwners_ = 0;
    RapOwnerEntry     remoteOwners_[MAX_REMOTE_OWNERS];
    uint8_t           nRemoteOwners_ = 0;
    uint16_t          rapTableVersion_ = 0;
    uint8_t           helloBackoffPow_ = 0;
};

}  // namespace meshroc::net::rap
