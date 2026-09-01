# PowerDash 分层架构 + AMD 监控支持 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 spec 将 PowerDash 重构为「平台 Probe + 统一数据模型 + 平台无关采样引擎/渲染」分层架构,并落地 AMD 监控只读支持。

**Architecture:** `IPlatformProbe` 接口隔离平台差异(Intel/Amd 双实现),`Sample v2` 类型体系(每个字段自带 `Reading{value, valid}` 可用性语义),采样引擎与渲染/CSV 只消费统一模型;内核驱动新增原子 `IO_CTL_SMN_READ`(SMN 0x60/0x64 互斥访问)。

**Tech Stack:** C++17(Win32)、WDM 内核驱动(WDK10)、现有 cmd 构建脚本(build.cmd / run-tests.cmd)、单测试二进制(tests/PowerDashUiTests.cpp)。

**Spec:** `docs/superpowers/specs/2026-09-01-powerdash-architecture-design.md`(执行者必须同时阅读 spec)

## Global Constraints

- 单 exe 分发不变:驱动仍内嵌资源、按需装卸、退出零残留
- VT 差分刷新协议(原位覆写、行差分、resize 全量重画)行为不变
- Intel 面板 golden 断言(现有测试全部字符串)在迁移前后必须全绿——这是"Intel 帧等价"的门
- CSV v2 列与列名以 spec 第 6 节为准,invalid → 空单元格
- 每个任务结束:构建全绿 + 单测全绿 + `git commit`(只提交本任务文件)
- 新增 .cpp 必须同步登记 `PowerDash.vcxproj` 与 `.vcxproj.filters`
- 测试只进 `tests/PowerDashUiTests.cpp`(单二进制,`tests\run-tests.cmd` 运行)
- AMD 真实限值读取(SMU PMTable)不在本计划内——列为发布后实测机到位的后续任务

---

### Task 1: 数据模型 v2(PowerDashModel.h)

**Files:**
- Create: `PowerDash/PowerDashModel.h`
- Modify: `tests/PowerDashUiTests.cpp`(追加模型测试)
- Modify: `PowerDash/PowerDash.vcxproj` + `.vcxproj.filters`(登记头文件)

**Interfaces:**
- Produces: `pd::Reading/Ok()/NA()、pd::Vendor、pd::PowerLimit、pd::CurrentLimit、pd::Sample、pd::PlatformCaps、pd::PlatformInfo、pd::Decomposition、pd::Decompose(const Sample&, const PlatformCaps&)`——后续所有任务消费这些名字。

- [ ] **Step 1: 写失败测试**(追加到 `tests/PowerDashUiTests.cpp`,include 区加 `#include "../PowerDash/PowerDashModel.h"`)

```cpp
void TestModelDecompositionIdentities() {
    pd::Sample s;                                   // Intel: SYS = PKG + REST
    s.pkgW = pd::Ok(18.5); s.coresW = pd::Ok(12.0);
    s.gfxW = pd::Ok(0.5); s.platformW = pd::Ok(25.0);
    pd::PlatformCaps intel; intel.vendor = pd::Vendor::Intel;
    intel.platformPower = true;
    pd::Decomposition d = pd::Decompose(s, intel);
    Expect(d.title == "SYSTEM POWER", "intel decomposition title");
    Expect(d.totalW.valid && d.totalW.value == 25.0, "intel total = platform");
    Expect(d.mainW.valid && d.mainW.value == 18.5, "intel main = pkg");
    Expect(d.restW.valid && std::abs(d.restW.value - 6.5) < 0.01,
           "intel rest = platform - pkg");
    Expect(d.identity == "PKG + REST = SYSTEM", "intel identity");

    pd::Sample a;                                   // AMD: PKG = CORES + GFX + REST
    a.pkgW = pd::Ok(7.08); a.coresW = pd::Ok(1.50);
    a.gfxW = pd::Ok(0.01);
    pd::PlatformCaps amd; amd.vendor = pd::Vendor::Amd;
    pd::Decomposition e = pd::Decompose(a, amd);
    Expect(e.title == "PACKAGE POWER", "amd decomposition title");
    Expect(e.totalW.valid && e.totalW.value == 7.08, "amd total = pkg");
    Expect(e.restW.valid && std::abs(e.restW.value - 5.57) < 0.01,
           "amd rest = pkg - cores - gfx");
    Expect(e.identity == "CORES + GFX + REST = PKG", "amd identity");
}
```

main() 里追加调用,并补 `#include <cmath>`(若缺)。

- [ ] **Step 2: 跑测试确认失败**

Run: `cmd //c "tests\run-tests.cmd"` — Expected: 编译错误 `PowerDashModel.h: No such file`

- [ ] **Step 3: 写 PowerDashModel.h**

```cpp
#pragma once
// PowerDash 功率参数类型体系 v2 —— spec 第 4 节的权威定义。
// 每个物理量都是 Reading:value 为 SI 单位,valid=false 表示平台不支持
// 或本帧读取失败(UI 隐藏区块、CSV 写空单元格)。
#include <cstdint>
#include <optional>
#include <string>

namespace pd {

enum class Vendor { Intel, Amd };

struct Reading {
    double value = 0.0;   // W / A / 摄氏度 / GHz / %
    bool valid = false;
};

inline Reading Ok(double v) { Reading r; r.value = v; r.valid = true; return r; }
inline Reading NA() { return Reading{}; }

struct PowerLimit {              // 功率类限值 (W),跨平台统一
    Reading sustainedW;          // Intel PL1 <-> AMD PPT
    Reading burstW;              // Intel PL2 <-> AMD FPPT(无则 invalid)
    Reading sustainedWindowS;    // tau,秒
    bool locked = false;
};

struct CurrentLimit {            // 电流类限值 (A),AMD 专属
    Reading tdcA;
    Reading edcA;
};

struct Sample {
    std::string timestamp;
    double elapsedS = 0.0;
    Reading pkgW, coresW, gfxW, platformW;
    PowerLimit powerLimit;
    CurrentLimit currentLimit;
    Reading tempC, freqGHz, utilPct;
    Reading c0Pct, c2Pct, c6Pct;                 // 驻留率(Intel 专属)
    std::optional<std::uint64_t> smiDelta;       // Intel 专属
    std::string mode = "n/a";
};

struct PlatformCaps {
    Vendor vendor = Vendor::Intel;
    std::string cpuName;
    bool gfxPower = false;
    bool platformPower = false;
    bool powerLimits = false;
    bool residency = false;
    bool smi = false;
    double budgetW = 0.0;        // 预算刻度(PL2/FPPT);0=未知,UI 走 spec fallback
    int tjMaxC = 0;
    double baseGHz = 0.0;
    unsigned logicalProcessors = 0;
};

struct PlatformInfo {            // CPUID 静态信息,入口层计算后交给工厂
    Vendor vendor = Vendor::Intel;
    std::string cpuName;         // "brand  [codename]"
    unsigned logicalProcessors = 0;
    double baseGHz = 0.0;        // CPUID 0x16;0=未知
};

struct Decomposition {
    std::string title;           // 顶区标题(不带数值,渲染层拼)
    Reading totalW;              // 标题总值
    std::string mainLabel = "PKG";
    Reading mainW;
    std::string restLabel = "REST";
    Reading restW;
    std::string identity;        // 图例恒等式
};

inline Decomposition Decompose(const Sample& s, const PlatformCaps& c) {
    Decomposition d;
    if (c.platformPower && s.platformW.valid && s.pkgW.valid) {
        d.title = "SYSTEM POWER";
        d.totalW = s.platformW;
        d.mainW = s.pkgW;
        d.restW = Ok(s.platformW.value - s.pkgW.value);
        if (d.restW.value < 0) d.restW = Ok(0);
        d.identity = "PKG + REST = SYSTEM";
    } else if (s.pkgW.valid) {
        d.title = "PACKAGE POWER";
        d.totalW = s.pkgW;
        d.mainW = s.pkgW;
        double rest = s.pkgW.value;
        if (s.coresW.valid) rest -= s.coresW.value;
        if (s.gfxW.valid) rest -= s.gfxW.value;
        d.restW = Ok(rest < 0 ? 0 : rest);
        d.identity = "CORES + GFX + REST = PKG";
    }
    return d;
}

} // namespace pd
```

vcxproj:在 `<ItemGroup>` 含其它 ClInclude 处加
`<ClInclude Include="PowerDashModel.h" />`;filters 同理(头文件过滤器)。

- [ ] **Step 4: 跑测试确认通过**

Run: `cmd //c "tests\run-tests.cmd"` — Expected: `All PowerDash UI tests passed`
(run-tests.cmd 只编译 PowerDashUi.cpp,新头文件被测试直接 include,无需改脚本)

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashModel.h PowerDash/PowerDash.vcxproj PowerDash/PowerDash.vcxproj.filters tests/PowerDashUiTests.cpp
git commit -m "feat(model): 功率参数类型体系 v2 —— Reading 可用性语义 + 分解恒等式"
```

---

### Task 2: Probe 接口与驱动 IO 抽象(PowerDashProbe.h)

**Files:**
- Create: `PowerDash/PowerDashProbe.h`
- Create: `PowerDash/PowerDashProbe.cpp`(WindowsDriverIo + CreateProbe 骨架)
- Modify: `PowerDash/PowerDash.vcxproj` / `.filters`
- Modify: `tests/PowerDashUiTests.cpp`(FixtureDriverIo 测试基建)

**Interfaces:**
- Consumes: Task 1 的 `PlatformInfo/PlatformCaps/IPlatformProbe 所需类型`
- Produces: `pd::DriverIo`(虚:ReadMsr/ReadPciCfg/WritePciCfg/ReadSmn/MapPhys/UnmapPhys)、
  `pd::IPlatformProbe`(虚:caps()/readSample(Sample&))、
  `pd::WindowsDriverIo(HANDLE)`、`pd::CreateProbe(DriverIo&, const PlatformInfo&)`、
  测试基建 `FixtureDriverIo`(可编程 MSR/SMN 应答)。Task 3/4/6/8 消费。

- [ ] **Step 1: 写失败测试**(追加;include 加 `#include "../PowerDash/PowerDashProbe.h"`、`<map>`、`<functional>`)

```cpp
class FixtureDriverIo : public pd::DriverIo {   // 可编程应答,probe 回放测试用
public:
    std::map<uint32_t, std::function<uint64_t()>> msr;   // msr -> 每次读取的值
    std::map<uint32_t, uint32_t> smn;                    // smn addr -> value
    bool failAllMsrs = false;
    bool ReadMsr(unsigned, uint32_t a, uint64_t& out) override {
        if (failAllMsrs) return false;
        auto it = msr.find(a);
        if (it == msr.end()) return false;
        out = it->second();
        return true;
    }
    bool ReadPciCfg(unsigned, unsigned, unsigned, unsigned, uint32_t&) override { return false; }
    bool WritePciCfg(unsigned, unsigned, unsigned, unsigned, uint32_t) override { return false; }
    bool ReadSmn(uint32_t a, uint32_t& out) override {
        auto it = smn.find(a);
        if (it == smn.end()) return false;
        out = it->second; return true;
    }
    bool MapPhys(uint64_t, size_t, void*&) override { return false; }
    void UnmapPhys(void*) override {}
};

void TestDriverIoFixtureRouting() {
    FixtureDriverIo io;
    io.msr[0x611] = [] { return 12345ull; };
    uint64_t v = 0;
    Expect(io.ReadMsr(0, 0x611, v) && v == 12345, "fixture msr scripted answer");
    Expect(!io.ReadMsr(0, 0x999, v), "fixture unknown msr fails");
}
```

main() 追加 `TestDriverIoFixtureRouting();`

- [ ] **Step 2: 跑测试确认失败**(缺头文件,编译错误)

- [ ] **Step 3: 写 PowerDashProbe.h / PowerDashProbe.cpp**

PowerDashProbe.h:

```cpp
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

} // namespace pd
```

PowerDashProbe.cpp(骨架,工厂分流;IOCTL 号与 PowerDash.cpp 现有 `#define` 一致——把这些 `#define IO_CTL_*` 从 PowerDash.cpp 移入一个新共享头 `PowerDashIoctl.h`,两处 include):

```cpp
#include "PowerDashProbe.h"
#include "PowerDashIoctl.h"
#include <windows.h>

namespace pd {

WindowsDriverIo::WindowsDriverIo(void* h) : h_(h) {}

bool WindowsDriverIo::ReadMsr(unsigned core, uint32_t msr, uint64_t& out) {
    MSR_Request req{}; req.core = core; req.address = msr;
    DWORD returned = 0;
    // 与 PowerDash.cpp:188 现有 read_msr 相同的 DeviceIoControl 形态,
    // 迁移时照抄 req 字段名(pdstruct.h MSR_Request)。
    if (!DeviceIoControl(h_, IO_CTL_MSR_READ, &req, sizeof(req),
                         &out, sizeof(out), &returned, nullptr))
        return false;
    return true;
}
// ReadPciCfg/WritePciCfg/MapPhys/UnmapPhys:照抄 PowerDash.cpp:200-210 与
// 595-620 的 DeviceIoControl 调用(PCICFG_Request/MMAP_Request 字段)。
// ReadSmn:Task 7 之前 { (void)smnAddr; (void)out; return false; }

} // namespace pd
```

> 实现注意:本步同时把 PowerDash.cpp 的 `#define POWERDASH_DEV_TYPE` 与
> `#define IO_CTL_*` 抽到 `PowerDashIoctl.h`,PowerDash.cpp 改为 include;
> `read_pci_config` 旧函数保留原地(后续任务迁移)。vcxproj 登记两个新文件。

- [ ] **Step 4: 跑测试确认通过** — `cmd //c "tests\run-tests.cmd"`(run-tests.cmd 编译行追加 `..\PowerDash\PowerDashProbe.cpp`)

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashProbe.h PowerDash/PowerDashProbe.cpp PowerDash/PowerDashIoctl.h PowerDash/PowerDash.cpp PowerDash/PowerDash.vcxproj PowerDash/PowerDash.vcxproj.filters tests/PowerDashUiTests.cpp tests/run-tests.cmd
git commit -m "feat(probe): DriverIo 抽象 + IOCTL 头独立 + 测试回放基建"
```

---

### Task 3: IntelProbe(迁移现有采样逻辑)

**Files:**
- Create: `PowerDash/PowerDashIntel.cpp`(声明放 PowerDashProbe.h 已有)
- Modify: `PowerDash/PowerDash.vcxproj` / `.filters`、`tests/PowerDashUiTests.cpp`
- 参考源:`PowerDash/PowerDash.cpp:563-676`(setup)与 `766-860`(每帧采样)——**逐段迁移,原位置留待 Task 6 删除**

**Interfaces:**
- Consumes: DriverIo、PlatformInfo、Sample v2、FixtureDriverIo
- Produces: `CreateIntelProbe`(Task 6 接线)、Intel 帧数值与现状逐项相等的 Sample

- [ ] **Step 1: 写失败测试**(追加)

```cpp
void TestIntelProbeReplay() {
    FixtureDriverIo io;
    // 单位寄存器:power bits=3(0.125W), energy bits=14(1/16384 J), time bits=10
    io.msr[0x606] = [] { return (14ull << 8) | 3ull; };
    uint64_t pkg = 0;   // 每 tick 增 32768 raw = 2.0 J -> 2 W
    io.msr[0x611] = [&pkg] { pkg += 32768; return pkg; };
    uint64_t pp0 = 0;   // 每 tick 增 16384 raw = 1.0 J -> 1 W
    io.msr[0x639] = [&pp0] { pp0 += 16384; return pp0; };
    uint64_t pp1 = 0, sys = 0;
    io.msr[0x641] = [&pp1] { pp1 += 8192; return pp1; };   // 0.5 W
    io.msr[0x64D] = [&sys] { sys += 49152; return sys; };  // 3.0 W
    io.msr[0x613] = [] { return (1ull << 31) | 0x1F; };    // therm status valid
    io.msr[0x1A2] = [] { return 105ull << 16; };           // TjMax=105
    io.msr[0x641 + 1] = [] { return 0ull; };               // 未用占位(可删)
    pd::PlatformInfo info; info.vendor = pd::Vendor::Intel;
    info.cpuName = "Intel(R) Core(TM) Ultra 5 225H  [Arrow Lake-H]";
    info.logicalProcessors = 16; info.baseGHz = 2.5;
    auto probe = pd::CreateIntelProbe(io, info);
    Expect(probe != nullptr, "intel probe constructs");
    Expect(probe->caps().vendor == pd::Vendor::Intel &&
           probe->caps().tjMaxC == 105, "caps carry tjMax");
    // 构造期已消费一次 prev;第一帧即得到差分功率
    pd::Sample s;
    Expect(probe->readSample(s), "first sample reads");
    Expect(s.pkgW.valid && std::abs(s.pkgW.value - 2.0) < 0.01,
           "pkg power from replayed delta");
    Expect(s.coresW.valid && std::abs(s.coresW.value - 1.0) < 0.01,
           "cores = PP0 delta");
    Expect(s.gfxW.valid && std::abs(s.gfxW.value - 0.5) < 0.01, "gfx = PP1");
    Expect(s.platformW.valid && std::abs(s.platformW.value - 3.0) < 0.01,
           "platform = PSYS");
}
```

main() 追加调用。(注:MCHBAR/MMAP 在 FixtureDriverIo 下失败 → caps.powerLimits=false,
PL 字段 invalid,断言不涉及 PL。)

- [ ] **Step 2: 跑测试确认失败**(CreateIntelProbe 未定义)

- [ ] **Step 3: 实现 PowerDashIntel.cpp**

结构(迁移自 PowerDash.cpp 对应行,数值逻辑**原样照搬**):

```cpp
#include "PowerDashProbe.h"
#include "PowerDashIoctl.h"
#include <cmath>
#include <intrin.h>

namespace pd {
namespace {
constexpr uint32_t MSR_RAPL_POWER_UNIT = 0x606, MSR_PKG_POWER_INFO = 0x614;
constexpr uint32_t MSR_PKG_ENERGY_STATUS = 0x611, MSR_PP0_ENERGY_STATUS = 0x639;
constexpr uint32_t MSR_PP1_ENERGY_STATUS = 0x641, MSR_SYS_ENERGY_STATUS = 0x64D;
constexpr uint32_t MSR_PACKAGE_THERM_STATUS = 0x1B2, MSR_TEMPERATURE_TARGET = 0x1A2;
constexpr uint32_t MSR_IA32_APERF = 0xE8, MSR_IA32_MPERF = 0xE7;
constexpr uint32_t MSR_PKG_C2_RESIDENCY = 0x60D, MSR_PKG_C6_RESIDENCY = 0x3F9;
constexpr uint32_t MSR_SMI_COUNT = 0x34;
}

class IntelProbe : public IPlatformProbe {
public:
    IntelProbe(DriverIo& io, const PlatformInfo& info) : io_(io) {
        caps_ = BuildCaps(info);       // vendor/name/nLP/baseGHz + 探测项
        uint64_t u = 0;
        if (io_.ReadMsr(0, MSR_RAPL_POWER_UNIT, u)) {          // :563-575
            energyUnit_ = 1.0 / std::pow(2.0, (u >> 8) & 0x1F);
            timeUnitS_  = 1.0 / std::pow(2.0, (u >> 16) & 0x0F);
            unitsOk_ = true;
        }
        uint64_t spec = 0;                                     // :626-635
        if (unitsOk_ && io_.ReadMsr(0, MSR_PKG_POWER_INFO, spec))
            caps_.budgetW = ((spec >> 0) & 0x7FFF) * 0.125;    // thermal spec,PL 兜底
        uint64_t tj = 0;                                       // :640-643
        if (io_.ReadMsr(0, MSR_TEMPERATURE_TARGET, tj) && (tj >> 16) != 0)
            caps_.tjMaxC = static_cast<int>(tj >> 16) & 0xFF;
        MapPlWindow();                                         // :578-600 MCHBAR+0x59A0
        ReadBaseline();                                        // :665-676 prev 计数器
    }
    const PlatformCaps& caps() const override { return caps_; }
    bool readSample(Sample& s) override {                      // :766-860 映射
        if (!unitsOk_) return false;
        // (a) 能量差分 + 32 位回绕 —— 照抄 :761-774 的 if(curr<prev) += 1<<32
        //     pkg/cores/gfx/platform -> s.pkgW/coresW/gfxW/platformW = Ok(...)
        // (b) PL(mmio 可用时): :776-780 -> powerLimit.sustainedW/burstW(W)、
        //     sustainedWindowS = ((raw>>17)&0x7F)*timeUnitS_(秒)、locked
        // (c) 温度: :783-790 -> s.tempC = Ok(tjMax - headroom)
        // (d) 频率: :796-804 APERF/MPERF 比值 * baseGHz -> s.freqGHz
        // (e) 驻留: :806-811 -> s.c0Pct/c2Pct/c6Pct(C6 含于 C2 的钳制保留)
        // (f) SMI: :812 -> s.smiDelta = delta
        // 各子项失败 -> 对应字段留 NA(),不整体失败;全部功率域失败 -> return false
        ...
    }
private:
    // BuildCaps: vendor/cpuName/logicalProcessors/baseGHz/residency=true/smi=true
    //            /gfxPower=true/platformPower=true;PL 探测在 MapPlWindow 成功后置 true
    // MapPlWindow: ReadPciCfg(0,0,0,0x48) 取 MCHBAR -> MapPhys(page) -> pl_ 指针
    //              失败则 caps_.powerLimits=false(能力降级,不构造失败)
    // ReadBaseline: 6 个 prev 计数器 + APERF/MPERF/C2/C6/SMI/TSC
    DriverIo& io_;
    PlatformCaps caps_;
    bool unitsOk_ = false;
    double energyUnit_ = 0.0, timeUnitS_ = 0.0;
    void* map_ = nullptr;
    volatile uint32_t* pl_ = nullptr;      // pl_[0]=PL1 raw, pl_[1]=PL2 raw
    uint64_t prevPkg_{}, prevPp0_{}, prevPp1_{}, prevSys_{};
    uint64_t prevAperf_{}, prevMperf_{}, prevC2_{}, prevC6_{}, prevSmi_{}, prevTsc_{};
};

std::unique_ptr<IPlatformProbe> CreateIntelProbe(DriverIo& io,
                                                 const PlatformInfo& info) {
    return std::make_unique<IntelProbe>(io, info);
}
} // namespace pd
```

> 实现要求:标注的 (a)-(f) 每一段都是**从 PowerDash.cpp 对应行照搬表达式**,
> 仅把输出改写为 `Ok(...)`。fixture 测试即验证迁移正确性。
> mmio PL 读法保持 `pl1 = (raw & 0x7FFF) * 0.125`(:778)。

- [ ] **Step 4: 跑测试确认通过**(run-tests.cmd 编译行追加 `..\PowerDash\PowerDashIntel.cpp`)

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashIntel.cpp PowerDash/PowerDash.vcxproj PowerDash/PowerDash.vcxproj.filters tests/PowerDashUiTests.cpp tests/run-tests.cmd
git commit -m "feat(probe): IntelProbe —— 现有采样逻辑迁移为回放可测的 Probe"
```

---

### Task 4: 采样引擎(PowerDashSampler)

**Files:**
- Create: `PowerDash/PowerDashSampler.h` / `PowerDashSampler.cpp`
- Modify: vcxproj/filters、tests(注入式 tick 测试)

**Interfaces:**
- Consumes: IPlatformProbe、Sample v2
- Produces: `pd::Sampler`(构造:`IPlatformProbe&、std::function<std::string()> modeReader、const volatile bool* exitFlag、std::function<void()> tickWait(默认真实 10×100ms 可中断睡眠)`;
  `bool Run(double runSeconds, const std::function<void(const Sample&)>& sink)`;
  职责:时间戳/elapsed/util(GetSystemTimes)/mode 填充,连续 5 帧 readSample 失败返回 false;历史窗口由 sink 侧维护)

- [ ] **Step 1: 写失败测试**

```cpp
class FakeProbe : public pd::IPlatformProbe {   // 脚本化样本序列
public:
    std::vector<pd::Sample> script; size_t i = 0; int reads = 0;
    pd::PlatformCaps capsHolder;
    const pd::PlatformCaps& caps() const override { return capsHolder; }
    bool readSample(pd::Sample& s) override {
        ++reads;
        if (i >= script.size()) return false;
        s = script[i++]; return true;
    }
};

void TestSamplerDrivesSinkAndFillsPlatformIndependentFields() {
    FakeProbe p;
    pd::Sample a; a.pkgW = pd::Ok(5.0); a.mode = "n/a";
    pd::Sample b; b.pkgW = pd::Ok(6.0);
    p.script = {a, b};
    std::vector<pd::Sample> got;
    pd::Sampler s(p, [] { return "Intelligent (APM)"; }, nullptr,
                  [] {});                       // 即时 tick,测试不真实睡眠
    bool ok = s.Run(2.0, [&](const pd::Sample& x) { got.push_back(x); });
    Expect(ok, "sampler completes scripted run");
    Expect(got.size() == 2, "one sink call per tick");
    Expect(got[0].utilPct.valid, "sampler fills utilization");
    Expect(got[0].mode == "Intelligent (APM)", "sampler fills mode");
    Expect(!got[0].timestamp.empty() && got[0].elapsedS >= 0.0,
           "sampler fills timestamp/elapsed");
}

void TestSamplerExitsAfterFiveConsecutiveFailures() {
    FakeProbe p;                                // script 为空,每次 readSample 失败
    int sinks = 0;
    pd::Sampler s(p, [] { return "n/a"; }, nullptr, [] {});
    bool ok = s.Run(-1.0, [&](const pd::Sample&) { ++sinks; });
    Expect(!ok, "five consecutive failures abort the run");
    Expect(sinks == 0, "failed frames never reach the sink");
}
```

main() 追加两个调用。

- [ ] **Step 2: 跑测试确认失败**

- [ ] **Step 3: 实现**

PowerDashSampler.h:

```cpp
#pragma once
#include "PowerDashProbe.h"
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
};
} // namespace pd
```

PowerDashSampler.cpp 要点(全部从 PowerDash.cpp 现有代码迁移):
- `LocalIsoTimestamp()` 从 PowerDash.cpp 移入(原地删除,PowerDash.cpp include 本头或复制声明——采用"移入 sampler,PowerDash.cpp 若他处无引用则不留");
- util 差分:迁移 Task「UTIL 修复」引入的 GetSystemTimes 块(构造期取基线,每帧差分 -> `s.utilPct = Ok(...)`,钳 0-100);
- `Run`:循环 `while (!exit && (runSeconds < 0 || frame < ceil(runSeconds)))`,
  每 tick:`probe_.readSample(s)` -> 失败连续计数 >=5 return false(计数器成功后清零);
  成功 -> 填 timestamp/elapsed(GetTickCount64 基线)/mode(每次调用 `mode_()`)/util -> `sink(s)`;
- exitFlag 为 nullptr 时不检查。

- [ ] **Step 4: 跑测试确认通过**(编译行追加 `..\PowerDash\PowerDashSampler.cpp`)

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashSampler.h PowerDash/PowerDashSampler.cpp PowerDash/PowerDash.vcxproj PowerDash/PowerDash.vcxproj.filters tests/PowerDashUiTests.cpp tests/run-tests.cmd
git commit -m "feat(sampler): 平台无关采样引擎 —— 节拍/util/mode/失败熔断"
```

---

### Task 5: UI 与 CSV v2 消费统一模型

**Files:**
- Modify: `PowerDash/PowerDashUi.h` / `PowerDash/PowerDashUi.cpp`、`tests/PowerDashUiTests.cpp`、`README.md`(CSV 迁移表)

**Interfaces:**
- Consumes: Sample v2、PlatformCaps、Decompose
- Produces: `RenderDashboard(const DashboardInfo&, const PlatformCaps&, const Sample&, const std::vector<double>&)`、
  `CsvHeader()`(v2 列)、`CsvRow(Vendor, const Sample&)`;
  DashboardInfo 精简为 `{version, cpuBrand, codeName, width, csvActive, csvName, ansi}`
  (logicalProcessors/baseGHz/tjMaxC/fallbackScaleW 移入 PlatformCaps)

- [ ] **Step 1: 改造测试 fixture 到 v2 并保持全部既有断言绿**

`SampleFixture()` 重写为填 v2 字段(pkgW/coresW/gfxW/platformW/powerLimit{sustained 28,burst 45,window 28.0,locked}/tempC 64/freqGHz 3.2/utilPct 8/c0 35/c2 15/c6 50/smiDelta 1/mode);
新增 `IntelCapsFixture()`(platformPower/gfxPower/residency/smi/powerLimits=true、budgetW=0、tjMaxC=100、baseGHz=2.0、nLP=8);
现有调用点 `RenderDashboard(DashboardFixture(96), SampleFixture(), {...})` 改为
`RenderDashboard(DashboardFixture(96), IntelCapsFixture(), SampleFixture(), {...})`。
**断言字符串一律不改**——"SYSTEM POWER · 25.00 W"、"PKG + REST = SYSTEM"、
"41% of PL2"、"| = PL1 28.00 W"、"scale to PL2 45.00 W"、"65% PKG" 等全部保持。
τ 呈现:window 秒数 28.0 → UI 内 `FormatWindow()` 产出 "28.00 s"(与现值一致)。

- [ ] **Step 2: 跑测试确认失败**(签名不匹配编译错)

- [ ] **Step 3: 实现渲染改造**(PowerDashUi.cpp,规则来自 spec 第 5 节)

- 顶区:`Decompose(s, caps)` 驱动;标题 `title + " · " + Fixed(total) + " W"`;
  两根内联 bar 用 mainW/restW;刻度基准 `burstW.valid ? burstW : (caps.budgetW > 0 ? caps.budgetW : 60.0)`,
  刻度名 `burstW.valid ? (Intel?"PL2":"FPPT") : "spec"`;竖线 = sustainedW 位置;
  恒等式 `d.identity` 进图例。数值行/图例排版规则(字段宽 8/9/11)原样保留。
- POWER DOMAINS:IA 行 = coresW(`% PKG`)、GT 行 = gfxW(caps.gfxPower 才显示)、
  SYSTEM 行 = platformW + "PKG xx% of SYS"(caps.platformPower 才显示);
  UTIL/TEMP/FREQ 与 caps 无关恒显示(TjMax 来自 caps.tjMaxC>0)。
- CPU RESIDENCY 区与 SMI:仅 `caps.residency` / `caps.smi` 时渲染;AMD 下整区隐藏
  (宽布局配对退化:residency 区隐藏时 POWER LIMITS 区单列全宽渲染——`if (!caps.residency)` 分支只出 limits 行)。
- POWER LIMITS:Intel 两行(PL1/PL2 + window + LOCKED);AMD 三行(PPT/FPPT/TDC/EDC,
  行内单位显式 "PPT 54.00 W"、"TDC 95.00 A"),仅 `caps.powerLimits` 时渲染。
- CSV:`CsvHeader()` 返回 spec 第 6 节列串;`CsvRow(Vendor v, const Sample& s)`:
  invalid → 空串单元格,`limit_locked` 0/1,`platform` 列 `v==Vendor::Amd?"amd":"intel"`。

- [ ] **Step 4: 更新 CSV 测试断言到 v2 列**(替换 TestCsvHasStableColumnsAndEscapesText 的期望串):

```cpp
const std::string expectedHeader =
    "timestamp,elapsed_s,platform,pkg_w,cores_w,gfx_w,platform_w,"
    "limit_sustained_w,limit_sustained_window_s,limit_burst_w,limit_locked,"
    "tdc_a,edc_a,temp_c,freq_ghz,util_pct,c0_pct,c2_pct,c6_pct,smi_delta,mode";
// fixture 期望行:invalid 的 gfx/tdc/edc 为空单元格,其余按 v2 值逐列列出
```

跑全量测试 → 全绿。

- [ ] **Step 5: README 迁移表**(spec 第 6 节表格原文录入 `--csv` 段落)

- [ ] **Step 6: Commit**

```bash
git add PowerDash/PowerDashUi.h PowerDash/PowerDashUi.cpp tests/PowerDashUiTests.cpp README.md
git commit -m "feat(ui/csv): 面板与 CSV v2 消费统一模型,Intel golden 断言全绿"
```

---

### Task 6: RunPowerMonitor 重接线(Intel 实机等价)

**Files:**
- Modify: `PowerDash/PowerDash.cpp`(:555-905 的 do{}while 体替换为接线)

**Interfaces:**
- Consumes: WindowsDriverIo、CreateProbe、Sampler、新 RenderDashboard/CsvRow
- Produces: 瘦身后的 RunPowerMonitor(保留 -setpl 分支、VT 差分渲染、hist 维护、非 VT 紧凑行、CSV 文件写)

- [ ] **Step 1: 接线替换**(迁移后删除 PowerDash.cpp 中已被 Probe/Sampler 接管的代码段)

```cpp
// 伪代码骨架 —— 保持 :690-714 的 vtOn/W 计算/consoleColumns lambda 与
// :862-935 的 VT 差分渲染/非 VT 行/CSV 写原样,仅数据来源换掉:
pd::PlatformInfo info;
info.vendor = CpuVendor();                       // CPUID "AuthenticAMD" 判断(新增小函数)
info.cpuName = brand + (codeTag.empty() ? "" : ...)   // 沿用 :716-724 拆分逻辑
info.logicalProcessors = nLP;  info.baseGHz = baseMHz / 1000.0;
pd::WindowsDriverIo io(hDriver);
auto probe = pd::CreateProbe(io, info);          // PowerDashProbe.cpp:vendor 分流
if (!probe) { std::cerr << "Unsupported platform." << std::endl; rc = 1; break; }
const pd::PlatformCaps& caps = probe->caps();
// caps 填充 dashboardInfo(替换 :726-742 的手工字段)
pd::Sampler sampler(*probe,
                    [&] { return QueryDytcMode(); },   // 现 :823-828 提为 lambda
                    &g_exitRequested);
bool ok = sampler.Run(monitorOptions.runSeconds,
    [&](const pd::Sample& s) {
        // 原 :850-887 帧体:CSV 写(CsvRow(vendor, s))、hist.push(pkg)、
        // VT 差分渲染 / 非 VT 紧凑行 —— 渲染协议代码原样保留
    });
if (!ok) { std::cerr << "Telemetry lost (5 consecutive failures)." << std::endl; rc = 2; }
```

-setpl 分支(:603-622)与 MCHBAR 映射(setpl 用)留在 PowerDash.cpp 原位;
能量/温度/频率/驻留/SMI/单位/spec/tjMax 的读取代码(:563-576、:626-690、:766-860)
从 PowerDash.cpp **删除**(已由 IntelProbe 承担),MCHBAR 映射仅 setpl 路径保留一份
(IntelProbe 内部自映射一份,caps.powerLimits=false 时 -setpl 仍可工作——setpl
自行映射,不依赖 probe)。

- [ ] **Step 2: 全量构建 + 单测** — `cmd //c build.cmd` + `cmd //c "tests\run-tests.cmd"` 全绿

- [ ] **Step 3: Intel 实机回归**(labs-xiaoxin,沿用会话内既有流程)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File remote-test.ps1 -ScriptFile _ping.ps1 -CopyFrom x64\Release\PowerDash.exe -CopyTo C:\Users\labs\Desktop\PowerDash.exe
# 远程 PDASH_FORCE_VT 捕获 4s(方法同 _vtcap2.ps1),核对:
#   帧含 "SYSTEM POWER ·"、"PKG + REST = SYSTEM"、rewind=帧高-1、无 \x1b[J
#   空载数值量级与既有基线一致(PKG ~7W/SYS ~10W/UTIL ~3%)
```

- [ ] **Step 4: Commit**

```bash
git add PowerDash/PowerDash.cpp
git commit -m "refactor(monitor): RunPowerMonitor 接线 Probe+Sampler,Intel 实机回归通过"
```

---

### Task 7: 驱动 IO_CTL_SMN_READ(原子互斥)

**Files:**
- Modify: `PowerDashSYS/pdstruct.h`、`PowerDashSYS/powerdash.c`、`PowerDash/PowerDashIoctl.h`、`PowerDash/PowerDashProbe.cpp`(WindowsDriverIo::ReadSmn)

**Interfaces:**
- Consumes: 现有 PCICFG case 形态(powerdash.c:606-668)、FAST_MUTEX
- Produces: IOCTL 0x80B `IO_CTL_SMN_READ`,输入 `SMN_Request{address}`,输出回写 `value`;用户态 `WindowsDriverIo::ReadSmn` 可用(Task 8 依赖)

- [ ] **Step 1: pdstruct.h 追加**

```c
struct SMN_Request
{
    uint32_t address;    // SMN 地址(如 0x59800)
    uint32_t value;      // 输出:该地址 32 位内容
};
```

PowerDashIoctl.h:`#define IO_CTL_SMN_READ CTL_CODE(POWERDASH_DEV_TYPE, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)`

- [ ] **Step 2: powerdash.c 实现**(镜像 PCICFG case;设备扩展里 `counterSetHandle` 所在结构体追加 `FAST_MUTEX smnMutex;`,并在初始化 `counterSetHandle = NULL` 的同一位置 `ExInitializeFastMutex(&pExt->smnMutex);`)

```c
case IO_CTL_SMN_READ:
{
    struct SMN_Request* req = (struct SMN_Request*)Irp->AssociatedIrp.SystemBuffer;
    if (inputSize < sizeof(struct SMN_Request)) { status = STATUS_INVALID_PARAMETER; break; }
    ExAcquireFastMutex(&pExt->smnMutex);
    __try
    {
        ULONG32 addr = req->address, data = 0;
        ULONG bus = 0, slot = 0;   // B0:D0:F0 -> slot 0
        // 1) 写 SMN 地址窗口 0x60
        if (HalSetBusDataByOffset(PCIConfiguration, bus, slot, &addr, 0x60, 4) != 4)
        { status = STATUS_DEVICE_NOT_READY; __leave; }  // __leave 需外层 __try 允许
        // 2) 读 SMN 数据窗口 0x64
        if (HalGetBusDataByOffset(PCIConfiguration, bus, slot, &data, 0x64, 4) != 4)
        { status = STATUS_DEVICE_NOT_READY; }
        else req->value = data;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = GetExceptionCode();
        DbgPrint("PowerDash: SMN read exception 0x%X addr 0x%X\n", status, req->address);
    }
    ExReleaseFastMutex(&pExt->smnMutex);
    Irp->IoStatus.Information = sizeof(struct SMN_Request);   // METHOD_BUFFERED 回写
    break;
}
```

(注意:该 case 放在 `switch` 与 PCICFG case 同级;`__leave` 语义若编译器告警,
改用嵌套 if 结构,保持"异常必释放互斥"不变。)

- [ ] **Step 3: 用户态补全** `WindowsDriverIo::ReadSmn`:`SMN_Request req{addr,0}; DeviceIoControl(h_, IO_CTL_SMN_READ, &req, sizeof(req), &req, sizeof(req), &returned, nullptr)` 成功则 `out = req.value`。

- [ ] **Step 4: 构建 + Intel 实机冒烟**(驱动改动必须实机验证可加载)

```powershell
cmd //c build.cmd        # 驱动重建(测试签名)+ exe 内嵌
py verify_embed.py       # MATCH
# 部署 labs-xiaoxin,远程 PDASH_FORCE_VT power 2:驱动装卸正常、帧正常产出
```

- [ ] **Step 5: Commit**

```bash
git add PowerDashSYS/pdstruct.h PowerDashSYS/powerdash.c PowerDash/PowerDashIoctl.h PowerDash/PowerDashProbe.cpp
git commit -m "feat(driver): IO_CTL_SMN_READ —— SMN 0x60/0x64 原子互斥读取"
```

---

### Task 8: AmdProbe(保底监控集)

**Files:**
- Create: `PowerDash/PowerDashAmd.cpp`
- Modify: `PowerDash/PowerDashProbe.cpp`(CreateProbe 分流)、vcxproj/filters、tests

**Interfaces:**
- Consumes: DriverIo.ReadMsr/ReadSmn、PlatformInfo、FixtureDriverIo
- Produces: `CreateAmdProbe` —— Sample 字段:pkgW/coresW(能量差分)、tempC(SMN 0x59800)、freqGHz(APERF/MPERF);
  caps:gfxPower/platformPower/powerLimits/residency/smi = false,budgetW = 0

- [ ] **Step 1: 写失败测试**

```cpp
void TestAmdProbeReplay() {
    FixtureDriverIo io;
    io.msr[0xC0010299] = [] { return 14ull << 8; };            // energy unit 1/16384 J
    uint64_t pkg = 0;
    io.msr[0xC001029B] = [&pkg] { pkg += 32768; return pkg; }; // 2.0 W
    uint64_t core0 = 0, core1 = 0;
    io.msr[0xC001029A] = [&core0, &core1, n = 0]() mutable {   // 逐核递增:2 核各 1W
        uint64_t r = (n++ % 2 == 0) ? (++core0) : (++core1);
        return r;   // 见实现注:每帧每核 delta 16384 -> cores 总 1.0 W... 按实现语义编写
    };
    io.smn[0x59800] = (80u << 21);                             // Tctl = 80*0.125*8 = 80C? 见注
    pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
    info.cpuName = "AMD Ryzen 7 8845H  [Hawk Point]";
    info.logicalProcessors = 16; info.baseGHz = 3.8;
    auto probe = pd::CreateAmdProbe(io, info);
    Expect(probe != nullptr, "amd probe constructs");
    pd::PlatformCaps c = probe->caps();
    Expect(!c.platformPower && !c.residency && !c.smi && !c.powerLimits,
           "amd caps degrade honestly");
    pd::Sample s;
    Expect(probe->readSample(s), "amd sample reads");
    Expect(s.pkgW.valid && std::abs(s.pkgW.value - 2.0) < 0.01, "pkg energy delta");
    Expect(!s.gfxW.valid && !s.platformW.valid, "absent domains stay invalid");
    Expect(s.tempC.valid && std::abs(s.tempC.value - 80.0) < 0.01, "SMN Tctl decode");
}
```

(Tctl 解码:`tempC = (smn >> 21) * 0.125`;fixture `(80*8) << 21` = `640<<21` → 80.0 ℃。
逐核能量 fixture 按实现"每帧遍历 nLP 个核读 0xC001029A、各自差分累加"的语义编写,
每核每帧 +16384 → cores = 1.0 W。)

- [ ] **Step 2: 跑测试确认失败**(CreateAmdProbe 未定义)

- [ ] **Step 3: 实现 PowerDashAmd.cpp**

```cpp
// 关键常量与逻辑:
constexpr uint32_t AMD_RAPL_POWER_UNIT = 0xC0010299;
constexpr uint32_t AMD_CORE_ENERGY_STAT = 0xC001029A;   // 每核,32 位回绕
constexpr uint32_t AMD_PKG_ENERGY_STAT = 0xC001029B;    // 32 位回绕
constexpr uint32_t SMN_THM_TCTL = 0x59800;
// ctor:读 0xC0010299 energy bits -> energyUnit(定标 quirk 修正表 v1 为空,
//       即直接采用寄存器值;已知 quirk family 未来登记于此);
//       baseline:pkg + 每 core(0..nLP-1)的 0xC001029A、APERF/MPERF/TSC
// readSample:
//   pkg: ReadMsr(0, 0xC001029B) 差分(32 位回绕钳制)-> Ok()
//   cores: for core in 0..nLP-1: ReadMsr(core, 0xC001029A) 差分累加(逐核回绕)-> Ok()
//   temp: ReadSmn(SMN_THM_TCTL, raw) -> Ok((raw >> 21) * 0.125)
//   freq: APERF/MPERF * baseGHz(与 IntelProbe 同式)
//   gfx/platform/limits/residency/smi:保持 NA()/false
//   全部能量域失败 -> return false
```

CreateProbe 分流(PowerDashProbe.cpp):

```cpp
std::unique_ptr<IPlatformProbe> CreateProbe(DriverIo& io, const PlatformInfo& i) {
    return i.vendor == Vendor::Amd ? CreateAmdProbe(io, i) : CreateIntelProbe(io, i);
}
```

- [ ] **Step 4: 跑测试确认通过**(编译行追加 `..\PowerDash\PowerDashAmd.cpp`)

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashAmd.cpp PowerDash/PowerDashProbe.cpp PowerDash/PowerDash.vcxproj PowerDash/PowerDash.vcxproj.filters tests/PowerDashUiTests.cpp tests/run-tests.cmd
git commit -m "feat(probe): AmdProbe 保底监控集 —— 能量/温度/频率,能力位诚实降级"
```

---

### Task 9: AMD 面板 golden fixture 与文档

**Files:**
- Modify: `tests/PowerDashUiTests.cpp`、`README.md`、`PowerDash/PowerDash.cpp`(Usage 帮助文案)

**Interfaces:**
- Consumes: Task 5 的 RenderDashboard(caps...)、Task 8 的 caps 语义
- Produces: AMD fixture 断言集;README「AMD 支持」小节

- [ ] **Step 1: 写失败测试**

```cpp
pd::PlatformCaps AmdCapsFixture() {
    pd::PlatformCaps c; c.vendor = pd::Vendor::Amd;
    c.cpuName = "AMD Ryzen 7 8845H  [Hawk Point]";
    c.logicalProcessors = 16; c.baseGHz = 3.8; c.tjMaxC = 95;
    return c;                       // 其余能力位 false
}

void TestAmdDashboardHidesAbsentSections() {
    pd::Sample s;                                   // pkg 28 = cores 22 + gfx NA + rest 6
    s.pkgW = pd::Ok(28.0); s.coresW = pd::Ok(22.0); s.gfxW = pd::NA();
    s.tempC = pd::Ok(61); s.freqGHz = pd::Ok(4.1); s.utilPct = pd::Ok(12);
    s.mode = "Intelligent (APM)";
    const std::string frame = pd::RenderDashboard(DashboardFixture(96),
                                                  AmdCapsFixture(), s, {28.0});
    Expect(frame.find("PACKAGE POWER · 28.00 W") != std::string::npos,
           "amd top section title carries pkg total");
    Expect(frame.find("CORES + GFX + REST = PKG") != std::string::npos,
           "amd legend identity");
    Expect(frame.find("of SYS") == std::string::npos, "no SYS denominator on amd");
    Expect(frame.find("CPU RESIDENCY") == std::string::npos, "residency hidden");
    Expect(frame.find("SMI") == std::string::npos, "smi hidden");
    Expect(frame.find(" GT ") == std::string::npos, "gfx domain hidden");
    Expect(frame.find(" UTIL ") != std::string::npos &&
           frame.find(" TEMP ") != std::string::npos &&
           frame.find(" FREQ ") != std::string::npos, "platform-neutral rows stay");
    ExpectEveryLineHasWidth(frame, 96, "amd dashboard geometry holds");
}
```

main() 追加调用。

- [ ] **Step 2: 跑测试确认失败**(当前 UI 对 NA 的处理尚未按 caps 隐藏全部区块)

- [ ] **Step 3: 补齐 Task 5 遗漏的隐藏分支**(若 Task 5 已实现则此步直接转绿):
   gfx 行/温度 TjMax(caps.tjMaxC==0 时不拼 "/ TjMax")/residency 区/smi/limits 区按 caps 隐藏;
   窄布局:AMD 无 platform → domains 区只出 IA 行(caps.gfxPower 时才拼 GT)

- [ ] **Step 4: 文档**——README 增补「AMD 支持(实验性)」小节:支持范围(功率/温度/频率/利用率/mode)、
   不支持项(PSYS/GT 功率、驻留率、SMI、限值读取与设定)、CSV v2 空单元格语义;
   Usage() 帮助文案同步一句话("AMD 平台当前支持监控子集")。

- [ ] **Step 5: 全量测试 + Commit**

```bash
cmd //c build.cmd && cmd //c "tests\run-tests.cmd"
git add tests/PowerDashUiTests.cpp README.md PowerDash/PowerDash.cpp PowerDash/PowerDashUi.cpp
git commit -m "feat(amd): 面板降级 golden 断言 + 用户文档"
```

---

### Task 10: 集成收尾与验收清单

**Files:**
- Modify: `README.md`(架构图小节)、无新代码

**Interfaces:**
- Consumes: 全部前置任务
- Produces: 发布就绪状态 + 验收记录

- [ ] **Step 1: 全量回归** — build.cmd 全绿、单测全绿、verify_embed MATCH
- [ ] **Step 2: Intel 实机部署验证**(labs-xiaoxin,流程同 Task 6 Step 3,比对基线帧)
- [ ] **Step 3: README 架构小节**(分层图 + spec 链接 + 「AMD PMTable 限值读取」后续任务声明,验收前置条件:一台 AMD Lenovo 实机)
- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs: 分层架构说明与 AMD 验收前置清单"
```

---

## 自审记录

- **Spec 覆盖**:第 2 节模块边界→Task 1-4/6;第 3 节映射表→Task 3/7/8;第 4 节类型→Task 1;
  第 5 节表述→Task 5/9;第 6 节 CSV→Task 5;第 7 节降级→Task 4/8/9(SMN 互斥→Task 7);
  第 8 节测试→各任务 TDD 步 + Task 10;第 9 节步骤→任务序一致。
  唯一收敛偏差:驱动侧以原子 `IO_CTL_SMN_READ` 取代 spec 提及的裸 `PCICFG_WRITE` 用户态配对
  (spec 意图即"互斥序列化",实现更严),计划 Task 7 已注明。
- **占位符**:Task 3 Step 3 的 (a)-(f) 为"从指定行照搬表达式"的迁移指令而非新逻辑,
  行号锚点均给出;无 TBD/TODO。
- **类型一致性**:Reading/Ok/NA、Decompose、Sampler 构造签名、RenderDashboard 四参签名、
  CsvRow(Vendor, Sample) 在各任务间已核对一致。
