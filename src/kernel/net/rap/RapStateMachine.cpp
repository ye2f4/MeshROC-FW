#include "net/rap/RapStateMachine.h"
#include <cstring>

namespace meshroc::net {

namespace {
void putU16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
uint16_t getU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
}  // namespace

uint16_t RapStateMachine::tick(uint32_t nowMs, uint8_t* outBuf, uint16_t bufCap)
{
    if (bufCap < kRapMsgMax) return 0;
    uint16_t outLen = 0;

    if (isBackbone_) {
        // 骨干侧：本骨架不主动广播 NODE_SET（TODO(rap): 骨干拓扑广播），
        // 仅作为终端的归属锚点存在，保持 ATTACHED。
        return 0;
    }

    switch (state_) {
        case rap::RapTerminalState::SCANNING:
            if (nowMs >= nextSendMs_) {
                buildMsg(rap::RapKind::HELLO, 0, outBuf, outLen);
                nextSendMs_ = nowMs + rap::HELLO_PERIOD_MS;
            }
            break;

        case rap::RapTerminalState::ATTACHED:
            if (nowMs - lastActivityMs_ > ATTACH_TIMEOUT_MS) {
                state_ = rap::RapTerminalState::SCANNING;
                attachedBb_ = 0;
                nextSendMs_ = nowMs;
                break;
            }
            if (nowMs >= nextSendMs_) {
                buildMsg(rap::RapKind::KEEPALIVE, attachedBb_, outBuf, outLen);
                nextSendMs_ = nowMs + KEEPALIVE_PERIOD_MS;
            }
            break;

        case rap::RapTerminalState::EVALUATING:
            // TODO(rap): 多骨干评估切换策略；骨架简化为评完回 SCANNING
            state_ = rap::RapTerminalState::SCANNING;
            nextSendMs_ = nowMs;
            break;

        default:
            break;
    }
    return outLen;
}

bool RapStateMachine::onRapFrame(const uint8_t* msg, uint16_t len, uint32_t nowMs)
{
    if (len < 1) return false;
    rap::RapKind kind = static_cast<rap::RapKind>(msg[0]);
    rap::RapTerminalState prev = state_;

    if (!isBackbone_) {
        switch (kind) {
            case rap::RapKind::ATTACH_ACK:
                if (len >= 5) {
                    attachedBb_ = getU16(msg + 3);  // dst 字段即骨干地址
                    state_ = rap::RapTerminalState::ATTACHED;
                    lastActivityMs_ = nowMs;
                    nextSendMs_ = nowMs + KEEPALIVE_PERIOD_MS;
                }
                break;
            case rap::RapKind::KEEPALIVE:
                lastActivityMs_ = nowMs;
                break;
            case rap::RapKind::DETACH:
                state_ = rap::RapTerminalState::SCANNING;
                attachedBb_ = 0;
                nextSendMs_ = nowMs;
                break;
            default:
                break;
        }
    } else {
        // TODO(rap): 骨干侧记录下属终端表、维护存活映射
        (void)kind;
    }
    return state_ != prev;
}

void RapStateMachine::buildMsg(rap::RapKind kind, uint16_t dst, uint8_t* out, uint16_t& outLen)
{
    out[0] = static_cast<uint8_t>(kind);
    putU16(out + 1, myAddr_);   // src
    putU16(out + 3, dst);       // dst
    outLen = 5;
}

}  // namespace meshroc::net
