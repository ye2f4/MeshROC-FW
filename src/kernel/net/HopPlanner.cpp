#include "kernel/net/HopPlanner.h"
#include <algorithm>

namespace meshroc::net {

uint8_t HopPlanner::estNetDiameter() const
{
    // 观测直径+1，与配置默认 hopLimit 取较大者
    uint8_t fromObs = observedDiameter_ > 0
                          ? static_cast<uint8_t>(observedDiameter_ + 1)
                          : 0;
    return std::max(fromObs, cfg_.lora.hopLimit);
}

uint8_t HopPlanner::adaptiveHopLimit() const
{
    uint8_t est = estNetDiameter();
    // 上限 7，防极端
    return std::min(est, static_cast<uint8_t>(7));
}

uint8_t HopPlanner::effectiveHopLimit(uint8_t datapackMaxHop) const
{
    // 双层协同：自适应跳数不可低于 datapack 硬下限
    return std::max(adaptiveHopLimit(), datapackMaxHop);
}

}  // namespace meshroc::net
