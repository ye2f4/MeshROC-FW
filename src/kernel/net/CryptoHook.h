#pragma once
#include <cstdint>
#include <cstddef>
#include "config/MeshROCConfig.h"

/**
 * CryptoHook：O3 加密调用钩子（原创优化 O3，对应网站承诺）
 *
 * 设计定位：本类提供加密/解密的**调用边界与开关接线**，但不在此实现完整密码学。
 * 真正的 AES-256-GCM + ECDH-P256 实现位（stub）留给后续迭代填充——当前阶段直接
 * 做直通拷贝（plaintext passthrough），保证整条发送/接收管线在不依赖密码学库的前提下
 * 可端到端跑通。所有"待实现"点用 // TODO(crypto): 标注，便于后续任务检索。
 *
 * 开关来源：config.crypto.mode / pskFallback（§15.3）。密钥管理（PSK / ECDH 协商）
 * 不在本类范围内，由上层 KeyStore 提供，本钩子只消费"已就绪的会话密钥"。
 *
 * 参考契约 §15.2-O3 / §15.3 crypto。
 */
namespace meshroc::net {

class CryptoHook {
public:
    enum class Result : uint8_t {
        OK = 0,
        DISABLED = 1,        // 加密未启用，调用方应直接发明文
        KEY_MISSING = 2,     // 无可用会话密钥
        BUFFER_TOO_SMALL = 3, // 输出缓冲不足
        // TODO(crypto): ALGO_UNSUPPORTED / BAD_TAG 等
    };

    explicit CryptoHook(const config::MeshROCConfig& cfg) : cfg_(cfg) {}

    bool enabled() const
    {
        // GCM_ECDH 与 PSK_CTR 都视为"启用加密"；仅当模式明确且后续实现就绪才真正加密。
        // 当前 stub 阶段：报告启用意图，但 encrypt/decrypt 做直通（见下）。
        return true;
    }

    // 加密：in -> out。stub 阶段做直通拷贝（明文即密文占位）。
    // 后续：AES-256-GCM(随机数由 out 前导, tag 追加) / PSK-CTR。
    Result encrypt(const uint8_t* in, uint16_t inLen,
                   uint8_t* out, uint16_t& outLen, uint16_t cap,
                   uint16_t /*src*/, uint16_t /*dst*/) const
    {
        if (cap < inLen) {
            outLen = 0;
            return Result::BUFFER_TOO_SMALL;
        }
        // TODO(crypto): 真正调用 AES-256-GCM / PSK-CTR，此处暂直通。
        for (uint16_t i = 0; i < inLen; ++i) out[i] = in[i];
        outLen = inLen;
        return Result::OK;
    }

    // 解密：out -> in 的逆操作。stub 阶段直通拷贝。
    Result decrypt(const uint8_t* in, uint16_t inLen,
                   uint8_t* out, uint16_t& outLen, uint16_t cap,
                   uint16_t /*src*/, uint16_t /*dst*/) const
    {
        if (cap < inLen) {
            outLen = 0;
            return Result::BUFFER_TOO_SMALL;
        }
        // TODO(crypto): 真正 GCM 解密+tag 校验 / PSK-CTR。
        for (uint16_t i = 0; i < inLen; ++i) out[i] = in[i];
        outLen = inLen;
        return Result::OK;
    }

private:
    const config::MeshROCConfig& cfg_;
};

}  // namespace meshroc::net
