# HWiNFO 级 CPU 遥测采集设计(v3 宽表)

日期:2026-09-03
状态:已评审(方案与七节设计均经用户确认;三项决策见第 1.2 节)
前置:2026-09-01-powerdash-architecture-design.md(四层架构与 Reading 类型体系不变,本文在其上扩展)

## 1. 背景与目标

PowerDash v2 采集 21 列固定宽表(pkg_w/cores_w/temp_c/...),字段广度远低于 HWiNFO。
用户在两台实测机(Lunar Lake `labs` 本机 / Krackan Point `labs-tb16g7` 远程)用 HWiNFO
采集了样本 CSV(`\\nas\labs\TEST\hwinfo-smaple\{amd,intel}.CSV`,271/411 列),要求:

1. CPU 相关数据的**广度与命名**尽可能贴合 HWiNFO(其余域——GPU/存储/网络/电池——后续再议)
2. 采集 CSV 的数据格式以 HWiNFO 为准
3. 代码质量与数据准确度优先(有公开数据源依据的实现,无据字段诚实省略)

### 1.1 已核实的公开数据源(实现依据)

| 数据 | Intel | AMD(Zen5 mobile) |
|---|---|---|
| 每核频率 | MSR 0x198 EAX[15:8] ratio × busClock(LibreHardwareMonitor IntelCpu.cs 同式, Nehalem+ 位布局) | MSR 0xC0010293:family 0x1A fid[11:0]×5 MHz;更老 fid[7:0]/dfsId[13:8]×200(LHM Amd17Cpu.cs) |
| 每核 VID | MSR 0x198 EDX[15:0] / 8192 V(LHM 同式) | MSR 0xC0010293 [21:14] → 1.550 − 0.00625×vid(LHM/zenpower 同式) |
| 有效频率(每线程) | ΔAPERF/Δt(周期计数/秒 = 平均有效 MHz, 含 C-state 在内摊薄) | 同式;读 RO 别名 MSR 0xC00000E7/E8 防复位语义 |
| 频率节流调整(AMD) | — | LHM:if ΔAPERF<ΔMPERF: clock ×= ΔA/ΔM |
| Bus Clock | 实测 TSC 频率 ÷ MSR 0xCE[15:8](最大非睿频倍频) | 实测 TSC ÷ P0 倍频(P0MHz/100) |
| 每核温度/距 TjMax | MSR 0x19C bit31 有效 + [22:16] readout;TjMax = 0x1A2[23:16](LHM 同式) | PMTable(本期不映射,见 1.2) |
| 封装温度/封装三态位 | MSR 0x1B1(thermal/critical/power-limit log 位) | — |
| 每核降频三态位 | MSR 0x19C log 位 1/5/11 | — |
| 功率域 | PKG 0x611 / IA 0x639 / GT 0x641 / PSYS 0x64D(v2 已有) | PKG 0xC001029B / 每核 0xC001029A(v2 已有) |
| PL1/PL2 | 静态 MSR 0x610 / 动态 MMIO 0x59A0(v2 已有动态) | PMTable STAPM/FPPT/SPPT(已知偏移) |
| cTDP 档 | MSR 0x64B [1:0] | — |
| 封装 C 驻留 | C2 0x60D / C3 0x3F8 / C6 0x3F9 / C8 0x630 / C10 0x632,÷ΔTSC | — |
| 每核 C 驻留 | C1 0x660 / C6 0x3FD / C7 0x3FE(按核探测可读性);C0 = 100−C1 | C0 = ΔMPERF/ΔTSC(turbostat Busy% 同式) |
| Limit Reasons | MSR 0x64F(IA)/0x650(GT)/0x651(Ring),log 位 [26:16] 共 11 位 | — |
| On-Demand Clock Modulation | MSR 0x19A(bit4 使能 + [3:0] 占空) | — |
| EPP | —(HWiNFO intel.CSV 无此列) | MSR 0xC00102B3[31:24] → %(AMD CPPC_REQ,PPR) |
| Uncore Ratio | MSR 0x620(语义实机核对后启用,否则省略) | — |
| 每线程 Usage/Max/Total | NtQuerySystemInformation(SystemProcessorPerformanceInformation) 差分 | 同式 |
| Utility | Usage × (ΔAPERF/ΔMPERF) 钳 0-100(微软 % Processor Utility 频率加权语义) | 同式 |
| 核型命名 | GetLogicalProcessorInformationEx(RelationProcessorCore).EfficiencyClass:0→"P-core"、1→"E-core"、≥2→"E-core (LP)" | 同源:class 0→"Zen5"、1→"Zen5c"(family 0x1A);单类回退"Core" |
| PMTable(AMD) | — | PSMU 邮箱 SMN 0x3B10a20(msg)/0x3B10a80(rep)/0x3B10a88+(args);消息:0x1 测试、0x6 版本、0x66 地址(arg0 lo,arg1 hi)、0x65 每帧刷新;物理内存映射读 float 表(ryzenAdj nb_smu_ops.c/api.c 同协议) |
| PMTable 已知偏移(Krackan 0x00650005) | — | 0x00/0x04 STAPM 限值/实际、0x08/0x0C FPPT、0x10/0x14 SPPT、0x18/0x1C APU-SPPT、0x30/0x34 TDC 限值/实际(A)、0x38/0x3C SoC VRM 电流限值/实际(A)、0x40/0x44 Tctl 限值/实际(°C)、0x90C/0x910 STAPM/slow 时间窗(s)(ryzenAdj api.c)。注意:核心 EDC(HWiNFO "CPU EDC [A]")偏移未知,不在本期列集 |
| AMD 其他温度 | — | Tctl SMN 0x59800 k10temp 语义(v2 已有);CCD/L3/SVI3/逐核温度 = PMTable 未知偏移(下期) |

无公开数据源、本期省略的 HWiNFO 字段(README 维护完整表):Ring/LLC Clock、Uncore VID、
VR VCC Current (SVID IOUT)、VccCLK/VccDDQ/VccIOG、System Agent/Rest-of-Chip/PCH Power、
CPU IA Cores / GT Cores 温度、Gear Mode 及内存时序组(内存域另期)、AMD 侧 SVI3 TFN 全组、
FCLK/UCLK/L3 Cache 时钟、逐核温度/C1/C6 驻留、DRAM 带宽、Average Active Core Count、
Frequency Limit - Global、AMD 降频标志(HTC/PROCHOT)——后四类待 PMTable 映射后补列。

### 1.2 三项已确认决策

1. **CSV 直接替换为 HWiNFO 格式**,v2 21 列宽表移除(不留双格式开关)
2. **AMD PMTable 分两期**:本期交付 SMU 邮箱基础设施 + 已知偏移 + `--pmdump` 导出工具;
   未知偏移(逐核温度/SVI3/FCLK/C1C6 等)待实机 dump 与 HWiNFO 比对后作为独立任务映射
3. **无公开数据源的字段整列省略**(列集随平台/机型动态生成,同 HWiNFO 行为)

### 1.3 数据模型决策

SensorTable 为**主数据源**,Sample(UI 模型)从表内规范键派生——同一物理量单条写路径,
CSV 与面板永不漂移。探针接口只新增 `sensors()` 访问器。

## 2. 模块与数据流

```
PowerDash.cpp           入口编排不变;CSV 写出改用宽表
  └─ CreateProbe()
       ├─ PowerDashIntel.cpp / PowerDashAmd.cpp
       │    ctor:拓扑+能力探测 → 构建列集(SensorTable schema)
       │    readSample:MSR/SMN/PMTable → SensorTable(先) → Sample 派生(后)
       ▼
PowerDashSensors.h/.cpp  SensorColumn{key,name(HWiNFO 全名含单位),fmt}
                         SensorTable{Add/Set/Find/大小固定后仅 Set}
                         Sample 派生键常量表(pkgW↔"CPU Package Power" 等)
PowerDashUsage.h/.cpp    NtQuerySystemInformation 逐 LP 差分(Usage/Max/Total)
PowerDashUi.*            RenderDashboard 不变(吃 Sample);CsvHeader/CsvRow(v2)删除
Csv 写出(新)            HWiNFO 格式:Date/Time/Elapsed [s]/Power Mode + 宽表
PowerDashSYS             新增 IO_CTL_SMN_WRITE(0x80C)
```

### 2.1 SensorTable 契约

```cpp
enum class SensorFmt { F1, F2, F3, PCT1, PCT2, RATIO2, YESNO, TEXT };
struct SensorColumn { std::string key;   // 规范键(代码内稳定标识,如 "core.3.clock")
                      std::string name;  // CSV 列名 = HWiNFO 全名,如 "E-core 3 Clock [MHz]"
                      SensorFmt fmt; };
class SensorTable {
  unsigned Add(key, name, fmt);            // 仅构造期调用
  void Set(unsigned idx, Reading r);       // 每帧;invalid → 空单元格
  const Reading& Get(unsigned idx) const;
  int Find(const char* key) const;         // -1 = 该平台无此列
};
```

派生规则(两平台探针共用,放 PowerDashSensors):`Sample.pkgW = table.Get(Find("pkg.power"))`
等;探针负责在构造期注册全部派生键或显式声明缺失(键缺失 → Sample 字段 NA,UI 按既有
caps 逻辑降级,渲染层零改动)。

## 3. CSV 格式规范(v3,HWiNFO 对齐)

- 表头:`Date,Time,"Elapsed [s]","Power Mode","<HWiNFO 列名>",...`
  - `Elapsed [s]`/`Power Mode` 为 PowerDash 自有扩展(项目使命需要档位关联),紧跟 Time,
    README 注明与 HWiNFO 原始格式的差异
  - 列名含逗号/单位后缀,一律双引号包裹(同 HWiNFO)
- Date = `d.m.yyyy`,Time = `h:mm:ss.fff`(本地时间,同 HWiNFO 观测格式)
- 布尔列 fmt=YESNO → `Yes`/`No`;无效读数 → 空单元格(延续 v2 语义,HWiNFO 同样留空)
- 数值列小数位:F1(时钟/温度整位带 1 位)、F2(Ratio 2 位)、F3(V/W 3 位)、PCT(1-2 位)
- 列序:电压组 → 时钟组 → 有效时钟组 → Usage 组 → Utility 组 → Ratio 组 → 温度组 →
  降频组 → 功率组 → 限值组 → 驻留组 → Limit Reasons 组(对齐两份样本 CSV 的组序)

## 4. 平台采集清单(本期交付列集)

### 4.1 Intel(实测机 Lunar Lake,8 核:4P/6E/2LP-E 布局按拓扑动态命名)

| 组 | 列(静态) | 列(每核/线程,动态) |
|---|---|---|
| 电压 | Core Voltages (avg) [V] | "<P/E/E (LP)-core n> Voltage [V]"(0x198 EDX/8192) |
| 时钟 | Core Clocks (avg) / Bus Clock [MHz] | 每核 Clock(0x198 ratio×bus) |
| 有效时钟 | Core Effective Clocks (avg) / Average Effective Clock [MHz] | 每核 Effective(ΔAPERF/Δt) |
| Usage | Core Usage (avg) / Max CPU/Thread Usage / Total CPU Usage / On-Demand Clock Modulation [%] | 每核 Usage |
| Utility | Core Utility (avg) / Total CPU Utility [%] | 每核 Utility |
| Ratio | Core Ratios (avg) [x](+Uncore Ratio 实机核对后) | 每核 Ratio(=clock/bus) |
| 温度 | Core Temperatures (avg) / Core Distance to TjMAX (avg) / CPU Package / Core Max [°C] | 每核温度、每核 Distance to TjMAX |
| 降频 | Package/Ring Thermal Throttling / Critical Temperature / Power Limit Exceeded [Yes/No] | 每核 Thermal Throttling / Critical Temperature / Power Limit Exceeded(log 位)+ (avg) |
| 功率 | CPU Package Power / IA Cores Power / GT Cores Power / Total System Power [W] | — |
| 限值 | PL1/PL2 Power Limit (Static)/(Dynamic) [W] / Current cTDP Level [] | — |
| 封装驻留 | Package C2/C3/C6/C8/C10 Residency [%] | — |
| 核驻留 | Core C0/C1/C6/C7 Residency (avg) [%] | 每核 C0(=100−C1)/C1(0x660)/C6(0x3FD)/C7(0x3FE),按核可读性。驻留列为计数器原值占比(嵌套包含:C1 含 C6,C6 含 C7,turbostat 同式);Intel 每核列按代表 LP 读取(SMT 线程级如需再扩展) |
| Limit Reasons | IA/GT/Ring Limit Reasons (avg) [Yes/No] + 各 11 位事件列(0x64F/0x650/0x651 log 位) | — |

### 4.2 AMD(实测机 Krackan Point,8C/16T 混合 Zen5+Zen5c)

| 组 | 来源 |
|---|---|
| Core VIDs (avg) + 每核 VID [V] | 0xC0010293[21:14] → 1.55−0.00625×vid |
| Core Clocks (avg) + 每核 Clock [MHz] + Bus Clock | 0xC0010293 Zen5 fid×5(+ΔA/ΔM 节流调整);bus=实测TSC÷P0 倍频 |
| 每线程 T0/T1 Effective Clock + (avg) + Average Effective | ΔAPERF/Δt(RO 别名) |
| 每线程 T0/T1 Usage + Max + Total | NtQuerySystemInformation |
| 每线程 Utility + (avg) + Total | Usage×ΔA/ΔM |
| 每核 Ratio [x] + (avg) | clock÷bus |
| 每核 C0 Residency + (avg) | ΔMPERF/ΔTSC |
| CPU (Tctl/Tdie) [°C] | SMN 0x59800 k10temp(v2 逻辑迁入宽表) |
| CPU Package Power [W] / Core Powers (avg) + 每核 Power [W] | 0xC001029B / 0xC001029A(v2 逻辑) |
| APU STAPM [W] + STAPM/FPPT/SPPT/APU-SPPT 限值与实际、TDC [A] + TDC Limit [%]、SoC VRM 电流限值/实际、Tctl 限值 [°C]、STAPM/slow 时间窗、Thermal Limit [%](实际/限值) | PMTable 已知偏移(1.1 表;核心 EDC 偏移未知,省略,验证节核对 SoC 电流与 HWiNFO "SoC Current (SVI3 TFN)" 的对应性) |
| Energy Performance Preference [%] | 0xC00102B3[31:24]/2.55 |
| TDC/STAPM/FPPT/SPPT/Thermal Limit [%] | PMTable 实际÷限值×100(已知偏移范围内;EDC 省略) |

命名:`Zen5 Core n`/`Zen5c Core n`(EfficiencyClass 0/1,family 0x1A);非混合回退 `Core n`。
HWiNFO 的 "(perf #N)" 内部计数器后缀不复刻(README 注明)。

## 5. 驱动变更

`IO_CTL_SMN_WRITE`(0x80C,与 SMN_READ 对称):同缓冲 `SMN_Request{address,value}`,
内核侧 smnMutex 内写 PCI 0x60(地址)→ 写 PCI 0x64(数据);`WindowsDriverIo::WriteSmn`
用户态对称封装。SMU 邮箱轮询响应(忙等上限 + 超时失败)在用户态探针内实现。

## 6. 测试策略

1. FixtureDriverIo 扩展:MSR/SMN 请求-应答表 + MapPhys 假内存(PMTable 浮点表注入)
2. 解码单测:两平台 VID 换算、Zen5/Zen4 fid 解码、ΔA/ΔM 节流调整、TjMax 差值温度、
   log 位→Yes/No 映射(0x19C/0x1B1/0x64F 族)、能量 32 位回绕、驻留 ÷ΔTSC、
   C0=100−C1、EPP%、PMTable 浮点偏移提取、STAPM% 计算
3. CSV 单测:表头引号/组序稳定、Yes/No、空单元格、小数位、Date/Time 格式
4. 一致性单测:Sample 派生值 == SensorTable 同键值;UI 渲染对派生 Sample 的行为不回退
5. 既有 v2 CSV 断言删除,替换为 v3 断言

## 7. 验证与验收

1. 本机 Intel:`PowerDash power 30 --csv intel_v3.csv` → 与 `intel.CSV` 列交集逐列比对
   (脚本化:温度 ≤2°C、功率 ≤10% 或 0.5W、时钟 idle 态差 ≤5%、Usage ≤3pp;结构性差异说明)
2. 远程 labs-tb16g7(remote-tb.ps1):同上比对 `amd.CSV`;PMTable 版本断言 0x00650005;
   STAPM/TDC/Tctl-限值与 HWiNFO 对应列一致;`--pmdump` 全表导出存档(nas)
3. 回归:面板正常渲染、连续 5 帧失败熔断、Ctrl+C/秒数到期干净退出、驱动零残留
4. 准确度红线:凡无把握的列宁可省略(空列也不写假值);每列在 README 映射表标注数据源

## 8. 明确非目标(本期)

- PMTable 未知偏移映射(逐核温度/SVI3 TFN/FCLK/UCLK/L3/C1C6/DRAM 带宽/平均活跃核数/
  频率限值/降频标志)——基础设施与 dump 工具就绪后独立任务
- GPU/NPU/存储/网络/电池/内存时序域
- AMD -setpl(SMU 写限值);Intel OC Mailbox(0x150)探测
- HWiNFO 的 "(perf #N)" 计数器注记与同值异源重复列(如两个 CPU Package [°C])的复刻
