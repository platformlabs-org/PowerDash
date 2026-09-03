// PowerDashSampler.cpp —— 平台无关采样引擎(Task 4)。
// 数值逻辑自 PowerDash.cpp RunMonitor 逐段迁移,算式原样照搬:
//   LocalIsoTimestamp   :523-531(唯一定义在本文件;旧监视循环里的那份
//                        副本已随 Task 6 删除)
//   可中断节拍等待      :757-758(10 x Sleep(100))
// v3 Task 7:utilPct 责任移交探针(usage 监视器双基线暖机),本文件的
// GetSystemTimes 差分(原 :676-677 基线 / :819-841 每帧差分)整体删除,
// Run 不再覆盖 s.utilPct —— 探针未填时保持 NA(暖机帧诚实缺席)。
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

} // namespace

Sampler::Sampler(IPlatformProbe& probe, std::function<std::string()> modeReader,
                 const volatile bool* exitFlag,
                 std::function<void()> tickWait)
    : probe_(probe), mode_(std::move(modeReader)), exit_(exitFlag),
      wait_(tickWait ? std::move(tickWait) : [this] { WaitDefault(); }) {
    startedTick_ = GetTickCount64();                 // :747 elapsed 基线
}

void Sampler::WaitDefault() {                        // :757-758 1 s 采样窗口
    for (int i = 0; i < 10; ++i) {
        if (exit_ && *exit_) break;                  // exitFlag 为空则不检查
        Sleep(100);
    }
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
        // utilPct 不再在此覆盖:探针 usage 监视器负责(暖机帧 NA)
        if (sink) sink(s);
        ++frame;
    }
    return true;
}

} // namespace pd
