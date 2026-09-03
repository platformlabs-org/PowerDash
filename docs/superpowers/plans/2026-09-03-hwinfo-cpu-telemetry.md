# HWiNFO 级 CPU 遥测采集(v3)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 以 HWiNFO 的字段广度/命名/CSV 格式为准,重构 PowerDash 的 CPU 遥测(SensorTable 主数据源 + 两平台探针扩充 + AMD SMU PMTable 基础设施)。

**Architecture:** 探针每帧先填 SensorTable(有序列集,HWiNFO 列名),Sample(UI 模型)从表派生;CSV v3 直接替换 v2;AMD PMTable 经驱动新增的 SMN 写 IOCTL 走 PSMU 邮箱。四层架构(入口→Probe→Sampler→UI/CSV)不变。

**Tech Stack:** C++17 / Win32(NtQuerySystemInformation、GetLogicalProcessorInformationEx)/ WDM 内核驱动(IOCTL)/ MSVC 2022。

**Spec:** `docs/superpowers/specs/2026-09-03-hwinfo-cpu-telemetry-design.md`(寄存器→数据映射表在 spec §1.1,本计划只引用不重复论证)

## Global Constraints

- **准确度红线**:凡无公开数据源依据的列宁可省略,禁止输出猜测值(spec §7.4)
- CSV v3 格式:`Date,Time,"Elapsed [s]","Power Mode","<HWiNFO 列>"...`;Date=`d.m.yyyy`、Time=`h:mm:ss.fff`(不补零,同 HWiNFO);布尔=`Yes/No`;无效读数=空单元格;列名一律双引号
- 列名必须与 spec §3/§4 的 HWiNFO 名逐字符一致(含 `[单位]`、`(avg)`、`(Static/Dynamic)` 等)
- 驱动 IOCTL 结构体保持 ASCII-only、无 `<stdint.h>`(pdstruct.h 既有约束)
- 既有熔断语义(连续 5 帧无有效功率域 → Run 失败)与 UI 渲染契约(Sample/Caps)不回退
- 每个 Task 结束:单元测试通过(`tests/run-tests.cmd`)+ 构建通过(`build.cmd`,驱动改动时)+ commit
- 测试文件为单文件 `tests/PowerDashUiTests.cpp`(既有约定),按区块追加

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `PowerDashSYS/pdstruct.h` | 改 | +`IO_CTL_SMN_WRITE`(0x80C) |
| `PowerDashSYS/powerdash.c` | 改 | +SMN 写 IOCTL 分支(smnMutex 内 0x60→0x64) |
| `PowerDash/PowerDashSensors.h/.cpp` | 新 | SensorColumn/SensorFmt/SensorTable、CSV v3 写出器、Sample 派生键约定 |
| `PowerDash/PowerDashUsage.h/.cpp` | 新 | 逐 LP Usage/Max/Total(NtQuerySystemInformation 差分,可注入源) |
| `PowerDash/PowerDashPmTable.h/.cpp` | 新 | SmuPmTable(PSMU 邮箱 + 物理映射 + float 读取)+ Krackan 偏移表 |
| `PowerDash/PowerDashModel.h` | 改 | PlatformInfo:+`std::vector<CoreInfo> cores`(repLP/threads/effClass) |
| `PowerDash/PowerDashProbe.h` | 改 | DriverIo:+`WriteSmn`;IPlatformProbe:+`sensors()`;+`CalibrateTscHz()` |
| `PowerDash/PowerDashProbe.cpp` | 改 | WindowsDriverIo::WriteSmn;CreateProbe 不变 |
| `PowerDash/PowerDashIntel.cpp` | 改 | 全列组宽表化(spec §4.1) |
| `PowerDash/PowerDashAmd.cpp` | 改 | MSR 列组宽表化 + PMTable 接入(spec §4.2) |
| `PowerDash/PowerDashUi.h/.cpp` | 改 | 删 `CsvHeader/CsvRow`(v2);面板不动 |
| `PowerDash/PowerDash.cpp` | 改 | CSV 接线 v3、拓扑 V2、`--pmdump`、Sampler utilPct 移交探针 |
| `PowerDash/PowerDash.vcxproj(+.filters)` | 改 | +4 个新文件 |
| `tests/PowerDashUiTests.cpp` | 改 | FixtureDriverIo 扩展 + v3 全量断言 |
| `tests/run-tests.cmd` | 改 | 编译列表 +3 cpp |
| `README.md` | 改 | CSV v3 说明、字段映射表、缺失字段清单 |

---

### Task 1: 驱动 IO_CTL_SMN_WRITE

**Files:**
- Modify: `PowerDashSYS/pdstruct.h`
- Modify: `PowerDashSYS/powerdash.c`(IO_CTL_SMN_READ 分支后)
- Modify: `PowerDash/PowerDashProbe.h`(DriverIo)、`PowerDash/PowerDashProbe.cpp`
- Test: `tests/PowerDashUiTests.cpp`(FixtureDriverIo 已有 WriteSmn 桩位对齐)

**Interfaces:**
- Produces: `#define IO_CTL_SMN_WRITE CTL_CODE(POWERDASH_DEV_TYPE, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)`;`DriverIo::WriteSmn(uint32_t smnAddr, uint32_t value)`(纯虚);`WindowsDriverIo::WriteSmn` 实现(复用 `SMN_Request` 同缓冲,address 入 value 入)

- [ ] **Step 1: pdstruct.h 加 IOCTL + 注释**

在 `IO_CTL_SMN_READ` 行后追加:

```c
#define IO_CTL_SMN_WRITE         CTL_CODE(POWERDASH_DEV_TYPE, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)
```

`SMN_Request` 注释改为:`value` 在读请求为输出、写请求为输入(SMU 邮箱 0x3B10xxx 需要写)。

- [ ] **Step 2: powerdash.c 加分支**

在 `case IO_CTL_SMN_READ` 块后,照抄其结构(互斥、异常保护、HalSetBusDataByOffset/HalGetBusDataByOffset),把第二步换成向 0x64 **写** `req->value`:

```c
case IO_CTL_SMN_WRITE:
{
    struct SMN_Request* req = (struct SMN_Request*)Irp->AssociatedIrp.SystemBuffer;
    ULONG32 smnAddr = 0, smnData = 0;
    if (inputSize < sizeof(struct SMN_Request))
    {
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    slot.u.AsULONG = 0;                          /* B0:D0:F0 */
    ExAcquireFastMutex(&pExt->smnMutex);
#pragma warning(push)
#pragma warning(disable: 4996)
    __try
    {
        smnAddr = req->address;
        smnData = req->value;
        if (HalSetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG,
                                  &smnAddr, 0x60, 4) != 4)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
        else if (HalSetBusDataByOffset(PCIConfiguration, 0, slot.u.AsULONG,
                                       &smnData, 0x64, 4) != 4)
        {
            status = STATUS_DEVICE_NOT_READY;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = GetExceptionCode();
        DbgPrint("PowerDash: SMN write exception 0x%X addr 0x%X\n", status, req->address);
    }
#pragma warning(pop)
    ExReleaseFastMutex(&pExt->smnMutex);
    Irp->IoStatus.Information = 0;
    break;
}
```

- [ ] **Step 3: DriverIo 接口与实现**

`PowerDashProbe.h` 的 `DriverIo` 纯虚区加 `virtual bool WriteSmn(uint32_t smnAddr, uint32_t value) = 0;`;`PowerDashProbe.cpp` 实现(同 ReadSmr 形态,无回读):

```cpp
bool WindowsDriverIo::WriteSmn(uint32_t smnAddr, uint32_t value) {
    SMN_Request req{};
    req.address = smnAddr;
    req.value = value;
    DWORD returned = 0;
    return DeviceIoControl(h_, IO_CTL_SMN_WRITE, &req, sizeof(req),
                           nullptr, 0, &returned, nullptr) != FALSE;
}
```

tests 内 `FixtureDriverIo` 同步加桩(Task 2 会扩展为可编程,此处先 `return false;` 保持编译)。

- [ ] **Step 4: 驱动构建验证**

Run: `build-sys.cmd` → 成功产出未签名 `PowerDash.sys`;`build.cmd` 全量通过 + `tests\run-tests.cmd` 通过。

- [ ] **Step 5: Commit**

```bash
git add PowerDashSYS/pdstruct.h PowerDashSYS/powerdash.c PowerDash/PowerDashProbe.h PowerDash/PowerDashProbe.cpp tests/PowerDashUiTests.cpp
git commit -m "feat(sys): IO_CTL_SMN_WRITE —— SMU 邮箱写入通道(0x60 地址/0x64 数据,互斥内原子)"
```

---

### Task 2: SensorTable 模型 + CSV v3 写出器

**Files:**
- Create: `PowerDash/PowerDashSensors.h`、`PowerDash/PowerDashSensors.cpp`
- Modify: `PowerDash/PowerDash.vcxproj`、`PowerDash.vcxproj.filters`(ClCompile/ClInclude)、`tests/run-tests.cmd`(编译列表加 `..\PowerDash\PowerDashSensors.cpp`)
- Test: `tests/PowerDashUiTests.cpp`

**Interfaces:**
- Produces(Task 4/5/7 依赖,签名精确):

```cpp
// PowerDashSensors.h
#pragma once
#include "PowerDashModel.h"
#include <string>
#include <vector>

namespace pd {

enum class SensorFmt { F1, F2, F3, RATIO2, PCT1, PCT2, YESNO, TEXT };

struct SensorColumn {
    std::string key;    // 代码内稳定键,如 "core.3.clock"(点分层,平台探针自定)
    std::string name;   // CSV 列名 = HWiNFO 全名,如 "E-core 3 Clock [MHz]"
    SensorFmt fmt;
};

class SensorTable {
public:
    unsigned Add(std::string key, std::string name, SensorFmt fmt);  // 构造期调用
    unsigned Count() const;
    const SensorColumn& Column(unsigned i) const;
    void Set(unsigned idx, Reading r);                               // 每帧
    void SetInvalid(unsigned idx) { Set(idx, NA()); }
    const Reading& Get(unsigned idx) const;
    int Find(const char* key) const;                                 // -1 = 无此列

    // Sample 派生:键缺失或 invalid → out NA(渲染层按 caps 降级)
    Reading Lookup(const char* key) const;
    double LookupOr(const char* key, double fallback) const;

private:
    std::vector<SensorColumn> cols_;
    std::vector<Reading> vals_;
};

// CSV v3(HWiNFO 对齐)。Date/Time/Elapsed/Mode 由调用方生成(入口层)。
std::string FormatSensorCell(const Reading& r, SensorFmt fmt);       // 单元格:invalid→""
std::string CsvHeaderV3(const SensorTable& t);
std::string CsvRowV3(const SensorTable& t, const std::string& date,
                     const std::string& time, double elapsedS,
                     const std::string& mode);

// HWiNFO 观测格式:d.m.yyyy / h:mm:ss.fff(不补零)
std::string FormatHwDate(const SYSTEMTIME& st);   // 实现内用 unsigned 拼,无 windows.h 依赖时改参数三无符号
std::string FormatHwTime(unsigned h, unsigned m, unsigned s, unsigned ms);

} // namespace pd
```

(实现细节:FormatHwDate 可做成 `FormatHwDate(unsigned d, unsigned mon, unsigned y)`,避免 Sensors 引 windows.h。)

- [ ] **Step 1: 写失败测试(追加到 tests 末尾 main 之前)**

```cpp
// ---------- v3 SensorTable / CSV ----------
void TestSensorTableBasics() {
    pd::SensorTable t;
    unsigned a = t.Add("pkg.power", "CPU Package Power [W]", pd::SensorFmt::F3);
    unsigned b = t.Add("flags.pkg.thermal", "Package/Ring Thermal Throttling [Yes/No]", pd::SensorFmt::YESNO);
    Expect(a == 0 && b == 1, "Add returns sequential indices");
    Expect(t.Find("pkg.power") == 0 && t.Find("nope") == -1, "Find by key");
    t.Set(a, pd::Ok(12.5));
    t.SetInvalid(b);
    Expect(!t.Get(b).valid, "SetInvalid");
    Expect(t.Lookup("nope").valid == false, "missing key -> NA");
    Expect(t.Lookup("pkg.power").value == 12.5, "Lookup value");
}

void TestFormatSensorCell() {
    Expect(pd::FormatSensorCell(pd::Ok(2417.44), pd::SensorFmt::F1) == "2417.4", "F1 1 decimal");
    Expect(pd::FormatSensorCell(pd::Ok(0.703125), pd::SensorFmt::F3) == "0.703", "F3 3 decimals");
    Expect(pd::FormatSensorCell(pd::Ok(17.0625), pd::SensorFmt::RATIO2) == "17.06", "ratio 2 decimals");
    Expect(pd::FormatSensorCell(pd::Ok(99.96), pd::SensorFmt::PCT1) == "100.0", "PCT1 rounds");
    Expect(pd::FormatSensorCell(pd::Ok(1.0), pd::SensorFmt::YESNO) == "Yes", "YESNO true");
    Expect(pd::FormatSensorCell(pd::Ok(0.0), pd::SensorFmt::YESNO) == "No", "YESNO false");
    Expect(pd::FormatSensorCell(pd::NA(), pd::SensorFmt::F1).empty(), "invalid -> empty cell");
}

void TestCsvV3HeaderAndRow() {
    pd::SensorTable t;
    t.Add("clock.avg", "Core Clocks (avg) [MHz]", pd::SensorFmt::F1);
    t.Add("flags.pkg.thermal", "Package/Ring Thermal Throttling [Yes/No]", pd::SensorFmt::YESNO);
    const std::string hdr = pd::CsvHeaderV3(t);
    Expect(hdr == "Date,Time,\"Elapsed [s]\",\"Power Mode\",\"Core Clocks (avg) [MHz]\","
                  "\"Package/Ring Thermal Throttling [Yes/No]\"", "v3 header exact");
    t.Set(0, pd::Ok(2417.44));
    t.SetInvalid(1);
    const std::string row = pd::CsvRowV3(t, "3.9.2026", "16:20:07.782", 1.0, "Intelligent (STD)");
    Expect(row == "3.9.2026,16:20:07.782,1.000,Intelligent (STD),2417.4,", "v3 row exact (trailing empty cell)");
}

void TestHwDateTimeFormats() {
    Expect(pd::FormatHwDate(3, 9, 2026) == "3.9.2026", "HWiNFO date no zero pad");
    Expect(pd::FormatHwTime(1, 20, 7, 782) == "1:20:07.782", "HWiNFO time no zero pad");
}
```

main() 中调用这 4 个测试。

- [ ] **Step 2: 运行确认失败**

Run: `tests\run-tests.cmd` → 编译失败(无 PowerDashSensors)。

- [ ] **Step 3: 实现 PowerDashSensors.h/.cpp**

实现要点(全部照签名):`FormatSensorCell` 用 `snprintf("%.1f"/"%.2f"/"%.3f")`;YESNO 以 `r.value >= 0.5` 判;invalid 一律返回 `""`。`CsvHeaderV3`/`CsvRowV3` 前缀 `Date,Time,"Elapsed [s]","Power Mode"`,行内前缀 `date,time,elapsed(F3),mode`(mode 含逗号时加引号——复用 PowerDashUi.cpp 的 CsvEscape,将其移入 Sensors 或复制为 static)。vcxproj/filters 加条目;run-tests.cmd 编译列表加 `..\PowerDash\PowerDashSensors.cpp`。

- [ ] **Step 4: 运行测试通过**

Run: `tests\run-tests.cmd` → PASS(含既有测试)。

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashSensors.h PowerDash/PowerDashSensors.cpp PowerDash/PowerDash.vcxproj PowerDash/PowerDash.vcxproj.filters tests/
git commit -m "feat(v3): SensorTable 宽表模型 + CSV v3 写出器(HWiNFO 命名/Date/Time/YesNo/空单元格)"
```

---

### Task 3: 拓扑 CoreInfo + TSC 校准 + 逐 LP Usage 监视器

**Files:**
- Create: `PowerDash/PowerDashUsage.h/.cpp`
- Modify: `PowerDash/PowerDashModel.h`(PlatformInfo)、`PowerDash/PowerDashProbe.h/.cpp`(`CalibrateTscHz` 声明+实现)、`PowerDash/PowerDash.cpp`(QueryCoreTopologyV2,接线在 Task 7 完成)、vcxproj/filters、run-tests.cmd
- Test: `tests/PowerDashUiTests.cpp`

**Interfaces:**

```cpp
// PowerDashModel.h 追加(PlatformInfo 内替换 coreLPs 字段):
struct CoreInfo {
    unsigned repLP = 0;                  // 代表 LP(mask 最低位,MSR 按核读取用)
    std::vector<unsigned> threads;       // 该核全部 LP(SMT 兄弟,Windows 相邻编号)
    unsigned effClass = 0;               // EfficiencyClass:0=性能核;1/2=能效核(分级)
};
// PlatformInfo: std::vector<CoreInfo> cores;  // 空 = 拓扑未知(探针退回全 LP)
//   —— 删除 coreLPs/physicalCores?保留 physicalCores(int)供 UI;coreLPs 删除,引用处本 Task 内改完
```

```cpp
// PowerDashUsage.h
#pragma once
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
};
class UsageMonitor {
public:
    explicit UsageMonitor(std::unique_ptr<IUsageSource> src);
    LpUsage Read();                        // 差分;首帧 ok=false
};
}
```

```cpp
// PowerDashProbe.h 追加自由函数:
double CalibrateTscHz();   // ~150ms QPC 对照 __rdtsc;失败返回 0(调用方退避)
```

- [ ] **Step 1: 写失败测试**

```cpp
// ---------- Usage 差分 / TSC 校准 ----------
class FakeUsageSource : public pd::IUsageSource {
public:
    std::vector<std::vector<double>> frames;   // 每帧每 LP busy(已算好)
    size_t i = 0;
    bool ReadPerLp(std::vector<double>& out) override {
        if (i >= frames.size()) return false;
        out = frames[i++]; return true;
    }
};
void TestUsageMonitorDiff() {
    // 首帧只建立基线 -> ok=false;第二帧直读
    auto fake = std::make_unique<FakeUsageSource>();
    fake->frames = { {50.0, 20.0}, {60.0, 80.0} };
    pd::UsageMonitor mon(std::move(fake));
    pd::LpUsage first = mon.Read();
    Expect(!first.ok, "first frame is baseline only");
    pd::LpUsage second = mon.Read();
    Expect(second.ok && second.totalPct == 70.0 && second.maxPct == 80.0, "second frame aggregates");
}
void TestNtUsageSourceInstantiates() {   // 只验证可构造+一次调用不崩(真机路径)
    pd::NtUsageSource src(2);
    std::vector<double> v;
    (void)src.ReadPerLp(v);              // CI 无断言;真机验证在 Task 9
}
void TestCalibrateTscHzPlausible() {
    double hz = pd::CalibrateTscHz();
    Expect(hz > 1e9 && hz < 8e9, "TSC in 1-8 GHz range on this class of hw");
}
void TestCoreInfoFallback() {            // PlatformInfo::cores 空时探针退回全 LP 的数据结构约定
    pd::PlatformInfo info;
    info.logicalProcessors = 4;
    info.cores.clear();
    Expect(info.cores.empty(), "fallback = empty cores (probe synthesizes all-LP)");
}
```

- [ ] **Step 2: 运行确认失败(编译错)**

- [ ] **Step 3: 实现**

`NtUsageSource`:动态取 `NtQuerySystemInformation`;class 8 `SystemProcessorPerformanceInformation`,缓冲 `nLP * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)`;busy% = 100×(1−ΔIdle/(ΔKernel+ΔUser)),Kernel 含 Idle,钳 0-100;返回的 vector 长度 != nLP → false。`UsageMonitor` 保存上次 per-LP idle/kernel/user,首帧只存基线。
`CalibrateTscHz`(PowerDashProbe.cpp):`QueryPerformanceCounter` 前后各 `__rdtsc`,`Sleep(150)`,hz = Δtsc/Δqpc×qpcFreq。
`PowerDash.cpp::QueryCoreTopologyV2`:`GetLogicalProcessorInformationEx(RelationProcessorCore, ...)`,`PROCESSOR_RELATIONSHIP::EfficiencyClass` + GroupMask 集齐该核全部 LP(跨 group 用 `RelationAllProcessorGroups`?本机单 group,按多 group 遍历 GroupCount 个 GroupMask 写清);repLP = threads 最小值;失败 → cores 空。替换旧 `QueryCoreTopology`,PlatformInfo 填充处改用 V2(入口层其余引用点同步改)。

- [ ] **Step 4: 运行测试通过;build.cmd 通过**

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashUsage.h PowerDash/PowerDashUsage.cpp PowerDash/PowerDashModel.h PowerDash/PowerDashProbe.h PowerDash/PowerDashProbe.cpp PowerDash/PowerDash.cpp PowerDash/PowerDash.vcxproj PowerDash/PowerDash.vcxproj.filters tests/
git commit -m "feat(v3): CoreInfo 拓扑(effClass/SMT 线程表)+ TSC 校准 + 逐 LP Usage 监视器"
```

---

### Task 4: Intel 探针宽表化

**Files:**
- Modify: `PowerDash/PowerDashIntel.cpp`(重写主体)、`PowerDash/PowerDashProbe.h`(IPlatformProbe + `virtual SensorTable* sensors() { return nullptr; }`)
- Test: `tests/PowerDashUiTests.cpp`

**Interfaces:**
- Consumes: Task 2 `SensorTable`、Task 3 `CoreInfo`/`UsageMonitor`/`CalibrateTscHz`、既有 `DriverIo`(ReadMsr/ReadPciCfg/MapPhys)
- Produces: `IntelProbe::sensors()` 返回非空;列键约定(供 Task 7 与测试):`vid.avg/vid.N`、`clock.avg/clock.N/clock.bus`、`eff.avg/eff.N/eff.all`、`usage.avg/usage.N/usage.max/usage.total/usage.clockmod`、`util.avg/util.N/util.total`、`ratio.avg/ratio.N/ratio.uncore`、`temp.avg/temp.N/tjmax.avg/tjmax.N/temp.pkg/temp.coremax`、`thr.N.thermal/thr.N.crit/thr.N.plim`(+`.avg`)、`thr.pkg.thermal/thr.pkg.crit/thr.pkg.plim`、`power.pkg/power.ia/power.gt/power.sys`、`pl1.static/pl1.dynamic/pl2.static/pl2.dynamic`、`ctdp.level`、`pkgres.c2/c3/c6/c8/c10`、`cores.c0.N(T 粒度按 HWiNFO 列名“<名> T0 C0 Residency”)/cores.c1.N/cores.c6.N/cores.c7.N`(+`.avg` 各自)、`lim.ia.<0-10>/lim.gt.<0-12>/lim.ring.<0-9>`(log 位序)

- [ ] **Step 1: 写失败测试(FixtureDriverIo 扩展 + Intel 回放)**

FixtureDriverIo 扩展(替换现有 ReadMsr/MapPhys 段):

```cpp
class FixtureDriverIo : public pd::DriverIo {
public:
    // (core, msr) -> 值;单核差异化。旧 msr map 语义保留为“全核同值”便捷入口。
    std::map<std::pair<unsigned, uint32_t>, uint64_t> msrPerCore;
    std::map<uint32_t, uint32_t> smn;
    std::map<uint32_t, uint32_t> smnWrites;                  // addr -> 最后写入值
    std::function<uint32_t(uint32_t addr)> smnReadHook;      // 动态 SMN(邮箱轮询)
    std::vector<uint8_t> physMem;                            // 假物理内存(MapPhys 返回其内部指针)
    uint64_t mapPhysBase = 0;
    std::function<bool(unsigned core, uint32_t msr)> msrFailure;
    bool failAllMsrs = false;
    bool ReadMsr(unsigned core, uint32_t a, uint64_t& out) override {
        if (failAllMsrs) return false;
        if (msrFailure && msrFailure(core, a)) return false;
        auto it = msrPerCore.find({core, a});
        if (it == msrPerCore.end()) return false;
        out = it->second; return true;
    }
    bool ReadPciCfg(unsigned, unsigned, unsigned, unsigned, uint32_t&) override { return false; }
    bool WritePciCfg(unsigned, unsigned, unsigned, unsigned, uint32_t) override { return false; }
    bool ReadSmn(uint32_t a, uint32_t& out) override {
        if (smnReadHook) { out = smnReadHook(a); return true; }
        auto it = smn.find(a);
        if (it == smn.end()) return false;
        out = it->second; return true;
    }
    bool WriteSmn(uint32_t a, uint32_t v) override { smnWrites[a] = v; return true; }
    bool MapPhys(uint64_t phys, size_t len, void*& virt) override {
        if (physMem.size() < len) return false;
        mapPhysBase = phys; virt = physMem.data(); return true;   // 假设 phys 页内偏移 0(测试构造保证)
    }
    void UnmapPhys(void*) override {}
};
```

Intel 回放测试(每帧两拍:构造 → readSample×2):

```cpp
void TestIntelProbeWideTable() {
    FixtureDriverIo io;
    // 基础:RAPL 单位 energy bits=16 -> 1/65536 J,power bits=3(0.125W),time bits=11
    io.msrPerCore[{0, 0x606}] = (16ull << 8) | 3ull | (11ull << 16);
    io.msrPerCore[{0, 0x614}] = 0x2FF00;                  // thermal spec 0x2FF*0.125=95.5W
    io.msrPerCore[{0, 0x1A2}] = 105ull << 16;             // TjMax 105
    io.msrPerCore[{0, 0xCE}] = 32ull << 8;                // 非睿频倍频 32(bus=tsc/32)
    // 能量计数器:帧间差 0x10000 raw = 1.0W
    static uint64_t pkgE = 0, iaE = 0, gtE = 0, sysE = 0;
    io.msrPerCore[{0, 0x611}] = 0;                        // 占位,下方用可变捕获
    // (改用 lambda 计数器 map 不可行 —— fixture 存值。用静态变量 + ReadMsr 后手动推进:
    //  简便法:测试里两次 SetMsr 递增值)
    auto bump = [&](uint32_t msr, uint64_t& counter) {
        counter += 0x10000; io.msrPerCore[{0, msr}] = counter;
    };
    // PERF_STATUS:ratio 40(0x28<<8),VID 5734(0x1666,=0.7V)
    for (unsigned c = 0; c < 2; ++c) {
        io.msrPerCore[{c, 0x198}] = (40ull << 8) | (5734ull << 32);
        io.msrPerCore[{c, 0x19C}] = (1ull << 31) | (5ull << 16) | (1ull << 1);  // 有效,readout=5,thermal log
        io.msrPerCore[{c, 0x660}] = 0;                    // C1 可读(0 驻留)
        io.msrPerCore[{c, 0x3FD}] = 0;                    // C6
    }
    io.msrPerCore[{0, 0x1B1}] = (1ull << 31) | (8ull << 16);
    io.msrPerCore[{0, 0x610}] = (224ull /*28W*/) | (1ull << 15) | (10ull << 17) | (1ull << 31)
                              | (368ull << 32) /*46W PL2*/ | (1ull << 47);
    io.msrPerCore[{0, 0x64B}] = 1;
    io.msrPerCore[{0, 0x64F}] = (1ull << 25);             // IA log: Max Turbo Limit
    // 驻留/APERF:首帧基线后第二帧给差值 —— 由 probe 内部差分,fixture 给恒 0 即可(0%)
    pd::PlatformInfo info;
    info.vendor = pd::Vendor::Intel;
    info.logicalProcessors = 2;
    info.cores = { {0, {0}, 0}, {1, {1}, 1} };            // P0 + E1
    info.baseGHz = 0;                                      // 强制走 0xCE 路径
    pd::IntelProbe* probe = nullptr;
    {
        auto p = pd::CreateIntelProbe(io, info);
        probe = dynamic_cast<pd::IntelProbe*>(p.get());   // 需要 Create 返回具体类型?见下
    }
    // NOTE:CreateIntelProbe 返回 unique_ptr<IPlatformProbe>;sensors() 经接口即可,无需 downcast。
    // 重写:直接用接口指针。
    ...
}
```

(注:执行时按上面 NOTE 用接口指针完成:两拍 `readSample`,`sensors()->Find/Get` 断言:
- `t.Find("clock.0")` 值 ≈ 40×bus(bus=CalibrateTscHz()/32;测试断言用相对比 `clock/bus≈40`,bus 列本身 95-105 区间)
- `t.Lookup("vid.0").value` 与 5734.0/8192 相等(±1e-9)
- `t.Lookup("temp.0")` = TjMax−5 = 100;`t.Lookup("tjmax.0")` = 5
- `t.Lookup("thr.0.thermal")` YESNO 有效且 value=1;`temp.pkg` = 105−8 = 97
- `pl1.static` = 28.0、`pl2.static` = 46.0、窗口 = 10×timeUnit(2^-11×10);locked=1
- `ctdp.level` = 1;`lim.ia.9`(Max Turbo bit 序 9)= 1
- 能量:bump 后第二拍 `power.pkg`=1.0W±1e-6;`power.ia/gt/sys` 同式
- Sample 派生:`s.pkgW.value==power.pkg`、`s.tempC.value==temp.pkg`、`freqGHz≈clock.avg/1000`
- C1/C6 恒 0 → `cores.c1.avg` 有效 0.0;`cores.c0.*` = ΔMPERF/ΔTSC(fixture 0 差 → 0%)

具体断言值在实现时按公式核对写入,禁止仅断言 valid。)

- [ ] **Step 2: 运行确认失败(编译错:IntelProbe 无 sensors)**

- [ ] **Step 3: 实现 IntelProbe 宽表化**

按 spec §1.1/§4.1 重写 `PowerDashIntel.cpp`,结构:

1. ctor:RAPL 单位(既有)→ `CalibrateTscHz()` → 0xCE 倍频 → busClock;MCHBAR PL 窗口(既有);每核能力探测(0x660/0x3FD/0x3FE/0x19C/0x198/0x1A2 各读一次 rep LP,成功才建列);包级驻留 MSR 探测;0x64F/0x650/0x651 探测(整组);`UsageMonitor(NtUsageSource)`;构建列集(键表见 Interfaces;列名模板,`CoreName(i)` = effClass→`"P-core %u"/"E-core %u"/"E-core (LP) %u"` + repLP 编号);`ReadBaseline()`。
2. `readSample`:每拍先 `SetInvalid` 全表(帧默认 NA),再按组读取:
   - 能量四域差分(既有公式)→ `power.*`
   - PL:静态 0x610(PL1 bits14:0×0.125、tau bits23:17×timeUnit、locked bit31;PL2 bits46:32、bit47)+ 动态 MMIO(既有)→ `pl1/pl2.*`
   - 温度:每核 0x19C(bit31→readout[22:16])→ `temp.N`(TjMax−readout)、`tjmax.N`(readout);log 位 1/5/11 → `thr.N.*`;0x1B1 → `temp.pkg`、`thr.pkg.*`;`temp.avg`、`temp.coremax`、`tjmax.avg`
   - 时钟:每核 0x198 EAX[15:8]×bus → `clock.N`;EDX[15:0]/8192 → `vid.N`;`clock.bus`=busClock;ratio=clock/bus
   - 有效/usage:每 LP APERF/MPERF 差(基线 ctor+逐帧);eff = ΔAPERF×tscHz/ΔTSC;`usage` 来自 UsageMonitor(每核=threads 均值);utility = usage×(ΔA/ΔM 钳 [0,4])×… 钳 0-100;`usage.clockmod` ← 0x19A(bit4 使能,duty[3:0]×12.5%)
   - 驻留:包级 ÷ΔTSC;每核 C1/C6/C7 ÷ΔTSC;每 LP C0=ΔMPERF/ΔTSC×100
   - Limit Reasons:0x64F/0x650/0x651 的 log 位(0x64F [16..26] 11 位、0x650 [16..28] 13 位、0x651 [16..25] 10 位;列名数组逐字符照 spec §4.1/intel.CSV)
   - SMI(既有,仅 Sample)
3. Sample 派生(Lookup):pkgW←power.pkg、coresW←power.ia、gfxW←power.gt、platformW←power.sys、tempC←temp.pkg、freqGHz←clock.avg/1000、utilPct←usage.total、c0/c2/c6 按既有包公式(pkgres.c2/c6)、powerLimit(动态优先静态)、smiDelta 直读。

**列名数组(Limit Reasons,log 位序)**:

```cpp
static const char* kIaReasons[11] = {
    "IA: PROCHOT", "IA: Thermal Event", "IA: Residency State Regulation",
    "IA: Running Average Thermal Limit", "IA: VR Thermal Alert", "IA: VR TDC",
    "IA: Electrical Design Point/Other (ICCmax PL4 SVID DDR RAPL)",
    "IA: Package-Level RAPL/PBM PL1", "IA: Package-Level RAPL/PBM PL2 PL3",
    "IA: Max Turbo Limit", "IA: Turbo Attenuation (MCT)" };
static const char* kGtReasons[13] = {
    "GT: PROCHOT", "GT: Thermal Event", "GT: DDR RAPL",
    "GT: Residency State Regulation", "GT: Running Average Thermal Limit",
    "GT: VR Thermal Alert", "GT: VR TDC", "GT: Max VR Voltage  ICCmax  PL4",
    "GT: Domain-Level PBM PLGT", "GT: Package-Level RAPL/PBM PL1",
    "GT: Package-Level RAPL/PBM PL2 PL3", "GT: Inefficient Operation", "GT: Fuses limit" };
static const char* kRingReasons[10] = {
    "RING: PROCHOT", "RING: Thermal Event", "RING: DDR RAPL",
    "RING: Residency State Regulation", "RING: Running Average Thermal Limit",
    "RING: VR Thermal Alert", "RING: VR TDC", "RING: Max VR Voltage  ICCmax  PL4",
    "RING: Package-Level RAPL/PBM PL1", "RING: Package-Level RAPL/PBM PL2 PL3" };
```

(GT/RING 位序以 HWiNFO 列序为据,IA 与 SDM 一致;Task 9 实机若发现 LNL 无 0x64F 族或语义不符 → 整组省略,不猜。)

- [ ] **Step 4: 测试通过 + build.cmd 通过**(既有 IntelProbe 旧测试更新为新断言;删 `CoreVoltage OC mailbox` 之类未实现路径)

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashIntel.cpp PowerDash/PowerDashProbe.h tests/
git commit -m "feat(v3-intel): 全列组宽表化 —— 时钟/VID/有效/温度/降频位/驻留/PLx 静动态/cTDP/Limit Reasons/Usage/Utility"
```

---

### Task 5: AMD 探针 MSR 宽表化

**Files:**
- Modify: `PowerDash/PowerDashAmd.cpp`
- Test: `tests/PowerDashUiTests.cpp`

**Interfaces:**
- Consumes: Task 2/3 产物;`DriverIo::ReadSmn`(既有)
- Produces: 键约定:`vid.avg/vid.N`、`clock.avg/clock.N/clock.bus`、`eff.avg/eff.<core>.<t>/eff.all`、`usage.avg/usage.<core>.<t>/usage.max/usage.total`、`util.avg/util.<core>.<t>/util.total`、`ratio.avg/ratio.N`、`cores.c0.avg/cores.c0.N`、`temp.tctl`、`power.pkg/power.core.avg/power.core.N`、`epp.N`(首列 `Energy Performance Preference [%]` 只出全局均值列,名同 HWiNFO 单列)、PMTable 键见 Task 6

- [ ] **Step 1: 写失败测试(AMD 回放)**

```cpp
void TestAmdProbeWideTable() {
    FixtureDriverIo io;
    io.msrPerCore[{0, 0xC0010299}] = 16ull << 8;                 // energy bits 16
    // Zen5 P-state:CpuFid[11:0]=0x2BC(700)->3500MHz; VID[21:14]=0x50(80)->1.05V
    io.msrPerCore[{0, 0xC0010293}] = (80ull << 14) | 0x2BCull;
    io.msrPerCore[{0, 0xC0010064}] = (700ull / 5);               // P0: fid 140 *5 = 700? 修正:700/5=140 -> 700MHz?
    // P0 解码输入:fid[11:0]。3500MHz -> fid=700(0x2BC)。
    io.msrPerCore[{0, 0xC001029B}] = 0; io.msrPerCore[{0, 0xC001029A}] = 0;
    io.msrPerCore[{0, 0xC00102B3}] = 128ull << 24;               // EPP 128 -> 50.2%
    io.smn[0x59800] = 0x510B0000u;                                // k10temp: (raw>>21)*0.125-49
    pd::PlatformInfo info;
    info.vendor = pd::Vendor::Amd;
    info.logicalProcessors = 4;
    info.family = 0x1A;
    info.cores = { {0, {0,1}, 0}, {2, {2,3}, 1} };                // Zen5(2T) + Zen5c(2T)
    auto probe = pd::CreateAmdProbe(io, info);
    pd::Sample s;
    (void)probe->readSample(s);
    // (帧 2 断言 —— 能量差分需要两拍;AP/MP 恒 0 差无效 -> eff NA 断言)
    const pd::SensorTable* t = probe->sensors();
    Expect(t && t->Find("vid.0") >= 0, "per-core VID column exists");
    // 断言:t->Lookup("clock.0").value == 3500.0
    //       t->Lookup("vid.0").value == 1.550 - 0.00625*80 == 1.05
    //       temp.tctl == ((0x510B0000>>21)*0.125)-49 == 32.0
    //       epp == 128/2.55 ≈ 50.196
    //       列名 Find 对应 "Zen5 Core 0 VID [V]" / "Zen5c Core 2 Clock [MHz]"
    //       threads 0/1 各有 eff.<core>.<t> 列;cores.c0.* 恒 0(AP/MP 0 差)
}
```

(断言按公式写死数值;P0 常量应为 fid=700 → 3500 MHz。)

- [ ] **Step 2: 运行确认失败**

- [ ] **Step 3: 实现**

重写 `PowerDashAmd.cpp`(照 Intel 结构):
1. ctor:RAPL 单位(既有);`CalibrateTscHz`;P0 → baseGHz + busClock = tscHz/(P0MHz/100);EPP/0xC0010293/0xC001029A 可读性探测;`CoreName(i)` = family 0x1A:effClass 0→`"Zen5 Core %u"`、1→`"Zen5c Core %u"`(repLP 编号),其他 family 单类→`"Core %u"`;UsageMonitor;列集(VID/Clock 每核、Effective/Usage/Utility 每线程 T0/T1、C0 每核、EPP 全局、temp.tctl、power 三式);基线。
2. readSample:能量差分(既有)→ power.pkg / power.core.N / power.core.avg;0xC0010293 每核(rep LP)→ clock=fid×5(family 0x1A)或 fid/dfsId×200(旧)+ ΔA/ΔM 节流调整(LHM 式:ΔAPERF<ΔMPERF 时乘比)、vid=1.55−0.00625×vid[21:14];ratio=clock/bus;AP/MP 每线程差→ eff=ΔAPERF×tscHz/ΔTSC、cores.c0.N=ΔMPERF/ΔTSC×100(核内线程均值);EPP=0xC00102B3[31:24]/2.55;Tctl(既有 k10temp);usage/utility(线程级列,`<CoreName> T<t> Usage [MHz→%]`)。
3. Sample 派生:pkgW、coresW=Σpower.core、tempC←temp.tctl、freqGHz←clock.avg/1000、utilPct←usage.total;powerLimit/currentLimit 留 NA(Task 6 填)。

- [ ] **Step 4: 测试通过 + build.cmd**

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashAmd.cpp tests/
git commit -m "feat(v3-amd): MSR 宽表化 —— 0xC0010293 时钟+VID/线程级有效时钟/C0/EPP/k10temp 温度/usage/utility"
```

---

### Task 6: SMU PMTable 基础设施 + AMD 接入

**Files:**
- Create: `PowerDash/PowerDashPmTable.h/.cpp`
- Modify: `PowerDash/PowerDashAmd.cpp`(接入)、vcxproj/filters、run-tests.cmd
- Test: `tests/PowerDashUiTests.cpp`

**Interfaces:**

```cpp
// PowerDashPmTable.h
#pragma once
#include "PowerDashProbe.h"
#include <memory>
namespace pd {
// ryzenAdj nb_smu_ops.c/api.c 协议(Krackan Point 走 PSMU 默认邮箱)
class SmuPmTable {
public:
    static std::unique_ptr<SmuPmTable> TryCreate(DriverIo& io);  // 失败 nullptr(诚实降级)
    bool Refresh();                       // 每帧:transfer 0x65(拒绝→10ms 重试一次)
    float At(uint32_t byteOff) const;     // float@偏移;越界/未刷新 NAN
    uint32_t version() const;
    uint64_t addr() const;
private:
    DriverIo& io_;
    void* map_ = nullptr;
    uint32_t version_ = 0, size_ = 0;
    uint64_t addr_ = 0;
    uint32_t SmuMsg(uint32_t msg, uint32_t args[6]);   // 返回 response(0x1=OK)
};
// Krackan Point PMTable 版本 0x00650005 已知偏移(ryzenAdj api.c)
struct KpPm {
    static constexpr uint32_t kVersion = 0x00650005;
    static constexpr uint32_t StapmLimit = 0x00, StapmValue = 0x04;
    static constexpr uint32_t FastLimit = 0x08, FastValue = 0x0C;   // FPPT/PPT FAST
    static constexpr uint32_t SlowLimit = 0x10, SlowValue = 0x14;   // SPPT/PPT SLOW
    static constexpr uint32_t ApuSlowLimit = 0x18, ApuSlowValue = 0x1C;
    static constexpr uint32_t TdcLimit = 0x30, TdcValue = 0x34;
    static constexpr uint32_t SocCurLimit = 0x38, SocCurValue = 0x3C;
    static constexpr uint32_t TctlLimit = 0x40, TctlValue = 0x44;
    static constexpr uint32_t StapmTimeS = 0x90C, SlowTimeS = 0x910;
};
}
```

实现要点:PSMU 常量 `kMsg=0x3B10a20, kRep=0x3B10a80, kArgs=0x3B10a88`;`SmuMsg`:清 rep(WriteSmn kRep=0)→ args 0-5 写 kArgs+4i → msg 写 kMsg → 轮询 ReadSmn(kRep)≤1e6 次(每次 0,非 0 即出)→ 回读 args[0..5];response==0x1 视为 OK。`TryCreate`:先写 arg0=0x47 回读自检(SMN 可写)→ 测试消息 0x1 → 消息 0x6 取版本 → 若版本 != KpPm::kVersion 仍继续(记录版本,偏移仅对 0x00650005 生效,其他版本 At() 仅读不解读)→ 消息 0x66 取地址(arg0 lo,arg1 hi)→ MapPhys(size 0x1000 页对齐处理)→ 首次 Refresh。

- [ ] **Step 1: 写失败测试**

```cpp
void TestSmuPmTableProtocol() {
    FixtureDriverIo io;
    // 假物理内存 0x1000,预置 STAPM 值等(小端 float)
    io.physMem.resize(0x1000);
    auto putf = [&](uint32_t off, float v) { memcpy(io.physMem.data() + off, &v, 4); };
    putf(0x00, 28.0f); putf(0x04, 15.5f);      // STAPM 限值/实际
    putf(0x30, 100.0f); putf(0x34, 42.5f);     // TDC 限值/实际
    putf(0x40, 100.0f); putf(0x44, 55.25f);    // Tctl 限值/实际
    // SMU 邮箱脚本:测试消息→OK;0x6→版本 0x00650005;0x66→地址 0x1000;0x65→OK
    static uint32_t lastMsg = 0; static uint32_t arg0 = 0;
    io.smnWrites;  // hook:
    io.smnReadHook = [&](uint32_t a) -> uint32_t {
        if (a == 0x3B10a80) return lastMsg ? 0x1 : 0x0;      // rep:msg 发出后 OK
        if (a == 0x3B10a88) {                                 // arg0 视 lastMsg 而定
            if (lastMsg == 0x6) return 0x00650005;
            if (lastMsg == 0x66) return 0x1000;
            return arg0;
        }
        return 0;
    };
    // 拦截 WriteSmn:记录 msg 并置 lastMsg(arg 透传 arg0)
    //   (FixtureDriverIo 的 WriteSmn 是直存 map;此处需要在测试内包装 —— 扩展 fixture:
    //    std::function<void(uint32_t,uint32_t)> smnWriteHook; WriteSmn 先调 hook 再记录)
    ...
    auto pm = pd::SmuPmTable::TryCreate(io);
    Expect(pm != nullptr, "SMU handshake succeeds");
    Expect(pm->version() == 0x00650005, "version from msg 0x6");
    Expect(pm->At(0x04) == 15.5f, "float read at offset");
    // 断言:io.smnWrites[0x3B10a20] 最后为 0x65;Refresh 后 At 值随 physMem 变化
}
```

AMD 接入测试:`CreateAmdProbe` fixture 加 SMU hook 后,`readSample` 产出 `pm.stapm.value=15.5`、`tdc.value=42.5`、`tdc.limit.pct=42.5%`、`thermal.limit.pct=55.25%`、Sample.currentLimit.tdcA=42.5;`"APU STAPM [W]"` 列名断言。

- [ ] **Step 2: 运行确认失败**

- [ ] **Step 3: 实现 PowerDashPmTable + AMD 探针接入**

AMD ctor 末尾:`pm_ = SmuPmTable::TryCreate(io_)`,成功且 version==KpPm::kVersion 时追加列组(键:`pm.stapm.limit/value`、`pm.fast.limit/value`、`pm.slow.limit/value`、`pm.apuslow.limit/value`、`pm.tdc.limit/value`、`pm.soccur.limit/value`、`pm.tctl.limit`、限值%键 `pct.tdc/pct.pptfast/pct.pptslow/pct.stapm/pct.thermal`);列名照 amd.CSV:`"APU STAPM [W]"`(=StapmValue)、`"CPU TDC [A]"`(=TdcValue)、`"SoC Current (SVI3 TFN) [A]"`(=SocCurValue,待 Task 10 实机核对,不符则删)、`"CPU TDC Limit [%]"`、`"CPU PPT FAST Limit [%]"`、`"CPU PPT SLOW Limit [%]"`、`"APU STAPM Limit [%]"`、`"Thermal Limit [%]"`。readSample:`pm_->Refresh()` 失败 → 本帧 PM 列全 NA(不清列);% = value/limit×100(limit>0)。Sample:powerLimit.sustainedW←pm.stapm.limit、burstW←pm.fast.limit、currentLimit.tdcA←pm.tdc.value。

- [ ] **Step 4: 测试通过 + build.cmd**

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashPmTable.h PowerDash/PowerDashPmTable.cpp PowerDash/PowerDashAmd.cpp PowerDash/PowerDash.vcxproj PowerDash/PowerDash.vcxproj.filters tests/
git commit -m "feat(v3-amd): SMU PSMU 邮箱 PMTable 基础设施 + Krackan 已知偏移列组(STAPM/PPT/TDC/Tctl/限值%)"
```

---

### Task 7: CSV v3 接线 + v2 移除

**Files:**
- Modify: `PowerDash/PowerDashUi.h/.cpp`(删 `CsvHeader/CsvRow` 与 `MonitorOptions` 不变)、`PowerDash/PowerDash.cpp`、`PowerDash/PowerDashSampler.cpp`(utilPct 移交探针:删 ReadUtilization,Run 不再填)、`PowerDash/PowerDashSampler.h`
- Test: `tests/PowerDashUiTests.cpp`

- [ ] **Step 1: 更新失败测试**

删除 `TestCsvHasStableColumnsAndEscapesText`(v2),新增:

```cpp
void TestSamplerLeavesUtilToProbe() {
    // FixtureDriverIo 无法直接驱动 Sampler::Run 的探针?—— 用 FakeProbe:
    class FakeProbe : public pd::IPlatformProbe {
    public:
        pd::PlatformCaps caps_;
        int calls = 0;
        const pd::PlatformCaps& caps() const override { return caps_; }
        bool readSample(pd::Sample& s) override {
            s.utilPct = pd::Ok(33.0); ++calls; return true;
        }
    };
    FakeProbe p;
    pd::Sampler sampler(p, nullptr, nullptr, [] {});   // 无等待,立即返回
    int seen = 0;
    sampler.Run(0.0001, [&](const pd::Sample& s) {     // runSeconds<1 → 至少 1 帧?
        Expect(s.utilPct.valid && s.utilPct.value == 33.0, "util comes from probe");
        ++seen;
    });
    Expect(seen >= 1, "at least one frame sampled");
}
```

(Sampler::Run 的 runSeconds 语义:秒数向下取帧;传 0.5 → 1 帧。测试用 0.5。)

- [ ] **Step 2: 确认失败**

- [ ] **Step 3: 实现**

1. `PowerDashUi.h/.cpp` 删 `CsvHeader/CsvRow` 声明与实现(保留 CsvEscape 移入 Sensors 供 mode 转义)。
2. `PowerDash.cpp::RunMonitor`:CSV 打开移到 probe 创建之后;`csv << pd::CsvHeaderV3(*probe->sensors())`;行写出:`GetLocalTime` → `FormatHwDate/Time` + `CsvRowV3(*probe->sensors(), date, time, sample.elapsedS, sample.mode)`。probe->sensors()==nullptr → 报错退出(不可达:两探针都有)。
3. `Sampler` 删 `ReadUtilization`/`GetSystemTimes` 基线;`Run` 不再覆盖 utilPct(探针已填)。
4. 无 VT 摘要行与非 VT 输出保持(字段来自 Sample,不受影响)。

- [ ] **Step 4: 全测试通过 + build.cmd;手动冒烟**

Run: 管理员 `x64\Release\PowerDash.exe power 3 --csv .scratch\smoke.csv` → CSV 表头/行符合 v3(肉眼 1 次 + 表头行数=sensors 数+4)。

- [ ] **Step 5: Commit**

```bash
git add PowerDash/PowerDashUi.h PowerDash/PowerDashUi.cpp PowerDash/PowerDash.cpp PowerDash/PowerDashSampler.h PowerDash/PowerDashSampler.cpp tests/
git commit -m "feat(v3): CSV v3 接线替换 v2,utilPct 归探针,Sampler 去重"
```

---

### Task 8: --pmdump 命令

**Files:**
- Modify: `PowerDash/PowerDash.cpp`(usage/命令分发)
- Test: 编译 + `--pmdump` 帮助行断言不适用(CLI 手测);单测无(纯 IO 编排)

- [ ] **Step 1: 实现 `CmdPmDump`**:`PowerDash --pmdump [hexaddr可选过滤]`:EnsureDriverLoaded → `SmuPmTable::TryCreate` → 失败打印版本/错误 → Refresh → 逐 4 字节行 `printf("%04x  %08X  %g\n", off, hex, float)`。生命周期同 `--smndbg`。
- [ ] **Step 2: 构建 + 管理员本机手测**(本机 Intel 无 AMD SMU → 预期打印 "SMU not available (Intel)" 路径;真验证在 Task 10)
- [ ] **Step 3: Commit** `feat(v3): --pmdump —— PMTable 全表浮点导出(下期偏移映射取证工具)`

---

### Task 9: 本机 Intel 实测 + HWiNFO 比对

**Files:**
- Create: `tools/compare-hwinfo.py`(纳入仓库;两平台共用)
- 无产品代码改动(比对发现问题 → 修复后重跑)

- [ ] **Step 1: 写比对脚本** `tools/compare-hwinfo.py`:两 CSV 按列名交集逐列统计(均值/区间/重叠度),输出 `列名 | hwinfo均值 | ours均值 | diff | 状态`;容差:温度 ≤2°C、功率 ≤max(10%,0.5W)、时钟 idle ≤5%、usage ≤3pp;超出标记 REVIEW。
- [ ] **Step 2: 采集**:管理员 `PowerDash power 60 --csv .scratch\intel-v3.csv`(后台跑轻载 60s)。
- [ ] **Step 3: 比对** `python tools/compare-hwinfo.py intel.CSV .scratch\intel-v3.csv`,逐条 REVIEW 项判定:
  - 列缺失(预期:Ring/LLC Clock、Uncore VID、SVID IOUT 等 —— README 记录)
  - 值不符:定位解码(优先核对:VID/8192、TjMax、ratio 位段、驻留单位、Limit Reasons 位序、Uncore Ratio 0x620 语义;**0x620 值与 HWiNFO 不符则删列**)
  - EfficiencyClass 核对:打印每核 class vs HWiNFO 命名(P/E/E(LP))
- [ ] **Step 4: 修复循环**(改代码 → 单测同步 → 重采 → 重比,直到全部列 PASS 或判定为已知缺失)
- [ ] **Step 5: Commit** `test(v3): HWiNFO intel.CSV 比对脚本 + 实测校准记录`

---

### Task 10: 远程 AMD 实测 + PMTable 校验

- [ ] **Step 1**: `build.cmd` 产出 exe → `remote-tb.ps1` 拷贝执行:`PowerDash power 60 --csv C:\temp\amd-v3.csv` 取回。
- [ ] **Step 2**: 比对 `amd.CSV`(同 Task 9 脚本)+ 专项断言:
  - PMTable version == 0x00650005(打印)
  - `APU STAPM [W]`/`CPU TDC [A]`/`Thermal Limit [%]` vs HWiNFO 同名列(值域重叠)
  - `SoC Current (SVI3 TFN) [A]` vs HWiNFO 同名列 —— **不符则删列并记录**
  - Zen5/Zen5c 命名 vs HWiNFO(EfficiencyClass 实测;单类机器 → 全 "Core n" 降级 + README 记录)
- [ ] **Step 3**: `--pmdump` 全表导出 → 存 `\\nas\labs\TEST\hwinfo-smaple\pmdump-kp.txt`(下期映射素材;同时在机跑 HWiNFO 采样 60s 存 nas 对照)。
- [ ] **Step 4**: 修复循环(同 Task 9)。
- [ ] **Step 5**: Commit `test(v3): AMD 实机校准 —— PMTable 版本断言/已知偏移比对/pmdump 存档`

---

### Task 11: 文档更新

- [ ] **Step 1**: README 重写 CSV 章节:v3 格式样例、列集表(Intel/AMD 分列,数据源列)、缺失字段清单(无公开源,标注原因与下期计划:PMTable 映射/OC Mailbox/Gear Mode 等)、`(perf #N)` 等命名偏差说明、`--pmdump` 用法;`Elapsed [s]/Power Mode` 扩展列说明。
- [ ] **Step 2**: spec 状态行改"已实施";README AMD 实测修复记录追加本轮。
- [ ] **Step 3**: Commit `docs: v3 采集字段映射表与 HWiNFO 对齐说明`

---

## Self-Review(写计划时已核)

1. **Spec 覆盖**:§2 模型→Task 2;§3 CSV→Task 2/7;§4.1→Task 4;§4.2→Task 5/6;§5 驱动→Task 1;§6 测试→各任务 TDD 步骤;§7 验证→Task 9/10;§8 非目标未被实现 ✓
2. **占位符**:无 TBD;Task 4/5 测试的"断言按公式写死"处均给出公式与例值 ✓
3. **类型一致**:SensorTable/Find/Lookup、WriteSmn、CoreInfo、sensors() 各任务签名一致 ✓
