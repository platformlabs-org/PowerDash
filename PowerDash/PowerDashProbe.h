#pragma once
#include "PowerDashModel.h"
#include <memory>

namespace pd {

// 驱动 IO 原语抽象:生产环境包装 DeviceIoControl(WindowsDriverIo),
// 测试中用 FixtureDriverIo 注入应答。SMN 访问必须走 ReadSmn(驱动内
// 原子互斥的 IO_CTL_SMN_READ),禁止用户态拆写 0x60/0x64。
class DriverIo {
public:
    virtual ~DriverIo() = default;
    virtual bool ReadMsr(unsigned core, uint32_t msr, uint64_t& out) = 0;
    virtual bool ReadPciCfg(unsigned bus, unsigned dev, unsigned fn,
                            unsigned reg, uint32_t& out) = 0;
    virtual bool WritePciCfg(unsigned bus, unsigned dev, unsigned fn,
                             unsigned reg, uint32_t value) = 0;
    virtual bool ReadSmn(uint32_t smnAddr, uint32_t& out) = 0;
    virtual bool MapPhys(uint64_t phys, size_t len, void*& virt) = 0;
    virtual void UnmapPhys(void* virt) = 0;
};

class WindowsDriverIo : public DriverIo {       // PowerDashProbe.cpp 实现
public:
    explicit WindowsDriverIo(void* driverHandle);
    bool ReadMsr(unsigned core, uint32_t msr, uint64_t& out) override;
    bool ReadPciCfg(unsigned bus, unsigned dev, unsigned fn,
                    unsigned reg, uint32_t& out) override;
    bool WritePciCfg(unsigned bus, unsigned dev, unsigned fn,
                     unsigned reg, uint32_t value) override;
    bool ReadSmn(uint32_t smnAddr, uint32_t& out) override;   // Task 7 前 return false
    bool MapPhys(uint64_t phys, size_t len, void*& virt) override;
    void UnmapPhys(void* virt) override;
private:
    void* h_;
};

class IPlatformProbe {
public:
    virtual ~IPlatformProbe() = default;
    virtual const PlatformCaps& caps() const = 0;
    virtual bool readSample(Sample& s) = 0;   // false = 本帧无任何有效功率域
};

std::unique_ptr<IPlatformProbe> CreateIntelProbe(DriverIo& io,
                                                 const PlatformInfo& info);
std::unique_ptr<IPlatformProbe> CreateAmdProbe(DriverIo& io,
                                               const PlatformInfo& info);

// vendor 分流工厂(Task 6 接线入口):按 PlatformInfo.vendor 选探针;
// AMD 探针落地前的窗口期返回 nullptr,由调用方报 "Unsupported platform"
// 退出(Task 8 把 nullptr 臂替换为 CreateAmdProbe 分流)。
std::unique_ptr<IPlatformProbe> CreateProbe(DriverIo& io,
                                            const PlatformInfo& info);

} // namespace pd
