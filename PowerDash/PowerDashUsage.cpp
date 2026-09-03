#include "PowerDashUsage.h"
// NtQuerySystemInformation 未在公开 SDK 头中声明(winternl.h 的版本不带
// class 8 布局),这里自声明 x64 结构:Idle/Kernel/User 位于偏移 0/8/16,
// 其余字段与 busy% 无关,仅用于钉住总大小 48 字节。
#include <windows.h>
#include <cstdint>

namespace pd {
namespace {

constexpr ULONG kSystemProcessorPerformanceInformation = 8;

struct SystemProcessorPerfInfo {   // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION(x64)
    LARGE_INTEGER IdleTime;        // +0
    LARGE_INTEGER KernelTime;      // +8(含 Idle)
    LARGE_INTEGER UserTime;        // +16
    LARGE_INTEGER DpcTime;         // +24(保留)
    LARGE_INTEGER InterruptTime;   // +32
    ULONG InterruptCount;          // +40
    ULONG Pad;                     // +44 -> sizeof = 48
};
static_assert(sizeof(SystemProcessorPerfInfo) == 48,
              "x64 layout: 48 bytes per LP entry");

using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);

NtQuerySystemInformationFn ResolveNtQuerySystemInformation() {
    // ntdll 永驻进程内,GetModuleHandle(不增引用计数)即够;失败返回
    // nullptr,调用方按数据源不可用处理。
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return nullptr;
    return reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(nt, "NtQuerySystemInformation"));
}

} // namespace

NtUsageSource::NtUsageSource(unsigned nLP) : nLP_(nLP) {}

bool NtUsageSource::ReadPerLp(std::vector<double>& out) {
    out.clear();
    if (nLP_ == 0) return false;
    NtQuerySystemInformationFn fn = ResolveNtQuerySystemInformation();
    if (!fn) return false;
    std::vector<SystemProcessorPerfInfo> buf(nLP_);
    ULONG returned = 0;
    LONG status = fn(kSystemProcessorPerformanceInformation, buf.data(),
                     static_cast<ULONG>(nLP_ * sizeof(SystemProcessorPerfInfo)),
                     &returned);
    if (status < 0) return false;   // NTSTATUS 错误:基线不动
    if (returned != nLP_ * sizeof(SystemProcessorPerfInfo))
        return false;               // 长度不符(如 >64 LP 被截断):基线不动
    if (!hasPrev_) {                // 首帧只存基线,尚无窗口可差分
        prevIdle_.resize(nLP_);
        prevKernel_.resize(nLP_);
        prevUser_.resize(nLP_);
        for (unsigned i = 0; i < nLP_; ++i) {
            prevIdle_[i] = (unsigned long long)buf[i].IdleTime.QuadPart;
            prevKernel_[i] = (unsigned long long)buf[i].KernelTime.QuadPart;
            prevUser_[i] = (unsigned long long)buf[i].UserTime.QuadPart;
        }
        hasPrev_ = true;
        return false;
    }
    out.resize(nLP_);
    for (unsigned i = 0; i < nLP_; ++i) {
        // busy% = 100*(1 - dIdle/(dKernel+dUser)),Kernel 含 Idle;计数器
        // 单调,无符号差分安全;结果钳 0-100(异常读数不外泄)。
        const unsigned long long dIdle =
            (unsigned long long)buf[i].IdleTime.QuadPart - prevIdle_[i];
        const unsigned long long dKernel =
            (unsigned long long)buf[i].KernelTime.QuadPart - prevKernel_[i];
        const unsigned long long dUser =
            (unsigned long long)buf[i].UserTime.QuadPart - prevUser_[i];
        const double denom = (double)(dKernel + dUser);
        double busy = 0.0;
        if (denom > 0.0)
            busy = 100.0 * (1.0 - (double)dIdle / denom);
        if (busy < 0.0) busy = 0.0;
        if (busy > 100.0) busy = 100.0;
        out[i] = busy;
        prevIdle_[i] = (unsigned long long)buf[i].IdleTime.QuadPart;
        prevKernel_[i] = (unsigned long long)buf[i].KernelTime.QuadPart;
        prevUser_[i] = (unsigned long long)buf[i].UserTime.QuadPart;
    }
    return true;
}

UsageMonitor::UsageMonitor(std::unique_ptr<IUsageSource> src)
    : src_(std::move(src)) {}

LpUsage UsageMonitor::Read() {
    LpUsage out;
    std::vector<double> v;
    if (!src_ || !src_->ReadPerLp(v) || v.empty())
        return out;                 // ok=false;基线保持,下次成功仍可差分
    if (!hasBaseline_) {            // 首帧只建立基线(差分窗口起点)
        hasBaseline_ = true;
        return out;
    }
    double sum = 0.0, mx = v[0];
    for (double b : v) {
        sum += b;
        if (b > mx) mx = b;
    }
    out.busyPct = std::move(v);
    out.totalPct = sum / (double)out.busyPct.size();
    out.maxPct = mx;
    out.ok = true;
    return out;
}

} // namespace pd
