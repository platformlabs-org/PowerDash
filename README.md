# PowerDash

Lenovo 小新/IdeaPad（83NC 一代）电源工具箱：功率监视、功耗墙设定、
电源模式切换（含 Fn+Q OSD 合成）。**单文件分发**——内核驱动以资源形式
内嵌在 exe 里，运行时自动安装/卸载，系统零残留。

## 功能

```
PowerDash                          双击/空参数：清屏置顶后打印使用说明，并驻留一个 cmd（已 cd 到 exe 目录）；每次运行都会清屏置顶并显示 CPU 信息（型号+代号）
PowerDash power [秒数] [--csv 文件]
                                  分组式实时功率面板；可同时逐秒保存完整采样 CSV
PowerDash -setpl <PL1> <PL2>       设定并锁定功耗墙（W）
PowerDash mode status              查询电源档位（无需本驱动）
PowerDash mode next                仅注入 Fn+Q 通知，由 Lenovo 组件切档并弹 OSD
PowerDash -h                       帮助
```

帮助页与无参数启动显示相同的三行 PowerDash ASCII Art。实时面板优先使用 96 列，
在 72–91 列窗口自动改为单列诊断布局；SYSTEM POWER 区以平台总功率为标题，
Pkg 与 Rest-of-System（PSYS − PKG，封装外的内存/板级损耗）双内联功率条
分解它（两者之和恒等于标题值），共享同一刻度（0 → PL2，竖线标记 PL1），
颜色区分正常、警告和危险状态，历史区显示最近 60 秒的 Min/Avg/Max。
非 VT 输出自动移除颜色。

`--csv` 可与秒数交换位置，例如 `PowerDash power --csv capture.csv 60`。
文件在启动时创建或覆盖；不指定秒数时持续记录到 Ctrl+C。CSV 为 v2 宽表
（union 列 + `platform` 列）：无效读数（平台不支持或本帧读取失败）写**空单元格**
（不是 0），`limit_locked` 为 0/1，`platform ∈ {intel, amd}`，列名单位后缀化：

```
timestamp,elapsed_s,platform,pkg_w,cores_w,gfx_w,platform_w,
limit_sustained_w,limit_sustained_window_s,limit_burst_w,limit_locked,
tdc_a,edc_a,temp_c,freq_ghz,util_pct,c0_pct,c2_pct,c6_pct,smi_delta,mode
```

v1 → v2 迁移表：

| v1 列 | v2 列 |
|---|---|
| pkg_w | pkg_w(不变) |
| ia_w | cores_w |
| gt_w | gfx_w |
| sys_w | platform_w |
| pl1_w / pl2_w | limit_sustained_w / limit_burst_w |
| (无) | limit_sustained_window_s, tdc_a, edc_a, platform, limit_locked |
| c0/c2/c6_pct, smi_delta, mode, temp_c, freq_ghz, util_pct | 同名保留 |

## AMD 支持（实验性）

AMD 平台（CPUID `AuthenticAMD` 自动识别，Ryzen 移动 APU）当前支持**监控子集**：

- **支持**：封装/核功率（能量计数器差分）、温度（Tctl）、频率（APERF/MPERF）、
  CPU 利用率、电源模式（mode）。
- **面板自动降级**：顶区标题为 PACKAGE POWER（无 PSYS 总功率），POWER DOMAINS
  只显示 IA 行（无 GT/SYSTEM），温度无 `/ TjMax` 后缀（TjMax 未知时），
  CPU RESIDENCY / SMI / POWER LIMITS 区整体隐藏，功率条刻度回退 60 W spec
  默认值（`spec` 标注）。
- **不支持**：PSYS/GT 功率、C0/C2/C6 驻留率、SMI 计数、功耗墙读取与设定
  （`-setpl` 走 Intel MCHBAR 路径，AMD 上明确拒绝并提示规划中）——SMU PMTable
  限值解码列为实测机到位后的后续任务。
- **CSV v2 语义**：平台不支持的列持续写**空单元格**（不是 0），`platform` 列
  为 `amd`；下游以空单元格区分"平台不支持"与"读数为零"。
- **实测验收前置**：逐核能量计数器（0xC001029A）按逻辑处理器遍历，若实测发现
  其按物理核计数（SMT 双计），需改为仅遍历物理核。

## 单 exe 无感驱动装卸

`PowerDash.exe` 内嵌 `PowerDash.sys`（资源 `IDR_SYS_DRIVER`）。需要内核功能的
命令会在后台自动完成：提取到 `%TEMP%\PowerDashDrv.sys` → 安装服务
`PowerDashSYS` → 启动 → 工作 → 停止 → 删服务 → 删文件。若设备已在运行
（外部手动加载）则直接复用、退出时不动它；若发现本工具先前实例硬杀留下的
孤儿服务（镜像路径匹配），则收养并在退出时回收。全程无需用户操作。

## 组成

| 工程 | 说明 |
|---|---|
| `PowerDashSYS` | 内核驱动：MSR 读写、PCI 配置读写、物理内存映射、PMU 计数器分配、**Fn+Q 通知注入**（`IO_CTL_FNQ_INJECT`） |
| `PowerDash` | 用户态 CLI |

## 架构（分层）

用户态按「入口 → Probe → Sampler → UI/CSV」四层拆分，平台差异收敛在 Probe 层，
下游只消费统一模型：

```
PowerDash.cpp          CLI 入口：参数解析、命令分发、驱动装卸生命周期（瘦身后仅剩编排）
  └─ CreateProbe()（PowerDashProbe.h：IPlatformProbe 接口 + PlatformCaps 能力位）
       ├─ PowerDashIntel.cpp    Intel：RAPL 能量 MSR、MCHBAR PL1/PL2、TjMax、
       │                        C2/C6 驻留、SMI（自旧 RunPowerMonitor 迁出）
       └─ PowerDashAmd.cpp      AMD：能量计数器差分、SMN 温度、APERF/MPERF 频率；
                                能力位诚实降级（无 PSYS/GT/驻留/SMI/限值）
            │ 每秒 readSample(Sample&)
            ▼
       PowerDashSampler.*        采样引擎：节拍、时间戳/elapsed/util/mode 填充（能量差分/回绕在各 Probe，历史窗口在入口 sink）
            │ 统一 Sample v2 模型（PowerDashModel.h：Reading 自带 valid 语义，
            │ 根除「0 是零值还是不支持」歧义；Decompose 供功率条分解）
            ├─ PowerDashUi.*     面板渲染：RenderDashboard 只读统一模型，
            │                   按 PlatformCaps 自动降级（见「AMD 支持」）
            └─ CSV v2 宽表      CsvRow(Vendor, Sample)：union 列 + platform 列，
                                无效读数写空单元格
PowerDashSYS            内核驱动新增 IO_CTL_SMN_READ：AMD SMN 地址/数据寄存器
                        （NB 0x60/0x64）单次 IOCTL 原子互斥读取，取代用户态
                        PCICFG 写→读配对，内核侧序列化防并发交错
```

设计全文（模块边界、类型体系、表述映射表、CSV v2 schema、降级矩阵、测试策略）：
[docs/superpowers/specs/2026-09-01-powerdash-architecture-design.md](docs/superpowers/specs/2026-09-01-powerdash-architecture-design.md)。

**后续任务**：AMD 限值读取（PPT/TDC/EDC，SMU PMTable 解码）待一台 **AMD Lenovo
实机**到位后实施——需实测核对 family 偏移表（PMTable 布局随 CPU 世代变化），
这是当前 AMD 面板 POWER LIMITS 区隐藏、CSV 限值列为空的唯一缺口。

## 构建

VS2022 + WDK 10（本机已含）。命令行：`build.cmd`（Release x64，驱动自动测试签名）。
应用工程引用 PowerDashSYS（先构建驱动）并把 `PowerDash.sys` 嵌入 exe 资源，
产出的 `x64\Release\PowerDash.exe` 即为单文件分发物。
若提示缺少 Spectre 缓解库，工程已配置禁用；如再遇到可加 `/p:SpectreMitigation=false`。

### 分离编译 + 手动签名（推荐分发流程）

**构建永不自动签名**——`PowerDash.sys` 产出为未签名状态，签名永远是你显式的一步：

```bat
build-sys.cmd        :: 1. 只构建驱动（未签名）
signtool sign /v /fd SHA256 /tr <时间戳服务器> /td SHA256 /a x64\Release\PowerDashSYS\PowerDash.sys
                      :: 2. 用你自己的证书签名（attestation/EV 证书同理）
build-exe.cmd        :: 3. 只构建 exe 并嵌入刚签好的驱动
python verify_embed.py
                      :: 4. 校验嵌入的驱动与签名版逐字节一致
```

开发自用可 `sign-test.cmd` 用本地测试证书显式签名（需 testsigning 模式）。

### INF 版本号

INF 的 `DriverVer` 版本已固定（默认 `1.0.0.0`）——默认行为是 WDK 每次构建
盖上时间派生版本（`HH.MM.SS.mmm`），看起来像随机数。指定版本：

```bat
build-sys.cmd 1.2.3        :: DriverVer=日期,1.2.3.0（stampinf 自动补足四段）
build.cmd 1.2.3            :: 全量构建同样支持
```

日期部分仍为构建当天（正式发布惯例），如需完全固定可在 vcxproj 中同时设置
`SpecifyDriverVerDirectiveDate` + `DateStamp`。

`build-exe.cmd` 使用 `/p:BuildProjectReferences=false` 防止 msbuild 重编驱动
覆盖你的签名，并强制删除缓存的 `.res`（MSBuild 不追踪 .rc 内引用的二进制，
不删会嵌入旧驱动）。

驱动加载需要系统开启测试签名（一次性，需重启）；**若用 attestation 证书
签名则无此要求，可直接在任意 Win10/11 上运行**：

```
bcdedit /set testsigning on
```

## INF 安装（primitive 驱动包方式，可选）

INF 为 **primitive driver package**（`PrimitiveDriver=1` + `PnpLockdown=1`，
驱动从 DriverStore（DIRID 13）隔离运行，无 PnP 设备节点）——通过 InfVerif
Desktop 与 Universal 双重验证，适用于正式分发（attestation 签名场景）：

```bat
build-sys.cmd [版本]          :: 未签名驱动
signtool sign ... PowerDash.sys   :: 你的证书
package.cmd                   :: 生成 package\{PowerDash.inf, .sys, .cat}
                                  （inf2cat + cat 测试签名，发布时换你的证书）
```

安装/卸载（纯 pnputil，无设备节点、无幽灵残留）：

```bat
pnputil /add-driver package\PowerDash.inf /install      :: 入库 + 创建服务 powerdash
sc start powerdash                                       :: 启动（按需）
sc stop powerdash
pnputil /delete-driver oem##.inf /uninstall /force       :: 卸载
```

安装后 `PowerDash.exe` 检测到 `\\.\POWERDASH` 已存在会直接复用、退出时不会
卸载；驱动内置双实例保护（后加载实例复用先加载实例的控制设备）。

测试证书注意：DriverStore 入库对目录做完整证书链校验（testsigning 不豁免），
测试机的自签证书需导入 `LocalMachine\Root` 和 `TrustedPublisher`。

## 手动加载驱动（可选）

设备名设计为 `\Driver\POWERDASH`，**服务名不能叫 `PowerDash`**（驱动对象与设备名
大小写不敏感冲突，StartService 报错 6）。自动装卸使用的服务名是 `PowerDashSYS`；
如需手动常驻：

```
sc create PowerDashSYS type= kernel start= demand binPath= "<绝对路径>\PowerDash.sys"
sc start PowerDashSYS
```

## Fn+Q 注入原理（IO_CTL_FNQ_INJECT）

真实按键时 EC 固件改写 DYTC 档位并发出 ACPI 通知；`AcpiVpc.sys` 收到后把
mode2 通知计数器置 1 并击发注册事件，`FnHotkeyUtility` 醒来读"当前档位"弹 OSD，
Lenovo Dispatcher 随后完成实际的档位切换。本驱动的注入 IOCTL 在内核里复刻这
一通知，**这是唯一的模式变更路径**——用户态从不写 DYTC：

```
AcpiVpc devext: mode2 计数器(+0xEC)=1、KeSetEvent 命名事件(+0x58)
  与全部 mode2 注册私有事件（驱动镜像 RVA 0x7C80 数组）
= FnHotkeyUtility 弹 OSD + Dispatcher 推进档位（实测与真实 Fn+Q 一致）
```

偏移针对 `AcpiVpc.sys 15.11.30.11`
（SHA256 `F589BB88137DED8BFEA1F2F741B51EF7F0BD27A4BE51A48978B91D0C29E936FE`，
换版本需重新核对 `powerdash.c` 中的偏移表）。

DYTC 档位只读查询（GET=0x2）：func=(raw>>8)&0xF，mode=(raw>>12)&0xF ——
(0|5,0xF)=智能，(11,2)=高性能，(11,3)=节能。

## 完整逆向档案

全部接口逆向与实机验证记录见
`EM-Driver/analysis/接口分析报告.md`（39 个 EnergyDrv IOCTL、DSDT 分析、
事件架构、注入实验全记录）。
