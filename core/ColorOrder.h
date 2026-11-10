#pragma once

#include <cstdint>
#include "Version.h"

namespace mosaicraft
{

// 浏览顺序独立于匹配特征；修改算法时仅重算此键，不升级特征版本。
std::int64_t meanLabColorOrderKey(double averageL, double averageA, double averageB);

} // namespace mosaicraft
