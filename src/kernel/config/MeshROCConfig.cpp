#include "kernel/config/MeshROCConfig.h"

// 全局唯一配置实例定义（声明见 MeshROCConfig.h）。
// 后续可在此接入 FlashKV / NVS 加载与保存：
//   - loadFromFlash()：开机从 FlashKV 读取填充本实例
//   - saveToFlash()：配置变更时持久化
// 当前为默认构造，固件行为与未配置一致，不依赖持久化即可编译运行。
namespace meshroc::config {

MeshROCConfig gMeshRocConfig;

} // namespace meshroc::config
