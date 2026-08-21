// 自研 MeshRocPacket 协议栈单元测试（原创优化 O1/O2/O3/O4/O5/归属 逻辑中枢）
//
// 框架：Unity（与原版固件 test/ 一致）。
// 编译需 include 路径： -Isrc -Isrc/mesh/generated  （EnvProfile 依赖 config.pb.h）
// 跑法：pio test -e native -f test_meshroc_stack
//
// 覆盖：AirtimeModel 同构原版公式 / 分片闭环 / RAP 状态机 / MeshRocStack 端到端。

#include <unity.h>
#include <cstring>
#include <vector>

#include "config/MeshROCConfig.h"
#include "rf/AirtimeModel.h"
#include "net/Reassembler.h"
#include "net/rap/RapStateMachine.h"
#include "net/MeshRocCodec.h"
#include "MeshRocStack.h"

using namespace meshroc;
using namespace meshroc::net;
using namespace meshroc::rf;

// ---------------------------------------------------------------------------
// AirtimeModel：与原版 getRetransmissionMsec 同构性验证
// ---------------------------------------------------------------------------
void test_airtime_basic()
{
    LoraParams lp{};
    // LONG_FAST: SF11 / 250kHz / CR5 (wideLora=false 窄带)
    TEST_ASSERT_TRUE(AirtimeModel::presetToParams(
        static_cast<int>(ModemPreset::LONG_FAST), false, lp));
    TEST_ASSERT_EQUAL(11, lp.sf);
    TEST_ASSERT_EQUAL(250000, static_cast<int>(lp.bwHz));
    TEST_ASSERT_EQUAL(5, lp.crDenom);

    // airtime 应随包长单调增、且为正
    double a1 = AirtimeModel::loraOnAirMs(50, lp);
    double a2 = AirtimeModel::loraOnAirMs(200, lp);
    TEST_ASSERT_TRUE(a1 > 0.0);
    TEST_ASSERT_TRUE(a2 > a1);

    // 重传基准 = 2*airtime + CW 项 + PROCESSING_TIME，必为正且 > airtime
    uint32_t r = AirtimeModel::retransmissionMsec(50, lp, 0, 13);
    TEST_ASSERT_TRUE(r > static_cast<uint32_t>(a1));
    // 高信道利用率应给出更大超时（CW 窗口变大）
    uint32_t rHi = AirtimeModel::retransmissionMsec(50, lp, 90, 13);
    TEST_ASSERT_TRUE(rHi >= r);
}

void test_airtime_preset_table()
{
    // 验证关键预设的 SF/BW 与原版 modemPresetToParams 对齐
    LoraParams lp{};
    TEST_ASSERT_TRUE(AirtimeModel::presetToParams(static_cast<int>(ModemPreset::SHORT_TURBO), false, lp));
    TEST_ASSERT_EQUAL(7, lp.sf);
    TEST_ASSERT_EQUAL(500000, static_cast<int>(lp.bwHz));

    TEST_ASSERT_TRUE(AirtimeModel::presetToParams(static_cast<int>(ModemPreset::LONG_SLOW), false, lp));
    TEST_ASSERT_EQUAL(12, lp.sf);
    TEST_ASSERT_EQUAL(125000, static_cast<int>(lp.bwHz));
}

// ---------------------------------------------------------------------------
// Reassembler：发送端切片 -> 接收端重组 闭环
// ---------------------------------------------------------------------------
void test_reassembler_roundtrip()
{
    Reassembler r{};
    // 模拟发送端把 300 字节切成 3 片（每片 100），fragId=0x1234
    const uint16_t fragId = 0x1234;
    const uint8_t total = 3;
    uint8_t full[300];
    for (int i = 0; i < 300; ++i) full[i] = static_cast<uint8_t>(i);

    uint8_t out[Reassembler::MAX_PAYLOAD];
    uint16_t outLen = 0;
    bool completed = false;

    // 乱序喂入：seq 2, 0, 1
    FragHeader fh{};
    fh.fragId = fragId; fh.total = total;
    const uint8_t chunkSizes[3] = {100, 100, 100};

    fh.seq = 2;
    completed = r.feed(0x0A, fh, full + 200, 100, out, outLen, 1000);
    TEST_ASSERT_FALSE(completed);

    fh.seq = 0;
    completed = r.feed(0x0A, fh, full, 100, out, outLen, 1001);
    TEST_ASSERT_FALSE(completed);

    fh.seq = 1;
    completed = r.feed(0x0A, fh, full + 100, 100, out, outLen, 1002);
    TEST_ASSERT_TRUE(completed);            // 收齐
    TEST_ASSERT_EQUAL(300, outLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(full, out, 300);
}

void test_reassembler_missing_nack()
{
    Reassembler r{};
    FragHeader fh{};
    fh.fragId = 0x99; fh.total = 3;
    uint8_t chunk[10] = {0};
    uint8_t out[Reassembler::MAX_PAYLOAD];
    uint16_t outLen = 0;

    fh.seq = 0; r.feed(0x0B, fh, chunk, 10, out, outLen, 2000);
    // 缺 seq 1,2
    uint8_t missing[8];
    uint8_t n = r.buildNack(0x0B, 0x99, missing, 8);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(1, missing[0]);
    TEST_ASSERT_EQUAL(2, missing[1]);
}

// ---------------------------------------------------------------------------
// RapStateMachine：SCANNING -> ATTACHED 迁移
// ---------------------------------------------------------------------------
void test_rap_terminal_attach()
{
    RapStateMachine rap(0x55, /*isBackbone=*/false);
    TEST_ASSERT_EQUAL(static_cast<int>(RapTerminalState::SCANNING), static_cast<int>(rap.state()));

    uint8_t out[16];
    // tick 应产出 HELLO（首帧）
    uint16_t n = rap.tick(1000, out, sizeof(out));
    TEST_ASSERT_TRUE(n >= 5);
    TEST_ASSERT_EQUAL(static_cast<int>(RapKind::HELLO), out[0]);

    // 骨干回 ATTACH_ACK（msg: [kind][src u16][dst u16]）
    uint8_t ack[5] = { static_cast<uint8_t>(RapKind::ATTACH_ACK), 0x55,0x00, 0xAA,0x00 };
    bool moved = rap.onRapFrame(ack, 5, 1001);
    TEST_ASSERT_TRUE(moved);
    TEST_ASSERT_EQUAL(static_cast<int>(RapTerminalState::ATTACHED), static_cast<int>(rap.state()));
    TEST_ASSERT_EQUAL(0xAA, rap.attachedBackbone());
}

// ---------------------------------------------------------------------------
// MeshRocStack 端到端：sendPayload -> 捕获 sendRaw -> ingestRaw -> onPacket
// ---------------------------------------------------------------------------
class CaptureStack : public MeshRocStack {
public:
    explicit CaptureStack(const config::MeshROCConfig& c) : MeshRocStack(c, 0x01, false) {}
    std::vector<uint8_t> lastFrame;
    bool gotPacket = false;
    std::vector<uint8_t> gotPayload;

protected:
    void sendRaw(const uint8_t* bytes, uint16_t len) override {
        lastFrame.assign(bytes, bytes + len);
    }
    void onPacket(const PacketReceived& pkt) override {
        gotPacket = true;
        gotPayload.assign(pkt.payload, pkt.payload + pkt.payloadLen);
    }
};

void test_stack_e2e_small()
{
    config::MeshROCConfig cfg{};
    CaptureStack stack(cfg);

    const uint8_t msg[] = "hello-meshroc";
    bool ok = stack.sendPayload(0x02, msg, sizeof(msg) - 1, MeshRocPriority::LOW, false);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(stack.lastFrame.size() > 10);

    // 把发出的帧喂回 ingestRaw，应触发 onPacket 且 payload 一致
    stack.ingestRaw(stack.lastFrame.data(), static_cast<uint16_t>(stack.lastFrame.size()), 5000);
    TEST_ASSERT_TRUE(stack.gotPacket);
    TEST_ASSERT_EQUAL(sizeof(msg) - 1, stack.gotPayload.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, stack.gotPayload.data(), sizeof(msg) - 1);
}

void test_stack_e2e_fragmented()
{
    config::MeshROCConfig cfg{};
    CaptureStack stack(cfg);

    // 构造一个超过分片阈值的载荷（触发 O5 分片）
    uint8_t big[700];
    for (size_t i = 0; i < sizeof(big); ++i) big[i] = static_cast<uint8_t>(i & 0xFF);

    bool ok = stack.sendPayload(0x02, big, sizeof(big), MeshRocPriority::LOW, false);
    TEST_ASSERT_TRUE(ok);
    // 分片后首帧应被捕获（多片逐次 sendRaw，lastFrame 为最后一片）
    TEST_ASSERT_TRUE(stack.lastFrame.size() > 10);

    // 端到端重组需完整分片集；此处仅验证发送端切片不崩溃且产出合法帧。
    // 完整多片重组闭环见 test_reassembler_roundtrip。
}

void setup()
{
    UNITY_BEGIN();
    RUN_TEST(test_airtime_basic);
    RUN_TEST(test_airtime_preset_table);
    RUN_TEST(test_reassembler_roundtrip);
    RUN_TEST(test_reassembler_missing_nack);
    RUN_TEST(test_rap_terminal_attach);
    RUN_TEST(test_stack_e2e_small);
    RUN_TEST(test_stack_e2e_fragmented);
    UNITY_END();
}

void loop() {}
