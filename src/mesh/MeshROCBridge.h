/**
 * RadioMeshRocBridge — 把自研 MeshRoc 协议栈（src/kernel）接到宿主射频（RadioLibInterface）。
 *
 * 这是 MESHROC 自研栈成为「第一公民」的关键接驳点：
 *   - 发送：MeshRocStack::sendRaw() 经 RadioLibInterface::startSendRaw() 直接发裸字节，
 *           绕过 meshtastic_MeshPacket 管线；不影响原版 meshtastic 流量。
 *   - 接收：RadioLibInterface 在收到空中帧时，先调本桥的 rawSink，把原始字节喂给
 *           MeshRocStack::ingestRaw()（10B 头 + TLV + CRC16），由自研栈决定是否为己方帧。
 *   - 射频参数：channelUtilization / slotTime 从真实 airtime 跟踪器读取，供 O4 ACK 超时基准。
 *
 * 设计铁律（见 docs/TRIPLE_STACK_COMPAT.md）：射频层只此一份（RadioLibInterface），
 * 自研栈不直连芯片，只通过本桥使用宿主已配置好的信道（region/modem preset/power）。
 */
#pragma once

#include "kernel/config/MeshROCConfig.h"  // config::MeshROCConfig
#include "kernel/MeshRocStack.h"   // meshroc::MeshRocStack
#include "RadioLibInterface.h"

namespace meshroc {

/**
 * 自研栈 → 宿主射频的桥。
 * 在 RadioLibInterface 已配置好信道（CN_470 / modem preset / power）的前提下运行，
 * 与同信道上的其它 MESHROC 节点原生互通。
 */
class RadioMeshRocBridge : public MeshRocStack {
public:
    RadioMeshRocBridge(const config::MeshROCConfig& cfg, uint16_t myAddr, bool isBackbone,
                       RadioLibInterface* radio);

    /** 把本桥注册为 RadioLibInterface 的原始帧下沉点，使其能接收 MeshRoc 帧。 */
    void registerWithRadio();

    // ---- MeshRocStack 射频回调重载 ----
    void sendRaw(const uint8_t* bytes, uint16_t len) override;
    uint8_t channelUtilization() const override;
    uint32_t slotTimeMsec() const override;

    /** 应用层收包（自研栈重组/解密完成后触发）。默认仅记录日志；业务层可派生重载。 */
    void onPacket(const PacketReceived& pkt) override;

    /**
     * RadioLibInterface::RawFrameSink 适配函数：把原始空中字节转交桥梁实例。
     * ctx 必须是 RadioMeshRocBridge*。
     */
    static void rawSinkAdapter(const uint8_t* buf, size_t len, int16_t rssi, int8_t snr, void* ctx);

private:
    RadioLibInterface* radio_;  // 非空；宿主射频，已配置好信道
};

}  // namespace meshroc
