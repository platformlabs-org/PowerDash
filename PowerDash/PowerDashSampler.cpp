// PowerDashSampler.cpp —— 平台无关采样引擎(Task 4)。
// 数值逻辑自 PowerDash.cpp RunMonitor 逐段迁移,算式原样照搬:
//   LocalIsoTimestamp   :523-531(唯一定义在本文件;旧监视循环里的那份
//                        副本已随 Task 6 删除)
//   util 基线           :676-677(构造期 GetSystemTimes)
//   util 每帧差分       :819-841(100*(1-dIdle/dTotal),钳 0-100)
//   可中断节拍等待      :757-758(10 x Sleep(100))
#include "PowerDashSampler.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <string>

namespace pd {
namespace {

std::string LocalIsoTimestamp() {                    // :523-531
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char text[24] = {};
    sprintf_s(text, "%04u-%02u-%02uT%02u:%02u:%02u",
              now.wYear, now.wMonth, now.wDay,
              now.wHour, now.wMinute, now.wSecond);
    return text;
}

std::uint64_t ToU64(const FILETIME& ft) {            // :823-826
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32)
           | ft.dwLowDateTime;
}

} // namespace

Sampler::Sampler(IPlatformProbe& probe, std::function<std::string()> modeReader,
                 const volatile bool* exitFlag,
                 std::function<void()> tickWait)
    : probe_(probe), mode_(std::move(modeReader)), exit_(exitFlag),
      wait_(tickWait ? std::move(tickWait) : [this] { WaitDefault(); }) {
    FILETIME idle = {}, kernel = {}, user = {};
    GetSystemTimes(&idle, &kernel, &user);           // :676-677 构造期基线
    prevIdle_ = ToU64(idle);
    prevKernel_ = ToU64(kernel);
    prevUser_ = ToU64(user);
    startedTick_ = GetTickCount64();                 // :747 elapsed 基线
}

void Sampler::WaitDefault() {                        // :757-758 1 s 采样窗口
    for (int i = 0; i < 10; ++i) {
        if (exit_ && *exit_) break;                  // exitFlag 为空则不检查
        Sleep(100);
    }
}

Reading Sampler::ReadUtilization() {                 // :819-841
    FILETIME idle = {}, kernel = {}, user = {};
    if (!GetSystemTimes(&idle, &kernel, &user))
        return NA();                                 // 本帧读取失败 -> invalid
    const double dIdle = static_cast<double>(ToU64(idle) - prevIdle_);
    const double dTotal = static_cast<double>(
        (ToU64(kernel) - prevKernel_) + (ToU64(user) - prevUser_));
    double utilPct = 0.0;                            // dTotal<=0 时与源一致取 0
    if (dTotal > 0.0) {
        utilPct = 100.0 * (1.0 - dIdle / dTotal);
        if (utilPct < 0.0) utilPct = 0.0;
        if (utilPct > 100.0) utilPct = 100.0;
    }
    prevIdle_ = ToU64(idle);
    prevKernel_ = ToU64(kernel);
    prevUser_ = ToU64(user);
    return Ok(utilPct);
}

bool Sampler::Run(double runSeconds,
                  const std::function<void(const Sample&)>& sink) {
    int consecutiveFailures = 0;
    int frame = 0;
    while ((!exit_ || !*exit_) &&
           (runSeconds < 0 ||
            frame < static_cast<int>(std::ceil(runSeconds)))) {
        if (wait_) wait_();                          // 源循环:先等 1 s 再采样

        Sample s;
        if (!probe_.readSample(s)) {                 // 本帧无任何有效功率域
            if (++consecutiveFailures >= 5)          // 连续 5 帧失败熔断
                return false;
            ++frame;
            continue;
        }
        consecutiveFailures = 0;                     // 成功即清零
        s.timestamp = LocalIsoTimestamp();
        s.elapsedS = static_cast<double>(GetTickCount64() - startedTick_)
                     / 1000.0;
        if (mode_) s.mode = mode_();                 // 每帧重读 mode
        s.utilPct = ReadUtilization();
        if (sink) sink(s);
        ++frame;
    }
    return true;
}

} // namespace pd
