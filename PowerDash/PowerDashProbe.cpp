#include "PowerDashProbe.h"
#include "PowerDashIoctl.h"
#include "../PowerDashSYS/pdstruct.h"   // MSR_Request / PCICFG_Request / MMAP_Request + IO_CTL_PCICFG_WRITE
#include <windows.h>

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
    // 驱动内 FAST_MUTEX 原子完成 0x60 写地址 / 0x64 读数据序列;
    // 结构体同缓冲进出(MethodBuffered),驱动 Information = sizeof(SMN_Request) 回写 value。
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

// CreateIntelProbe 落地于 PowerDashIntel.cpp;CreateAmdProbe 由 Task 8 落地。
std::unique_ptr<IPlatformProbe> CreateProbe(DriverIo& io,
                                            const PlatformInfo& i) {
    if (i.vendor == Vendor::Amd) return nullptr;   /* AmdProbe 落地于后续任务 */
    return CreateIntelProbe(io, i);
}

} // namespace pd
