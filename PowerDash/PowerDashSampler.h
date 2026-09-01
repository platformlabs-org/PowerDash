#pragma once
// PowerDashSampler.h —— 平台无关采样引擎(Task 4)。
// 节拍驱动任意 IPlatformProbe:时间戳/elapsed/util/mode 等平台无关字段
// 在此填充,平台字段由 probe 负责;历史窗口由 sink 侧维护。
#include "PowerDashProbe.h"
#include <cstdint>
#include <functional>
namespace pd {
class Sampler {
public:
    Sampler(IPlatformProbe& probe, std::function<std::string()> modeReader,
            const volatile bool* exitFlag,
            std::function<void()> tickWait = {});
    bool Run(double runSeconds, const std::function<void(const Sample&)>& sink);
private:
    void WaitDefault();                 // 10 x Sleep(100),可被 exitFlag 打断
    Reading ReadUtilization();          // GetSystemTimes 差分,钳 0-100
    IPlatformProbe& probe_;
    std::function<std::string()> mode_;
    const volatile bool* exit_;
    std::function<void()> wait_;
    std::uint64_t prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;
    std::uint64_t startedTick_ = 0;
};
} // namespace pd
