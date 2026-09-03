#pragma once
// PowerDashSampler.h —— 平台无关采样引擎(Task 4)。
// 节拍驱动任意 IPlatformProbe:时间戳/elapsed/mode 等平台无关字段在此
// 填充,平台字段(含 utilPct —— 探针 usage 监视器负责)由 probe 负责;
// 历史窗口由 sink 侧维护。
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
    IPlatformProbe& probe_;
    std::function<std::string()> mode_;
    const volatile bool* exit_;
    std::function<void()> wait_;
    std::uint64_t startedTick_ = 0;
};
} // namespace pd
