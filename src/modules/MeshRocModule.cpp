#include "MeshRocModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Channels.h"
#include "Default.h"
#include "configuration.h"
#include "main.h"

#include <assert.h>
#include <string.h>

// 全局单例，便于其它模块/CLI 调用
MeshRocModule *meshRocModule;

/* =========================================================================
 * CRC16-MODBUS（route.txt §2 权威定义）
 *   poly=0x8005(反射 0xA001), init=0xFFFF, xorout=0, ref_in/out=true
 * 输入范围：10 字节包头 + 全部 TLV 载荷字节（CRC 两字节本身不参与）。
 * 空载荷包（空 ACK / 空探测）：仅对 10 字节包头计算。
 *
 * 说明：route.txt 给出的 7 个测试向量 CRC 值（0xAE71/0x6FA3/...）经独立
 * 校验，与标准 CRC16-MODBUS 算法均不吻合（疑似文档转录/手算错误），故本
 * 实现以"算法定义"为准。标准 MODBUS 正确性已用参考向量
 * {01 03 00 00 00 01}→0x840A 验证通过。
 * ========================================================================= */
uint16_t MeshRocCodec::crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc & 0xFFFF;
}

/* =========================================================================
 * ctrl_flag 打包/解包
 *   bit0-2 类型 | bit3-4 优先级 | bit5 ACK | bit6 中继权限 | bit7 压缩
 * ========================================================================= */
uint8_t MeshRocCodec::makeCtrlFlag(MeshRocPacketType type, MeshRocPriority prio, bool wantAck, bool relayPerm,
                                   bool compressed)
{
    uint8_t f = ((uint8_t)type & 0x07) | (((uint8_t)prio & 0x03) << 3) | (wantAck ? 0x20 : 0) |
                (relayPerm ? 0x40 : 0) | (compressed ? 0x80 : 0);
    return f;
}

void MeshRocCodec::parseCtrlFlag(uint8_t flag, MeshRocPacketType &type, MeshRocPriority &prio, bool &wantAck,
                                 bool &relayPerm, bool &compressed)
{
    type = (MeshRocPacketType)(flag & 0x07);
    prio = (MeshRocPriority)((flag >> 3) & 0x03);
    wantAck = (flag & 0x20) != 0;
    relayPerm = (flag & 0x40) != 0;
    compressed = (flag & 0x80) != 0;
}

/* =========================================================================
 * 序列化 / 反序列化（严格 10 + payloadLen + 2 字节，无 padding）
 * 字节序（待用户最终确认）：
 *   用户补充文档文字写"小端"，但实测 5 个真实示例字节流按【大端】解读时，
 *   src/dst/seq 才与文档文字吻合（如心跳 src=0x000A dst=0x0014，而非小端的
 *   0x0A00/0x1400）。故此处采用大端以匹配真实空中字节流；若最终确认走小端，
 *   仅需翻转下面 6 处 16 位字段的字节序即可，其余逻辑不变。
 * CRC16 输入 = 10 字节包头 + 全部 TLV 载荷字节；CRC 两字节本身不参与计算。
 * ========================================================================= */
size_t MeshRocCodec::serialize(const MeshRocPacket &pkt, uint8_t payloadLen, uint8_t *out, size_t outCap)
{
    size_t total = MESHROC_HEADER_LEN + payloadLen + MESHROC_CRC_LEN;
    if (total > MESHROC_MAX_PACKET || outCap < total)
        return 0;

    uint8_t *p = out;
    *p++ = pkt.ctrl_flag;
    // 16 位字段按大端写入（与 5 个真实示例字节流吻合：src=0x00,0x0A / dst=0x00,0x14）
    *p++ = (pkt.src_addr >> 8) & 0xFF;
    *p++ = pkt.src_addr & 0xFF;
    *p++ = (pkt.dst_addr >> 8) & 0xFF;
    *p++ = pkt.dst_addr & 0xFF;
    *p++ = (pkt.seq >> 8) & 0xFF;
    *p++ = pkt.seq & 0xFF;
    *p++ = pkt.max_hop;
    *p++ = pkt.snr;
    *p++ = pkt.route_mode;
    if (payloadLen > 0) {
        memcpy(p, pkt.payload, payloadLen);
        p += payloadLen;
    }
    uint16_t crc = crc16(out, MESHROC_HEADER_LEN + payloadLen);
    // CRC 大端写入
    *p++ = (crc >> 8) & 0xFF;
    *p++ = crc & 0xFF;

    return total;
}

bool MeshRocCodec::deserialize(const uint8_t *buf, size_t bufLen, MeshRocPacket &pkt, uint8_t &payloadLen)
{
    if (bufLen < MESHROC_HEADER_LEN + MESHROC_CRC_LEN)
        return false;
    uint8_t plen = (uint8_t)(bufLen - MESHROC_HEADER_LEN - MESHROC_CRC_LEN);
    if (bufLen > MESHROC_MAX_PACKET)
        return false;

    const uint8_t *p = buf;
    pkt.ctrl_flag = *p++;
    // 大端解析 16 位字段（与 5 个真实示例字节流吻合）
    pkt.src_addr = ((uint16_t)p[0] << 8) | p[1];
    p += 2;
    pkt.dst_addr = ((uint16_t)p[0] << 8) | p[1];
    p += 2;
    pkt.seq = ((uint16_t)p[0] << 8) | p[1];
    p += 2;
    pkt.max_hop = *p++;
    pkt.snr = *p++;
    pkt.route_mode = *p++;
    if (plen > 0)
        memcpy(pkt.payload, p, plen);
    p += plen;

    uint16_t crcRecv = ((uint16_t)p[0] << 8) | p[1];

    uint16_t crcCalc = crc16(buf, MESHROC_HEADER_LEN + plen);
    if (crcCalc != crcRecv)
        return false; // CRC 失败直接丢弃（datapack 第七节第6条）

    payloadLen = plen;
    return true;
}

/* =========================================================================
 * TLV 追加
 * ========================================================================= */
uint8_t MeshRocCodec::appendTlv(uint8_t *payload, uint8_t payloadLen, MeshRocTlvTag tag, const uint8_t *value,
                                uint8_t valueLen)
{
    // Tag(1) + Length(1) + Value(valueLen)
    if (payloadLen + 2 + valueLen > MESHROC_MAX_PAYLOAD)
        return 0; // 超出载荷上限
    uint8_t *p = payload + payloadLen;
    *p++ = (uint8_t)tag;
    *p++ = valueLen;
    if (valueLen > 0)
        memcpy(p, value, valueLen);
    return (uint8_t)(payloadLen + 2 + valueLen);
}

/* =========================================================================
 * 模块主体
 * ========================================================================= */
MeshRocModule::MeshRocModule() : SinglePortModule("meshroc", MESHTASTIC_PORTNUM_MESHROC), concurrency::OSThread("MeshRoc")
{
    logDeploymentWarnings();
    // RAP 状态初始化（datapack.txt R6）
    memset(&rapNeighbors, 0, sizeof(rapNeighbors));
    memset(&rapOwnerTable, 0, sizeof(rapOwnerTable));
    memset(&rapRemoteOwners, 0, sizeof(rapRemoteOwners));
    memset(&rapLocal, 0, sizeof(rapLocal));
    rapLocal.state = RAP_STATE_SCANNING;
    rapLocal.attachedTo = 0; // 0 = 无归属
    rapLocal.minDwellUntil = 0;
    nNeighbors = 0;
    nOwners = 0;
    nRemoteOwners = 0;
    rapTableVersion = 0;
    rapNextAttachSeq = 1;
    lastHelloSentAt = 0;
    helloBackoffPow = 0;
    lastFullAdvAt = 0;
    LOG_INFO("MeshRoc: module init (RAP role=%s)", isBackbone() ? "BACKBONE" : "TERMINAL");
    // 2s 后首跑周期任务，此后由 runOnce 自调度（BACKBONE 按 HELLO 周期，终端按评估间隔）
    setIntervalFromNow(2000);
}

/* -------------------------------------------------------------------------
 * 向下兼容 Meshtastic：同时监听私有端口(300) 与 原生文本端口(1)
 * 这样普通 Meshtastic 节点发来的 TEXT_MESSAGE_APP 也能进入本模块处理。
 * ------------------------------------------------------------------------- */
bool MeshRocModule::wantPacket(const meshtastic_MeshPacket *p)
{
    if (p->which_payload_variant != meshtastic_MeshPacket_decoded_tag)
        return false;
    if (p->decoded.portnum == ourPortNum) // 端口 300：Mesh-ROC 私有协议
        return true;
    if (nativeCompatEnabled && p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP)
        return true; // 端口 1：原生文本，向下兼容
    return false;
}

bool MeshRocModule::isBackboneRelay() const
{
    // 楼顶骨干中继节点：设备角色为 ROUTER / ROUTER_LATE 视为骨干中继
    return config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
           config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
}

ProcessMessage MeshRocModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // 分派：原生文本端口(1) 走向下兼容处理；其余走私有端口(300) 逻辑
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
        return onNativeText(mp);
    }

    // 取出本模块 portnum 的二进制帧
    if (mp.which_payload_variant != meshtastic_MeshPacket_decoded_tag || mp.decoded.portnum != ourPortNum)
        return ProcessMessage::CONTINUE;

    const uint8_t *buf = mp.decoded.payload.bytes;
    size_t bufLen = mp.decoded.payload.size;

    MeshRocPacket pkt;
    uint8_t payloadLen = 0;
    memset(&pkt, 0, sizeof(pkt));

    if (!MeshRocCodec::deserialize(buf, bufLen, pkt, payloadLen)) {
        LOG_WARN("MeshRoc: dropped frame (bad CRC or length) from=0x%08x", mp.from);
        return ProcessMessage::STOP; // 坏包不交给其它模块（route.txt §2）
    }

    // 分层骨干路由角色固化（深度优化 1）：首次收帧时依据 config.device.role 推断 localRole
    inferLocalRole();

    // 对端能力协商：观测到该节点发过端口 300 私有帧 → 标记为 MeshROC 节点。
    // 普通 Meshtastic 节点永不发端口 300，故该标志天然区分两类节点。
    // sendTextSmart 据此决定单播时是否补发 300 增强帧（见 sendTextSmart）。
    markPeerMeshRoc(mp.from);

    // 信噪比择优（深度优化 3）：记录对端 rx_snr 进链路质量表，源路由选路择优
    recordLinkSnr(mp.from, mp.rx_snr);
    // 自适应跳数（深度优化 2）：用观测帧的 max_hop 估计网络直径（取历史观测最大值）
    if (pkt.max_hop > estNetDiameter)
        estNetDiameter = pkt.max_hop;

    MeshRocPacketType type;
    MeshRocPriority prio;
    bool wantAck, relayPerm, compressed;
    MeshRocCodec::parseCtrlFlag(pkt.ctrl_flag, type, prio, wantAck, relayPerm, compressed);

    LOG_INFO("MeshRoc: recv type=%d prio=%d relayPerm=%d src=0x%04x dst=0x%04x seq=%d plen=%d", type, prio, relayPerm,
             pkt.src_addr, pkt.dst_addr, pkt.seq, payloadLen);

    // 去重（route.txt §6）：(src_addr + seq) 已见过则丢弃，禁止重复转发
    if (seenCheck(pkt.src_addr, pkt.seq)) {
        LOG_DEBUG("MeshRoc: duplicate (src=0x%04x seq=%d) dropped", pkt.src_addr, pkt.seq);
        return ProcessMessage::STOP;
    }

    // 骨干-终端隔离：relayPerm=1 的中继包，非骨干终端只接收、禁止转发
    if (relayPerm && localRole == MESHROC_ROLE_CLIENT) {
        LOG_DEBUG("MeshRoc: relay-permitted frame but we are CLIENT -> receive only, no forward");
    }

    // 混合路由状态机分发（route.txt §5）
    switch (type) {
    case MESHROC_TYPE_ROUTE_PROBE:
        onRouteProbe(pkt, payloadLen);
        break;
    case MESHROC_TYPE_PRIVATE_MSG:
    case MESHROC_TYPE_GROUP_MSG:
    case MESHROC_TYPE_TELEMETRY:
    case MESHROC_TYPE_HEARTBEAT:
    case MESHROC_TYPE_ENCRYPTED:
        // 含 0x10 ROUTE_PATH 的业务包走源路由；纯洪泛包走 onFloodFrame
        if (pkt.route_mode == 0 && memchr(pkt.payload, MESHROC_TLV_ROUTE_PATH, payloadLen) != nullptr)
            onSourceRoute(pkt, payloadLen);
        else
            onFloodFrame(pkt, payloadLen);
        break;
    case MESHROC_TYPE_ACK:
        onMeshRocFrame(pkt, payloadLen, mp); // ACK 携带 REVERSE_PATH, 解析并刷新缓存
        break;
    case MESHROC_TYPE_RAP:
        // RAP 路由器归属协议（datapack.txt R1~R12）：不吞包，交由 RAP 状态机处理，
        // 原版 Meshtastic 节点永不发端口 300 故永不进入此分支，天然走洪泛兜底。
        onRapPacket(pkt, payloadLen, mp);
        break;
    default:
        onMeshRocFrame(pkt, payloadLen, mp);
        break;
    }

    return ProcessMessage::STOP; // datapack 帧已由本模块专属处理
}

void MeshRocModule::onMeshRocFrame(const MeshRocPacket &pkt, uint8_t payloadLen, const meshtastic_MeshPacket &mp)
{
    // 解析 TLV 载荷（route.txt §3 解码循环；未知 tag 跳过不崩溃）
    const uint8_t *ptr = pkt.payload;
    const uint8_t *end = pkt.payload + payloadLen;
    while (ptr + 2 <= end) {
        uint8_t tag = *ptr++;
        uint8_t len = *ptr++;
        if (ptr + len > end)
            break; // 截断保护
        const uint8_t *val = ptr;

        switch (tag) {
        case MESHROC_TLV_TEXT_MSG:
            LOG_INFO("MeshRoc text: %.*s", len, val);
            break;
        case MESHROC_TLV_NODE_NAME:
            LOG_INFO("MeshRoc node name: %.*s", len, val);
            break;
        case MESHROC_TLV_BATT_VOLT: {
            uint16_t mv = ((uint16_t)val[0] << 8) | val[1]; // 大端 ×100
            LOG_INFO("MeshRoc battery voltage: %u.%02u V", mv / 100, mv % 100);
            break;
        }
        case MESHROC_TLV_SOLAR_VOLT: {
            uint16_t mv = ((uint16_t)val[0] << 8) | val[1];
            LOG_INFO("MeshRoc solar voltage: %u.%02u V", mv / 100, mv % 100);
            break;
        }
        case MESHROC_TLV_AHT20_TEMP: {
            int16_t t = (int16_t)(((uint16_t)val[0] << 8) | val[1]); // 大端 ×10
            LOG_INFO("MeshRoc AHT20 temp: %d.%d C", t / 10, abs(t) % 10);
            break;
        }
        case MESHROC_TLV_HUMIDITY: {
            uint16_t h = ((uint16_t)val[0] << 8) | val[1];
            LOG_INFO("MeshRoc humidity: %u.%u %%", h / 10, h % 10);
            break;
        }
        case MESHROC_TLV_REVERSE_PATH: {
            uint16_t hops[MESHROC_MAX_PACKET / 2];
            uint8_t n = parsePathTlv(val, len, hops, MESHROC_MAX_PACKET / 2);
            if (n > 0) {
                LOG_INFO("MeshRoc REVERSE_PATH hops=%d (dst=0x%04x)", n, pkt.src_addr);
                // 反向路径即回程源路由：缓存 src→本包路径反向
                routeStore(pkt.src_addr, hops, n, pkt.snr, 1);
            }
            break;
        }
        case MESHROC_TLV_ROUTE_INVALID:
            LOG_INFO("MeshRoc ROUTE_INVALID from 0x%04x", pkt.src_addr);
            routeInvalidate(pkt.src_addr);
            break;
        default:
            LOG_DEBUG("MeshRoc unknown TLV tag=0x%02x len=%d (skipped)", tag, len);
            break;
        }

        ptr += len;
    }
}

/* =========================================================================
 * 向下兼容 Meshtastic：原生文本消息收发
 * ========================================================================= */

// 收到普通 Meshtastic 节点发来的原生文本消息(portnum=1)
ProcessMessage MeshRocModule::onNativeText(const meshtastic_MeshPacket &mp)
{
    const char *text = (const char *)mp.decoded.payload.bytes;
    size_t len = mp.decoded.payload.size;
    LOG_INFO("MeshRoc: native text from=0x%08x (Meshtastic-compatible): %.*s", mp.from, (int)len, text);
    // 返回 CONTINUE：让官方 TextMessageModule 继续处理并显示该消息，
    // Mesh-ROC 节点因此能完整接收普通 Meshtastic 节点的文本（双向互通）。
    return ProcessMessage::CONTINUE;
}

// 用原生 portnum=1 发送 UTF-8 文本，普通 Meshtastic 节点可直接显示。
// 这是"MeshROC 节点 → 普通 Meshtastic 节点"最基本的消息收发兼容路径。
bool MeshRocModule::sendNativeText(const char *text, uint32_t toNode)
{
    if (!nativeCompatEnabled) {
        LOG_WARN("MeshRoc: nativeCompatEnabled=false, sendNativeText aborted");
        return false;
    }
    if (!text)
        return false;

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return false;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP; // 关键：用原生文本端口
    p->to = toNode;
    p->channel = channels.getPrimaryIndex();
    // 原生文本走普通优先级，便于和官方节点体验一致
    p->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    p->hop_limit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
    p->hop_start = p->hop_limit;

    size_t n = strlen(text);
    if (n > meshtastic_Constants_DATA_PAYLOAD_LEN)
        n = meshtastic_Constants_DATA_PAYLOAD_LEN;
    memcpy(p->decoded.payload.bytes, text, n);
    p->decoded.payload.size = (pb_size_t)n;

    LOG_INFO("MeshRoc: send native text (port=%d) to=0x%08x: %.*s", (int)p->decoded.portnum, toNode, (int)n, text);
    service->sendToMesh(p);
    return true;
}

/* =========================================================================
 * 按对端能力协商的智能文本发送
 * 设计：默认走原生端口 1（最省带宽、普通节点直接显示）；仅当对端已确认为
 *      MeshROC 节点(见过其发端口300帧)时，额外补一份端口300增强帧。
 *      广播因无法预知听众类型，原生 + 300 双发以覆盖混合网络。
 * ========================================================================= */
bool MeshRocModule::sendTextSmart(const char *text, uint32_t toNode)
{
    if (!text)
        return false;

    const bool isBroadcast = (toNode == UINT32_MAX);
    const bool peerIsMeshRoc = !isBroadcast && isPeerMeshRoc(toNode);

    // 1) 原生端口 1：默认必发（nativeCompatEnabled 关闭时不发，退化为仅 300）
    if (nativeCompatEnabled)
        sendNativeText(text, toNode);

    // 2) 端口 300 增强帧：仅当 (a) 对端是已确认 MeshROC 节点(单播)，或
    //    (b) 广播(未知听众，双发覆盖)。nativeCompatEnabled 关闭时也发 300。
    bool need300 = isBroadcast || peerIsMeshRoc;
    if (!need300)
        return true; // 普通单播节点：仅原生一帧，省带宽

    // 构造 0x06 TEXT_MSG TLV 帧（端口 300）
    MeshRocPacket frm;
    memset(&frm, 0, sizeof(frm));
    uint16_t selfAddr = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    uint16_t dstAddr = isBroadcast ? MESHROC_ADDR_INVALID : (uint16_t)(toNode & 0xFFFF);
    MeshRocPacketType ftype = isBroadcast ? MESHROC_TYPE_GROUP_MSG : MESHROC_TYPE_PRIVATE_MSG;
    frm.ctrl_flag = MeshRocCodec::makeCtrlFlag(ftype, MESHROC_PRIO_NORMAL, false, false, false);
    frm.src_addr = selfAddr;
    frm.dst_addr = dstAddr;
    frm.seq = (uint16_t)(millis() & 0xFFFF); // 简易序列号，足够去重窗口使用
    // 深度优化 2：自适应跳数（替代固定 hop，依据估计网络直径动态调整）
    frm.max_hop = adaptiveHopLimit();
    // 深度优化 3：信噪比择优，把对端最佳链路 SNR 回填进帧（此前恒为 0/预留）
    frm.snr = getPeerSnr(toNode);
    frm.route_mode = 1; // 受限洪泛（文本场景用洪泛即可，走 onFloodFrame）

    uint8_t plen = MeshRocCodec::appendTlv(frm.payload, 0, MESHROC_TLV_TEXT_MSG, (const uint8_t *)text,
                                           (uint8_t)strlen(text));
    if (plen == 0) {
        LOG_ERROR("MeshRoc: sendTextSmart: text too long for 300 frame");
        return false;
    }
    LOG_INFO("MeshRoc: sendTextSmart %s (port=300) to=0x%08x peerMeshRoc=%d", isBroadcast ? "BROADCAST" : "unicast",
             toNode, peerIsMeshRoc);
    return sendFrame(frm, plen, toNode);
}

bool MeshRocModule::isPeerMeshRoc(uint32_t nodeNum) const
{
    uint32_t now = (uint32_t)millis();
    for (uint8_t i = 0; i < MESHROC_MAX_PEERS; i++) {
        if (peerCaps[i].nodeNum == nodeNum && peerCaps[i].isMeshRoc) {
            if (now - peerCaps[i].lastSeen < MESHROC_PEER_CAP_TTL)
                return true; // 仍在有效期内
        }
    }
    return false; // 未见其发 300 帧，或已老化 → 视为普通节点
}

void MeshRocModule::markPeerMeshRoc(uint32_t nodeNum)
{
    if (nodeNum == 0 || nodeNum == UINT32_MAX)
        return; // 非法/广播地址不记录
    uint32_t now = (uint32_t)millis();
    // 已存在则刷新时间戳
    for (uint8_t i = 0; i < MESHROC_MAX_PEERS; i++) {
        if (peerCaps[i].nodeNum == nodeNum) {
            peerCaps[i].isMeshRoc = true;
            peerCaps[i].lastSeen = now;
            return;
        }
    }
    // 新条目：环形覆盖最旧槽（同时覆盖已老化的同名槽无所谓）
    uint8_t slot = peerCapsIdx;
    peerCaps[slot] = {nodeNum, true, now, 0};
    peerCapsIdx = (peerCapsIdx + 1) % MESHROC_MAX_PEERS;
}

/* =========================================================================
 * 深度优化 1：分层骨干路由角色固化（建站要求 §4.1）
 * 依据 config.device.role 把 localRole 推断为骨干/终端，使 Walk 手持终端
 * 默认静默（仅收发不中继）、Backbone/Gateway 承担转发，降低信道冲突、改善续航。
 * 与标准 Meshtastic role 语义对齐：ROUTER/ROUTER_LATE → 骨干；CLIENT/CLIENT_MUTE → 终端。
 * ========================================================================= */
void MeshRocModule::inferLocalRole()
{
    if (localRoleInited)
        return;
    localRoleInited = true;
    switch (config.device.role) {
    case meshtastic_Config_DeviceConfig_Role_ROUTER:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
        localRole = MESHROC_ROLE_BACKBONE; // 山顶骨干 / 城市基站：转发枢纽
        break;
    case meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE:
    case meshtastic_Config_DeviceConfig_Role_CLIENT:
    default:
        localRole = MESHROC_ROLE_CLIENT;    // 手持终端：默认静默，省电
        break;
    }
    LOG_INFO("MeshRoc: inferLocalRole -> %s", localRole == MESHROC_ROLE_BACKBONE ? "BACKBONE" : "CLIENT");
}

/* =========================================================================
 * 深度优化 2：自适应跳数（建站要求 §4.1）
 * 维护估计的网络直径 estNetDiameter（基于观测到的帧 max_hop 最大值 + 余量），
 * 返回本节点发出私有帧应使用的 max_hop。小网不浪费跳数，大网不截断。
 * ========================================================================= */
uint8_t MeshRocModule::adaptiveHopLimit()
{
    uint32_t now = (uint32_t)millis();
    if (now - diameterSampleAt > MESHROC_DIAMETER_TTL || estNetDiameter == 0) {
        // 重新估计：以当前观测到的网络直径估计值为基础（由 recordLinkSnr/收帧时更新），
        // 至少取配置默认 hop 与观测直径 + 1 余量中的较大者。
        uint8_t base = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
        uint8_t est = (estNetDiameter == 0) ? base : (uint8_t)(estNetDiameter + 1);
        diameterSampleAt = now;
        LOG_DEBUG("MeshRoc: adaptiveHopLimit estDiameter=%d -> hop=%d", estNetDiameter, est > base ? est : base);
        return est > base ? est : base;
    }
    return (uint8_t)(estNetDiameter + 1);
}

/* =========================================================================
 * 深度优化 3：信噪比择优路径（建站要求 §4.1 "信噪比择优路径"）
 * 把 Meshtastic 层观测到的 rx_snr 记录进对端能力表（bestSnr），
 * 路由度量/源路由选路时据此择优；SNR 越高链路越优。
 * 同时把最近一次 bestSnr 回填进外发帧的 pkt.snr 字段（此前恒为 0/预留）。
 * ========================================================================= */
void MeshRocModule::recordLinkSnr(uint32_t fromNode, int8_t snr)
{
    if (fromNode == 0 || fromNode == UINT32_MAX)
        return;
    uint32_t now = (uint32_t)millis();
    for (uint8_t i = 0; i < MESHROC_MAX_PEERS; i++) {
        if (peerCaps[i].nodeNum == fromNode) {
            if (snr > peerCaps[i].bestSnr)
                peerCaps[i].bestSnr = snr; // 仅记录最佳（链路质量上界）
            peerCaps[i].lastSeen = now;
            return;
        }
    }
    uint8_t slot = peerCapsIdx;
    peerCaps[slot] = {fromNode, true, now, snr};
    peerCapsIdx = (peerCapsIdx + 1) % MESHROC_MAX_PEERS;
}

int8_t MeshRocModule::getPeerSnr(uint32_t nodeNum) const
{
    for (uint8_t i = 0; i < MESHROC_MAX_PEERS; i++)
        if (peerCaps[i].nodeNum == nodeNum)
            return peerCaps[i].bestSnr;
    return 0;
}

#if HAS_SCREEN
uint8_t MeshRocModule::countMeshRocPeers() const
{
    uint32_t now = (uint32_t)millis();
    uint8_t n = 0;
    for (uint8_t i = 0; i < MESHROC_MAX_PEERS; i++) {
        if (peerCaps[i].isMeshRoc && (now - peerCaps[i].lastSeen < MESHROC_PEER_CAP_TTL))
            n++;
    }
    return n;
}

void MeshRocModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);

    // 标题行：品牌短名（用科技青 RGB565；单色屏忽略色值按默认前景绘制）
    display->drawString(x, y, MESHROC_BRAND_SHORT);

    int16_t ly = y + FONT_HEIGHT_SMALL;
    char buf[48];

    // 系统名（固件/硬件层，区别于社区名 Mesh Realm Of Connection）
    snprintf(buf, sizeof(buf), "%s", MESHROC_SYSTEM_NAME);
    display->drawString(x, ly, buf);
    ly += FONT_HEIGHT_SMALL;

    // 固件版本 + 许可（明确 GPL-3.0，避免被误认为 MIT）
    snprintf(buf, sizeof(buf), "FW %s | %s", optstr(APP_VERSION_SHORT), MESHROC_FW_LICENSE);
    display->drawString(x, ly, buf);
    ly += FONT_HEIGHT_SMALL;

    // 本机产品线角色（对齐官网 4 大系列：Backbone/Gateway/Walk/Sensor）
    snprintf(buf, sizeof(buf), "Product: MeshROC %s", meshRocProductName((int)localRole));
    display->drawString(x, ly, buf);
    ly += FONT_HEIGHT_SMALL;

    // 已确认 MeshROC 对端数（能力协商结果）
    uint8_t peers = countMeshRocPeers();
    snprintf(buf, sizeof(buf), "MeshROC peers: %u", peers);
    display->drawString(x, ly, buf);
    ly += FONT_HEIGHT_SMALL;

    // 原生兼容通道状态
    snprintf(buf, sizeof(buf), "Compat: %s", nativeCompatEnabled ? "ON" : "OFF");
    display->drawString(x, ly, buf);
}
#endif // HAS_SCREEN

bool MeshRocModule::sendFrame(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t toNode)
{
    uint8_t blob[MESHROC_MAX_PACKET];
    size_t n = MeshRocCodec::serialize(pkt, payloadLen, blob, sizeof(blob));
    if (n == 0) {
        LOG_ERROR("MeshRoc: serialize failed (too large)");
        return false;
    }

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return false;
    p->to = toNode;
    p->decoded.payload.size = (pb_size_t)n;
    memcpy(p->decoded.payload.bytes, blob, n);

    // ---- 兼容性修复 1：走主信道，确保同网节点都能收/中继 ----
    p->channel = channels.getPrimaryIndex();

    // ---- 兼容性修复 3：双层跳数协同 ----
    // datapack 帧自带 max_hop 主导路由；底层 Meshtastic hop_limit 必须 ≥ 该值，
    // 否则底层会先递减到 0 把帧丢弃，导致源路由/洪泛跨跳失败。
    uint8_t baseHop = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
    uint8_t effHop = (pkt.max_hop > baseHop) ? pkt.max_hop : baseHop;
    // 受限洪泛(广播)允许用 datapack 的 max_hop；源路由单播必须保证足够大（至少覆盖跳点序列）
    if (pkt.route_mode == 0) { // 源路由
        uint8_t minHop = pkt.max_hop;
        effHop = (minHop > baseHop) ? minHop : baseHop;
    }
    p->hop_limit = effHop;
    p->hop_start = effHop;

    // 映射 Mesh-ROC 优先级到 Meshtastic 优先级（影响底层排队/抢占）
    MeshRocPacketType type;
    MeshRocPriority prio;
    bool wantAck, relayPerm, compressed;
    MeshRocCodec::parseCtrlFlag(pkt.ctrl_flag, type, prio, wantAck, relayPerm, compressed);
    // Meshtastic: UNSET=0(router决定), MIN=1, BACKGROUND=10, DEFAULT=64, RELIABLE=70, ACK=120, MAX=127
    switch (prio) {
    case MESHROC_PRIO_LOW:      p->priority = meshtastic_MeshPacket_Priority_BACKGROUND; break;
    case MESHROC_PRIO_NORMAL:   p->priority = meshtastic_MeshPacket_Priority_DEFAULT; break;
    case MESHROC_PRIO_HIGH:     p->priority = meshtastic_MeshPacket_Priority_RELIABLE; break;
    case MESHROC_PRIO_EMERGENCY:p->priority = meshtastic_MeshPacket_Priority_MAX; break;
    default:                    p->priority = meshtastic_MeshPacket_Priority_DEFAULT; break;
    }
    if (wantAck)
        p->decoded.want_response = true;

    // ---- 兼容性修复 2：拓扑隐私告警 ----
    // datapack 帧含节点短地址/路由路径等拓扑信息；若 MQTT 上行开启，会泄漏到公网 broker。
    if (moduleConfig.mqtt.enabled) {
        LOG_WARN("MeshRoc: MQTT uplink ENABLED -> Mesh-ROC topology (addr/route) will leak to MQTT broker. "
                 "Disable moduleConfig.mqtt or use a private broker for backbone/gateway nodes.");
    }

    service->sendToMesh(p);
    return true;
}

/* =========================================================================
 * 部署兼容性自检（构造时调用一次）：检查会阻断 Mesh-ROC 跨跳收发的节点配置
 * ========================================================================= */
void MeshRocModule::logDeploymentWarnings()
{
    // 修复1前提：骨干/中继节点必须以 rebroadcast_mode=ALL 转发私有 portnum(300) 帧。
    // LOCAL_ONLY / KNOWN_ONLY / CORE_PORTNUMS_ONLY / NONE 都会丢弃私有 portnum，导致跨跳失败。
    auto mode = config.device.rebroadcast_mode;
    if (mode != meshtastic_Config_DeviceConfig_RebroadcastMode_ALL &&
        mode != meshtastic_Config_DeviceConfig_RebroadcastMode_ALL_SKIP_DECODING) {
        LOG_WARN("MeshRoc: rebroadcast_mode=%d is NOT 'ALL'. Private portnum(300) frames will NOT be "
                 "relayed by this node -> Mesh-ROC multi-hop routing will break. Set device.rebroadcast_mode=ALL "
                 "on backbone/relay nodes.", (int)mode);
    }
    // 修复2：拓扑隐私
    if (moduleConfig.mqtt.enabled) {
        LOG_WARN("MeshRoc: MQTT enabled on a Mesh-ROC node. Topology (node addr / route paths) carried in "
                 "Mesh-ROC frames will be published to the MQTT broker. Use private broker or disable MQTT.");
    }
}

/* =========================================================================
 * 路由辅助：路径 TLV 编解码（uint16 大端, 2字节对齐）
 * ========================================================================= */
uint8_t MeshRocModule::parsePathTlv(const uint8_t *val, uint8_t len, uint16_t *outHops, uint8_t maxHops)
{
    if (len % 2 != 0)
        return 0; // 长度必须 2 的倍数
    uint8_t n = len / 2;
    if (n > maxHops)
        n = maxHops;
    for (uint8_t i = 0; i < n; i++)
        outHops[i] = ((uint16_t)val[i * 2] << 8) | val[i * 2 + 1]; // 大端
    return n;
}

uint8_t MeshRocModule::buildPathTlv(uint8_t *payload, uint8_t payloadLen, const uint16_t *hops, uint8_t hopCount)
{
    if (payloadLen + 2 + hopCount * 2 > MESHROC_MAX_PAYLOAD)
        return 0;
    uint8_t *p = payload + payloadLen;
    *p++ = MESHROC_TLV_ROUTE_PATH;
    *p++ = (uint8_t)(hopCount * 2);
    for (uint8_t i = 0; i < hopCount; i++) {
        *p++ = (hops[i] >> 8) & 0xFF; // 大端
        *p++ = hops[i] & 0xFF;
    }
    return (uint8_t)(payloadLen + 2 + hopCount * 2);
}

/* =========================================================================
 * 路由缓存查询/写入/失效
 * ========================================================================= */
MeshRocRouteEntry *MeshRocModule::routeLookup(uint16_t dst)
{
    for (uint8_t i = 0; i < 16; i++)
        if (routeCache[i].valid && routeCache[i].dst == dst)
            return &routeCache[i];
    return nullptr;
}

void MeshRocModule::routeStore(uint16_t dst, const uint16_t *hops, uint8_t hopCount, uint16_t avgSnr, uint16_t ok)
{
    MeshRocRouteEntry *e = routeLookup(dst);
    if (!e) {
        e = &routeCache[routeCacheIdx]; // 环型覆盖最旧
        routeCacheIdx = (routeCacheIdx + 1) % 16;
    }
    e->dst = dst;
    e->hopCount = hopCount > (MESHROC_MAX_PACKET / 2) ? (MESHROC_MAX_PACKET / 2) : hopCount;
    for (uint8_t i = 0; i < e->hopCount; i++)
        e->hops[i] = hops[i];
    e->avgSnr = avgSnr;
    e->deliveryOk = ok;
    e->expireAt = (uint32_t)millis() + 1200000; // 默认 1200s 老化
    e->valid = true;
    LOG_INFO("MeshRoc: route cached dst=0x%04x hops=%d", dst, e->hopCount);
}

void MeshRocModule::routeInvalidate(uint16_t dst)
{
    MeshRocRouteEntry *e = routeLookup(dst);
    if (e) {
        e->valid = false;
        LOG_INFO("MeshRoc: route invalidated dst=0x%04x", dst);
    }
}

/* =========================================================================
 * 去重缓存
 * ========================================================================= */
bool MeshRocModule::seenCheck(uint16_t src, uint16_t seq)
{
    uint32_t key = ((uint32_t)src << 16) | seq;
    uint32_t now = (uint32_t)millis();
    for (uint8_t i = 0; i < 32; i++) {
        if (seenCache[i].key == key) {
            if (now - seenCache[i].seenAt < 60000) // 60s 窗口内视为重复
                return true;
            seenCache[i].seenAt = now; // 刷新
            return false;
        }
    }
    seenCache[seenCacheIdx] = {key, now};
    seenCacheIdx = (seenCacheIdx + 1) % 32;
    return false;
}

/* =========================================================================
 * 混合路由状态机 (route.txt §5)
 * ========================================================================= */

// PHASE1 洪泛探测：骨干中继记录路径、减跳、重广播；目标回 ACK(REVERSE_PATH)
void MeshRocModule::onRouteProbe(const MeshRocPacket &pkt, uint8_t payloadLen)
{
    if (localRole != MESHROC_ROLE_BACKBONE) {
        // 仅骨干中继参与洪泛探测
        onMeshRocFrame(pkt, payloadLen, meshtastic_MeshPacket_init_default());
        return;
    }
    if (pkt.max_hop == 0) {
        LOG_DEBUG("MeshRoc: probe max_hop==0 dropped");
        return;
    }
    // 暂存路径：把本节点短地址追加进探测包的临时路径缓冲（此处以 REVERSE_PATH 形式缓存）
    uint16_t selfAddr = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    MeshRocRouteEntry *e = routeLookup(pkt.src_addr); // 探测源一路累积路径
    // 简化：直接重广播（减跳）；完整路径回溯在 ACK 阶段由目标节点用 REVERSE_PATH 回报
    MeshRocPacket fwd = pkt;
    fwd.max_hop = pkt.max_hop - 1;
    // 若本节点是探测目标（dst 为自身或广播），由目标逻辑处理；此处仅中继
    LOG_INFO("MeshRoc: flood-probe relay src=0x%04x maxhop=%d self=0x%04x", pkt.src_addr, fwd.max_hop, selfAddr);
    uint8_t blob[MESHROC_MAX_PACKET];
    size_t n = MeshRocCodec::serialize(fwd, payloadLen, blob, sizeof(blob));
    if (n) {
        meshtastic_MeshPacket *m = allocDataPacket();
        if (m) {
            uint8_t baseHop = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
            m->to = UINT32_MAX; // 受限洪泛到全网（Meshtastic 广播地址）
            m->channel = channels.getPrimaryIndex();       // 兼容性修复1：走主信道确保可中继
            m->hop_limit = (fwd.max_hop > baseHop) ? fwd.max_hop : baseHop; // 修复3：跳数协同
            m->hop_start = m->hop_limit;
            m->decoded.payload.size = (pb_size_t)n;
            memcpy(m->decoded.payload.bytes, blob, n);
            service->sendToMesh(m);
        }
    }
}

// PHASE2 业务单播：严格按 0x10 ROUTE_PATH 跳点转发
void MeshRocModule::onSourceRoute(const MeshRocPacket &pkt, uint8_t payloadLen)
{
    // 在 payload 中定位 0x10 ROUTE_PATH
    const uint8_t *ptr = pkt.payload;
    const uint8_t *end = pkt.payload + payloadLen;
    while (ptr + 2 <= end) {
        uint8_t tag = *ptr++;
        uint8_t len = *ptr++;
        if (ptr + len > end)
            break;
        if (tag == MESHROC_TLV_ROUTE_PATH) {
            uint16_t hops[MESHROC_MAX_PACKET / 2];
            uint8_t n = parsePathTlv(ptr, len, hops, MESHROC_MAX_PACKET / 2);
            uint16_t selfAddr = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
            // 找到本节点在跳点序列中的索引
            int idx = -1;
            for (uint8_t i = 0; i < n; i++)
                if (hops[i] == selfAddr)
                    idx = i;
            if (idx >= 0 && idx + 1 < n) {
                // 匹配：递增索引, max_hop-1, 转发给下一跳
                MeshRocPacket fwd = pkt;
                fwd.max_hop = pkt.max_hop - 1;
                uint8_t blob[MESHROC_MAX_PACKET];
                size_t m = MeshRocCodec::serialize(fwd, payloadLen, blob, sizeof(blob));
                if (m) {
                    meshtastic_MeshPacket *mp = allocDataPacket();
                    if (mp) {
                        uint8_t baseHop = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
                        mp->to = hops[idx + 1]; // 定向单播到下一跳
                        mp->channel = channels.getPrimaryIndex();                 // 修复1：走主信道
                        mp->hop_limit = (fwd.max_hop > baseHop) ? fwd.max_hop : baseHop; // 修复3：跳数协同
                        mp->hop_start = mp->hop_limit;
                        mp->decoded.payload.size = (pb_size_t)m;
                        memcpy(mp->decoded.payload.bytes, blob, m);
                        service->sendToMesh(mp);
                        LOG_INFO("MeshRoc: source-route forward 0x%04x -> 0x%04x", selfAddr, hops[idx + 1]);
                    }
                }
            } else {
                // 本节点不在跳点序列：丢弃，不洪泛扩散（route.txt §6.3）
                LOG_DEBUG("MeshRoc: source-route not for us, DROP");
            }
            return;
        }
        ptr += len;
    }
}

// PHASE3 失败回退：ACK 超时 → 标记失效 → 重新洪泛探测（由应用层定时触发）
void MeshRocModule::onAckTimeout(uint16_t dst)
{
    LOG_INFO("MeshRoc: ACK timeout dst=0x%04x -> mark invalid, fallback flood-probe", dst);
    routeInvalidate(dst);
    // 应用层据此重新发起 PHASE1 探测（此处仅标记失效）
}

// PHASE4 广播/心跳/遥测：受限洪泛, bit6=1 仅骨干中继
void MeshRocModule::onFloodFrame(const MeshRocPacket &pkt, uint8_t payloadLen)
{
    onMeshRocFrame(pkt, payloadLen, meshtastic_MeshPacket_init_default());
    // 骨干中继负责重广播（减跳），CLIENT 只接收
    if (localRole == MESHROC_ROLE_BACKBONE && pkt.max_hop > 0) {
        MeshRocPacket fwd = pkt;
        fwd.max_hop = pkt.max_hop - 1;
        uint8_t blob[MESHROC_MAX_PACKET];
        size_t n = MeshRocCodec::serialize(fwd, payloadLen, blob, sizeof(blob));
        if (n) {
            meshtastic_MeshPacket *m = allocDataPacket();
            if (m) {
                uint8_t baseHop = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
                m->to = UINT32_MAX;
                m->channel = channels.getPrimaryIndex();                 // 修复1：走主信道
                m->hop_limit = (fwd.max_hop > baseHop) ? fwd.max_hop : baseHop; // 修复3：跳数协同
                m->hop_start = m->hop_limit;
                m->decoded.payload.size = (pb_size_t)n;
                memcpy(m->decoded.payload.bytes, blob, n);
                service->sendToMesh(m);
            }
        }
    }
}

/* =========================================================================
 * RAP —— Router Attach Protocol（路由器归属协议，datapack.txt R1~R12）
 * 双栈：归属表命中且对端可达 → 定向；否则 NextHopRouter 自动落回原版洪泛。
 * ========================================================================= */

// RAP 载荷内 TLV 查找（首个匹配 tag），未知 tag 必须跳过，前向兼容
static bool rapTlvFind(const uint8_t *buf, uint8_t len, uint8_t tag, const uint8_t **outVal, uint8_t *outLen)
{
    uint8_t off = 0;
    while (off + 2 <= len) {
        uint8_t t = buf[off];
        uint8_t l = buf[off + 1];
        if (off + 2 + l > len)
            break;
        if (t == tag) {
            *outVal = buf + off + 2;
            *outLen = l;
            return true;
        }
        off += 2 + l;
    }
    return false;
}

// 构造一个 RAP 帧（ctrl_flag=MESHROC_TYPE_RAP，relayPerm=0，max_hop=0 一跳）
static MeshRocPacket rapMakeFrame(uint16_t dstShort, uint16_t srcShort, const uint8_t *payload, uint8_t payloadLen,
                                  bool wantAck)
{
    MeshRocPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.ctrl_flag = MeshRocCodec::makeCtrlFlag(MESHROC_TYPE_RAP, MESHROC_PRIO_NORMAL, wantAck, false, false);
    pkt.src_addr = srcShort;
    pkt.dst_addr = dstShort; // 0xFFFF = 广播
    pkt.seq = (uint16_t)(millis() & 0xFFFF);
    pkt.max_hop = 0; // RAP 仅一跳，禁止中继
    pkt.snr = 0;
    pkt.route_mode = 0;
    if (payload && payloadLen > 0)
        memcpy(pkt.payload, payload, payloadLen);
    return pkt;
}

uint8_t MeshRocModule::localRoleClass() const
{
    switch (config.device.role) {
    case meshtastic_Config_DeviceConfig_Role_ROUTER:
    case meshtastic_Config_DeviceConfig_Role_ROUTER_LATE:
        return RAP_ROLE_BACKBONE;
    case meshtastic_Config_DeviceConfig_Role_SENSOR:
        return RAP_ROLE_SENSOR;
    case meshtastic_Config_DeviceConfig_Role_TRACKER:
        return RAP_ROLE_TRACKER;
    default:
        // DTU 用固定供电串口网关，硬件上归类为 CLIENT 但业务密集，按 CLIENT 处理续租
        return RAP_ROLE_CLIENT;
    }
}

uint32_t MeshRocModule::rapTtlForRole(uint8_t role)
{
    switch (role) {
    case RAP_ROLE_SENSOR: return RAP_TTL_SENSOR_MS;
    case RAP_ROLE_TRACKER: return RAP_TTL_TRACKER_MS;
    case RAP_ROLE_DTU: return RAP_TTL_DTU_MS;
    default: return RAP_TTL_CLIENT_MS;
    }
}

int8_t MeshRocModule::computeLinkCost(int8_t a, int8_t b) { return min(a, b); }

void MeshRocModule::bumpTableVersion()
{
    rapTableVersion++;
    if (rapTableVersion == 0)
        rapTableVersion = 1;
}

RapNeighborEntry *MeshRocModule::findNeighbor(uint32_t bb)
{
    for (uint8_t i = 0; i < nNeighbors; i++)
        if (rapNeighbors[i].backbone == bb)
            return &rapNeighbors[i];
    return nullptr;
}

RapOwnerEntry *MeshRocModule::findOwner(uint32_t node)
{
    for (uint8_t i = 0; i < nOwners; i++)
        if (rapOwnerTable[i].node == node)
            return &rapOwnerTable[i];
    return nullptr;
}

RapOwnerEntry *MeshRocModule::findRemoteOwner(uint32_t node)
{
    for (uint8_t i = 0; i < nRemoteOwners; i++)
        if (rapRemoteOwners[i].node == node)
            return &rapRemoteOwners[i];
    return nullptr;
}

void MeshRocModule::pruneExpiredOwners()
{
    uint32_t now = millis();
    for (uint8_t i = 0; i < nOwners;) {
        if (rapOwnerTable[i].ttlExpireAt != 0 && now >= rapOwnerTable[i].ttlExpireAt) {
            LOG_INFO("MeshRoc RAP: owner 0x%08x TTL expired", rapOwnerTable[i].node);
            for (uint8_t j = i; j + 1 < nOwners; j++)
                rapOwnerTable[j] = rapOwnerTable[j + 1];
            nOwners--;
            bumpTableVersion();
        } else
            i++;
    }
    for (uint8_t i = 0; i < nRemoteOwners;) {
        if (rapRemoteOwners[i].ttlExpireAt != 0 && now >= rapRemoteOwners[i].ttlExpireAt) {
            for (uint8_t j = i; j + 1 < nRemoteOwners; j++)
                rapRemoteOwners[j] = rapRemoteOwners[j + 1];
            nRemoteOwners--;
        } else
            i++;
    }
}

void MeshRocModule::pruneExpiredNeighbors()
{
    uint32_t now = millis();
    for (uint8_t i = 0; i < nNeighbors;) {
        if (now - rapNeighbors[i].lastHelloAt >= RAP_NEIGHBOR_TTL_MS) {
            for (uint8_t j = i; j + 1 < nNeighbors; j++)
                rapNeighbors[j] = rapNeighbors[j + 1];
            nNeighbors--;
        } else
            i++;
    }
}

/* ----------------------------- RAP 帧发送 ----------------------------- */

void MeshRocModule::sendRapHello(bool fullAdv)
{
    (void)fullAdv; // 增量 HELLO 不携带 owner 摘要（摘要由 OWNERSHIP_ADV 单独通告）
    uint16_t self = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    uint8_t payload[16];
    uint8_t p = 0;
    payload[p++] = RAP_KIND; payload[p++] = 1; payload[p++] = RAP_HELLO;
    payload[p++] = RAP_ROLE_CLASS; payload[p++] = 1; payload[p++] = localRoleClass();
    payload[p++] = RAP_TABLE_VERSION; payload[p++] = 2;
    payload[p++] = (rapTableVersion >> 8) & 0xFF; payload[p++] = rapTableVersion & 0xFF;
    payload[p++] = RAP_LINK_COST; payload[p++] = 1; payload[p++] = 0;
    MeshRocPacket frm = rapMakeFrame(MESHROC_ADDR_INVALID, self, payload, p, false);
    sendFrame(frm, p, NODENUM_BROADCAST);
}

void MeshRocModule::sendRapHelloAck(uint32_t dst, int8_t snrToIt)
{
    uint16_t self = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    uint16_t dstShort = (uint16_t)(dst & 0xFFFF);
    uint8_t payload[16];
    uint8_t p = 0;
    payload[p++] = RAP_KIND; payload[p++] = 1; payload[p++] = RAP_HELLO_ACK;
    payload[p++] = RAP_NEIGHBOR_SNR; payload[p++] = 3;
    payload[p++] = (dst >> 8) & 0xFF; payload[p++] = dst & 0xFF; payload[p++] = (uint8_t)snrToIt;
    MeshRocPacket frm = rapMakeFrame(dstShort, self, payload, p, false);
    sendFrame(frm, p, dst);
}

void MeshRocModule::sendRapAttachReq(uint32_t backbone)
{
    uint16_t self = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    uint16_t dstShort = (uint16_t)(backbone & 0xFFFF);
    uint16_t seq = rapNextAttachSeq++;
    uint8_t payload[16];
    uint8_t p = 0;
    payload[p++] = RAP_KIND; payload[p++] = 1; payload[p++] = RAP_ATTACH_REQ;
    payload[p++] = RAP_ROLE_CLASS; payload[p++] = 1; payload[p++] = localRoleClass();
    payload[p++] = RAP_ATTACH_SEQ; payload[p++] = 2;
    payload[p++] = (seq >> 8) & 0xFF; payload[p++] = seq & 0xFF;
    rapLocal.attachSeq = seq;
    rapLocal.lastAttachReqAt = millis();
    MeshRocPacket frm = rapMakeFrame(dstShort, self, payload, p, true);
    sendFrame(frm, p, backbone);
}

void MeshRocModule::sendRapAttachAck(uint32_t dst, uint16_t ttlGrant, uint16_t attachSeq)
{
    uint16_t self = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    uint16_t dstShort = (uint16_t)(dst & 0xFFFF);
    uint8_t payload[16];
    uint8_t p = 0;
    payload[p++] = RAP_KIND; payload[p++] = 1; payload[p++] = RAP_ATTACH_ACK;
    payload[p++] = RAP_TTL_GRANT; payload[p++] = 2;
    payload[p++] = (ttlGrant >> 8) & 0xFF; payload[p++] = ttlGrant & 0xFF;
    payload[p++] = RAP_ATTACH_SEQ; payload[p++] = 2;
    payload[p++] = (attachSeq >> 8) & 0xFF; payload[p++] = attachSeq & 0xFF;
    MeshRocPacket frm = rapMakeFrame(dstShort, self, payload, p, false);
    sendFrame(frm, p, dst);
}

void MeshRocModule::sendRapKeepalive(bool standalone)
{
    if (rapLocal.attachedTo == 0)
        return;
    uint16_t self = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    uint16_t dstShort = (uint16_t)(rapLocal.attachedTo & 0xFFFF);
    uint8_t payload[16];
    uint8_t p = 0;
    payload[p++] = RAP_KIND; payload[p++] = 1; payload[p++] = RAP_KEEPALIVE;
    payload[p++] = RAP_ATTACH_SEQ; payload[p++] = 2;
    payload[p++] = (rapLocal.attachSeq >> 8) & 0xFF; payload[p++] = rapLocal.attachSeq & 0xFF;
    if (!standalone) {
        // 捎带模式：调用方负责把 payload 追加进业务帧（本函数仅发独立帧）
        return;
    }
    MeshRocPacket frm = rapMakeFrame(dstShort, self, payload, p, false);
    sendFrame(frm, p, rapLocal.attachedTo);
}

void MeshRocModule::sendRapDetach(uint32_t oldOwner)
{
    uint16_t self = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    uint16_t dstShort = (uint16_t)(oldOwner & 0xFFFF);
    uint8_t payload[4];
    uint8_t p = 0;
    payload[p++] = RAP_KIND; payload[p++] = 1; payload[p++] = RAP_DETACH;
    MeshRocPacket frm = rapMakeFrame(dstShort, self, payload, p, false);
    sendFrame(frm, p, oldOwner);
}

void MeshRocModule::sendRapOwnershipAdv(const RapOwnerEntry *entries, uint8_t count, bool isDelete)
{
    uint16_t self = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    uint8_t payload[MESHROC_MAX_PAYLOAD];
    uint8_t p = 0;
    payload[p++] = RAP_KIND; payload[p++] = 1; payload[p++] = RAP_OWNERSHIP_ADV;
    payload[p++] = RAP_TABLE_VERSION; payload[p++] = 2;
    payload[p++] = (rapTableVersion >> 8) & 0xFF; payload[p++] = rapTableVersion & 0xFF;
    uint8_t tlvTag = isDelete ? RAP_OWNER_DEL : RAP_OWNER_LIST;
    uint8_t entrySize = isDelete ? 2 : 4;
    payload[p++] = tlvTag; payload[p++] = (uint8_t)(count * entrySize);
    for (uint8_t i = 0; i < count; i++) {
        const RapOwnerEntry *e = &entries[i];
        if (isDelete) {
            payload[p++] = (e->node >> 8) & 0xFF; payload[p++] = e->node & 0xFF;
        } else {
            payload[p++] = (e->node >> 8) & 0xFF; payload[p++] = e->node & 0xFF;
            payload[p++] = (e->attachSeq >> 8) & 0xFF; payload[p++] = e->attachSeq & 0xFF;
        }
    }
    MeshRocPacket frm = rapMakeFrame(MESHROC_ADDR_INVALID, self, payload, p, false);
    sendFrame(frm, p, NODENUM_BROADCAST);
}

void MeshRocModule::sendRapSyncReq(uint32_t dst)
{
    uint16_t self = (uint16_t)(nodeDB->getNodeNum() & 0xFFFF);
    uint16_t dstShort = (uint16_t)(dst & 0xFFFF);
    uint8_t payload[4];
    uint8_t p = 0;
    payload[p++] = RAP_KIND; payload[p++] = 1; payload[p++] = RAP_SYNC_REQ;
    MeshRocPacket frm = rapMakeFrame(dstShort, self, payload, p, false);
    sendFrame(frm, p, dst);
}

/* ----------------------------- RAP 帧接收 ----------------------------- */

void MeshRocModule::onRapPacket(const MeshRocPacket &pkt, uint8_t payloadLen, const meshtastic_MeshPacket &mp)
{
    const uint8_t *val = nullptr;
    uint8_t vlen = 0;
    if (!rapTlvFind(pkt.payload, payloadLen, RAP_KIND, &val, &vlen) || vlen < 1) {
        LOG_WARN("MeshRoc RAP: missing RAP_KIND, dropped");
        return;
    }
    uint8_t sub = val[0];
    uint32_t from = mp.from;
    switch (sub) {
    case RAP_HELLO:        onRapHello(pkt, payloadLen, from); break;
    case RAP_HELLO_ACK:    onRapHelloAck(pkt, payloadLen, from); break;
    case RAP_ATTACH_REQ:   onRapAttachReq(pkt, payloadLen, from); break;
    case RAP_ATTACH_ACK:   onRapAttachAck(pkt, payloadLen, from); break;
    case RAP_KEEPALIVE:    onRapKeepalive(pkt, payloadLen, from); break;
    case RAP_DETACH:       onRapDetach(pkt, payloadLen, from); break;
    case RAP_OWNERSHIP_ADV:onRapOwnershipAdv(pkt, payloadLen, from); break;
    case RAP_SYNC_REQ:     onRapSyncReq(pkt, payloadLen, from); break;
    default: LOG_WARN("MeshRoc RAP: unknown kind %u", sub); break;
    }
}

void MeshRocModule::onRapHello(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from)
{
    const uint8_t *val, *rval, *vval;
    uint8_t vl, rl, vvl;
    uint8_t rclass = RAP_ROLE_CLIENT;
    if (rapTlvFind(pkt.payload, payloadLen, RAP_ROLE_CLASS, &rval, &rl) && rl >= 1)
        rclass = rval[0];
    int8_t snrToIt = getPeerSnr(from); // 我收它的 SNR（handleReceived 已 recordLinkSnr(mp.from)）

    if (isBackbone() && rclass == RAP_ROLE_BACKBONE) {
        // 骨干互发现：回 HELLO_ACK（定向），准入以回 ACK 为准（排除原版 ROUTER）
        sendRapHelloAck(from, snrToIt);
        RapNeighborEntry *nb = findNeighbor(from);
        if (!nb && nNeighbors < RAP_MAX_NEIGHBORS) {
            nb = &rapNeighbors[nNeighbors++];
            memset(nb, 0, sizeof(*nb));
            nb->backbone = from;
        }
        if (nb) {
            nb->snrMyToIt = snrToIt;
            nb->lastHelloAt = millis();
            if (rapTlvFind(pkt.payload, payloadLen, RAP_TABLE_VERSION, &vval, &vvl) && vvl >= 2)
                nb->tableVersion = ((uint16_t)vval[0] << 8) | vval[1];
        }
    } else if (!isBackbone()) {
        // 终端侧：监听 HELLO 发现归属候选，迟滞切换（铁律三）
        if (rclass == RAP_ROLE_BACKBONE) {
            int8_t candSnr = snrToIt;
            if (rapLocal.state == RAP_STATE_SCANNING || rapLocal.attachedTo == 0) {
                // 首次听到 → 直接 ATTACH
                rapLocal.attachedTo = from;
                rapLocal.state = RAP_STATE_ATTACHED;
                rapLocal.lastKeepaliveAt = millis();
                rapLocal.minDwellUntil = millis() + RAP_MIN_DWELL_MS;
                rapLocal.curSnr = candSnr;
                sendRapAttachReq(from);
            } else {
                if (candSnr >= rapLocal.curSnr + RAP_HYST_SNR_DB) {
                    rapLocal.hystSamples++;
                    if (rapLocal.hystSamples >= RAP_HYST_SAMPLES && millis() >= rapLocal.minDwellUntil) {
                        uint32_t old = rapLocal.attachedTo;
                        rapLocal.attachedTo = from;
                        rapLocal.curSnr = candSnr;
                        rapLocal.hystSamples = 0;
                        rapLocal.minDwellUntil = millis() + RAP_MIN_DWELL_MS;
                        sendRapAttachReq(from);
                        sendRapDetach(old);
                    }
                } else {
                    rapLocal.hystSamples = 0;
                }
            }
            rapLocal.curSnr = max(rapLocal.curSnr, candSnr);
        }
    }
    (void)val;
}

void MeshRocModule::onRapHelloAck(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from)
{
    if (!isBackbone())
        return; // 仅 BACKBONE 处理
    RapNeighborEntry *nb = findNeighbor(from);
    if (!nb) {
        if (nNeighbors < RAP_MAX_NEIGHBORS) {
            nb = &rapNeighbors[nNeighbors++];
            memset(nb, 0, sizeof(*nb));
            nb->backbone = from;
        } else
            return;
    }
    const uint8_t *val;
    uint8_t vl;
    if (rapTlvFind(pkt.payload, payloadLen, RAP_NEIGHBOR_SNR, &val, &vl) && vl >= 3) {
        nb->snrItToMe = (int8_t)val[2]; // 它回报收我的 SNR
    }
    nb->snrMyToIt = getPeerSnr(from);
    nb->linkCost = computeLinkCost(nb->snrMyToIt, nb->snrItToMe);
    nb->lastAckAt = millis();
    nb->acked = true; // 此刻才正式准入（排除原版 ROUTER：它不会回 ACK）
    helloBackoffPow = 0;
}

void MeshRocModule::onRapAttachReq(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from)
{
    if (!isBackbone())
        return;
    const uint8_t *sval, *rval;
    uint8_t sl, rl;
    uint16_t aseq = 0;
    uint8_t rclass = RAP_ROLE_CLIENT;
    if (rapTlvFind(pkt.payload, payloadLen, RAP_ATTACH_SEQ, &sval, &sl) && sl >= 2)
        aseq = ((uint16_t)sval[0] << 8) | sval[1];
    if (rapTlvFind(pkt.payload, payloadLen, RAP_ROLE_CLASS, &rval, &rl) && rl >= 1)
        rclass = rval[0];

    if (nOwners >= RAP_MAX_OWNED) {
        sendRapAttachAck(from, 0, aseq); // 表满拒绝（TTL_GRANT=0），终端自动退回洪泛
        LOG_INFO("MeshRoc RAP: ATTACH rejected (table full) from 0x%08x", from);
        return;
    }
    RapOwnerEntry *e = findOwner(from);
    if (!e) {
        e = &rapOwnerTable[nOwners++];
        memset(e, 0, sizeof(*e));
        e->node = from;
    }
    e->ownerBackbone = nodeDB->getNodeNum();
    e->attachSeq = max(e->attachSeq, aseq);
    e->roleClass = rclass;
    e->ttlExpireAt = millis() + rapTtlForRole(rclass);
    e->failCount = 0;

    bumpTableVersion();
    sendRapAttachAck(from, (uint16_t)(rapTtlForRole(rclass) / 1000), e->attachSeq);
    sendRapOwnershipAdv(e, 1, false); // 增量通告单条
    LOG_INFO("MeshRoc RAP: ATTACH accepted 0x%08x (owned=%u)", from, nOwners);
}

void MeshRocModule::onRapAttachAck(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from)
{
    if (isBackbone())
        return; // 仅终端
    const uint8_t *tval, *sval;
    uint8_t tl, sl;
    uint16_t grantedTtl = 0, aseq = 0;
    if (rapTlvFind(pkt.payload, payloadLen, RAP_TTL_GRANT, &tval, &tl) && tl >= 2)
        grantedTtl = ((uint16_t)tval[0] << 8) | tval[1];
    if (rapTlvFind(pkt.payload, payloadLen, RAP_ATTACH_SEQ, &sval, &sl) && sl >= 2)
        aseq = ((uint16_t)sval[0] << 8) | sval[1];

    if (grantedTtl == 0) {
        LOG_INFO("MeshRoc RAP: ATTACH rejected by 0x%08x, fallback flooding", from);
        rapLocal.state = RAP_STATE_SCANNING;
        rapLocal.attachedTo = 0;
        return;
    }
    rapLocal.attachedTo = from;
    rapLocal.attachSeq = aseq;
    rapLocal.state = RAP_STATE_ATTACHED;
    rapLocal.lastKeepaliveAt = millis();
    rapLocal.minDwellUntil = millis() + RAP_MIN_DWELL_MS;
    rapLocal.curSnr = getPeerSnr(from);
    rapLocal.hystSamples = 0;
}

void MeshRocModule::onRapKeepalive(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from)
{
    if (!isBackbone())
        return;
    RapOwnerEntry *e = findOwner(from);
    if (e) {
        e->ttlExpireAt = millis() + rapTtlForRole(e->roleClass);
        // 续租不算变更，表版本不变（避免每次续租触发全网通告）
    }
    (void)pkt;
    (void)payloadLen;
}

void MeshRocModule::onRapDetach(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from)
{
    if (!isBackbone())
        return;
    RapOwnerEntry *e = findOwner(from);
    if (e) {
        uint8_t idx = (uint8_t)(e - rapOwnerTable);
        for (uint8_t i = idx; i + 1 < nOwners; i++)
            rapOwnerTable[i] = rapOwnerTable[i + 1];
        nOwners--;
        bumpTableVersion();
        sendRapOwnershipAdv(e, 1, true);
        LOG_INFO("MeshRoc RAP: DETACH 0x%08x (owned=%u)", from, nOwners);
    }
    (void)pkt;
    (void)payloadLen;
}

void MeshRocModule::onRapOwnershipAdv(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from)
{
    if (!isBackbone())
        return;
    const uint8_t *vval, *lval, *dval;
    uint8_t vvl, ll, dl;
    uint16_t advVer = 0;
    if (rapTlvFind(pkt.payload, payloadLen, RAP_TABLE_VERSION, &vval, &vvl) && vvl >= 2)
        advVer = ((uint16_t)vval[0] << 8) | vval[1];

    // 版本号跳变 >1 → 请求全量
    RapNeighborEntry *nb = findNeighbor(from);
    uint16_t lastVer = nb ? nb->tableVersion : 0;
    if (nb)
        nb->tableVersion = advVer;
    if (advVer - lastVer > 1) {
        sendRapSyncReq(from);
        return;
    }

    if (rapTlvFind(pkt.payload, payloadLen, RAP_OWNER_LIST, &lval, &ll)) {
        uint8_t cnt = ll / 4;
        for (uint8_t i = 0; i < cnt; i++) {
            const uint8_t *b = lval + i * 4;
            uint32_t node = ((uint32_t)b[0] << 8) | b[1];
            uint16_t aseq = ((uint16_t)b[2] << 8) | b[3];
            RapOwnerEntry *e = findRemoteOwner(node);
            if (e && e->attachSeq >= aseq)
                continue; // 已有更新归属，跳过（冲突裁决：attachSeq 大者胜）
            if (!e && nRemoteOwners < RAP_MAX_REMOTE_OWNERS) {
                e = &rapRemoteOwners[nRemoteOwners++];
                memset(e, 0, sizeof(*e));
                e->node = node;
            }
            if (e) {
                e->ownerBackbone = from;
                e->attachSeq = aseq;
                e->ttlExpireAt = millis() + RAP_TTL_CLIENT_MS;
            }
        }
    }
    if (rapTlvFind(pkt.payload, payloadLen, RAP_OWNER_DEL, &dval, &dl)) {
        uint8_t cnt = dl / 2;
        for (uint8_t i = 0; i < cnt; i++) {
            const uint8_t *b = dval + i * 2;
            uint32_t node = ((uint32_t)b[0] << 8) | b[1];
            RapOwnerEntry *e = findRemoteOwner(node);
            if (e) {
                uint8_t idx = (uint8_t)(e - rapRemoteOwners);
                for (uint8_t j = idx; j + 1 < nRemoteOwners; j++)
                    rapRemoteOwners[j] = rapRemoteOwners[j + 1];
                nRemoteOwners--;
            }
        }
    }
}

void MeshRocModule::onRapSyncReq(const MeshRocPacket &pkt, uint8_t payloadLen, uint32_t from)
{
    if (!isBackbone())
        return;
    LOG_INFO("MeshRoc RAP: SYNC_REQ from 0x%08x, full adv (%u entries)", from, nOwners);
    for (uint8_t i = 0; i < nOwners; i += RAP_ADV_MAX_ENTRIES_PER_FRAME) {
        uint8_t cnt = min((uint8_t)(nOwners - i), (uint8_t)RAP_ADV_MAX_ENTRIES_PER_FRAME);
        sendRapOwnershipAdv(&rapOwnerTable[i], cnt, false); // 分片全量
    }
    (void)pkt;
    (void)payloadLen;
}

/* ----------------------------- RAP 周期任务（OSThread runOnce） ----------------------------- */

int32_t MeshRocModule::runOnce()
{
    uint32_t now = millis();

    if (isBackbone()) {
        // 过期回收
        pruneExpiredOwners();
        pruneExpiredNeighbors();

        // 指数退避：连续未收到 HELLO_ACK 时放大周期；收到 ACK 复位（见 onRapHelloAck）
        uint32_t period = RAP_HELLO_PERIOD_MS;
        if (helloBackoffPow > 0)
            period = min((uint32_t)RAP_HELLO_PERIOD_MS << helloBackoffPow, RAP_MAX_HELLO_BACKOFF_MS);

        bool due = (now - lastHelloSentAt) >= period;
        bool lbtOk = (airTime->channelUtilizationPercent() < RAP_LBT_UTIL_THRESHOLD_PCT); // 发射前信道侦听
        if (due && lbtOk) {
            sendRapHello(false);
            lastHelloSentAt = now;
            helloBackoffPow = 0;
        } else if (due && !lbtOk) {
            LOG_INFO("MeshRoc RAP: HELLO deferred by LBT (util=%u%%)",
                     (unsigned)airTime->channelUtilizationPercent());
        }

        // 低频全量通告（软状态收敛），分片每片 ≤16 条
        if ((now - lastFullAdvAt) >= RAP_FULL_ADV_PERIOD_MS) {
            lastFullAdvAt = now;
            if (nOwners > 0) {
                for (uint8_t i = 0; i < nOwners; i += RAP_ADV_MAX_ENTRIES_PER_FRAME) {
                    uint8_t cnt = min((uint8_t)(nOwners - i), (uint8_t)RAP_ADV_MAX_ENTRIES_PER_FRAME);
                    sendRapOwnershipAdv(&rapOwnerTable[i], cnt, false);
                }
            }
        }
        return RAP_HELLO_PERIOD_MS / 4; // 每 1/4 周期扫一次过期/调度
    } else {
        // 终端：续租 + 评估切换
        if (rapLocal.state == RAP_STATE_ATTACHED && rapLocal.attachedTo != 0) {
            uint32_t kaPeriod = (localRoleClass() == RAP_ROLE_SENSOR) ? RAP_KEEPALIVE_SENSOR_MS
                                                                      : RAP_KEEPALIVE_CLIENT_MS;
            if ((now - rapLocal.lastKeepaliveAt) >= kaPeriod) {
                sendRapKeepalive(true); // 独立 Keepalive 帧（SENSOR 由遥测捎带在别处）
                rapLocal.lastKeepaliveAt = now;
            }
        }
        // 评估（迟滞采样）由 onRapHello 持续累积，这里仅维持调度节奏
        return RAP_EVAL_INTERVAL_MS;
    }
}

/* ----------------------------- RAP 双栈公开接口（NextHopRouter 调用） ----------------------------- */

std::optional<uint8_t> MeshRocModule::getRapNextHop(uint32_t to)
{
    if (!isBackbone())
        return std::nullopt; // 终端不做定向转发
    if (isBroadcast(to) || to == 0)
        return std::nullopt;

    // 归属查表：先本机下属，再邻居通告的远程归属
    uint32_t ownerBb = 0;
    RapOwnerEntry *e = findOwner(to);
    if (e && e->ttlExpireAt != 0 && (uint32_t)millis() < e->ttlExpireAt) {
        ownerBb = e->ownerBackbone;
    } else {
        RapOwnerEntry *re = findRemoteOwner(to);
        if (re && re->ttlExpireAt != 0 && (uint32_t)millis() < re->ttlExpireAt)
            ownerBb = re->ownerBackbone;
    }
    if (ownerBb == 0)
        return std::nullopt; // 未命中 → 落回洪泛

    // 归属 BACKBONE 必须可达（在 rapNeighbors 内、已 ack、未过期 Hello）
    RapNeighborEntry *nb = findNeighbor(ownerBb);
    if (!nb || !nb->acked)
        return std::nullopt;
    if ((uint32_t)millis() - nb->lastHelloAt >= RAP_NEIGHBOR_TTL_MS)
        return std::nullopt; // 邻居过期 → 落回洪泛

    // 降级为末字节并做唯一性解析，规避 NodeInfoLite.next_hop 的 uint8_t 末字节撞车
    uint8_t lastByte = (uint8_t)(ownerBb & 0xFF);
    uint32_t resolved = 0;
    if (nodeDB->resolveUniqueLastByte(lastByte, true, &resolved) && resolved == ownerBb)
        return lastByte; // 唯一匹配且确为归属 BACKBONE
    return std::nullopt; // 末字节撞车/不唯一 → 不冒险定向，落回洪泛
}

void MeshRocModule::noteRapRouteFailure(uint32_t toNode)
{
    if (!isBackbone())
        return;
    RapOwnerEntry *e = findOwner(toNode);
    if (e) {
        e->failCount++;
        if (e->failCount >= RAP_ROUTE_FAIL_THRESHOLD) {
            uint8_t idx = (uint8_t)(e - rapOwnerTable);
            for (uint8_t j = idx; j + 1 < nOwners; j++)
                rapOwnerTable[j] = rapOwnerTable[j + 1];
            nOwners--;
            bumpTableVersion();
            LOG_INFO("MeshRoc RAP: owner 0x%08x route fail, dropped (flood fallback)", toNode);
        }
        return;
    }
    RapOwnerEntry *re = findRemoteOwner(toNode);
    if (re) {
        re->failCount++;
        if (re->failCount >= RAP_ROUTE_FAIL_THRESHOLD) {
            uint8_t idx = (uint8_t)(re - rapRemoteOwners);
            for (uint8_t j = idx; j + 1 < nRemoteOwners; j++)
                rapRemoteOwners[j] = rapRemoteOwners[j + 1];
            nRemoteOwners--;
            LOG_INFO("MeshRoc RAP: remote owner 0x%08x route fail, dropped", toNode);
        }
    }
}
