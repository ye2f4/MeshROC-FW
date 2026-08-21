#include "kernel/net/rap/RapStateMachine.h"
#include <cstring>
#include <algorithm>

namespace meshroc::net::rap {

namespace {
// 大端写入/读取（D3 决策：统一大端，匹配真实 datapack 空中字节）
void putU16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
uint16_t getU16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
}  // namespace

// ---------------------------------------------------------------------------
// 终端侧 tick：驱动 HELLO / KEEPALIVE / 迟滞评估（平移 MeshRocModule::tick 逻辑）
// ---------------------------------------------------------------------------
uint16_t RapStateMachine::tick(uint32_t nowMs, uint8_t* outBuf, uint16_t bufCap)
{
    if (bufCap < kRapMsgMax) return 0;

    if (isBackbone_) {
        // 骨干侧：周期性全量/增量 OWNERSHIP_ADV（软状态收敛），此处做老化剪枝。
        pruneExpiredOwners(nowMs);
        pruneExpiredNeighbors(nowMs);
        return 0; // 骨干不主动发 HELLO（由终端 ATTACH 触发交互）
    }

    switch (state_) {
        case RapTerminalState::SCANNING:
            if (nowMs >= nextSendMs_) {
                sendHello(/*fullAdv=*/false, nowMs);
                // HELLO 抖动 + 退避
                uint32_t backoff = HELLO_JITTER_MS * (1u << helloBackoffPow_);
                if (backoff > HELLO_BACKOFF_MAX_MS) backoff = HELLO_BACKOFF_MAX_MS;
                nextSendMs_ = nowMs + HELLO_PERIOD_MS + backoff;
            }
            break;

        case RapTerminalState::ATTACHED:
            if (nowMs - lastActivityMs_ > ATTACH_TIMEOUT_MS) {
                state_ = RapTerminalState::SCANNING;
                rapLocal_.attachedTo = 0;
                rapLocal_.attachSeq = 0;
                nextSendMs_ = nowMs;
                break;
            }
            if (nowMs >= nextSendMs_) {
                sendKeepalive(/*standalone=*/true, nowMs);
                nextSendMs_ = nowMs + KEEPALIVE_PERIOD_MS;
            }
            break;

        case RapTerminalState::EVALUATING:
            // 多骨干评估切换策略（铁律三）：暂简化为评完回 SCANNING 重新发现
            state_ = RapTerminalState::SCANNING;
            rapLocal_.attachedTo = 0;
            nextSendMs_ = nowMs;
            break;

        default:
            break;
    }
    // tick 不直接产出帧缓冲（发送经 sendFn_ 异步完成），返回 0。
    (void)outBuf;
    return 0;
}

// ---------------------------------------------------------------------------
// 接收处理入口
// ---------------------------------------------------------------------------
bool RapStateMachine::onRapFrame(const uint8_t* msg, uint16_t len, uint32_t nowMs, uint32_t fromNode)
{
    if (len < 1) return false;
    rap::RapKind kind = static_cast<rap::RapKind>(msg[0]);
    RapTerminalState prev = state_;

    // msg 布局：[kind][srcBE][dstBE][TLVs...]；TLV 从偏移 5 起。
    const uint8_t* tlvs = (len > 5) ? (msg + 5) : nullptr;
    uint16_t tlvLen = (len > 5) ? (len - 5) : 0;

    switch (kind) {
        case rap::RapKind::HELLO:        onHello(tlvs, tlvLen, fromNode, nowMs); break;
        case rap::RapKind::HELLO_ACK:    onHelloAck(tlvs, tlvLen, fromNode, nowMs); break;
        case rap::RapKind::ATTACH_REQ:   onAttachReq(tlvs, tlvLen, fromNode, nowMs); break;
        case rap::RapKind::ATTACH_ACK:   onAttachAck(tlvs, tlvLen, fromNode, nowMs); break;
        case rap::RapKind::KEEPALIVE:    onKeepalive(tlvs, tlvLen, fromNode, nowMs); break;
        case rap::RapKind::DETACH:       onDetach(tlvs, tlvLen, fromNode, nowMs); break;
        default: break;
    }
    // 骨干侧在 onXxx 内处理 OWNERSHIP/SYNC（由 ATTACH 等触发），此处不重复
    (void)prev;
    // 状态迁移标志：仅终端 SCANNING<->ATTACHED 变化才算迁移
    return state_ != prev;
}

// ---------------------------------------------------------------------------
// 内部发送辅助
// ---------------------------------------------------------------------------
uint16_t RapStateMachine::buildMsg(rap::RapKind kind, uint16_t dst, uint8_t* out, uint8_t& outLen)
{
    out[0] = static_cast<uint8_t>(kind);
    putU16(out + 1, myAddr_);  // src
    putU16(out + 3, dst);      // dst
    outLen = 5;
    return 5;
}

void RapStateMachine::sendRapFrame(rap::RapKind kind, uint16_t dstShort,
                                   const uint8_t* extra, uint8_t extraLen, uint32_t /*nowMs*/)
{
    uint8_t body[kRapMsgMax];
    uint8_t baseLen = 0;
    buildMsg(kind, dstShort, body, baseLen);
    if (extra && extraLen > 0 && baseLen + extraLen <= kRapMsgMax) {
        ::memcpy(body + baseLen, extra, extraLen);
        baseLen += extraLen;
    }
    if (sendFn_) sendFn_(ctx_, body, baseLen, dstShort);
}

void RapStateMachine::sendHello(bool /*fullAdv*/, uint32_t nowMs)
{
    uint8_t extra[16];
    uint8_t p = 0;
    extra[p++] = static_cast<uint8_t>(rap::RapTlv::ROLE_CLASS); extra[p++] = 1;
    extra[p++] = static_cast<uint8_t>(localRoleClass());
    extra[p++] = static_cast<uint8_t>(rap::RapTlv::TABLE_VERSION); extra[p++] = 2;
    extra[p++] = static_cast<uint8_t>((rapTableVersion_ >> 8) & 0xFF);
    extra[p++] = static_cast<uint8_t>(rapTableVersion_ & 0xFF);
    sendRapFrame(rap::RapKind::HELLO, 0xFFFF, extra, p, nowMs);
}

void RapStateMachine::sendHelloAck(uint32_t dst, int8_t snrToIt, uint32_t nowMs)
{
    uint8_t extra[16];
    uint8_t p = 0;
    extra[p++] = static_cast<uint8_t>(rap::RapTlv::NEIGHBOR_SNR); extra[p++] = 3;
    extra[p++] = static_cast<uint8_t>((dst >> 8) & 0xFF);
    extra[p++] = static_cast<uint8_t>(dst & 0xFF);
    extra[p++] = static_cast<uint8_t>(snrToIt);
    sendRapFrame(rap::RapKind::HELLO_ACK, static_cast<uint16_t>(dst & 0xFFFF), extra, p, nowMs);
}

void RapStateMachine::sendAttachReq(uint32_t backbone, uint32_t nowMs)
{
    rapLocal_.attachSeq++;
    rapLocal_.lastAttachReqAt = nowMs;
    uint8_t extra[16];
    uint8_t p = 0;
    extra[p++] = static_cast<uint8_t>(rap::RapTlv::ROLE_CLASS); extra[p++] = 1;
    extra[p++] = static_cast<uint8_t>(localRoleClass());
    extra[p++] = static_cast<uint8_t>(rap::RapTlv::ATTACH_SEQ); extra[p++] = 2;
    extra[p++] = static_cast<uint8_t>((rapLocal_.attachSeq >> 8) & 0xFF);
    extra[p++] = static_cast<uint8_t>(rapLocal_.attachSeq & 0xFF);
    sendRapFrame(rap::RapKind::ATTACH_REQ, static_cast<uint16_t>(backbone & 0xFFFF), extra, p, nowMs);
}

void RapStateMachine::sendAttachAck(uint32_t dst, uint16_t ttlGrant, uint16_t attachSeq, uint32_t nowMs)
{
    uint8_t extra[16];
    uint8_t p = 0;
    extra[p++] = static_cast<uint8_t>(rap::RapTlv::TTL_GRANT); extra[p++] = 2;
    extra[p++] = static_cast<uint8_t>((ttlGrant >> 8) & 0xFF);
    extra[p++] = static_cast<uint8_t>(ttlGrant & 0xFF);
    extra[p++] = static_cast<uint8_t>(rap::RapTlv::ATTACH_SEQ); extra[p++] = 2;
    extra[p++] = static_cast<uint8_t>((attachSeq >> 8) & 0xFF);
    extra[p++] = static_cast<uint8_t>(attachSeq & 0xFF);
    sendRapFrame(rap::RapKind::ATTACH_ACK, static_cast<uint16_t>(dst & 0xFFFF), extra, p, nowMs);
}

void RapStateMachine::sendKeepalive(bool standalone, uint32_t nowMs)
{
    if (rapLocal_.attachedTo == 0) return;
    uint8_t extra[16];
    uint8_t p = 0;
    extra[p++] = static_cast<uint8_t>(rap::RapTlv::ATTACH_SEQ); extra[p++] = 2;
    extra[p++] = static_cast<uint8_t>((rapLocal_.attachSeq >> 8) & 0xFF);
    extra[p++] = static_cast<uint8_t>(rapLocal_.attachSeq & 0xFF);
    if (!standalone) return; // 捎带模式由调用方追加，此处仅发独立帧
    sendRapFrame(rap::RapKind::KEEPALIVE, static_cast<uint16_t>(rapLocal_.attachedTo & 0xFFFF), extra, p, nowMs);
}

void RapStateMachine::sendDetach(uint32_t oldOwner, uint32_t nowMs)
{
    sendRapFrame(rap::RapKind::DETACH, static_cast<uint16_t>(oldOwner & 0xFFFF), nullptr, 0, nowMs);
}

void RapStateMachine::sendOwnershipAdv(const RapOwnerEntry* entries, uint8_t count, bool isDelete, uint32_t nowMs)
{
    uint8_t extra[256];
    uint8_t p = 0;
    extra[p++] = static_cast<uint8_t>(rap::RapTlv::TABLE_VERSION); extra[p++] = 2;
    extra[p++] = static_cast<uint8_t>((rapTableVersion_ >> 8) & 0xFF);
    extra[p++] = static_cast<uint8_t>(rapTableVersion_ & 0xFF);
    uint8_t tlvTag = isDelete ? static_cast<uint8_t>(rap::RapTlv::OWNER_DEL)
                              : static_cast<uint8_t>(rap::RapTlv::OWNER_LIST);
    uint8_t entrySize = isDelete ? 2 : 4;
    extra[p++] = tlvTag; extra[p++] = static_cast<uint8_t>(count * entrySize);
    for (uint8_t i = 0; i < count; i++) {
        const RapOwnerEntry* e = &entries[i];
        if (isDelete) {
            extra[p++] = static_cast<uint8_t>((e->node >> 8) & 0xFF);
            extra[p++] = static_cast<uint8_t>(e->node & 0xFF);
        } else {
            extra[p++] = static_cast<uint8_t>((e->node >> 8) & 0xFF);
            extra[p++] = static_cast<uint8_t>(e->node & 0xFF);
            extra[p++] = static_cast<uint8_t>((e->attachSeq >> 8) & 0xFF);
            extra[p++] = static_cast<uint8_t>(e->attachSeq & 0xFF);
        }
    }
    sendRapFrame(rap::RapKind::OWNERSHIP_ADV, 0xFFFF, extra, p, nowMs);
}

void RapStateMachine::sendSyncReq(uint32_t dst, uint32_t nowMs)
{
    sendRapFrame(rap::RapKind::SYNC_REQ, static_cast<uint16_t>(dst & 0xFFFF), nullptr, 0, nowMs);
}

// ---------------------------------------------------------------------------
// 接收：HELLO（终端发现候选/骨干互发现）
// ---------------------------------------------------------------------------
void RapStateMachine::onHello(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs)
{
    uint8_t rclass = static_cast<uint8_t>(config::MeshRocRole::CLIENT);
    // 解析 RAP_ROLE_CLASS / TABLE_VERSION
    uint16_t advVer = 0;
    // body 已是 TLV 段（偏移5之后），逐 TLV 扫描
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t t = body[i], l = body[i + 1];
        const uint8_t* v = body + i + 2;
        if (t == static_cast<uint8_t>(rap::RapTlv::ROLE_CLASS) && l >= 1) rclass = v[0];
        if (t == static_cast<uint8_t>(rap::RapTlv::TABLE_VERSION) && l >= 2)
            advVer = getU16(v);
        i += 2 + l;
    }

    int8_t snrToIt = link_.getPeerSnr(fromNode);

    if (isBackbone_ && rclass == static_cast<uint8_t>(config::MeshRocRole::BACKBONE)) {
        // 骨干互发现：回 HELLO_ACK（定向），准入以回 ACK 为准（排除原版 ROUTER）
        sendHelloAck(fromNode, snrToIt, nowMs);
        RapNeighborEntry* nb = findNeighbor(fromNode);
        if (!nb && nNeighbors_ < MAX_NEIGHBORS) {
            nb = &neighbors_[nNeighbors_++];
            ::memset(nb, 0, sizeof(*nb));
            nb->backbone = fromNode;
        }
        if (nb) {
            nb->snrMyToIt = snrToIt;
            nb->lastHelloAt = nowMs;
            nb->tableVersion = advVer;
        }
    } else if (!isBackbone_) {
        // 终端侧：监听 HELLO 发现归属候选，迟滞切换（铁律三）
        if (rclass == static_cast<uint8_t>(config::MeshRocRole::BACKBONE)) {
            if (rapLocal_.state == RapTerminalState::SCANNING || rapLocal_.attachedTo == 0) {
                rapLocal_.attachedTo = fromNode;
                rapLocal_.state = RapTerminalState::ATTACHED;
                rapLocal_.lastKeepaliveAt = nowMs;
                rapLocal_.minDwellUntil = nowMs + RAP_MIN_DWELL_MS;
                rapLocal_.curSnr = snrToIt;
                rapLocal_.hystSamples = 0;
                sendAttachReq(fromNode, nowMs);
            } else {
                if (snrToIt >= rapLocal_.curSnr + RAP_HYST_SNR_DB) {
                    rapLocal_.hystSamples++;
                    if (rapLocal_.hystSamples >= RAP_HYST_SAMPLES && nowMs >= rapLocal_.minDwellUntil) {
                        uint32_t old = rapLocal_.attachedTo;
                        rapLocal_.attachedTo = fromNode;
                        rapLocal_.curSnr = snrToIt;
                        rapLocal_.hystSamples = 0;
                        rapLocal_.minDwellUntil = nowMs + RAP_MIN_DWELL_MS;
                        sendAttachReq(fromNode, nowMs);
                        sendDetach(old, nowMs);
                    }
                } else {
                    rapLocal_.hystSamples = 0;
                }
            }
            rapLocal_.curSnr = static_cast<int8_t>(std::max(rapLocal_.curSnr, snrToIt));
        }
    }
}

void RapStateMachine::onHelloAck(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs)
{
    if (!isBackbone_) return; // 仅 BACKBONE 处理
    RapNeighborEntry* nb = findNeighbor(fromNode);
    if (!nb) {
        if (nNeighbors_ < MAX_NEIGHBORS) {
            nb = &neighbors_[nNeighbors_++];
            ::memset(nb, 0, sizeof(*nb));
            nb->backbone = fromNode;
        } else return;
    }
    // 解析 RAP_NEIGHBOR_SNR
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t t = body[i], l = body[i + 1];
        const uint8_t* v = body + i + 2;
        if (t == static_cast<uint8_t>(rap::RapTlv::NEIGHBOR_SNR) && l >= 3)
            nb->snrItToMe = static_cast<int8_t>(v[2]);
        i += 2 + l;
    }
    nb->snrMyToIt = link_.getPeerSnr(fromNode);
    nb->linkCost = computeLinkCost(nb->snrMyToIt, nb->snrItToMe);
    nb->lastAckAt = nowMs;
    nb->acked = true; // 此刻才正式准入（排除原版 ROUTER：它不会回 ACK）
    helloBackoffPow_ = 0;
}

void RapStateMachine::onAttachReq(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs)
{
    if (!isBackbone_) return;
    uint16_t aseq = 0;
    uint8_t rclass = static_cast<uint8_t>(config::MeshRocRole::CLIENT);
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t t = body[i], l = body[i + 1];
        const uint8_t* v = body + i + 2;
        if (t == static_cast<uint8_t>(rap::RapTlv::ATTACH_SEQ) && l >= 2) aseq = getU16(v);
        if (t == static_cast<uint8_t>(rap::RapTlv::ROLE_CLASS) && l >= 1) rclass = v[0];
        i += 2 + l;
    }
    if (nOwners_ >= MAX_OWNED) {
        sendAttachAck(fromNode, 0, aseq, nowMs); // 表满拒绝（TTL_GRANT=0），终端退回洪泛
        return;
    }
    RapOwnerEntry* e = findOwner(fromNode);
    if (!e) {
        e = &owners_[nOwners_++];
        ::memset(e, 0, sizeof(*e));
        e->node = fromNode;
    }
    e->ownerBackbone = myAddr_; // 仅上空口降级短地址（与 MeshRocModule 一致）
    e->attachSeq = std::max(e->attachSeq, aseq);
    e->roleClass = rclass;
    e->ttlExpireAt = nowMs + rapTtlForRole(rclass);
    e->failCount = 0;
    bumpTableVersion();
    sendAttachAck(fromNode, static_cast<uint16_t>(rapTtlForRole(rclass) / 1000), e->attachSeq, nowMs);
    sendOwnershipAdv(e, 1, false, nowMs);
}

void RapStateMachine::onAttachAck(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs)
{
    if (isBackbone_) return; // 仅终端
    uint16_t grantedTtl = 0, aseq = 0;
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t t = body[i], l = body[i + 1];
        const uint8_t* v = body + i + 2;
        if (t == static_cast<uint8_t>(rap::RapTlv::TTL_GRANT) && l >= 2) grantedTtl = getU16(v);
        if (t == static_cast<uint8_t>(rap::RapTlv::ATTACH_SEQ) && l >= 2) aseq = getU16(v);
        i += 2 + l;
    }
    if (grantedTtl == 0) {
        rapLocal_.state = RapTerminalState::SCANNING;
        rapLocal_.attachedTo = 0;
        return;
    }
    rapLocal_.attachedTo = fromNode;
    rapLocal_.attachSeq = aseq;
    rapLocal_.state = RapTerminalState::ATTACHED;
    rapLocal_.lastKeepaliveAt = nowMs;
    rapLocal_.minDwellUntil = nowMs + RAP_MIN_DWELL_MS;
    rapLocal_.curSnr = link_.getPeerSnr(fromNode);
    rapLocal_.hystSamples = 0;
}

void RapStateMachine::onKeepalive(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs)
{
    if (!isBackbone_) return;
    (void)body; (void)len;
    RapOwnerEntry* e = findOwner(fromNode);
    if (e) {
        e->ttlExpireAt = nowMs + rapTtlForRole(e->roleClass); // 续租不算变更，表版本不变
    }
}

void RapStateMachine::onDetach(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs)
{
    if (!isBackbone_) return;
    (void)body; (void)len;
    RapOwnerEntry* e = findOwner(fromNode);
    if (e) {
        uint8_t idx = static_cast<uint8_t>(e - owners_);
        for (uint8_t i = idx; i + 1 < nOwners_; i++)
            owners_[i] = owners_[i + 1];
        nOwners_--;
        bumpTableVersion();
        sendOwnershipAdv(e, 1, true, nowMs);
    }
}

void RapStateMachine::onOwnershipAdv(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs)
{
    if (!isBackbone_) return; // 骨干才汇总远端归属表
    uint16_t advVer = 0;
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t t = body[i], l = body[i + 1];
        const uint8_t* v = body + i + 2;
        if (t == static_cast<uint8_t>(rap::RapTlv::TABLE_VERSION) && l >= 2) advVer = getU16(v);
        i += 2 + l;
    }
    RapNeighborEntry* nb = findNeighbor(fromNode);
    uint16_t lastVer = nb ? nb->tableVersion : 0;
    if (nb) nb->tableVersion = advVer;
    if (advVer - lastVer > 1) {
        sendSyncReq(fromNode, nowMs);
        return;
    }
    // OWNER_LIST / OWNER_DEL
    i = 0;
    while (i + 2 <= len) {
        uint8_t t = body[i], l = body[i + 1];
        const uint8_t* v = body + i + 2;
        if (t == static_cast<uint8_t>(rap::RapTlv::OWNER_LIST)) {
            uint8_t cnt = l / 4;
            for (uint8_t k = 0; k < cnt; k++) {
                const uint8_t* b = v + k * 4;
                uint32_t node = ((uint32_t)b[0] << 8) | b[1];
                uint16_t aseq = ((uint16_t)b[2] << 8) | b[3];
                RapOwnerEntry* e = findRemoteOwner(node);
                if (e && e->attachSeq >= aseq) continue;
                if (!e && nRemoteOwners_ < MAX_REMOTE_OWNERS) {
                    e = &remoteOwners_[nRemoteOwners_++];
                    ::memset(e, 0, sizeof(*e));
                    e->node = node;
                }
                if (e) {
                    e->ownerBackbone = fromNode;
                    e->attachSeq = aseq;
                    e->ttlExpireAt = nowMs + static_cast<uint32_t>(TTL_CLIENT_S) * 1000;
                }
            }
        } else if (t == static_cast<uint8_t>(rap::RapTlv::OWNER_DEL)) {
            uint8_t cnt = l / 2;
            for (uint8_t k = 0; k < cnt; k++) {
                const uint8_t* b = v + k * 2;
                uint32_t node = ((uint32_t)b[0] << 8) | b[1];
                RapOwnerEntry* e = findRemoteOwner(node);
                if (e) {
                    uint8_t idx = static_cast<uint8_t>(e - remoteOwners_);
                    for (uint8_t j = idx; j + 1 < nRemoteOwners_; j++)
                        remoteOwners_[j] = remoteOwners_[j + 1];
                    nRemoteOwners_--;
                }
            }
        }
        i += 2 + l;
    }
}

void RapStateMachine::onSyncReq(const uint8_t* body, uint16_t len, uint32_t fromNode, uint32_t nowMs)
{
    if (!isBackbone_) return;
    (void)body; (void)len;
    for (uint8_t i = 0; i < nOwners_; i += ADV_MAX_ENTRIES_PER_FRAME) {
        uint8_t cnt = static_cast<uint8_t>(std::min<uint8_t>(nOwners_ - i, ADV_MAX_ENTRIES_PER_FRAME));
        sendOwnershipAdv(&owners_[i], cnt, false, nowMs);
    }
}

// ---------------------------------------------------------------------------
// 表维护
// ---------------------------------------------------------------------------
RapNeighborEntry* RapStateMachine::findNeighbor(uint32_t bb)
{
    for (uint8_t i = 0; i < nNeighbors_; i++)
        if (neighbors_[i].backbone == bb) return &neighbors_[i];
    return nullptr;
}

RapOwnerEntry* RapStateMachine::findOwner(uint32_t node)
{
    for (uint8_t i = 0; i < nOwners_; i++)
        if (owners_[i].node == node) return &owners_[i];
    return nullptr;
}

RapOwnerEntry* RapStateMachine::findRemoteOwner(uint32_t node)
{
    for (uint8_t i = 0; i < nRemoteOwners_; i++)
        if (remoteOwners_[i].node == node) return &remoteOwners_[i];
    return nullptr;
}

void RapStateMachine::pruneExpiredOwners(uint32_t nowMs)
{
    for (uint8_t i = 0; i < nOwners_;) {
        if (owners_[i].ttlExpireAt != 0 && nowMs >= owners_[i].ttlExpireAt) {
            for (uint8_t j = i; j + 1 < nOwners_; j++) owners_[j] = owners_[j + 1];
            nOwners_--;
            bumpTableVersion();
        } else i++;
    }
    for (uint8_t i = 0; i < nRemoteOwners_;) {
        if (remoteOwners_[i].ttlExpireAt != 0 && nowMs >= remoteOwners_[i].ttlExpireAt) {
            for (uint8_t j = i; j + 1 < nRemoteOwners_; j++) remoteOwners_[j] = remoteOwners_[j + 1];
            nRemoteOwners_--;
        } else i++;
    }
}

void RapStateMachine::pruneExpiredNeighbors(uint32_t nowMs)
{
    for (uint8_t i = 0; i < nNeighbors_;) {
        if (nowMs - neighbors_[i].lastHelloAt >= NEIGHBOR_TTL_MS) {
            for (uint8_t j = i; j + 1 < nNeighbors_; j++) neighbors_[j] = neighbors_[j + 1];
            nNeighbors_--;
        } else i++;
    }
}

void RapStateMachine::bumpTableVersion()
{
    rapTableVersion_++;
    if (rapTableVersion_ == 0) rapTableVersion_ = 1;
}

uint8_t RapStateMachine::localRoleClass() const
{
    return static_cast<uint8_t>(myRole_); // config::MeshRocRole 整数值即 RAP_ROLE_CLASS（含 DTU=4/GATEWAY=5）
}

uint32_t RapStateMachine::rapTtlForRole(uint8_t role) const
{
    // 平移 rapTtlMs，但保留 MeshRocModule 原语义（秒→毫秒）
    return config::rapTtlMs(static_cast<config::MeshRocRole>(role));
}

}  // namespace meshroc::net::rap
