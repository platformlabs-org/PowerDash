# PowerDash

Lenovo 小新/IdeaPad 电源工具箱:CPU 遥测采集(HWiNFO 对齐宽表 CSV)、功耗墙设定、电源模式查询与切换。内核驱动以内嵌资源随单文件 exe 分发,按需自动装卸,系统零残留。

## 命令

```
PowerDash                          无参数:显示帮助并驻留 cmd(双击场景)
PowerDash power [秒数] [--csv 文件]
                                   实时面板;可选逐秒保存 CSV(不指定秒数则运行至 Ctrl+C)
PowerDash -setpl <PL1> <PL2>       设定并锁定功耗墙(瓦,仅 Intel)
PowerDash mode status              查询当前电源档位与固件能力档(无需本驱动)
PowerDash mode next                注入 Fn+Q 通知(由 Lenovo 组件切档并弹 OSD)
PowerDash -h                       帮助
```

`--csv` 与秒数可互换位置。加载内核驱动需管理员权限。

## 调试 / 取证命令(隐藏)

```
PowerDash --smndbg <hexaddr>            读一个 SMN 寄存器(连读两次)
PowerDash --msrdbg <core> <hexmsr>      读一个 per-core MSR(间隔 1s 连读两次)
PowerDash --pcidbg <b> <d> <f> <reg> [val]
                                        读(或写后读)一个 PCI 配置 dword
PowerDash --pmdump [file]               SMU PMTable 握手诊断 + 全表浮点导出
PowerDash --pmscan [start end]          物理内存 PMTable 特征扫描
PowerDash --pmxfer [tableId]            SMU 传输消息表选择子取证
```

## CSV 格式(v3,HWiNFO 对齐)

表头 `Date,Time,"Elapsed [s]","Power Mode",<HWiNFO 传感器名>...`;Date=`d.m.yyyy`、Time=`h:mm:ss.fff`;布尔列输出 `Yes/No`;无效读数写空单元格(不是 0);列集按平台与拓扑动态生成,不可采的域整列省略。`Elapsed [s]` 与 `Power Mode` 为本工具扩展列。

列组(按平台自动选择):

| 平台 | 列组 |
|---|---|
| Intel | 每核 Clock/VID、每线程 Effective/Usage/Utility、每核温度/距 TjMax/降频三态位、封装与每核 C-state 驻留、PL1/PL2(静态/动态)、cTDP、IA/GT/Ring Limit Reasons、功率域(PKG/IA/GT/PSYS) |
| AMD | 每核 Clock/VID、每线程 Effective/Usage/Utility、每核 C0 驻留、Tctl、逐核功率、EPP、功率域(PKG/每核) |

核型命名:P-core/E-core/Zen5/Zen5c(按 Windows EfficiencyClass,编号为顺序核索引)。

与 HWiNFO 采样 CSV 逐列比对:

```
python tools/compare-hwinfo.py <hwinfo.csv> <ours.csv>
```

## 构建

VS2022 + WDK 10。常规构建(驱动产物未签名):

```
build.cmd            :: 全量(驱动 + exe)
build-sys.cmd [版本] :: 仅驱动
build-exe.cmd        :: 仅 exe(嵌入 x64\Release\PowerDashSYS\PowerDash.sys)
```

分发签名流程(attestation 证书,加载无需 testsigning):

```
1. build-sys.cmd 产出未签名驱动
2. 提交微软 attestation 签名
3. use-signed.cmd <已签名 PowerDash.sys 路径>   :: 拷入嵌入位 + 重编 exe + 校验一致
4. python verify_embed.py                       :: RESULT: MATCH
```

开发自用可用 `sign-test.cmd` 本地测试证书签名(需 `bcdedit /set testsigning on` 并重启)。

驱动亦可经 `package.cmd` 打成 INF 驱动包(`package\` 目录),`pnputil /add-driver package\PowerDash.inf /install` 安装为常驻服务。

## 支持状态

- Intel:完整列集(RAPL/DTS/驻留/PLx/Limit Reasons,已实机验证)
- AMD:MSR/SMN 域完整列组;SMU PMTable 协议已通(握手/版本/传输),深层字段(STAPM/TDC/SVI3/逐核温度等)待表布局映射后补列,期间该组列诚实输出空单元格
- 模式切换仅支持 Lenovo(EnergyDrv/DYTC 通道);`-setpl` 仅 Intel(MCHBAR 路径)
