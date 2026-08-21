#include "MeshRocStack.h"
#include <cstring>
#include <algorithm>

namespace meshroc {

namespace {
// 帧格式：[10B header][TLV segment][2B crc16]，全大端短地址（D3：匹配真实 datapack 空中字节）。
constexpr size_t HDR_OFF_CTRL   = 0;
constexpr size_t HDR_OFF_SRC    = 1;
constexpr size_t HDR_OFF_DST    = 3;
constexpr size_t HDR_OFF_SEQ    = 5;
constexpr size_t HDR_OFF_MAXHOP = 7;
constexpr size_t HDR_OFF_SNR    = 8;
constexpr size_t HDR_OFF_ROUTE  = 9;  // 第 10 字节为 route_mode，随后 TLV 段，帧尾 2B CRC16
constexpr size_t TLV_OFF        = 10;

// 大端写入/读取（D3 决策：统一大端）
void putU16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>((v >> 8) & 0xFF); p[1] = static_cast<uint8_t>(v & 0xFF); }
uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]); }

// 把 MeshRocPacket 头写入 buffer[0..9]（10 字节大端），CRC16 由调用方附在帧尾。
void writeHeader(uint8_t* buf, const net::MeshRocPacket& h)
{
    buf[HDR_OFF_CTRL]   = h.ctrl_flag;
    putU16(buf + HDR_OFF_SRC, h.src);
    putU16(buf + HDR_OFF_DST, h.dst);
    putU16(buf + HDR_OFF_SEQ, h.seq);
    buf[HDR_OFF_MAXHOP] = h.max_hop;
    buf[HDR_OFF_SNR]    = static_cast<uint8_t>(h.snr);
    buf[HDR_OFF_ROUTE]  = h.route_mode;
}

bool parseHeader(const uint8_t* buf, net::MeshRocPacket& h)
{
    if ((buf[HDR_OFF_CTRL] & 0x07) > 7) return false;
    h.ctrl_flag  = buf[HDR_OFF_CTRL];
    h.src        = getU16(buf + HDR_OFF_SRC);
    h.dst        = getU16(buf + HDR_OFF_DST);
    h.seq        = getU16(buf + HDR_OFF_SEQ);
    h.max_hop    = buf[HDR_OFF_MAXHOP];
    h.snr        = static_cast<int8_t>(buf[HDR_OFF_SNR]);
    h.route_mode = buf[HDR_OFF_ROUTE];
    return true;
}
}  // anonymous namespace

MeshRocStack::MeshRocStack(const config::MeshROCConfig& cfg, uint16_t myAddr, bool isBackbone)
    : cfg_(cfg), myAddr_(myAddr),
      env_(cfg), reassembler_(), hopPlanner_(cfg),
      router_(myAddr, cfg.deviceRole, &MeshRocStack::routeSendFn, this, cfg.lora.hopLimit), ack_(),
      link_(), rap_(myAddr, cfg.deviceRole, link_, &MeshRocStack::rapSendFn, this),
      tdma_(cfg.rf.tdma.enabled, cfg.rf.tdma.slotCount),
      crypto_(cfg), airtime_()
{
}

bool MeshRocStack::sendPayload(uint16_t dst, const uint8_t* payload, uint16_t len,
                               net::MeshRocPriority prio, bool wantAck)
{
    if (len == 0 || len > net::Reassembler::MAX_PAYLOAD) return false;

    // O2 TDMA：非本节点发送窗口则拒绝（关闭时恒放行）
    // TODO(rf): 真同步后改为"延迟到窗口"而非直接丢弃
    if (tdma_.enabled() && !tdma_.isMyTxWindow(currentTickApprox_, myAddr_)) {
        return false;  // 不在本节点时隙，丢弃（骨架阶段保守处理）
    }

    // O3 加密（当前 stub 直通）：密文写入 work 缓冲
    uint8_t cipher[net::Reassembler::MAX_PAYLOAD];
    uint16_t cipherLen = 0;
    if (crypto_.encrypt(payload, len, cipher, cipherLen, sizeof(cipher), myAddr_, dst)
            != net::CryptoHook::Result::OK) {
        return false;
    }

    // 路由（双层跳数）：深度优化 2，原样复刻 MeshRocModule::adaptiveHopLimit()
    // 以观测网络直径动态调整发出帧 max_hop；至少取配置默认。
    uint8_t hopLimit = router_.adaptiveHopLimit();

    if (cipherLen > net::Reassembler::MAX_PAYLOAD) return false;

    if (cipherLen > MAX_PAYLOAD_FRAG_THRESHOLD) {
        return sendFragmented(dst, cipher, cipherLen, prio, wantAck, hopLimit, fragIdSeq_++);
    }

    // 单帧：用 TEXT_MSG TLV 承载密文
    return emitSingle(dst, cipher, cipherLen, net::MeshRocPacketType::PRIVATE,
                      prio, wantAck, hopLimit, net::MeshRocTlv::TEXT_MSG);
}

bool MeshRocStack::emitSingle(uint16_t dst, const uint8_t* data, uint16_t len,
                              net::MeshRocPacketType opcode, net::MeshRocPriority prio,
                              bool wantAck, uint8_t hopLimit, net::MeshRocTlv payloadTag)
{
    net::MeshRocPacket h{};
    h.ctrl_flag  = net::MeshRocCodec::makeCtrlFlag(opcode, prio, wantAck, true, false);
    h.src        = myAddr_;
    h.dst        = dst;
    h.seq        = seq_++;
    h.max_hop    = hopLimit;
    // 深度优化 3：信噪比择优，把对端最佳链路 SNR 回填进帧（原版 getPeerSnr(toNode)，此前恒为 0）
    h.snr        = static_cast<uint8_t>(static_cast<int8_t>(link_.getPeerSnr(static_cast<uint32_t>(dst))));
    h.route_mode = (opcode == net::MeshRocPacketType::RAP) ? 2 : 0;

    // 计算 O4 ACK 基准超时（依赖 AirtimeModel，复刻原版 getRetransmissionMsec 公式）
    // 仅 wantAck 帧需要；基准 = airtime(整帧字节) 推导的重传超时。
    uint32_t baseTimeout = 0;
    if (wantAck) {
        // 当前预设意图 -> (sf,bw)（射频层未接入时退化为 LONG_FAST 默认参数）
        rf::LoraParams lp{};
        int presetVal = static_cast<int>(rf::ModemPreset::LONG_FAST);
        if (env_.hasPresetIntent()) {
            // 把 meshtastic 枚举值转 int 交给 AirtimeModel
            presetVal = static_cast<int>(env_.intendedPreset());
        }
        rf::AirtimeModel::presetToParams(presetVal, /*wideLora=*/false, lp);
        const uint16_t frameBytes = net::MeshRocPacket::HEADER_LEN + len + 2;  // +CRC16
        baseTimeout = rf::AirtimeModel::retransmissionMsec(
            frameBytes, lp, channelUtilization(), slotTimeMsec());
        // AckPolicy 按优先级做倍率微调（不另立超时表，基准即原版同构）
        net::AckPlan plan = ack_.planFor(prio, /*urgent=*/false);
        baseTimeout = ack_.resolveTimeoutMs(baseTimeout, plan);
    }
    (void)baseTimeout;  // TODO(rf): 接入原版 ReliableRouter 重传调度时作为超时依据

    // 组装帧：header(10B) + TLV + crc16(尾,大端)
    uint8_t frame[MAX_FRAME];
    writeHeader(frame, h);

    size_t off = TLV_OFF;
    size_t written = net::MeshRocCodec::appendTlv(frame, MAX_FRAME, off,
                                                 static_cast<uint8_t>(payloadTag), data, static_cast<uint8_t>(len));
    if (written == 0) return false;
    off += written;

    // CRC16 尾
    uint16_t crc = net::MeshRocCodec::crc16(frame, off);
    frame[off] = crc & 0xFF;
    frame[off + 1] = (crc >> 8) & 0xFF;
    off += 2;

    sendRaw(frame, static_cast<uint16_t>(off));
    return true;
}

bool MeshRocStack::sendFragmented(uint16_t dst, const uint8_t* payload, uint16_t len,
                                  net::MeshRocPriority prio, bool wantAck, uint8_t hopLimit,
                                  uint16_t fragId)
{
    // O5 发送端分片：每片 = FRAG_HEADER TLV（fragId+total+seq）+ 本片密文
    const uint16_t chunkCap = MAX_PAYLOAD_FRAG_THRESHOLD;  // 每片载荷上限
    uint8_t total = static_cast<uint8_t>((len + chunkCap - 1) / chunkCap);
    if (total == 0 || total > net::Reassembler::MAX_FRAG_TOTAL) return false;

    // 登记发送端重传缓存（FRAG_NACK 选择性重传需要后续重发缺失片）
    PendingFrag* pf = nullptr;
    for (uint8_t i = 0; i < kMaxPendingFragGroups; ++i) {
        if (!pendingFrags_[i].active) { pf = &pendingFrags_[i]; break; }
    }
    if (!pf) {
        // 缓存槽满：丢弃最旧（近似 LRU），腾出空间
        uint32_t oldest = 0xFFFFFFFF; uint8_t oi = 0;
        for (uint8_t i = 0; i < kMaxPendingFragGroups; ++i)
            if (pendingFrags_[i].lastSentMs < oldest) { oldest = pendingFrags_[i].lastSentMs; oi = i; }
        pf = &pendingFrags_[oi];
    }
    uint8_t groupIdx = static_cast<uint8_t>(pf - pendingFrags_);

    pf->active = true;
    pf->dst = dst;
    pf->fragId = fragId;
    pf->chunkCap = 0;
    pf->total = total;
    pf->count = 0;
    pf->retries = 0;
    pf->lastSentMs = currentTickApprox_;

    // 计算 O4 基准重传超时（airtime + 信道占用 CW），委托 AckPolicy 倍率微调，
    // 不自造超时表（遵循 ACK/重传铁律：复用原版同构公式）。
    {
        rf::LoraParams lp{};
        int presetVal = static_cast<int>(rf::ModemPreset::LONG_FAST);
        if (env_.hasPresetIntent()) presetVal = static_cast<int>(env_.intendedPreset());
        rf::AirtimeModel::presetToParams(presetVal, /*wideLora=*/false, lp);
        const uint16_t frameBytes = net::MeshRocPacket::HEADER_LEN + (4 + chunkCap) + 2;
        uint32_t baseT = rf::AirtimeModel::retransmissionMsec(frameBytes, lp,
                                                              channelUtilization(), slotTimeMsec());
        net::AckPlan plan = ack_.planFor(prio, /*priorityLevelsEnabled=*/false);
        pf->retransMsec = ack_.resolveTimeoutMs(baseT, plan);
    }
    for (uint8_t s = 0; s < total; ++s) pf->missing[s] = false;

    for (uint8_t seq = 0; seq < total; ++seq) {
        uint16_t chunkOff = static_cast<uint16_t>(seq) * chunkCap;
        uint16_t chunkLen = (seq + 1 == total) ? (len - chunkOff) : chunkCap;

        // FRAG_HEADER value = [fragId u16 BE][total u8][seq u8][chunk...]  (D3: 大端)
        uint8_t fragVal[4 + kMaxFragChunkPayload];
        putU16(fragVal, fragId);
        fragVal[2] = total;
        fragVal[3] = seq;
        ::memcpy(fragVal + 4, payload + chunkOff, chunkLen);

        // 直接构造整帧并缓存，便于 NACK 触发时精准重发本片
        uint8_t frame[MAX_FRAME];
        uint16_t frameLen = buildFragmentFrame(frame, sizeof(frame), dst, fragVal,
                                               static_cast<uint16_t>(4 + chunkLen),
                                               prio, wantAck, hopLimit, seq);
        if (frameLen == 0) { pf->active = false; return false; }

        // 持久缓存（重传需要）
        ::memcpy(fragStore_[groupIdx][seq], frame, frameLen);
        pf->chunk[seq] = fragStore_[groupIdx][seq];
        pf->chunkLen[seq] = frameLen;
        pf->count = static_cast<uint8_t>(seq + 1);

        sendRaw(frame, frameLen);
    }
    return true;
}

// 构造一个 FRAG_HEADER 分片的完整 MeshRocPacket 帧（header+TLV+CRC16），返回帧长
uint16_t MeshRocStack::buildFragmentFrame(uint8_t* out, uint16_t outCap, uint16_t dst,
                                         const uint8_t* fragVal, uint16_t fragValLen,
                                         net::MeshRocPriority prio, bool wantAck, uint8_t hopLimit,
                                         uint8_t /*seq*/)
{
    net::MeshRocPacket h{};
    h.ctrl_flag  = net::MeshRocCodec::makeCtrlFlag(net::MeshRocPacketType::PRIVATE, prio, wantAck, true, false);
    h.src        = myAddr_;
    h.dst        = dst;
    h.seq        = seq_++;
    h.max_hop    = hopLimit;
    // 深度优化 3：信噪比择优（与单帧一致，回填对端 SNR）
    h.snr        = static_cast<uint8_t>(static_cast<int8_t>(link_.getPeerSnr(static_cast<uint32_t>(dst))));
    h.route_mode = 0;

    // 帧头(10B) + TLV + crc16(尾,大端)
    uint8_t frame[MAX_FRAME];
    writeHeader(frame, h);

    size_t off = TLV_OFF;
    size_t written = net::MeshRocCodec::appendTlv(frame, MAX_FRAME, off,
                                                 static_cast<uint8_t>(net::MeshRocTlv::FRAG_HEADER),
                                                 fragVal, static_cast<uint8_t>(fragValLen));
    if (written == 0) return 0;
    off += written;

    uint16_t crc = net::MeshRocCodec::crc16(frame, off);
    frame[off] = crc & 0xFF;
    frame[off + 1] = (crc >> 8) & 0xFF;
    off += 2;

    if (off > outCap) return 0;
    ::memcpy(out, frame, off);
    return static_cast<uint16_t>(off);
}

void MeshRocStack::sendRapFrame(const uint8_t* rapBytes, uint16_t rapLen, uint32_t /*nowMs*/)
{
    // RAP 消息体包进 RapTlv::KIND(0x20) TLV 发出
    emitSingle(0xFFFF, rapBytes, rapLen, net::MeshRocPacketType::RAP,
               net::MeshRocPriority::LOW, /*wantAck=*/false, 1,
               static_cast<net::MeshRocTlv>(static_cast<uint8_t>(net::rap::RapTlv::KIND)));
}

// RAP 引擎发送回调适配器（RapSendFn 签名）→ 转发到本栈 sendRapFrame
void MeshRocStack::rapSendFn(void* ctx, const uint8_t* rapBody, uint16_t len, uint16_t /*dstShort*/)
{
    auto* self = static_cast<MeshRocStack*>(ctx);
    if (self) self->sendRapFrame(rapBody, len, 0);
}

// 四相混合路由发送回调适配器（RouteSendFn 签名）→ 转发到本栈唯一字节出口 sendRaw
void MeshRocStack::routeSendFn(void* ctx, const uint8_t* frame, uint16_t len)
{
    auto* self = static_cast<MeshRocStack*>(ctx);
    if (self) self->sendRaw(frame, len);
}

void MeshRocStack::ingestRaw(const uint8_t* frame, uint16_t len, uint32_t nowMs, int8_t rxSnr)
{
    if (len < TLV_OFF + 2) return;  // 至少 header+crc16
    // CRC16 校验
    uint16_t gotCrc = static_cast<uint16_t>(frame[len - 2]) |
                      (static_cast<uint16_t>(frame[len - 1]) << 8);
    uint16_t calcCrc = net::MeshRocCodec::crc16(frame, len - 2);
    if (gotCrc != calcCrc) return;

    net::MeshRocPacket h{};
    if (!parseHeader(frame, h)) return;

    // 记录对端链路 SNR（平移 MeshRocModule::recordLinkSnr），供 RAP 归属择优
    link_.recordLinkSnr(h.src, rxSnr, nowMs);
    // 深度优化 2：自适应跳数，用观测帧的 max_hop 估计网络直径（取历史观测最大值）
    router_.observeDiameter(h.max_hop, nowMs);

    const uint8_t* tlv = frame + TLV_OFF;
    uint16_t tlvLen = static_cast<uint16_t>(len - TLV_OFF - 2);

    net::MeshRocPacketType type = static_cast<net::MeshRocPacketType>(h.ctrl_flag & 0x07);
    if (type == net::MeshRocPacketType::RAP) {
        handleRap(tlv, tlvLen, nowMs, h.src);
        return;
    }
    // 数据帧：先经四相混合路由分发（D2 route_mode），本节点为接收方时回落 handleData
    handleRoute(h, frame, len, nowMs);
}

void MeshRocStack::handleRap(const uint8_t* tlv, uint16_t tlvLen, uint32_t nowMs, uint16_t fromNode)
{
    uint8_t vlen = 0;
    const uint8_t* rap = net::MeshRocCodec::findTlv(tlv, tlvLen,
                                                    static_cast<uint8_t>(net::rap::RapTlv::KIND), vlen);
    if (!rap || vlen < 1) return;
    rap_.onRapFrame(rap, vlen, nowMs, fromNode);
}

// 四相混合路由分发（D2 route_mode → PHASE1/2/4），平移自 MeshRocModule 路由逻辑：
//   route_mode 0 = 洪泛探测(PHASE1)：仅骨干中继（onRouteProbe）
//   route_mode 1 = 源路由(PHASE2)：严格按 ROUTE_PATH 跳点转发（onSourceRoute）
//   route_mode 2 = RAP 定向交付：本节点为目标才本地投递，否则不入洪泛（由 RAP 维护路径）
//   受限洪泛(PHASE4)：route_mode bit6=1 仅骨干中继（onFloodFrame）
// 命中本节点（dst==myAddr 或广播）则本地投递 handleData；否则按模式中继。
void MeshRocStack::handleRoute(const net::MeshRocPacket& pkt, const uint8_t* frame,
                               uint16_t frameLen, uint32_t nowMs)
{
    // 去重：避免探测/洪泛帧在网内无限重播
    if (router_.seenCheck(pkt.src, pkt.seq, nowMs)) return;

    const bool forMe = (pkt.dst == myAddr_) || (pkt.dst == 0xFFFF);

    switch (pkt.route_mode & 0x0F) {
    case 0:  // PHASE1 洪泛探测
        if (!forMe) router_.onRouteProbe(frame, frameLen, nowMs);
        break;
    case 1:  // PHASE2 源路由
        if (!forMe) router_.onSourceRoute(frame, frameLen, nowMs);
        break;
    case 2:  // RAP 定向：非本节点则不再洪泛（路径由 RAP 维护）
        break;
    default:
        break;
    }

    // 受限洪泛（PHASE4）：bit6 置位时仅骨干中继
    if ((pkt.route_mode & 0x40) && !forMe) {
        router_.onFloodFrame(frame, frameLen, nowMs);
    }

    // 本节点为接收方 → 本地投递（解分片/解 TLV/上抛应用）
    if (forMe) {
        const uint8_t* tlv = frame + TLV_OFF;
        uint16_t tlvLen = static_cast<uint16_t>(frameLen - TLV_OFF - 2);
        handleData(pkt, tlv, tlvLen, nowMs);
    }
}

void MeshRocStack::handleData(const net::MeshRocPacket& pkt, const uint8_t* tlv,
                              uint16_t tlvLen, uint32_t nowMs)
{
    // FRAG_NACK（选择性重传请求）？
    uint8_t nvlen = 0;
    const uint8_t* nack = net::MeshRocCodec::findTlv(tlv, tlvLen,
                                                     static_cast<uint8_t>(net::MeshRocTlv::FRAG_NACK), nvlen);
    if (nack) {
        handleFragNack(pkt, tlv, tlvLen, nowMs);
        return;
    }

    // O5 分片？
    uint8_t fvlen = 0;
    const uint8_t* frag = net::MeshRocCodec::findTlv(tlv, tlvLen,
                                                    static_cast<uint8_t>(net::MeshRocTlv::FRAG_HEADER), fvlen);
    if (frag && fvlen >= 4) {
        net::FragHeader fh;
        fh.fragId = getU16(frag);
        fh.total  = frag[2];
        fh.seq    = frag[3];
        const uint8_t* chunk = frag + 4;
        uint16_t chunkLen = static_cast<uint16_t>(fvlen - 4);

        uint16_t outLen = 0;
        if (reassembler_.feed(pkt.src, fh, chunk, chunkLen, reassembleBuf_, outLen, nowMs)) {
            if (outLen > 0) {
                // 重组完成：先解密（O3，当前直通）再回调
                uint8_t plain[net::Reassembler::MAX_PAYLOAD];
                uint16_t plainLen = 0;
                crypto_.decrypt(reassembleBuf_, outLen, plain, plainLen,
                               sizeof(plain), pkt.src, myAddr_);
                PacketReceived pr{ pkt.src, plain, plainLen, pkt.priority(), pkt.wantAck() };
                onPacket(pr);
                // 重组完成即收齐，通知发送方（清其重传缓存）
                sendFragNack(pkt.src, fh.fragId, nullptr, 0);
            }
        } else {
            // 尚未收齐：生成选择性重传 NACK（声明缺失分片序号），回给发送方
            uint8_t missing[net::Reassembler::MAX_FRAG_TOTAL];
            uint8_t nMissing = reassembler_.buildNack(pkt.src, fh.fragId, missing, sizeof(missing));
            if (nMissing > 0) {
                sendFragNack(pkt.src, fh.fragId, missing, nMissing);
            }
        }
        return;
    }

    // 普通数据：TEXT_MSG TLV
    uint8_t tvlen = 0;
    const uint8_t* text = net::MeshRocCodec::findTlv(tlv, tlvLen,
                                                    static_cast<uint8_t>(net::MeshRocTlv::TEXT_MSG), tvlen);
    if (text && tvlen > 0) {
        uint8_t plain[net::Reassembler::MAX_PAYLOAD];
        uint16_t plainLen = 0;
        crypto_.decrypt(text, tvlen, plain, plainLen, sizeof(plain), pkt.src, myAddr_);
        PacketReceived pr{ pkt.src, plain, plainLen, pkt.priority(), pkt.wantAck() };
        onPacket(pr);
    }
    // TODO(rf): ROUTE_PATH / 遥测等其它 TLV 处理
}

void MeshRocStack::sendFragNack(uint16_t dst, uint16_t fragId, const uint8_t* missingSeqs,
                                uint8_t missingCount)
{
    // FRAG_NACK value = [fragId u16 BE][total? 此处用 0 占位][nMissing u8][missingSeqs...]  (D3: 大端)
    // 发送方用 fragId 在 pendingFrags_ 找到本组即可；nMissing=0 表示"收齐/放弃"确认。
    uint8_t val[2 + 1 + net::Reassembler::MAX_FRAG_TOTAL];
    putU16(val, fragId);
    val[2] = missingCount;
    if (missingCount > 0 && missingSeqs) {
        ::memcpy(val + 3, missingSeqs, missingCount);
    }
    uint8_t valLen = static_cast<uint8_t>(3 + missingCount);

    emitSingle(dst, val, valLen, net::MeshRocPacketType::PRIVATE,
               net::MeshRocPriority::LOW, /*wantAck=*/false, /*hopLimit=*/1,
               net::MeshRocTlv::FRAG_NACK);
}

void MeshRocStack::handleFragNack(const net::MeshRocPacket& pkt, const uint8_t* tlv,
                                  uint16_t tlvLen, uint32_t /*nowMs*/)
{
    uint8_t vlen = 0;
    const uint8_t* nack = net::MeshRocCodec::findTlv(tlv, tlvLen,
                                                     static_cast<uint8_t>(net::MeshRocTlv::FRAG_NACK), vlen);
    if (!nack || vlen < 3) return;

    uint16_t fragId = getU16(nack);
    uint8_t nMissing = nack[2];
    const uint8_t* missing = nack + 3;

    // 在发送端缓存中找到对应组
    PendingFrag* pf = nullptr;
    for (uint8_t i = 0; i < kMaxPendingFragGroups; ++i) {
        if (pendingFrags_[i].active && pendingFrags_[i].fragId == fragId &&
            pendingFrags_[i].dst == pkt.src) {
            pf = &pendingFrags_[i];
            break;
        }
    }
    if (!pf) return;  // 不是本节点发的分组，或已清理

    if (nMissing == 0) {
        // 对端确认收齐：释放缓存
        pf->active = false;
        return;
    }

    // 选择性重发缺失分片
    for (uint8_t m = 0; m < nMissing && m < net::Reassembler::MAX_FRAG_TOTAL; ++m) {
        uint8_t seq = missing[m];
        if (seq < pf->count && pf->chunk[seq] && pf->chunkLen[seq] > 0) {
            sendRaw(pf->chunk[seq], pf->chunkLen[seq]);
        }
    }
    pf->lastSentMs = currentTickApprox_;
}

void MeshRocStack::tick(uint32_t nowMs)
{
    currentTickApprox_ = nowMs;

    // O1 环境采样：周期把最近链路采样喂给 EnvProfile，刷新预设意图
    if (nowMs - lastEnvFeedMs_ >= ENV_FEED_PERIOD_MS) {
        lastEnvFeedMs_ = nowMs;
        // TODO(rf): 用真实 PHY 采样填充 LinkSample；当前用中性样本驱动分类
        rf::LinkSample s{};
        s.snrDb = 0; s.rssiDbm = -100; s.noiseFloorDbm = -110; s.rxOk = 1; s.rxTot = 1;
        env_.feedSample(s);
    }

    // O归属 RAP：终端状态机按 HELLO_PERIOD 驱动发 HELLO/KEEPALIVE
    uint8_t rapOut[net::rap::RapStateMachine::kRapMsgMax];
    uint16_t rapLen = rap_.tick(nowMs, rapOut, sizeof(rapOut));
    if (rapLen > 0) {
        sendRapFrame(rapOut, rapLen, nowMs);
    }

    // O5 分片重组超时清理
    if (nowMs - lastReassExpireMs_ >= REASS_EXPIRE_PERIOD_MS) {
        lastReassExpireMs_ = nowMs;
        reassembler_.expire(nowMs);
    }

    // 发送端分片重传：若超过 O4 基准超时仍未收到对端 NACK（说明其可能未收到/静默），
    // 主动整组重发一次；超过原版同构上限 NUM_RELIABLE_RETX(3) 次则放弃并清理。
    static constexpr uint8_t kMaxRetries = 3;  // 对应原版 NUM_RELIABLE_RETX
    for (uint8_t i = 0; i < kMaxPendingFragGroups; ++i) {
        PendingFrag& pf = pendingFrags_[i];
        if (!pf.active) continue;
        if (nowMs - pf.lastSentMs >= pf.retransMsec) {
            if (pf.retries >= kMaxRetries) {
                pf.active = false;  // 放弃，释放缓存
                continue;
            }
            // 整组重发（对端若有缺失会再回 NACK，精准补发）
            for (uint8_t s = 0; s < pf.count; ++s) {
                if (pf.chunk[s] && pf.chunkLen[s] > 0) {
                    sendRaw(pf.chunk[s], pf.chunkLen[s]);
                }
            }
            pf.retries++;
            pf.lastSentMs = nowMs;
        }
    }
}

}  // namespace meshroc
