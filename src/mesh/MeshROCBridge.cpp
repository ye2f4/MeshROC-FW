/**
 * RadioMeshRocBridge 实现：自研 MeshRoc 协议栈 → 宿主 RadioLibInterface 射频。
 * 详见 MeshROCBridge.h 头部说明。
 */
#include "MeshROCBridge.h"

#include "kernel/MeshRocStack.h"  // MeshRocStack::MAX_FRAME
#include "configuration.h" // LOG_*

namespace meshroc {

RadioMeshRocBridge::RadioMeshRocBridge(const config::MeshROCConfig& cfg, uint16_t myAddr,
                                       bool isBackbone, RadioLibInterface* radio)
    : MeshRocStack(cfg, myAddr, isBackbone), radio_(radio)
{
    // 射频必须已经由宿主配置好（region / modem preset / power）。
    // 本桥不重新配置射频，只复用现有信道。
}

void RadioMeshRocBridge::registerWithRadio()
{
    if (radio_) {
        radio_->setRawFrameSink(&RadioMeshRocBridge::rawSinkAdapter, this);
    }
}

void RadioMeshRocBridge::sendRaw(const uint8_t* bytes, uint16_t len)
{
    if (!radio_) {
        LOG_ERROR("MeshRoc sendRaw: no radio bound");
        return;
    }
    // 帧长上限保护：避免溢出底层 radioBuffer。MeshRoc 单帧上限由 MAX_FRAME 约束。
    if (len == 0 || len > MeshRocStack::MAX_FRAME) {
        LOG_ERROR("MeshRoc sendRaw: bad len=%u (max %u)", len, MeshRocStack::MAX_FRAME);
        return;
    }
    // 经宿主射频直接发射裸字节（绕过 meshtastic_MeshPacket 管线）。
    radio_->startSendRaw(bytes, len);
}

uint8_t RadioMeshRocBridge::channelUtilization() const
{
    return radio_ ? radio_->currentChannelUtilization() : 0;
}

uint32_t RadioMeshRocBridge::slotTimeMsec() const
{
    return radio_ ? radio_->currentSlotTimeMsec() : MeshRocStack::slotTimeMsec();
}

void RadioMeshRocBridge::onPacket(const PacketReceived& pkt)
{
    // 默认实现：记录自研栈已成功解出应用层包。业务层（如 HTTP/MeshRocHandlers）
    // 可派生本类并重载此方法，把明文 payload 投递到上层。
    LOG_INFO("MeshRoc RX from 0x%04x len=%u prio=%u ack=%u", pkt.src, pkt.payloadLen,
             static_cast<unsigned>(pkt.priority), pkt.wantAck ? 1u : 0u);
}

void RadioMeshRocBridge::rawSinkAdapter(const uint8_t* buf, size_t len, int16_t rssi, int8_t snr,
                                        void* ctx)
{
    auto* self = static_cast<RadioMeshRocBridge*>(ctx);
    if (!self || !buf || len == 0 || len > 0xFFFF) return;
    // 原始空中字节交给自研栈解析（ingestRaw 内部会做 CRC16 校验与帧类型分发，
    // 非 MeshRoc 帧会被静默丢弃，不影响 meshtastic 路径）。rxSnr 喂链路质量表供 RAP 择优。
    self->ingestRaw(buf, static_cast<uint16_t>(len), millis(), snr);
    (void)rssi;
}

}  // namespace meshroc
