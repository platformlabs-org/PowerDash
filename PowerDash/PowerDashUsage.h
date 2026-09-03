#pragma once
// PowerDashUsage.h —— v3 Task 3:逐逻辑处理器(LP)使用率监视器。
// IUsageSource 抽象数据源(单测注入 Fake);NtUsageSource 走
// NtQuerySystemInformation(SystemProcessorPerformanceInformation)内部差分,
// 每次成功调用返回"距上次成功调用"窗口内的 busy%;首次调用只存基线返回
// false。UsageMonitor 首帧只建基线(ok=false),其后帧聚合 total/max。
// 接线进 RunMonitor 在 Task 7 完成。
#include <memory>
#include <vector>

namespace pd {

struct LpUsage {
    std::vector<double> busyPct;   // 每 LP 0-100
    double totalPct = 0.0;         // 全体平均
    double maxPct = 0.0;           // 最大 LP
    bool ok = false;
};

class IUsageSource {                       // 可注入(单测)
public:
    virtual ~IUsageSource() = default;
    virtual bool ReadPerLp(std::vector<double>& busyPct) = 0;  // false=失败
};

class NtUsageSource : public IUsageSource { // NtQuerySystemInformation(8)
public:
    explicit NtUsageSource(unsigned nLP);
    bool ReadPerLp(std::vector<double>& out) override;
private:   // 计数器基线按 uint64 三元组保存,头文件不引入 windows.h
    unsigned nLP_ = 0;
    bool hasPrev_ = false;
    std::vector<unsigned long long> prevIdle_, prevKernel_, prevUser_;
};

class UsageMonitor {
public:
    explicit UsageMonitor(std::unique_ptr<IUsageSource> src);
    LpUsage Read();                        // 差分;首帧 ok=false
private:
    std::unique_ptr<IUsageSource> src_;
    bool hasBaseline_ = false;   // 读取失败时保持原值(下次成功仍可差分)
};

} // namespace pd
