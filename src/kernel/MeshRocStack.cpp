#include "MeshRocStack.h"
#include <cstring>
#include <algorithm>

namespace meshroc {

namespace {
// 帧格式：[10B header][TLV segment][2B crc16]，全小端短地址。
constexpr size_t HDR_OFF_CTRL   = 0;
constexpr size_t HDR_OFF_SRC    = 1;
constexpr size_t HDR_OFF_DST    = 3;
constexpr size_t HDR_OFF_SEQ    = 5;
constexpr size_t HDR_OFF_MAXHOP = 7;
constexpr size_t HDR_OFF_SNR    = 8;
constexpr size_t HDR_OFF_ROUTE  = 9;  // 第 10 字节为 CRC8，随后才是 TLV 段
constexpr size_t HDR_OFF_CRC    = 10;
constexpr size_t TLV_OFF        = 11;

void putU16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }

// 把 MeshRocPacket 头写入 buffer[0..9]（不含 CRC8 字节位），随后由调用方填 CRC。
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
    h.crc        = buf[HDR_OFF_CRC];
    return true;
}
}  // anonymous namespace

MeshRocStack::MeshRocStack(const config::MeshROCConfig& cfg, uint16_t myAddr, bool isBackbone)
    : cfg_(cfg), myAddr_(myAddr),
      env_(cfg), reassembler_(), router_(cfg), ack_(),
      peerCaps_(), rap_(myAddr, isBackbone),
      tdma_(cfg.rf.tdmaEnabled, cfg.rf.tdmaSlots),
      crypto_(cfg), airtime_()
{
}

bool MeshRocStack::sendPayload(uint16_t dst, const uint8_t* payload, uint16_t len,
                               MeshRocPriority prio, bool wantAck)
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

    // 路由（双层跳数）：datapack 硬下限取自配置；O1 路由由 NextHopRouter 协同
    uint8_t hopLimit = router_.effectiveHopLimit(cfg_.lora.hopLimit);

    if (cipherLen > net::Reassembler::MAX_PAYLOAD) return false;

    if (cipherLen > MAX_PAYLOAD_FRAG_THRESHOLD) {
        return sendFragmented(dst, cipher, cipherLen, prio, wantAck, hopLimit, fragIdSeq_++);
    }

    // 单帧：用 TEXT_MSG TLV 承载密文
    return emitSingle(dst, cipher, cipherLen, net::MeshRocPacketType::PRIVATE,
                      prio, wantAck, hopLimit, net::MeshRocTlv::TEXT_MSG);
}

bool MeshRocStack::emitSingle(uint16_t dst, const uint8_t* data, uint16_t len,
                              net::MeshRocPacketType opcode, MeshRocPriority prio,
                              bool wantAck, uint8_t hopLimit, net::MeshRocTlv payloadTag)
{
    net::MeshRocPacket h{};
    h.ctrl_flag  = net::MeshRocCodec::makeCtrlFlag(opcode, prio, wantAck, true, false);
    h.src        = myAddr_;
    h.dst        = dst;
    h.seq        = seq_++;
    h.max_hop    = hopLimit;
    h.snr        = 0;
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
        net::AckPolicy::AckPlan plan = ack_.planFor(prio, /*urgent=*/false);
        baseTimeout = ack_.resolveTimeoutMs(baseTimeout, plan);
    }
    (void)baseTimeout;  // TODO(rf): 接入原版 ReliableRouter 重传调度时作为超时依据

    // 组装帧：header + TLV + crc16
    uint8_t frame[MAX_FRAME];
    writeHeader(frame, h);
    // header CRC8（覆盖前 10 字节中的前 9 字节）
    net::MeshRocPacket hdrForCrc = h;
    net::MeshRocCodec::finalizeHeader(hdrForCrc);
    frame[HDR_OFF_CRC] = hdrForCrc.crc;

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
                                  MeshRocPriority prio, bool wantAck, uint8_t hopLimit,
                                  uint16_t fragId)
{
    // O5 发送端分片：每片 = FRAG_HEADER TLV（fragId+total+seq）+ 本片密文
    const uint16_t chunkCap = MAX_PAYLOAD_FRAG_THRESHOLD;  // 每片载荷上限
    uint8_t total = static_cast<uint8_t>((len + chunkCap - 1) / chunkCap);
    if (total == 0 || total > net::Reassembler::MAX_FRAG_TOTAL) return false;

    for (uint8_t seq = 0; seq < total; ++seq) {
        uint16_t chunkOff = static_cast<uint16_t>(seq) * chunkCap;
        uint16_t chunkLen = (seq + 1 == total) ? (len - chunkOff) : chunkCap;

        // FRAG_HEADER value = [fragId u16 LE][total u8][seq u8][chunk...]
        uint8_t fragVal[4 + chunkCap];
        putU16(fragVal, fragId);
        fragVal[2] = total;
        fragVal[3] = seq;
        ::memcpy(fragVal + 4, payload + chunkOff, chunkLen);

        if (!emitSingle(dst, fragVal, static_cast<uint16_t>(4 + chunkLen),
                        net::MeshRocPacketType::PRIVATE, prio, wantAck, hopLimit,
                        net::MeshRocTlv::FRAG_HEADER)) {
            return false;
        }
    }
    return true;
}

void MeshRocStack::sendRapFrame(const uint8_t* rapBytes, uint16_t rapLen, uint32_t /*nowMs*/)
{
    // RAP 消息体包进 RapTlv::KIND(0x20) TLV 发出
    emitSingle(0xFFFF, rapBytes, rapLen, net::MeshRocPacketType::RAP,
               MeshRocPriority::LOW, /*wantAck=*/false, 1,
               static_cast<net::MeshRocTlv>(static_cast<uint8_t>(net::rap::RapTlv::KIND)));
}

void MeshRocStack::ingestRaw(const uint8_t* frame, uint16_t len, uint32_t nowMs)
{
    if (len < TLV_OFF + 2) return;  // 至少 header+crc16
    // CRC16 校验
    uint16_t gotCrc = static_cast<uint16_t>(frame[len - 2]) |
                      (static_cast<uint16_t>(frame[len - 1]) << 8);
    uint16_t calcCrc = net::MeshRocCodec::crc16(frame, len - 2);
    if (gotCrc != calcCrc) return;

    net::MeshRocPacket h{};
    if (!parseHeader(frame, h)) return;
    if (!net::MeshRocCodec::verifyHeader(h)) return;

    const uint8_t* tlv = frame + TLV_OFF;
    uint16_t tlvLen = static_cast<uint16_t>(len - TLV_OFF - 2);

    net::MeshRocPacketType type = static_cast<net::MeshRocPacketType>(h.ctrl_flag & 0x07);
    if (type == net::MeshRocPacketType::RAP) {
        handleRap(tlv, tlvLen, nowMs);
        return;
    }
    handleData(h, tlv, tlvLen, nowMs);
}

void MeshRocStack::handleRap(const uint8_t* tlv, uint16_t tlvLen, uint32_t nowMs)
{
    uint8_t vlen = 0;
    const uint8_t* rap = net::MeshRocCodec::findTlv(tlv, tlvLen,
                                                    static_cast<uint8_t>(net::rap::RapTlv::KIND), vlen);
    if (!rap || vlen < 1) return;
    rap_.onRapFrame(rap, vlen, nowMs);
}

void MeshRocStack::handleData(const net::MeshRocPacket& pkt, const uint8_t* tlv,
                              uint16_t tlvLen, uint32_t nowMs)
{
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
                // 选择性重传：若后续有缺失，FRAG_NACK 由 reassembler.buildNack 生成（TODO(rf) 发送）
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

void MeshRocStack::tick(uint32_t nowMs)
{
    currentTickApprox_ = nowMs;

    // O1 环境采样：周期把最近链路采样喂给 EnvProfile，刷新预设意图
    if (nowMs - lastEnvFeedMs_ >= ENV_FEED_PERIOD_MS) {
        lastEnvFeedMs_ = nowMs;
        // TODO(rf): 用真实 PHY 采样填充 LinkSample；当前用中性样本驱动分类
        rf::EnvProfile::LinkSample s{};
        s.snrDb = 0; s.rssiDbm = -100; s.noiseFloorDbm = -110; s.rxOk = 1; s.rxTot = 1;
        env_.feedSample(s);
    }

    // O归属 RAP：终端状态机按 HELLO_PERIOD 驱动发 HELLO/KEEPALIVE
    uint8_t rapOut[net::kRapMaxFrame];
    uint16_t rapLen = rap_.tick(nowMs, rapOut, sizeof(rapOut));
    if (rapLen > 0) {
        sendRapFrame(rapOut, rapLen, nowMs);
    }

    // O5 分片重组超时清理
    if (nowMs - lastReassExpireMs_ >= REASS_EXPIRE_PERIOD_MS) {
        lastReassExpireMs_ = nowMs;
        reassembler_.expire(nowMs);
    }

    // TODO(o4): 重传队列超时（复用 AckPolicy + AirtimeModel 基准，接入射频后启用）
}

}  // namespace meshroc
