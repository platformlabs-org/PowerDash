#pragma once
#include "PowerDashModel.h"
#include <memory>

namespace pd {

// 驱动 IO 原语抽象:生产环境包装 DeviceIoControl(WindowsDriverIo),
// 测试中用 FixtureDriverIo 注入应答。SMN 访问必须走 ReadSmn/WriteSmn
// (驱动内原子互斥的 IO_CTL_SMN_READ/WRITE),禁止用户态拆写 0x60/0x64。
class DriverIo {
public:
    virtual ~DriverIo() = default;
    virtual bool ReadMsr(unsigned core, uint32_t msr, uint64_t& out) = 0;
    virtual bool ReadPciCfg(unsigned bus, unsigned dev, unsigned fn,
                            unsigned reg, uint32_t& out) = 0;
    virtual bool WritePciCfg(unsigned bus, unsigned dev, unsigned fn,
                             unsigned reg, uint32_t value) = 0;
    virtual bool ReadSmn(uint32_t smnAddr, uint32_t& out) = 0;
    virtual bool WriteSmn(uint32_t smnAddr, uint32_t value) = 0;
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
    bool WriteSmn(uint32_t smnAddr, uint32_t value) override;
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

// vendor 分流工厂:按 PlatformInfo.vendor 选探针(Intel/AMD 双臂已落地)。
std::unique_ptr<IPlatformProbe> CreateProbe(DriverIo& io,
                                            const PlatformInfo& info);

} // namespace pd
