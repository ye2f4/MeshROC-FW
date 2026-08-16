#pragma once
#include <cstdint>
#include "net/rap/RapProtocol.h"  // rap::RapKind / RapTerminalState / RapTlv / 常量

/**
 * RapStateMachine：归属协议状态机（原创优化 "O归属" —— MeshROC 私有 RAP 层）
 *
 * 设计定位：把 RapProtocol.h 里定义的终端状态（SCANNING/ATTACHED/EVALUATING）与
 * RAP 子类型（HELLO / ATTACH_ACK / KEEPALIVE / DETACH，见 rap::RapKind）落为
 * 最小可运行状态机。**仅协议层骨架**：不碰射频，状态迁移与待发帧由引擎 tick 驱动，
 * 通过 ingestRaw/onRapFrame 接收对端帧。真正的归属拓扑决策（选哪台骨干、负载均衡）
 * 留 TODO 接后续策略。
 *
 * 帧封装：本状态机输出"RAP 消息体"（即 RapTlv::KIND=0x20 的 TLV value）：
 *   [rapKind u8][src u16LE][dst u16LE]  （共 5 字节）
 * 引擎负责把它包进 MeshRocPacket(opcode=RAP) 的 KIND TLV 发出。
 *
 * 与原版关系：RAP 是 MeshROC 私有"终端-骨干"归属层，原版 Meshtastic 无对应概念；
 * 故完全独立于原版 Router，仅在字节进出层面通过引擎 sendRaw/ingestRaw 与外界交互。
 * 参考契约 §15.2-O归属 / §15.3 rap。
 */
namespace meshroc::net {

class RapStateMachine {
public:
    // 单个 RAP 消息体最大长度（KIND TLV value 容量）
    static constexpr uint8_t kRapMsgMax = 16;

    RapStateMachine(uint16_t myAddr, bool isBackbone)
        : myAddr_(myAddr), isBackbone_(isBackbone),
          state_(isBackbone ? rap::RapTerminalState::ATTACHED : rap::RapTerminalState::SCANNING),
          lastActivityMs_(0), nextSendMs_(0) {}

    rap::RapTerminalState state() const { return state_; }
    uint16_t myAddr() const { return myAddr_; }
    bool isBackbone() const { return isBackbone_; }

    // 引擎周期性调用：返回此刻需要发出的 RAP 消息体（写入 outBuf，<=kRapMsgMax）。
    // 返回写入字节数（0=无需发）。
    uint16_t tick(uint32_t nowMs, uint8_t* outBuf, uint16_t bufCap);

    // 处理收到的 RAP 消息体（KIND TLV 的 value 部分）。返回 true 表示状态迁移。
    bool onRapFrame(const uint8_t* msg, uint16_t len, uint32_t nowMs);

    // 当前归属的骨干地址（终端侧；ATTACHED 后有效）。0=未归属。
    uint16_t attachedBackbone() const { return attachedBb_; }

private:
    static constexpr uint32_t KEEPALIVE_PERIOD_MS = 30'000;   // 续租周期（骨架）
    static constexpr uint32_t ATTACH_TIMEOUT_MS   = 60'000;   // 归属保活超时

    void buildMsg(rap::RapKind kind, uint16_t dst, uint8_t* out, uint16_t& outLen);

    uint16_t            myAddr_;
    bool                isBackbone_;
    rap::RapTerminalState state_;
    uint32_t            lastActivityMs_;
    uint32_t            nextSendMs_;
    uint16_t            attachedBb_ = 0;
};

}  // namespace meshroc::net
