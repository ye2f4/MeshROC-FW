#pragma once

/**
 * MeshROC（Mesh Radio-Optimized Communications）固件适配辅助。
 *
 * 把官网「全国地貌模板」映射为标准 LoRaConfig 字段。固件本身是 Meshtastic 分支，
 * 原生支持这些字段，因此 Web 客户端 / 串口工具下发 set_config(LoRaConfig) 后固件自动按对应参数工作。
 *
 * 本文件提供的是「待接线」的归总实现：只写入标准 config.lora.* 全局；
 * 真正让改动生效需要在调用处触发固件的配置持久化 + 射频重配置
 *（参考固件中 AdminMessage.set_config 的处理：service->reloadConfig() 或等价调用）。
 * 当前未自动调用，避免在未编译验证的环境下改变设备行为。
 */
void MeshROC_applyEnvTemplate(int id);
