#pragma once
#include <cstdint>
#include <cstddef>
#include "config/MeshROCConfig.h"
#include "net/MeshRocPacket.h"
#include "net/MeshRocCodec.h"
#include "net/Reassembler.h"
#include "net/NextHopRouter.h"
#include "net/AckPolicy.h"
#include "net/PeerCaps.h"
#include "net/rap/RapStateMachine.h"
#include "net/TdmaScheduler.h"
#include "net/CryptoHook.h"
#include "rf/EnvProfile.h"
#include "rf/AirtimeModel.h"

/**
 * MeshRocStack：自研 MeshRocPacket 协议栈引擎（原创优化 O1/O2/O3/O4/O5/归属 的逻辑中枢）
 *
 * 职责：把原本孤立的 kernel/ 模块（EnvProfile / AckPolicy / Reassembler / NextHopRouter /
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
        MeshRocPriority priority;
        bool wantAck;
    };

    MeshRocStack(const config::MeshROCConfig& cfg, uint16_t myAddr, bool isBackbone = false);

    uint16_t myAddr() const { return myAddr_; }

    // ---- 应用层入口 ----
    // 发送载荷。内部完成：O3 加密 -> O5 分片(若超长) -> 路由跳数(O1路由) ->
    // O4 ACK 策略 -> 构造 MeshRocPacket -> sendRaw。
    // 返回 false 表示载荷非法或当前 TDMA 非本节点窗口（O2 限制）。
    bool sendPayload(uint16_t dst, const uint8_t* payload, uint16_t len,
                     MeshRocPriority prio = MeshRocPriority::LOW,
                     bool wantAck = false);

    // ---- 射频层回调（后续接入时调用）----
    // 把从射频收到的原始字节喂给协议栈（解码 + 分发 RAP / 分片重组 / 应用）。
    void ingestRaw(const uint8_t* frame, uint16_t len, uint32_t nowMs);

    // ---- 周期性驱动（引擎 tick）----
    // 驱动：环境采样(O1) / RAP 状态机(O归属) / TDMA 时隙(O2 窗口查询) /
    //       分片重组超时清理(O5) / 重传超时(O4)。
    void tick(uint32_t nowMs);

    // 应用层可重载：收包通知
    virtual void onPacket(const PacketReceived& pkt) { (void)pkt; }

    // RAP 状态查询（归属层）
    MeshRocPacket::RapTerminalState rapState() const { return rap_.state(); }
    uint16_t attachedBackbone() const { return rap_.attachedBackbone(); }
    const rf::EnvProfile& env() const { return env_; }
    const rf::AirtimeModel& airtime() const { return airtime_; }

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
                    net::MeshRocPacketType opcode, MeshRocPriority prio,
                    bool wantAck, uint8_t hopLimit, net::MeshRocTlv payloadTag);

    // 发送端分片（O5）：把 payload 切成 <=MAX_PAYLOAD 的片，每片带 FRAG_HEADER TLV
    bool sendFragmented(uint16_t dst, const uint8_t* payload, uint16_t len,
                        MeshRocPriority prio, bool wantAck, uint8_t hopLimit,
                        uint16_t fragId);

    // 把 RAP 帧（来自 RapStateMachine）包成 MeshRocPacket(OPCODE_RAP) 发出
    void sendRapFrame(const uint8_t* rapBytes, uint16_t rapLen, uint32_t nowMs);

    // 处理收到的 RAP 帧（payload 内已含 RAP 子类型字节）
    void handleRap(const uint8_t* payload, uint16_t len, uint32_t nowMs);

    // 处理收到的普通/分片包：分片走 Reassembler，重组完成或普通包触发 onPacket
    void handleData(const net::MeshRocPacket& pkt, const uint8_t* tlv,
                    uint16_t tlvLen, uint32_t nowMs);

    const config::MeshROCConfig& cfg_;
    uint16_t myAddr_;

    rf::EnvProfile     env_;
    net::Reassembler   reassembler_;
    net::NextHopRouter router_;
    net::AckPolicy     ack_;
    net::PeerCaps      peerCaps_;
    net::RapStateMachine rap_;
    net::TdmaScheduler tdma_;
    net::CryptoHook    crypto_;
    rf::AirtimeModel   airtime_;  // 纯函数工具，无状态

    // 重组输出缓冲（定长，零动态分配）
    uint8_t reassembleBuf_[net::Reassembler::MAX_PAYLOAD];
    // 编码缓冲（定长，最大帧 = 头(10) + 最大载荷TLV + CRC16(2)）
    static constexpr uint16_t MAX_FRAME = net::Reassembler::MAX_PAYLOAD + 64;
    uint8_t encodeBuf_[MAX_FRAME];

    uint16_t fragIdSeq_ = 0;          // 发送端分片组 ID 发生器
    uint16_t seq_ = 0;                // MeshRocPacket 序列号发生器
    uint32_t currentTickApprox_ = 0;  // 最近一次 tick 时刻（供 sendPayload 查询 TDMA 窗口）
    uint32_t lastEnvFeedMs_ = 0;      // 上次环境采样时刻
    uint32_t lastReassExpireMs_ = 0;  // 上次重组超时清理
    static constexpr uint32_t ENV_FEED_PERIOD_MS = 1000;
    static constexpr uint32_t REASS_EXPIRE_PERIOD_MS = 500;
    // 单帧载荷阈值：超过则走 O5 分片（留足 TLV 头与 CRC 余量）
    static constexpr uint16_t MAX_PAYLOAD_FRAG_THRESHOLD = net::Reassembler::MAX_PAYLOAD - 16;
};

}  // namespace meshroc
