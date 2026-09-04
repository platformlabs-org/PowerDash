#include "PowerDashProbe.h"
#include "PowerDashIoctl.h"
#include "../PowerDashSYS/pdstruct.h"   // MSR_Request / PCICFG_Request / MMAP_Request + IO_CTL_PCICFG_WRITE
#include <windows.h>
#include <cmath>
#include <intrin.h>

namespace pd {

WindowsDriverIo::WindowsDriverIo(void* h) : h_(h) {}

bool WindowsDriverIo::ReadMsr(unsigned core, uint32_t msr, uint64_t& out) {
    MSR_Request req{};
    req.core_id = (int)core;
    req.msr_address = msr;
    DWORD returned = 0;
    // 与 PowerDash.cpp 现有 read_msr 相同的 DeviceIoControl 形态,
    // 字段名照抄 pdstruct.h 的 MSR_Request。
    if (!DeviceIoControl(h_, IO_CTL_MSR_READ, &req, sizeof(req),
                         &out, sizeof(out), &returned, nullptr))
        return false;
    return true;
}

bool WindowsDriverIo::ReadPciCfg(unsigned bus, unsigned dev, unsigned fn,
                                 unsigned reg, uint32_t& out) {
    PCICFG_Request req{};
    req.bus = bus;
    req.dev = dev;
    req.func = fn;
    req.reg = reg;
    req.bytes = 4;
    ULONG64 value = 0;
    DWORD returned = 0;
    if (!DeviceIoControl(h_, IO_CTL_PCICFG_READ, &req, sizeof(req),
                         &value, sizeof(value), &returned, nullptr))
        return false;
    out = (uint32_t)value;
    return true;
}

bool WindowsDriverIo::WritePciCfg(unsigned bus, unsigned dev, unsigned fn,
                                  unsigned reg, uint32_t value) {
    PCICFG_Request req{};
    req.bus = bus;
    req.dev = dev;
    req.func = fn;
    req.reg = reg;
    req.bytes = 4;
    req.write_value = value;
    DWORD returned = 0;
    // 驱动侧 IO_CTL_PCICFG_WRITE 不返回数据(powerdash.c: Information = 0)
    return DeviceIoControl(h_, IO_CTL_PCICFG_WRITE, &req, sizeof(req),
                           nullptr, 0, &returned, nullptr) != FALSE;
}

bool WindowsDriverIo::ReadSmn(uint32_t smnAddr, uint32_t& out) {
    // 驱动内 FAST_MUTEX 原子完成 0x60 写地址 / 0x64 读数据序列:Hal 总线
    // 数据接口 @ B0:D0:F0(读实证可用,Tctl 一路正确;实证注记见
    // powerdash.c)。结构体同缓冲进出(MethodBuffered),驱动
    // Information = sizeof(SMN_Request) 回写 value。
    SMN_Request req{};
    req.address = smnAddr;
    req.value = 0;
    DWORD returned = 0;
    if (!DeviceIoControl(h_, IO_CTL_SMN_READ, &req, sizeof(req),
                         &req, sizeof(req), &returned, nullptr))
        return false;
    out = req.value;
    return true;
}

bool WindowsDriverIo::WriteSmn(uint32_t smnAddr, uint32_t value) {
    // 同 ReadSmn 形态但无回读:驱动侧在互斥内 Hal 写 0x60(地址)/0x64
    // (数据)。注:0x64 数据口写经 pci.sys 被 Krackan 拒(实证),SMU
    // 邮箱写入走用户态 ECAM(SmnEcam);本通道留作其他 SMN 域未来使用。
    SMN_Request req{};
    req.address = smnAddr;
    req.value = value;
    DWORD returned = 0;
    return DeviceIoControl(h_, IO_CTL_SMN_WRITE, &req, sizeof(req),
                           nullptr, 0, &returned, nullptr) != FALSE;
}

bool WindowsDriverIo::MapPhys(uint64_t phys, size_t len, void*& virt) {
    MMAP_Request req{};
    req.address.QuadPart = (LONGLONG)phys;
    req.size = len;
    uint64_t userVirtual = 0;   // 驱动返回映射基址(ULONG64)
    DWORD returned = 0;
    if (!DeviceIoControl(h_, IO_CTL_MMAP, &req, sizeof(req),
                         &userVirtual, sizeof(userVirtual), &returned, nullptr))
        return false;
    virt = reinterpret_cast<void*>(userVirtual);
    return true;
}

void WindowsDriverIo::UnmapPhys(void* virt) {
    MMAP_Request unmap{};
    unmap.address.QuadPart = (LONGLONG)(uintptr_t)virt;   // 用户态 VA 原样回传
    DWORD returned = 0;
    DeviceIoControl(h_, IO_CTL_MUNMAP, &unmap, sizeof(unmap),
                    nullptr, 0, &returned, nullptr);
}

// CreateIntelProbe 落地于 PowerDashIntel.cpp,CreateAmdProbe 落地于
// PowerDashAmd.cpp —— 按 vendor 分流,双臂均已落地。
std::unique_ptr<IPlatformProbe> CreateProbe(DriverIo& io,
                                            const PlatformInfo& i) {
    return i.vendor == Vendor::Amd ? CreateAmdProbe(io, i)
                                   : CreateIntelProbe(io, i);
}

// 测试注入点(见 PowerDashProbe.h 注释):生产恒 nullptr。
double (*TscCalibrationOverride)() = nullptr;

double CalibrateTscHz() {
    // QPC 与 __rdtsc 成对采样,间隔 ~150 ms:
    // hz = Δtsc * qpcFreq / Δqpc。结果非正/非有限 -> 0(调用方退避)。
    LARGE_INTEGER freq = {}, q1 = {}, q2 = {};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0)
        return 0.0;
    if (!QueryPerformanceCounter(&q1)) return 0.0;
    const unsigned long long t1 = __rdtsc();
    Sleep(150);
    if (!QueryPerformanceCounter(&q2)) return 0.0;
    const unsigned long long t2 = __rdtsc();
    const long long dq = q2.QuadPart - q1.QuadPart;
    if (dq <= 0) return 0.0;
    const double hz =
        (double)(t2 - t1) * (double)freq.QuadPart / (double)dq;
    if (!(hz > 0.0) || !std::isfinite(hz)) return 0.0;
    return hz;
}

} // namespace pd
