#include "ProtocolManager.h"
#include "kernel/MeshRocStack.h"   // 自研原生栈（kernel/ 内部，允许在 mesh/ 层依赖）
#include <new>

namespace meshroc {

ProtocolManager::ProtocolManager(const config::MeshROCConfig& cfg,
                                 uint16_t myAddr,
                                 bool isBackbone)
    : cfg_(cfg), stack_(nullptr)
{
    // 自研原生栈：MESHROC 固件的主体协议。
    // 其 sendRaw() 仍为空 TODO(rf)，接入射频后派生重载；射频接入前仅作算法/状态机演练。
    stack_ = new (std::nothrow) MeshRocStack(cfg_, myAddr, isBackbone);
}

ProtocolManager::~ProtocolManager()
{
    delete stack_;
    stack_ = nullptr;
    // 第三方桥接器（meshtasticBridge_/meshcoreBridge_）由各自的拥有者释放，
    // 本管理器仅持有裸指针占位，不负责 delete（避免跨模块所有权混乱）。
}

void ProtocolManager::tick(uint32_t nowMs)
{
    // 推进自研原生栈：环境采样(O1)/RAP/O2 TDMA/O5 重组超时/O4 重传。
    if (stack_) {
        stack_->tick(nowMs);
    }
    // 第三方兼容桥接器（未来）：在各自独立信道上侦听帧 → 解析 → 桥接进 MESHROC 网络。
    // 当前 meshtasticBridge_ / meshcoreBridge_ 均为 nullptr，跳过。
    // 注意：不在此驱动任何「第三方全栈」，它们不是本固件的运行组件。
}

}  // namespace meshroc
