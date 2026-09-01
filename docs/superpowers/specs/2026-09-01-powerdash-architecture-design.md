# PowerDash 整体架构与功率参数类型体系设计

日期:2026-09-01
状态:已评审(三节设计均经用户逐节确认)
范围决策:功率数据模型为主(不含 CLI 参数体系重设计);AMD 第一版监控只读;CSV 允许 v2 一次性重排

## 1. 背景与目标

PowerDash 现为 Intel 单平台实现:采样字段(`PowerSample`)的 Intel 语义硬编码在采样器、
CSV、UI 三端;`PowerDash.cpp`(~1000 行)混合了驱动生命周期、平台知识(MSR/MCHBAR)、
采样循环与 VT 渲染协议。AMD 支持调研(见 README 引用的调研结论与本文附录 A)表明:
监控只读所需接口 AMD 均有等价物,但字段可用性集合不同(AMD 无 PSYS/GT 功率、无 C 驻留、
无 SMI,限值体系为 PPT/TDC/EDC 而非 PL1/PL2)。

本设计目标:

1. **分层后端架构**:平台差异收敛到可替换的 Probe 实现,采样引擎/渲染/CSV 平台无关
2. **功率参数类型体系**:字段自带可用性语义(valid),根除"0 是零值还是不支持"的歧义
3. **统一表述规则**:面板与 CSV 从同一张权威映射表生成,跨平台视觉结构一致

非目标(明确不做):AMD SMU 写操作(PPT 设定)、插件化/运行时供应商发现、
Ryzen Master SDK 依赖(与单文件零残留分发冲突)、CLI 参数体系重设计。

## 2. 模块边界与数据流

文件布局(平铺,贴合现有结构,vcxproj 改动最小):

| 文件 | 职责 | 现状对应 |
|---|---|---|
| `PowerDash.cpp` | CLI 入口、参数解析、命令分发、驱动装载生命周期 | 瘦身后保留 |
| `PowerDashProbe.h` | `IPlatformProbe` 接口 + `PlatformCaps` + 工厂 `CreateProbe()` | 新增 |
| `PowerDashIntel.cpp` | Intel 实现:RAPL 能量 MSR(0x611/0x639/0x641/0x64D)、MCHBAR PL1/PL2、TjMax、C2/C6 驻留、SMI | 从 `RunPowerMonitor` 迁出 |
| `PowerDashAmd.cpp` | AMD 实现:能量 MSR(0xC0010299/0x029A/0x029B)、SMN 温度(0x59800)、APERF/MPERF | 新增 |
| `PowerDashModel.h` | 功率参数类型体系(第 4 节) | `PowerSample` 演进 |
| `PowerDashSampler.cpp` | 采样引擎:节拍、差分、回绕、历史窗口——平台无关 | 采样循环骨架迁出 |
| `PowerDashUi.*` | 面板渲染,只消费统一模型 | 现文件调整 |
| CSV 写入 | v2 schema(第 6 节) | `PowerDashUi` 中 CSV 函数迁出 |
| `PowerDashSYS` | 内核驱动,新增 `IO_CTL_PCICFG_WRITE`(AMD SMN 地址寄存器 0x60 需要写) | 微改 |

核心接口(示意):

```cpp
struct PlatformCaps {               // Probe 构造期一次性探测
    Vendor vendor;                  // Intel / AMD
    std::string cpuName;            // "Intel(R) Core(TM) Ultra 5 225H  [Arrow Lake-H]"
    bool gfxPower = false;          // Graphics 域可用(Intel PP1;AMD 无)
    bool platformPower = false;     // Platform 域可用(Intel PSYS;AMD 无)
    bool powerLimits = false;       // 限值读取(Intel PL1/PL2;AMD PPT/TDC/EDC)
    bool residency = false;         // C-state 驻留(Intel 专属)
    bool smi = false;               // SMI 计数(Intel 专属)
    double budgetW = 0.0;           // 预算刻度基准(PL2 或 PPT),0 = 未知
};

class IPlatformProbe {
public:
    virtual ~IPlatformProbe() = default;
    virtual PlatformCaps caps() const = 0;
    virtual bool readSample(Sample& s) = 0;   // 每秒 tick;内部维护计数器差分
    // v2 预留(AMD v1 不实现):virtual bool setLimits(const LimitRequest&)
};
```

数据流:`CLI 入口 → 驱动装载 → CreateProbe(CPUID 分流) → Sampler 循环
{ probe.readSample → Sample → Ui 渲染 + CSV 行 }`。

约束:

- 渲染层与 CSV 层不得出现任何 MSR/平台寄存器概念
- 驱动生命周期归入口层;Probe 只持有驱动句柄与预读静态信息(能量单位、基频、TjMax)
- 单 exe 分发不变:驱动仍以资源内嵌、按需装卸

## 3. 平台接口映射(权威表)

| 模型概念 | Intel 来源 | AMD 来源(Zen 17h/19h/1Ah) |
|---|---|---|
| Package 功率 | MSR 0x611 能量差分 | MSR 0xC001029B 能量差分 |
| Cores 功率 | MSR 0x639 (PP0) | MSR 0xC001029A 逐核聚合 |
| Graphics 功率 | MSR 0x641 (PP1) | 不可用(invalid) |
| Platform 功率 | MSR 0x64D (PSYS) | 不可用(v2 留 PMTable 扩展位) |
| 能量单位 | MSR 0x606 | MSR 0xC0010299;已知 family 定标 quirk 按修正表硬编码 |
| 功率限值 sustained | PL1(MCHBAR MMIO) | PPT(SMU PMTable,v1 仅已知 family) |
| 功率限值 burst | PL2(MCHBAR MMIO) | FPPT(部分 APU;无则 invalid) |
| 电流限值 TDC/EDC | 不可用(invalid) | SMU PMTable |
| 温度 | THERM_STATUS + TjMax(0x1A2) | SMN 0x59800 Tctl;顶温按 SKU 表 |
| 频率 | APERF/MPERF(0xE7/0xE8) | 同左,零改动 |
| C0/C2/C6 驻留 | PKG_C2/C6_RESIDENCY | 不可用(invalid) |
| SMI 增量 | MSR_SMI_COUNT | 不可用(无此概念) |
| 利用率 | GetSystemTimes(平台无关) | 同左 |
| 模式(DYTC) | Lenovo EnergyDrv(平台无关) | 同左(需实机核对 AcpiVpc 版本偏移) |

SMN 访问:B0:D0:F0 配置空间 0x60(地址)/0x64(数据);驱动侧以 fast mutex 串行化。
Fn+Q 注入偏移绑定 AcpiVpc.sys 版本而非 CPU 厂商,AMD Lenovo 机型需按版本核对,流程同现状。

## 4. 功率参数类型体系

```cpp
struct Reading {                    // 一切物理量的统一表达
    double value = 0.0;             // SI 单位:W / A / °C / GHz / %
    bool valid = false;             // false = 平台不支持或本帧读取失败
};

enum class Domain { Package, Cores, Graphics, Platform };

struct PowerLimit {                 // 功率类限值 (W),跨平台统一
    Reading sustained;              // Intel PL1 ↔ AMD PPT
    Reading burst;                  // Intel PL2 ↔ AMD FPPT(无则 invalid)
    Reading sustainedWindow;        // τ,秒
    bool locked = false;
};

struct CurrentLimit {               // 电流类限值 (A),AMD 专属;Intel 全 invalid
    Reading tdc, edc;
};

struct Sample {                     // v2:每个字段自带可用性
    Reading pkg, cores, gfx, platform;
    PowerLimit powerLimit;
    CurrentLimit currentLimit;
    Reading temp, freqGhz, utilPct;
    Reading c0, c2, c6;             // 驻留率(Intel;AMD invalid)
    std::optional<uint64_t> smiDelta;   // Intel 专属
    std::string mode;               // Lenovo DYTC,平台无关
    double elapsedS = 0.0;
    std::string timestamp;
};
```

派生量(采样引擎计算,非 Probe 产出):

- Intel:`offPackage = platform − pkg`(REST-of-SYS 分解剩余)
- AMD:`restPackage = pkg − cores − gfx`(clamp ≥ 0,分解剩余)

## 5. 面板表述规则(单一权威)

1. **顶区统一为"总量 = 主域 + 剩余"两根内联进度条**,标题携带分解目标总值:
   - Intel:`SYSTEM POWER · x W`,恒等式 PKG + REST = SYSTEM
   - AMD(无 PSYS):`PACKAGE POWER · x W`,恒等式 CORES + GFX + REST = PKG,
     两根条为 PKG 与 REST-of-PKG
   - 两平台视觉结构一致,仅分母换名
2. **百分比永远显式分母**:"34% of PL2" / "19% of SYS" / "27% of PPT";
   分母与限值一律用平台本名(Intel: PL1/PL2;AMD: PPT/FPPT/TDC/EDC)。
   刻度图例沿用现状结构:`0 W … scale to X W · | = PL1 53.00 W`(Intel)/
   `| = PPT 54.00 W`(AMD)· 恒等式`
3. **valid=false 的区块整体隐藏**,不显示 0/N/A 堆砌。AMD 面板自然无 SYSTEM 行、
   C 驻留区、SMI 行;温度/频率/利用率/模式恒显示
4. 限值区按平台呈现:Intel 两行(PL1/PL2 + τ + LOCKED);AMD 三行(PPT/FPPT、TDC、EDC),
   每行行内显式单位:"PPT 54.00 W"、"TDC 95.00 A"

历史 sparkline 与 Min/Avg/Max 统计基于 pkg,规则不变。

## 6. CSV v2 schema

宽表(union 列)+ platform 列;invalid → 空单元格(不是 0):

```
timestamp, elapsed_s, platform,
pkg_w, cores_w, gfx_w, platform_w,
limit_sustained_w, limit_sustained_window_s, limit_burst_w, limit_locked,
tdc_a, edc_a,
temp_c, freq_ghz, util_pct,
c0_pct, c2_pct, c6_pct, smi_delta, mode
```

规则:列名单位后缀化(`_w/_a/_c/_ghz/_pct`);`platform ∈ {intel, amd}`;
`limit_locked` 为 0/1。README 附迁移表:

| v1 列 | v2 列 |
|---|---|
| pkg_w | pkg_w(不变) |
| ia_w | cores_w |
| gt_w | gfx_w |
| sys_w | platform_w |
| pl1_w / pl2_w | limit_sustained_w / limit_burst_w |
| (无) | limit_sustained_window_s, tdc_a, edc_a, platform, limit_locked |
| c0/c2/c6_pct, smi_delta, mode, temp_c, freq_ghz, util_pct | 同名保留 |

## 7. 错误处理与降级

- **能力探测一次性**(Probe 构造期):逐项试探(如 AMD 读 0xC001029B 失败 → Package
  invalid);caps 决定呈现集合。未知 AMD family 不猜 PMTable 偏移,收缩到保底集
  (能量 MSR + 温度 + 频率)并在面板提示一行
- **帧级失败**:个别 Reading invalid → 该帧该字段空;`readSample` 返回 false
  (无法产出任何有效功率域)连续 5 帧 → 报错退出
- **计数器回绕**:32 位能量计数器(AMD 每核尤甚)回绕差分统一在采样引擎层
- **SMN 串行化**:驱动内 0x60/0x64 访问对以 fast mutex 保护(多实例/并发访问会损坏,
  与 Linux 内核同款教训)
- **`-setpl`**:Intel 行为不变;AMD v1 明确拒绝并输出"暂不支持,规划中"
  (接口已留 `setLimits()` 占位)

## 8. 测试策略

| 层 | 方式 |
|---|---|
| model | 恒等式属性测试:两平台 fixture 下"总量 = Σ分量"(误差 < 0.01 W) |
| probe | 录制 MSR/SMN 序列回放(Intel 实机录制;AMD 按调研数据构造 fixture) |
| UI | 现有 golden 断言推广为 Intel/AMD 双 fixture,每平台一套期望帧 |
| CSV | schema 断言 + invalid → 空单元格断言 |
| 实机 | Intel(labs-xiaoxin)全量回归;AMD 实机验收为发布前置条件 |

保留 `PDASH_FORCE_VT` 捕获钩子;平台由 CPUID 决定,测试经 fixture 注入,不做伪装变量。

## 9. 迁移步骤(实施计划骨架)

1. Model v2 + Probe 接口 + IntelProbe 迁移(golden 测试锁定现有 Intel 行为)
2. 采样引擎抽出 + CSV v2(README 迁移表同步)
3. UI 消费 v2,Intel 帧与现状逐字节等价(差分渲染协议不变)
4. 驱动新增 `IO_CTL_PCICFG_WRITE` + SMN mutex + AmdProbe(监控只读)
5. AMD 降级路径双平台 fixture 测试 + README/文档更新

每步独立可构建、可部署验证(Intel 机型回归)。

## 附录 A:AMD 接口调研来源

- Linux `msr-index.h`(AMD RAPL MSR 定义)、turbostat AMD Fam17h RAPL 支持
- k10temp(SMN 0x59800 温度基址)、zenpower(SVI/SMN 遥测逆向)
- Linux 内核 SMN 0x60/0x64 互斥保护补丁系列
- SMUDebugTool(SMU 信箱消息 ID 逆向,PPT/TDC/EDC)
- LIKWID issue #241(AMD 能量单位寄存器定标 quirk)
