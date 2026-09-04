#pragma once
#include "PowerDashModel.h"
#include <memory>

namespace pd {

class SensorTable;   // v3 宽表(PowerDashSensors.h);接口层只出指针,不引入定义

// 驱动 IO 原语抽象:生产环境包装 DeviceIoControl(WindowsDriverIo),
// 测试中用 FixtureDriverIo 注入应答。SMN 直读(如 AMD k10temp Tctl)走
// ReadSmn/WriteSmn(驱动内原子互斥的 IO_CTL_SMN_READ/WRITE,Hal
// 0x60/0x64 读实证路径);SMU 邮箱不走此处 —— 用户态 ECAM(SmnEcam,
// PowerDashPmTable.h:0x64 数据口写经 pci.sys 被拒、CF8/CFC 本平台异常)。
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
    // v3 宽表访问器:宽表探针返回内部 SensorTable(构造期定列,每帧
    // Set 回填);保底探针保持默认 nullptr(CSV/渲染层按缺席降级)。
    virtual SensorTable* sensors() { return nullptr; }
};

std::unique_ptr<IPlatformProbe> CreateIntelProbe(DriverIo& io,
                                                 const PlatformInfo& info);
std::unique_ptr<IPlatformProbe> CreateAmdProbe(DriverIo& io,
                                               const PlatformInfo& info);

// vendor 分流工厂:按 PlatformInfo.vendor 选探针(Intel/AMD 双臂已落地)。
std::unique_ptr<IPlatformProbe> CreateProbe(DriverIo& io,
                                            const PlatformInfo& info);

// TSC 频率校准(v3 Task 3):~150 ms QPC 对照 __rdtsc;
// hz = Δtsc * qpcFreq / Δqpc。失败返回 0(调用方退避)。
double CalibrateTscHz();

// TSC 校准测试注入点:探针 ctor 优先调用此指针(非空时),生产恒
// nullptr -> 走 CalibrateTscHz()。单测注入恒 0 覆盖"校准失败 ->
// eff 列 NA"守卫分支(真失败依赖 QPC/计时器,单测环境不可构造;
// 全局函数指针,仅单线程测试使用)。
extern double (*TscCalibrationOverride)();

} // namespace pd
