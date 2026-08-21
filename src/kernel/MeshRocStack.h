#pragma once
#include <cstdint>
#include <cstddef>
#include "kernel/config/MeshROCConfig.h"
#include "kernel/net/MeshRocPacket.h"
#include "kernel/net/MeshRocCodec.h"
#include "kernel/net/Reassembler.h"
#include "kernel/net/HopPlanner.h"
#include "kernel/net/AckPolicy.h"
#include "kernel/net/Router.h"
#include "kernel/net/rap/RapStateMachine.h"
#include "kernel/net/rap/LinkQuality.h"
#include "kernel/net/TdmaScheduler.h"
#include "kernel/net/CryptoHook.h"
#include "kernel/rf/EnvProfile.h"
#include "kernel/rf/AirtimeModel.h"

/**
 * MeshRocStack：自研 MeshRocPacket 协议栈引擎（原创优化 O1/O2/O3/O4/O5/归属 的逻辑中枢）
 *
 * 职责：把原本孤立的 kernel/ 模块（EnvProfile / AckPolicy / Reassembler / HopPlanner /
 * RapStateMachine / TdmaScheduler / CryptoHook）串成一条可运行管线。
 *
 * 关键边界（用户 2026-08-16 明确）：
 *   - **先不碰射频驱动层**。本引擎是唯一"字节出口"的 owner：所有真正发字节的动作归集到
 *     虚钩子 sendRaw()，默认空实现 + // TODO(rf):。后续接入原版 SX126xInterface 时，只需在
 *     派生类里实现 sendRaw()，把字节交给 radio，整条协议栈即被射频层驱动，无需改其他逻辑。
 *   - 所有"射频相关参数"（当前 ModemPreset、信道利用率、slotTime）通过虚钩子/读 EnvProfile
 *     意图**读取**，不在此写 SF/BW/功率（O1 只产出"预设意图"，由射频层下调 reconfigure()）。
 *   - O4 重传基准**委托原版同构算法**（AirtimeModel 复刻 getRetransmissionMsec），不另立超时死表。
 *
 * 参考契约 §15.2 / §15.3，及对原版 RadioInterface / ReliableRouter 的借鉴。
 */
namespace meshroc {

class MeshRocStack {
public:
    // 应用层收包回调（重组完成或普通包抵达时触发）。可在派生类重载。
    struct PacketReceived {
        uint16_t src;
        const uint8_t* payload;
        uint16_t payloadLen;
        net::MeshRocPriority priority;
        bool wantAck;
    };

    MeshRocStack(const config::MeshROCConfig& cfg, uint16_t myAddr, bool isBackbone = false);

    uint16_t myAddr() const { return myAddr_; }

    // 配置只读访问（供 UI 等宿主层读取角色/兼容开关）
    const config::MeshROCConfig& cfg() const { return cfg_; }

    // 已确认 MeshROC 对端数（链路质量表，有效期内）
    uint8_t meshRocPeerCount(uint32_t nowMs) const { return link_.countMeshRocPeers(nowMs); }

    // ---- 应用层入口 ----
    // 发送载荷。内部完成：O3 加密 -> O5 分片(若超长) -> 路由跳数(O1路由) ->
    // O4 ACK 策略 -> 构造 MeshRocPacket -> sendRaw。
    // 返回 false 表示载荷非法或当前 TDMA 非本节点窗口（O2 限制）。
    bool sendPayload(uint16_t dst, const uint8_t* payload, uint16_t len,
                     net::MeshRocPriority prio = net::MeshRocPriority::LOW,
                     bool wantAck = false);

    // ---- 射频层回调（后续接入时调用）----
    // 把从射频收到的原始字节喂给协议栈（解码 + 分发 RAP / 分片重组 / 应用）。
    void ingestRaw(const uint8_t* frame, uint16_t len, uint32_t nowMs, int8_t rxSnr = 0);

    // ---- 周期性驱动（引擎 tick）----
    // 驱动：环境采样(O1) / RAP 状态机(O归属) / TDMA 时隙(O2 窗口查询) /
    //       分片重组超时清理(O5) / 重传超时(O4)。
    void tick(uint32_t nowMs);

    // 应用层可重载：收包通知
    virtual void onPacket(const PacketReceived& pkt) { (void)pkt; }

    // RAP 状态查询（归属层）
    net::rap::RapTerminalState rapState() const { return rap_.state(); }
    uint16_t attachedBackbone() const { return rap_.attachedBackbone(); }
    const rf::EnvProfile& env() const { return env_; }
    const rf::AirtimeModel& airtime() const { return airtime_; }

    // 路由查询/失效（PHASE3 ACK 超时回退）：上层可靠发送失败时调用
    const net::Router& router() const { return router_; }
    void notifyDeliveryFailure(uint16_t dst) { router_.onAckTimeout(dst); }

protected:
    // 唯一射频字节出口（本次空实现，TODO(rf): 接入 SX126xInterface）
    virtual void sendRaw(const uint8_t* bytes, uint16_t len) { (void)bytes; (void)len; /* TODO(rf): 交给原版 RadioInterface 发送 */ }

    // 当前信道利用率（0..100），默认 0；射频层接入后可重载返回真实值供 O4 基准
    virtual uint8_t channelUtilization() const { return 0; }

    // 当前 slotTime（ms），默认 13（与原版一致）；射频层可重载
    virtual uint32_t slotTimeMsec() const { return rf::AirtimeModel::DEFAULT_SLOT_TIME_MSEC; }

private:
    // 发送单个分片/整包：构造 MeshRocPacket 并 sendRaw
    bool emitSingle(uint16_t dst, const uint8_t* data, uint16_t len,
                    net::MeshRocPacketType opcode, net::MeshRocPriority prio,
                    bool wantAck, uint8_t hopLimit, net::MeshRocTlv payloadTag);

    // 发送端分片（O5）：把 payload 切成 <=MAX_PAYLOAD 的片，每片带 FRAG_HEADER TLV
    bool sendFragmented(uint16_t dst, const uint8_t* payload, uint16_t len,
                        net::MeshRocPriority prio, bool wantAck, uint8_t hopLimit,
                        uint16_t fragId);

    // 把 RAP 帧（来自 RapStateMachine）包成 MeshRocPacket(OPCODE_RAP) 发出
    void sendRapFrame(const uint8_t* rapBytes, uint16_t rapLen, uint32_t nowMs);

    // RAP 引擎发送回调适配器（RapSendFn 签名）
    static void rapSendFn(void* ctx, const uint8_t* rapBody, uint16_t len, uint16_t dstShort);

    // 四相混合路由发送回调适配器（RouteSendFn 签名）→ 转发到 sendRaw
    static void routeSendFn(void* ctx, const uint8_t* frame, uint16_t len);

    // 处理收到的 RAP 帧（payload 内已含 RAP 子类型字节）
    void handleRap(const uint8_t* payload, uint16_t len, uint32_t nowMs, uint16_t fromNode);

    // 处理收到的数据帧的路由分发（D2：route_mode 0/1/2 → 四相混合路由 PHASE1/2/4）
    // frame 为完整空中帧（10B头+TLV+2B CRC，已校验 CRC）。
    void handleRoute(const net::MeshRocPacket& pkt, const uint8_t* frame, uint16_t frameLen,
                     uint32_t nowMs);

    // 处理收到的普通/分片包：分片走 Reassembler，重组完成或普通包触发 onPacket
    void handleData(const net::MeshRocPacket& pkt, const uint8_t* tlv,
                    uint16_t tlvLen, uint32_t nowMs);

    // 处理收到的 FRAG_NACK（选择性重传请求）：重发本组缺失分片
    void handleFragNack(const net::MeshRocPacket& pkt, const uint8_t* tlv,
                        uint16_t tlvLen, uint32_t nowMs);

    // 构造一个 FRAG_HEADER 分片的完整帧（供发送与重传复用）
    uint16_t buildFragmentFrame(uint8_t* out, uint16_t outCap, uint16_t dst,
                                const uint8_t* fragVal, uint16_t fragValLen,
                                net::MeshRocPriority prio, bool wantAck, uint8_t hopLimit,
                                uint8_t seq);

    // 接收端重组未齐时，给发送方回 FRAG_NACK（声明缺失分片）
    void sendFragNack(uint16_t dst, uint16_t fragId, const uint8_t* missingSeqs,
                      uint8_t missingCount);

    const config::MeshROCConfig& cfg_;
    uint16_t myAddr_;

    rf::EnvProfile     env_;
    net::Reassembler   reassembler_;
    net::HopPlanner    hopPlanner_;  // O1 跳数上限规划（effectiveHopLimit）
    net::Router        router_;     // 四相混合路由（平移 MeshRocModule，PHASE1/2/4）
    net::AckPolicy     ack_;
    net::rap::LinkQuality link_;        // 链路质量表（SNR 择优/归属候选）
    net::rap::RapStateMachine rap_;
    net::TdmaScheduler tdma_;
    net::CryptoHook    crypto_;
    rf::AirtimeModel   airtime_;  // 纯函数工具，无状态

    // 重组输出缓冲（定长，零动态分配）
    uint8_t reassembleBuf_[net::Reassembler::MAX_PAYLOAD];
protected:
    // 编码缓冲（定长，最大帧 = 头(10) + 最大载荷TLV + CRC16(2)）。
    // protected：派生类（RadioMeshRocBridge）做帧长上限校验需要它，但不对外暴露。
    static constexpr uint16_t MAX_FRAME = net::Reassembler::MAX_PAYLOAD + 64;
private:
    uint8_t encodeBuf_[MAX_FRAME];

    uint16_t fragIdSeq_ = 0;          // 发送端分片组 ID 发生器
    uint16_t seq_ = 0;                // MeshRocPacket 序列号发生器

    // ---- 发送端分片重传状态（FRAG_NACK 选择性重传） ----
    // 发送大包后，按 fragId 缓存各分片密文与元数据，等待对端 NACK 重传缺失片，
    // 或超时（未再收到 NACK）即视为对端已收齐/放弃，清理缓存。
    static constexpr uint16_t kMaxFragChunkPayload = net::Reassembler::MAX_PAYLOAD - 16; // 单分片载荷上限
    struct PendingFrag {
        uint16_t dst = 0;                       // 接收方地址
        uint16_t fragId = 0;                    // 本组分片 id
        uint8_t chunkCap = 0;                   // 单分片有效载荷上限
        uint8_t total = 0;                      // 分片总数
        uint8_t count = 0;                      // 已缓存分片数
        bool missing[net::Reassembler::MAX_FRAG_TOTAL] = {false}; // 每片是否仍需重发
        const uint8_t* chunk[net::Reassembler::MAX_FRAG_TOTAL] = {nullptr}; // 指向 fragStore_
        uint16_t chunkLen[net::Reassembler::MAX_FRAG_TOTAL] = {0};
        uint32_t lastSentMs = 0;                // 最近一次（重）发送时刻
        uint32_t retransMsec = 0;               // O4 基准重传超时（airtime+CW 推导，委托 AckPolicy）
        uint8_t retries = 0;                    // 已重传次数
        bool active = false;
    };
    static constexpr uint8_t kMaxPendingFragGroups = 2;  // 同时缓存的发送分片组（压 RAM：原 4）
    PendingFrag pendingFrags_[kMaxPendingFragGroups];
    // 分片密文暂存（sendFragmented 的栈缓冲在重传时需持久，故搬到这里）
    uint8_t fragStore_[kMaxPendingFragGroups][net::Reassembler::MAX_FRAG_TOTAL][kMaxFragChunkPayload];
    uint32_t currentTickApprox_ = 0;  // 最近一次 tick 时刻（供 sendPayload 查询 TDMA 窗口）
    uint32_t lastEnvFeedMs_ = 0;      // 上次环境采样时刻
    uint32_t lastReassExpireMs_ = 0;  // 上次重组超时清理
    static constexpr uint32_t ENV_FEED_PERIOD_MS = 1000;
    static constexpr uint32_t REASS_EXPIRE_PERIOD_MS = 500;
    // 单帧载荷阈值：超过则走 O5 分片（留足 TLV 头与 CRC 余量）
    static constexpr uint16_t MAX_PAYLOAD_FRAG_THRESHOLD = net::Reassembler::MAX_PAYLOAD - 16;
};

}  // namespace meshroc
