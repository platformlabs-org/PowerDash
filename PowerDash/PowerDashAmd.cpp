// PowerDashAmd.cpp —— AMD Zen(family 17h/19h/1Ah)探针宽表化(Task 5,v3)。
// SensorTable 为主数据源:ctor 拓扑规范化 + 能力探测 -> 构建 HWiNFO 命名
// 列集(spec §4.2,组序对齐 amd.CSV);readSample 每帧先 SetInvalid 全表
// 再按组回填(读取失败留 NA,绝不残留旧值),Sample 从表内规范键派生
// —— 同一物理量单条写路径,CSV 与面板永不漂移(spec §1.3)。
// 解码依据 spec §1.1(公开数据源,LHM Amd17Cpu/k10temp/turbostat 同式):
//   时钟/VID   0xC0010293(repLP 读):family 0x1A fid[11:0]×5 MHz,更老
//              fid[7:0]/dfsId[13:8]×200 MHz;VID[21:14] -> 1.550−0.00625×vid;
//              ΔAPERF<ΔMPERF(均>0)时 clock ×= ΔA/ΔM(LHM 节流调整)
//   bus        CalibrateTscHz() ÷ (P0MHz/100);tsc/P0 任一失败 -> bus/
//              比率列恒 NA(时钟列不受影响 —— fid 解码不依赖 bus)
//   有效时钟   ΔAPERF×tscHz/ΔTSC(每线程;RO 别名 0xC00000E8/E7 读,
//              免疫 APERF/MPERF 复位语义);C0 = ΔMPERF/ΔTSC×100(核 =
//              线程均值);eff.all = 全线程均值
//   EPP        0xC00102B3[31:24]/2.55(每核读,全局均值单列)
//   Tctl       SMN 0x59800 k10temp 语义(v2 原样迁入宽表)
//   功率       RAPL 能量差分:0xC001029B(pkg)+ 0xC001029A(每核,既有
//              SMT 去重/逐核独立回绕/陈旧核恢复语义原样保留)
//   Usage      NtQuerySystemInformation 差分(真源首 2 帧暖期 NA);
//   Utility    busy×(ΔA/ΔM 钳 [0,4]) 钳 0-100(微软频率加权语义)
// PMTable(Task 6 接入,PowerDashPmTable.h):SMU PSMU 邮箱握手成功且
// 版本 == Krackan 0x00650005 时追加 PM 列组(STAPM/TDC/SoC 电流实际 +
// 限值%),Sample.powerLimit/currentLimit 自 PMTable 偏移派生;握手失败/
// 版本未知 -> 无 PM 列、相关 Sample 字段恒 NA(诚实降级)。
#include "PowerDashProbe.h"
#include "PowerDashPmTable.h"
#include "PowerDashSensors.h"
#include "PowerDashUsage.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <string>
#include <vector>

namespace pd {
namespace {
// AMD RAPL 寄存器族(Zen 17h+,与 Intel RAPL MSR 位兼容 —— msr-index.h
// "MSR_AMD_RAPL_POWER_UNIT ... bit-compatible";turbostat 对 0xC0010299 同样
// 按 energy bits 8-12 解码:rapl_energy_units = ldexp(1, -(msr>>8 & 0x1f)))。
constexpr uint32_t AMD_RAPL_POWER_UNIT = 0xC0010299;
constexpr uint32_t AMD_CORE_ENERGY_STAT = 0xC001029A;   // 每核,32 位回绕
constexpr uint32_t AMD_PKG_ENERGY_STAT = 0xC001029B;    // 32 位回绕
constexpr uint32_t AMD_PSTATE_0 = 0xC0010064;           // P-state 0(P0 标称)
constexpr uint32_t AMD_COFVID_STATUS = 0xC0010293;      // 每核时钟 fid + VID
constexpr uint32_t AMD_CPPC_REQUEST = 0xC00102B3;       // 每核 CPPC 请求(EPP)
// APERF/MPERF 只读别名(LHM Amd17Cpu 同式):0xE7/0xE8 原地址在 AMD 上
// 受性能计数模式位复位语义影响(写 0 清零),RO 视图恒为单调计数器,
// 差分窗口不被破坏。
constexpr uint32_t AMD_RO_APERF = 0xC00000E8, AMD_RO_MPERF = 0xC00000E7;
// SMN THM_TCTL(ZEN_REPORTED_TEMP_CTRL_BASE,Linux k10temp.c:"Common for Zen
// CPU families (Family 17h and 18h and 19h and 1Ah)";经驱动原子
// IO_CTL_SMN_READ 读取,禁止用户态拆写 0x60/0x64)。
constexpr uint32_t SMN_THM_TCTL = 0x59800;
// k10temp 解码:temp_mC = (raw >> 21) * 125;若 RANGE_SEL(bit19)=1 或
// TJ_SEL([17:16])=0b11,再 -49 C(传感器量程扩展偏移)。tb16g7(family
// 0x1A)实测 raw 恒带 bit19(低 16 位为 0,[19:16]=0xB):idle 0x510B0000
// -> 32.0 C、21 W 载荷 0x7D8B0000 -> 76.5 C、52 W -> 92.5 C;旧式无偏移
// 解码在同一台机上读出 81/126/141 C(假值)。17h 上该位通常为 0,故旧
// 解码恰好正确 —— 按 k10temp 语义统一处理,两代皆准。
constexpr uint32_t TEMP_RANGE_SEL = 0x80000u;   // BIT(19)
constexpr uint32_t TEMP_TJ_SEL = 0x30000u;      // GENMASK(17,16)

/* P-state P0(MSR 0xC0010064)CpuFid/CpuDfsId → 标称核心频率 MHz。
 * 世代布局不同(AMD PPR CoreCOF 定义;LibreHardwareMonitor Amd17Cpu.cs
 * 同式实现并引用同名文档):
 *   family 17h/19h(PPR 55570-B1、PPR 19h Model 70h A0):
 *     CpuFid[7:0] / CpuDfsId[13:8] * 200 MHz
 *   family 0x1A(Zen5,PPR 57896-B0):CpuFid 扩为 [11:0](吞并旧 DfsId
 *     位域,无除数):CpuFid[11:0] * 5 MHz
 * AMD 不实现 CPUID 0x16(读 0),这是 AMD 平台 baseGHz 的权威来源。
 * 返回 0 = 解码失败/无 P-state(调用方保持 NA)。 */
double DecodePstateCofMHz(uint32_t pstateLo, unsigned family) {
    const uint32_t fid = family == 0x1A ? (pstateLo & 0xFFFu)
                                        : (pstateLo & 0xFFu);
    if (fid == 0) return 0.0;
    if (family == 0x1A)
        return (double)fid * 5.0;
    const uint32_t dfsId = (pstateLo >> 8) & 0x3Fu;
    if (dfsId == 0) return 0.0;
    return (double)fid / (double)dfsId * 200.0;
}

/* 每核 COFVID 状态(MSR 0xC0010293)低 32 位 → 当前核心频率 MHz。
 * 与 P0 同式分世代解码(LHM Amd17Cpu GetClock 同源);返回 0 = 解码
 * 失败(dfsId=0 等,调用方时钟列留 NA,VID 列不受影响)。VID 在
 * [21:14]:1.550 − 0.00625×vid(LHM/zenpower 同式,255 级满量程)。 */
double DecodeCofVidMHz(uint64_t cofvid, unsigned family) {
    const uint32_t lo = (uint32_t)cofvid;
    if (family == 0x1A) {
        const uint32_t fid = lo & 0xFFFu;
        return fid ? (double)fid * 5.0 : 0.0;
    }
    const uint32_t fid = lo & 0xFFu;
    const uint32_t dfsId = (lo >> 8) & 0x3Fu;
    if (fid == 0 || dfsId == 0) return 0.0;
    return (double)fid / (double)dfsId * 200.0;
}

// 能量定标 quirk 修正表:v1 为空 —— 已知 Zen 家族(17h/19h/1Ah)的
// RAPL_POWER_UNIT 读数即真实定标,寄存器值直接采用;未来发现虚报定标的
// family(对照 Linux intel_rapl/turbostat 的家族特判)时在此登记:
//   constexpr AmdEnergyQuirk kAmdEnergyQuirks[] = { {family, bits}, ... };
// (届时 family 需经 PlatformInfo 传入;MSVC 不允许 0 长数组,空表以
// 条目数常量占位。)
struct AmdEnergyQuirk { uint32_t family; uint8_t energyBits; };
[[maybe_unused]] constexpr size_t kAmdEnergyQuirkCount = 0;

uint8_t DecodeEnergyBits(uint64_t unitRaw) {
    return static_cast<uint8_t>((unitRaw >> 8) & 0x1F);   // 同 Intel 位布局
}

// HWiNFO 核名(spec §4.2):family 0x1A 混合拓扑 effClass 1→"Zen5 Core n"、
// 0→"Zen5c Core n";effClass ≥2(非预期)与其他 family(单类)回退
// "Core n"。编号 = 顺序核索引(cores_ 向量 0 基位置)—— 实测 amd.CSV
// (Krackan Point 8C/16T)核列为 "Zen5 Core 0/Zen5c Core 1/Zen5 Core 2/
// Zen5c Core 3/…"(顺序物理核索引);repLP 取号在 SMT 机上会跳号
// (代表集 0/2/4/… -> Zen5c Core 2/6/…),与 CSV 不符。Intel 探针同口径
// (LNL 1T/核下 index==repLP,数值不变)。
// 实测口径(2026-09 两台实机取证,测量优先于文档直觉):Krackan 上冲
// 5050 MHz 的核(真 Zen5)携带 class 1,3080 MHz 核(Zen5c)携带 class 0,
// HWiNFO amd.CSV 恰把冲高的偶数位核命名为 "Zen5" —— 与 class 1 = Zen5
// 吻合;Intel 侧 ARL-H 255H CPUID 0x1A 交叉核对同式(1=性能核)。经典
// 文档直觉虽是 0=性能,实测为准。
// "(perf #N)" 内部计数器后缀不复刻。
std::string CoreName(size_t idx, const CoreInfo& c, unsigned family) {
    char buf[48];
    if (family == 0x1A && c.effClass == 1)
        snprintf(buf, sizeof(buf), "Zen5 Core %u", (unsigned)idx);
    else if (family == 0x1A && c.effClass == 0)
        snprintf(buf, sizeof(buf), "Zen5c Core %u", (unsigned)idx);
    else
        snprintf(buf, sizeof(buf), "Core %u", (unsigned)idx);
    return buf;
}

// 均值累加器:avgs/total 列只聚合本帧有效读数(空集 -> 列保持 NA)
struct Mean {
    double sum = 0.0;
    unsigned n = 0;
    void Add(double v) { sum += v; ++n; }
    bool Get(double& out) const {
        if (n == 0) return false;
        out = sum / (double)n;
        return true;
    }
};

} // namespace

class AmdProbe : public IPlatformProbe {
public:
    AmdProbe(DriverIo& io, const PlatformInfo& info) : io_(io) {
        caps_ = BuildCaps(info);       // vendor/name/nLP/baseGHz(P0 随后回填)
        NormalizeTopology(info);       // cores 空/越界 -> 每 LP 合成单线程核
        family_ = info.family;
        uint64_t u = 0;
        if (io_.ReadMsr(0, AMD_RAPL_POWER_UNIT, u)) {
            energyUnit_ = 1.0 / std::pow(2.0, DecodeEnergyBits(u));
            unitsOk_ = true;
        }
        /* P0 → baseGHz caps(既有行为)+ busClock = 实测 TSC ÷ (P0MHz/100)
         * (spec §1.1 LHM 同式)。tsc 校准/P0 解码任一失败 -> bus=0,
         * bus/比率列恒 NA;时钟列照常(family 0x1A fid×5 与旧式
         * fid/dfsId×200 均不依赖 bus,不做名义 100 MHz 兜底)。 */
        double p0MHz = 0.0;
        uint64_t p0 = 0;
        if (io_.ReadMsr(0, AMD_PSTATE_0, p0)) {
            p0MHz = DecodePstateCofMHz((uint32_t)p0, family_);
            if (p0MHz > 0.0) caps_.baseGHz = p0MHz / 1000.0;
        }
        tscHz_ = TscCalibrationOverride ? TscCalibrationOverride()
                                        : CalibrateTscHz();
        if (tscHz_ > 0.0 && p0MHz > 0.0)
            busClock_ = tscHz_ / (p0MHz / 100.0) / 1e6;   // MHz
        ProbeCores();      // 每核 0xC0010293/0xC00102B3 可读性(失败不建列/不读)
        usage_ = std::make_unique<UsageMonitor>(
            std::make_unique<NtUsageSource>(nLP_));
        BuildColumns();    // HWiNFO 命名列集(键表见 task-5-brief Interfaces)
        ReadBaseline();    // pkg + 每核能量基线 + 逐线程 RO A/M + TSC 锚点
        /* PMTable(Task 6):SMU PSMU 邮箱握手 + 首刷新(TryCreate 内完成)。
         * 失败 -> nullptr 诚实降级(无 PM 列、Sample 恒 NA、powerLimits
         * caps 不置);版本 != Krackan 0x00650005 -> 偏移不可信,同样不
         * 建列,仅留 pmVersionMismatch_ 记号(Task 10 实机核对后扩展)。 */
        pm_ = SmuPmTable::TryCreate(io_);
        if (pm_ && pm_->version() == KpPm::kVersion) {
            pmKnown_ = true;
            BuildPmColumns();          // 追加在主列组之后(amd.CSV 序)
            caps_.powerLimits = true;  // UI POWER LIMITS 区(Intel MapPlWindow 同口径)
        } else if (pm_) {
            pmVersionMismatch_ = true;
        }
    }

    const PlatformCaps& caps() const override { return caps_; }
    SensorTable* sensors() override { return &table_; }

    bool readSample(Sample& s) override {
        if (!unitsOk_) return false;

        /* (0) 帧默认 NA:整表置无效,成功读取逐一回填(失败列不残留旧值)。 */
        for (unsigned i = 0; i < table_.Count(); ++i) table_.SetInvalid(i);
        const uint64_t tsc = __rdtsc();
        const double dtsc = (double)(tsc - prevTsc_);
        prevTsc_ = tsc;
        for (LpState& L : lp_) L.diffOk = false;   // 节流调整的 ΔA/ΔM 失效
        bool anyPower = false;
        double v = 0.0;

        /* (a) pkg 能量差分 + 32 位回绕钳制(既有公式)。
         * 与源一致按 1 s 采样窗口把能量差直接当功率数值。 */
        uint64_t curr = 0;
        if (io_.ReadMsr(0, AMD_PKG_ENERGY_STAT, curr)) {
            curr &= 0xFFFFFFFFull;
            if (curr < prevPkg_) curr += (1ULL << 32);
            table_.Set(idxPowerPkg_, Ok((curr - prevPkg_) * energyUnit_));
            prevPkg_ = curr;
            anyPower = true;
        }

        /* (b) cores:逐物理核读 0xC001029A(每核一个代表 LP —— SMT 兄弟
         * 共享同一计数器,遍历全部 nLP 会双计,tb16g7 实测 IA 一度读出
         * PKG 的 167%,见 NormalizeTopology 注释),各核独立差分(独立
         * 回绕钳制)。域语义(v2 原样):本帧任一核读取失败 -> 整域 NA
         * (不输出残缺和,防静默少计);失败核标记 stale,恢复帧只刷新
         * 基线、跳过一次差分(陈旧 prev 直接差分会把两帧能量算进一个
         * 采样窗口,造成单帧尖峰)。值先算后发布(域判定要等整圈读完)。 */
        {
            bool anyCore = false, anyFailed = false;
            std::vector<Reading> pw(cores_.size());
            for (size_t i = 0; i < cores_.size(); ++i) {
                uint64_t e = 0;
                if (!io_.ReadMsr(cores_[i].repLP, AMD_CORE_ENERGY_STAT, e)) {
                    staleCore_[i] = true;   // 下次成功读取只重置基线
                    anyFailed = true;
                    continue;
                }
                e &= 0xFFFFFFFFull;
                if (staleCore_[i]) {
                    staleCore_[i] = false;  // 恢复:刷新基线,跳过本帧差分
                    prevCoreE_[i] = e;
                    continue;
                }
                if (e < prevCoreE_[i]) e += (1ULL << 32);
                pw[i] = Ok((e - prevCoreE_[i]) * energyUnit_);
                prevCoreE_[i] = e;
                anyCore = true;
            }
            if (!anyFailed && anyCore) {
                Mean avg;                    // (avg) = 帧内已差分核的均值
                for (size_t i = 0; i < cores_.size(); ++i)
                    if (pw[i].valid) {
                        table_.Set(coreSt_[i].idxPower, pw[i]);
                        avg.Add(pw[i].value);
                    }
                if (avg.Get(v)) table_.Set(idxPowerCoreAvg_, Ok(v));
                anyPower = true;
            }
        }

        /* (c) 逐线程 RO APERF/MPERF 差分(ctor 基线,读取失败保持旧基线,
         * 首个成功帧只建基线)-> 有效时钟/Usage/Utility/C0:
         * eff = ΔAPERF×tscHz/ΔTSC/1e6(tscHz 为 Hz,列单位 MHz)、
         * C0 = ΔMPERF/ΔTSC×100(turbostat Busy% 同式,核值 = 线程均值);
         * Usage 来自 UsageMonitor(真源首 2 帧暖期 ok=false -> NA);
         * utility = busy×(ΔA/ΔM 钳 [0,4]) 钳 0-100。ΔA/ΔM 留存 lp_
         * 供 (d) 节流调整。 */
        const LpUsage lu = usage_->Read();
        std::vector<Mean> effCore(cores_.size()), usageCore(cores_.size()),
            utilCore(cores_.size()), c0Core(cores_.size());
        Mean effAll, utilAll;
        for (size_t i = 0; i < cores_.size(); ++i) {
            for (size_t t = 0; t < cores_[i].threads.size(); ++t) {
                const unsigned lp = cores_[i].threads[t];
                if (lp >= nLP_ || lp >= lp_.size()) continue;   // 越界防线
                LpState& L = lp_[lp];
                uint64_t a = 0, m = 0;
                if (!io_.ReadMsr(lp, AMD_RO_APERF, a) ||
                    !io_.ReadMsr(lp, AMD_RO_MPERF, m))
                    continue;                       // 一对完整才有差分窗口
                if (!L.amBase) {                    // 首个成功帧只建基线
                    L.prevAperf = a;
                    L.prevMperf = m;
                    L.amBase = true;
                    continue;
                }
                L.dA = (double)(a - L.prevAperf);
                L.dM = (double)(m - L.prevMperf);
                L.prevAperf = a;
                L.prevMperf = m;
                if (dtsc <= 0.0) continue;
                L.diffOk = true;
                /* eff 依赖 tscHz:校准失败(tscHz_=0)时整族 eff 列保持
                 * NA(帧首已置无效)—— 不把 0×ΔA 的假 0 MHz 当有效值,
                 * 与 bus/比率列的守卫同口径;C0/节流(ΔA/ΔM 比值)不
                 * 依赖 tsc,照常出。 */
                if (tscHz_ > 0.0) {
                    const double eff = L.dA * tscHz_ / dtsc / 1e6;
                    table_.Set(L.idxEff, Ok(eff));
                    effCore[i].Add(eff);
                    effAll.Add(eff);
                }
                c0Core[i].Add(L.dM / dtsc * 100.0);
                if (lu.ok && lp < lu.busyPct.size()) {
                    table_.Set(L.idxUsage, Ok(lu.busyPct[lp]));
                    usageCore[i].Add(lu.busyPct[lp]);
                    if (L.dM > 0.0) {
                        double r = L.dA / L.dM;
                        if (r < 0.0) r = 0.0;
                        if (r > 4.0) r = 4.0;
                        double util = lu.busyPct[lp] * r;
                        if (util < 0.0) util = 0.0;
                        if (util > 100.0) util = 100.0;
                        table_.Set(L.idxUtil, Ok(util));
                        utilCore[i].Add(util);
                        utilAll.Add(util);
                    }
                }
            }
            if (c0Core[i].Get(v)) table_.Set(coreSt_[i].idxC0, Ok(v));
        }
        if (lu.ok) {
            table_.Set(idxUsageMax_, Ok(lu.maxPct));
            table_.Set(idxUsageTotal_, Ok(lu.totalPct));
        }
        Mean effAvgC, usageAvgC, utilAvgC, c0Avg;   // avg 列 = 核均值的均值
        for (size_t i = 0; i < cores_.size(); ++i) {
            double cv = 0.0;
            if (effCore[i].Get(cv)) effAvgC.Add(cv);
            if (usageCore[i].Get(cv)) usageAvgC.Add(cv);
            if (utilCore[i].Get(cv)) utilAvgC.Add(cv);
            if (c0Core[i].Get(cv)) c0Avg.Add(cv);
        }
        if (effAvgC.Get(v)) table_.Set(idxAvgEff_, Ok(v));
        if (effAll.Get(v)) table_.Set(idxAllEff_, Ok(v));
        if (usageAvgC.Get(v)) table_.Set(idxAvgUsage_, Ok(v));
        if (utilAvgC.Get(v)) table_.Set(idxAvgUtil_, Ok(v));
        if (utilAll.Get(v)) table_.Set(idxTotalUtil_, Ok(v));
        if (c0Avg.Get(v)) table_.Set(idxAvgC0_, Ok(v));

        /* (d) 每核时钟/VID/比率 —— 0xC0010293(repLP):世代分式 fid 解码
         * + VID[21:14];本核 repLP 线程 ΔAPERF<ΔMPERF(均>0)时按 LHM
         * 节流调整 clock ×= ΔA/ΔM;bus 未知 -> 比率列 NA(时钟列不依赖
         * bus)。vid 与 clock 独立回填(解码失败只废时钟)。 */
        Mean clockAvg, vidAvg, ratioAvg;
        if (busClock_ > 0.0) table_.Set(idxClockBus_, Ok(busClock_));
        for (size_t i = 0; i < cores_.size(); ++i) {
            CoreState& cs = coreSt_[i];
            if (!cs.cofvidOk) continue;               // ctor 探测失败无列
            uint64_t cv = 0;
            if (!io_.ReadMsr(cores_[i].repLP, AMD_COFVID_STATUS, cv)) continue;
            const double vid = 1.550 - 0.00625 * (double)((cv >> 14) & 0xFF);
            table_.Set(cs.idxVid, Ok(vid));
            vidAvg.Add(vid);
            double mhz = DecodeCofVidMHz(cv, family_);
            if (mhz <= 0.0) continue;                 // dfsId=0 等解码失败
            const unsigned repLP = cores_[i].repLP;
            if (repLP < lp_.size() && lp_[repLP].diffOk &&
                lp_[repLP].dA > 0.0 && lp_[repLP].dM > 0.0 &&
                lp_[repLP].dA < lp_[repLP].dM)
                mhz *= lp_[repLP].dA / lp_[repLP].dM;
            table_.Set(cs.idxClock, Ok(mhz));
            clockAvg.Add(mhz);
            if (busClock_ > 0.0) {
                const double ratio = mhz / busClock_;
                table_.Set(cs.idxRatio, Ok(ratio));
                ratioAvg.Add(ratio);
            }
        }
        if (clockAvg.Get(v)) table_.Set(idxAvgClock_, Ok(v));
        if (vidAvg.Get(v)) table_.Set(idxAvgVid_, Ok(v));
        if (ratioAvg.Get(v)) table_.Set(idxAvgRatio_, Ok(v));

        /* (e) EPP —— 0xC00102B3[31:24]/2.55(0-255 线性映射 0-100%);
         * 每核读、单条全局均值列;ctor 探测失败的核不读,本帧全失败 ->
         * NA(帧首 SetInvalid 已兜底)。 */
        {
            Mean epp;
            for (size_t i = 0; i < cores_.size(); ++i) {
                if (!coreSt_[i].eppOk) continue;
                uint64_t r = 0;
                if (!io_.ReadMsr(cores_[i].repLP, AMD_CPPC_REQUEST, r))
                    continue;
                epp.Add((double)((r >> 24) & 0xFF) / 2.55);
            }
            if (epp.Get(v)) table_.Set(idxEpp_, Ok(v));
        }

        /* (f) Tctl —— SMN THM_TCTL,k10temp 语义(见常量区注解):
         * (raw >> 21) * 0.125 C,RANGE_SEL/TJ_SEL=11 时再 -49 C;失败 -> NA。 */
        {
            uint32_t raw = 0;
            if (io_.ReadSmn(SMN_THM_TCTL, raw)) {
                double tC = (double)(raw >> 21) * 0.125;
                if ((raw & TEMP_RANGE_SEL) || (raw & TEMP_TJ_SEL) == TEMP_TJ_SEL)
                    tC -= 49.0;
                table_.Set(idxTempTctl_, Ok(tC));
            }
        }

        /* (f2) PMTable —— transfer(0x65)成功后按 Krackan 已知偏移回填;
         * 刷新失败/偏移越界(At 返 NaN)-> 本帧 PM 列全 NA(帧首
         * SetInvalid 已兜底,列不清、帧不废 —— PM 域与功率熔断无关,
         * 下一帧恢复即回值)。% = 实际/限值×100,限值 NaN 或 ≤0 -> NA
         * (不猜,准确度红线)。 */
        const bool pmOk = pmKnown_ && pm_->Refresh();
        if (pmOk) {
            const auto pmSet = [&](int idx, uint32_t off) {
                if (idx < 0) return;
                const float v = pm_->At(off);
                if (!std::isnan(v)) table_.Set((unsigned)idx, Ok(v));
            };
            const auto pmPct = [&](int idx, uint32_t valOff, uint32_t limOff) {
                if (idx < 0) return;
                const float v = pm_->At(valOff), l = pm_->At(limOff);
                if (!std::isnan(v) && !std::isnan(l) && l > 0.0f)
                    table_.Set((unsigned)idx,
                               Ok((double)v / (double)l * 100.0));
            };
            pmSet(idxPmStapm_, KpPm::StapmValue);   // "APU STAPM [W]"
            pmSet(idxPmTdc_, KpPm::TdcValue);       // "CPU TDC [A]"
            pmSet(idxPmSoc_, KpPm::SocCurValue);    // "SoC Current (SVI3 TFN) [A]"
            pmPct(idxPctTdc_, KpPm::TdcValue, KpPm::TdcLimit);
            pmPct(idxPctFast_, KpPm::FastValue, KpPm::FastLimit);
            pmPct(idxPctSlow_, KpPm::SlowValue, KpPm::SlowLimit);
            pmPct(idxPctStapm_, KpPm::StapmValue, KpPm::StapmLimit);
            pmPct(idxPctThermal_, KpPm::TctlValue, KpPm::TctlLimit);
        }

        /* (g) Sample 派生 —— 全部经表 Lookup(键缺失/无效 -> NA,UI 按
         * 既有 caps 逻辑降级,渲染层零改动)。coresW 是"和"而
         * power.core.avg 是"均值"(语义不同):从每核功率列求和派生。 */
        s.pkgW = table_.Lookup("power.pkg");
        double coreSum = 0.0;
        bool anyCoreW = false;
        for (size_t i = 0; i < cores_.size(); ++i) {
            if (coreSt_[i].idxPower < 0) continue;
            const Reading r = table_.Get((unsigned)coreSt_[i].idxPower);
            if (r.valid) { coreSum += r.value; anyCoreW = true; }
        }
        s.coresW = anyCoreW ? Ok(coreSum) : NA();
        s.tempC = table_.Lookup("temp.tctl");
        const Reading clockAvgR = table_.Lookup("clock.avg");
        s.freqGHz = clockAvgR.valid ? Ok(clockAvgR.value / 1000.0) : NA();
        s.utilPct = table_.Lookup("usage.total");
        /* (h) 本平台不提供/未接入的域 —— 恒 NA/false(诚实降级,
         * 不猜)。gfx(无 PP1 等价域)/platform(无 PSYS)/tau 窗口
         * (KpPm::StapmTimeS 偏移在,本期不建列)/TDC-EDC(核心 EDC
         * 偏移未知)/包级驻留/SMI。PMTable 域在 (f2) 已刷新。 */
        s.gfxW = NA();
        s.platformW = NA();
        s.powerLimit.sustainedW = pmOk ? PmAt(KpPm::StapmLimit) : NA();
        s.powerLimit.burstW = pmOk ? PmAt(KpPm::FastLimit) : NA();
        s.powerLimit.sustainedWindowS = NA();
        s.powerLimit.locked = false;
        s.currentLimit.tdcA = pmOk ? PmAt(KpPm::TdcValue) : NA();
        s.currentLimit.edcA = NA();
        s.c0Pct = NA(); s.c2Pct = NA(); s.c6Pct = NA();
        s.smiDelta = std::nullopt;

        return anyPower;   // 全部功率域失败才整体失败(熔断契约)
    }

private:
    // 每核状态:ctor 可读性结论 + 列索引(-1 = 无列)。
    struct CoreState {
        bool cofvidOk = false;   // 0xC0010293 可读 -> 时钟/VID/比率列
        bool eppOk = false;      // 0xC00102B3 可读 -> 帧内尝试读取
        int idxVid = -1, idxClock = -1, idxRatio = -1;
        int idxC0 = -1;
        int idxPower = -1;       // "Core N Power"(功率列恒建,帧失败 NA)
    };
    // 每 LP 状态:线程级列 + RO APERF/MPERF 差分基线/本帧差值
    struct LpState {
        int idxEff = -1, idxUsage = -1, idxUtil = -1;
        uint64_t prevAperf = 0, prevMperf = 0;
        bool amBase = false;     // false 时下次成功读取只建基线(不差分)
        double dA = 0.0, dM = 0.0;
        bool diffOk = false;     // 本帧差分有效(节流调整消费)
    };

    // vendor/cpuName/logicalProcessors/baseGHz 来自 PlatformInfo;
    // 保底能力位:gfx(无 PP1 等价域)/platform(无 PSYS)/residency
    // (Sample c0/c2/c6 恒 NA,宽表 cores.c0.* 列独立存在,不受 caps 位
    // 影响)/smi 均不支持;powerLimits 由 ctor 在 PMTable 握手成功且
    // 版本匹配时置位(Task 6,Intel MapPlWindow 同口径);budgetW=0(UI
    // 走 spec fallback),tjMaxC=0(AMD Tctl 偏移未知,UI 隐藏 TjMax);
    // baseGHz 由 ctor 从 P-state P0 解码覆盖(AMD 权威来源)。
    static PlatformCaps BuildCaps(const PlatformInfo& info) {
        PlatformCaps c;
        c.vendor = info.vendor;
        c.cpuName = info.cpuName;
        c.logicalProcessors = info.logicalProcessors;
        c.baseGHz = info.baseGHz;
        return c;   // 其余字段默认 false/0 即诚实降级
    }

    // 拓扑规范化:cores 空/repLP 或线程 LP 越界 = 拓扑未知,退回
    // "每 LP 一个单线程核"(全部合成 effClass 0:family 0x1A 下命名即
    // "Zen5c Core n" —— 单类合成拓扑,无 Zen5;其他 family 为
    // "Core n";编号 = 顺序核索引,单线程合成下恰等于 LP 号)—— 保底
    // 覆盖全部 LP,与 v2 全 LP 遍历语义一致(0xC001029A 逐核读退化为
    // 逐 LP 读,无 SMT 去重但保底不残缺;见 PowerDashModel.h cores 注释)。
    void NormalizeTopology(const PlatformInfo& info) {
        nLP_ = info.logicalProcessors;
        cores_ = info.cores;
        bool ok = !cores_.empty();
        for (const CoreInfo& c : cores_) {
            if (c.repLP >= nLP_ || c.threads.empty()) ok = false;
            for (unsigned lp : c.threads)
                if (lp >= nLP_) ok = false;
        }
        if (!ok) {
            cores_.assign(nLP_, CoreInfo{});
            for (unsigned lp = 0; lp < nLP_; ++lp) {
                cores_[lp].repLP = lp;
                cores_[lp].threads.push_back(lp);
            }
        }
    }

    // 每核能力探测(各读一次 repLP;瞬态寄存器只看可读性)—— 失败即
    // 不建对应列/不逐帧读取(准确度红线:无把握的列宁可省略)。0xC0010293
    // 同时决定时钟/VID/比率三组列;0xC001029A 不在此探测(能量列恒建,
    // 帧读取失败按既有 stale 域语义走 NA)。
    void ProbeCores() {
        coreSt_.resize(cores_.size());
        uint64_t v = 0;
        for (size_t i = 0; i < cores_.size(); ++i) {
            coreSt_[i].cofvidOk =
                io_.ReadMsr(cores_[i].repLP, AMD_COFVID_STATUS, v);
            coreSt_[i].eppOk =
                io_.ReadMsr(cores_[i].repLP, AMD_CPPC_REQUEST, v);
        }
    }

    int AddCol(const char* key, const std::string& name, SensorFmt fmt) {
        return (int)table_.Add(key, name, fmt);
    }
    std::string SuffixName(size_t idx, const char* suffix) {
        return CoreName(idx, cores_[idx], family_) + suffix;
    }

    // 构建列集:组序对齐 amd.CSV(电压→时钟→有效→Usage→Utility→比率→
    // C0 驻留→温度→功率→EPP;组内 avg 列在前);列名逐字符照 amd.CSV
    // 表头(\\nas\labs\TEST\hwinfo-smaple\amd.CSV 核对,HWiNFO 的
    // "(perf #N)" 内部计数器后缀不复刻)。探测失败的能力不建列(动态
    // 列集,同 HWiNFO)。功率每核列名用纯核序号 "Core %u Power"
    // (amd.CSV 即如此,不带核型名);C0 键 = 核序号(AMD 按 HWiNFO 命名
    // 是每核列,与 Intel 的每线程 cores.c0.lp<N> 约定不同)。
    void BuildColumns() {
        char key[48], tail[48], name[48];
        const size_t n = cores_.size();
        coreSt_.resize(n);
        lp_.assign(nLP_, LpState{});

        /* 1 电压 */
        idxAvgVid_ = AddCol("vid.avg", "Core VIDs (avg) [V]", SensorFmt::F3);
        for (size_t i = 0; i < n; ++i)
            if (coreSt_[i].cofvidOk) {
                snprintf(key, sizeof(key), "vid.%zu", i);
                coreSt_[i].idxVid =
                    AddCol(key, SuffixName(i," VID [V]"),
                           SensorFmt::F3);
            }

        /* 2 时钟(bus 列在每核时钟后,amd.CSV 序) */
        idxAvgClock_ = AddCol("clock.avg", "Core Clocks (avg) [MHz]",
                              SensorFmt::F1);
        for (size_t i = 0; i < n; ++i)
            if (coreSt_[i].cofvidOk) {
                snprintf(key, sizeof(key), "clock.%zu", i);
                coreSt_[i].idxClock =
                    AddCol(key, SuffixName(i," Clock [MHz]"),
                           SensorFmt::F1);
            }
        idxClockBus_ = AddCol("clock.bus", "Bus Clock [MHz]", SensorFmt::F1);

        /* 3 有效时钟(每线程 T0/T1;eff.all = 全线程均值) */
        idxAvgEff_ = AddCol("eff.avg", "Core Effective Clocks (avg) [MHz]",
                            SensorFmt::F1);
        for (size_t i = 0; i < n; ++i)
            for (size_t t = 0; t < cores_[i].threads.size(); ++t) {
                const unsigned lp = cores_[i].threads[t];
                if (lp >= nLP_ || lp >= lp_.size()) continue;
                snprintf(key, sizeof(key), "eff.%zu.%zu", i, t);
                snprintf(tail, sizeof(tail), " T%zu Effective Clock [MHz]", t);
                lp_[lp].idxEff =
                    AddCol(key, SuffixName(i,tail), SensorFmt::F1);
            }
        idxAllEff_ = AddCol("eff.all", "Average Effective Clock [MHz]",
                            SensorFmt::F1);

        /* 4 Usage(每线程 T0/T1;Max/Total 为全局列) */
        idxAvgUsage_ = AddCol("usage.avg", "Core Usage (avg) [%]",
                              SensorFmt::PCT1);
        for (size_t i = 0; i < n; ++i)
            for (size_t t = 0; t < cores_[i].threads.size(); ++t) {
                const unsigned lp = cores_[i].threads[t];
                if (lp >= nLP_ || lp >= lp_.size()) continue;
                snprintf(key, sizeof(key), "usage.%zu.%zu", i, t);
                snprintf(tail, sizeof(tail), " T%zu Usage [%%]", t);
                lp_[lp].idxUsage =
                    AddCol(key, SuffixName(i,tail), SensorFmt::PCT1);
            }
        idxUsageMax_ = AddCol("usage.max", "Max CPU/Thread Usage [%]",
                              SensorFmt::PCT1);
        idxUsageTotal_ = AddCol("usage.total", "Total CPU Usage [%]",
                                SensorFmt::PCT1);

        /* 5 Utility(每线程 T0/T1;Total 为全局列) */
        idxAvgUtil_ = AddCol("util.avg", "Core Utility (avg) [%]",
                             SensorFmt::PCT1);
        for (size_t i = 0; i < n; ++i)
            for (size_t t = 0; t < cores_[i].threads.size(); ++t) {
                const unsigned lp = cores_[i].threads[t];
                if (lp >= nLP_ || lp >= lp_.size()) continue;
                snprintf(key, sizeof(key), "util.%zu.%zu", i, t);
                snprintf(tail, sizeof(tail), " T%zu Utility [%%]", t);
                lp_[lp].idxUtil =
                    AddCol(key, SuffixName(i,tail), SensorFmt::PCT1);
            }
        idxTotalUtil_ = AddCol("util.total", "Total CPU Utility [%]",
                               SensorFmt::PCT1);

        /* 6 比率(= clock/bus;bus 未知 -> 列在,值恒 NA) */
        idxAvgRatio_ = AddCol("ratio.avg", "Core Ratios (avg) [x]",
                              SensorFmt::RATIO2);
        for (size_t i = 0; i < n; ++i)
            if (coreSt_[i].cofvidOk) {
                snprintf(key, sizeof(key), "ratio.%zu", i);
                coreSt_[i].idxRatio =
                    AddCol(key, SuffixName(i," Ratio [x]"),
                           SensorFmt::RATIO2);
            }

        /* 7 C0 驻留(每核 = 线程均值,键 = 核序号无 lp 后缀;SuffixName
           为纯拼接,单 % 即字面量) */
        idxAvgC0_ = AddCol("cores.c0.avg", "Core C0 Residency (avg) [%]",
                           SensorFmt::PCT1);
        for (size_t i = 0; i < n; ++i) {
            snprintf(key, sizeof(key), "cores.c0.%zu", i);
            coreSt_[i].idxC0 =
                AddCol(key, SuffixName(i," C0 Residency [%]"),
                       SensorFmt::PCT1);
        }

        /* 8 温度(单列;CCD/逐核温度 = PMTable 未知偏移,下期) */
        idxTempTctl_ = AddCol("temp.tctl", "CPU (Tctl/Tdie) [°C]",
                              SensorFmt::F1);

        /* 9 功率(pkg → (avg) → 每核,amd.CSV 序;每核列名 = 纯核序号) */
        idxPowerPkg_ = AddCol("power.pkg", "CPU Package Power [W]",
                              SensorFmt::F3);
        idxPowerCoreAvg_ = AddCol("power.core.avg", "Core Powers (avg) [W]",
                                  SensorFmt::F3);
        for (size_t i = 0; i < n; ++i) {
            snprintf(key, sizeof(key), "power.core.%zu", i);
            snprintf(name, sizeof(name), "Core %zu Power [W]", i);
            coreSt_[i].idxPower = AddCol(key, name, SensorFmt::F3);
        }

        /* 10 EPP(单条全局列;均值 over 可读核) */
        idxEpp_ = AddCol("epp.avg", "Energy Performance Preference [%]",
                         SensorFmt::PCT1);
    }

    // PM 列组(Task 6):追加在全部主列组之后,组内序 STAPM 实际 ->
    // TDC 实际 -> SoC 电流实际 -> 限值%(TDC/PPT FAST/PPT SLOW/STAPM/
    // Thermal)。列名照 amd.CSV 原文;HWiNFO 无原始 STAPM/PPT 限值 W 列
    // —— 限值只以 % 列出现(value/limit×100),STAPM 限值 W 值仅入
    // Sample.powerLimit。SoC 电流列待 Task 10 实机与 HWiNFO 对应性核对,
    // 不符则删(spec §4.2 注)。版本门控在 ctor(pmKnown_)。
    void BuildPmColumns() {
        idxPmStapm_ = AddCol("pm.stapm.value", "APU STAPM [W]", SensorFmt::F3);
        idxPmTdc_ = AddCol("pm.tdc.value", "CPU TDC [A]", SensorFmt::F3);
        idxPmSoc_ = AddCol("pm.soccur.value", "SoC Current (SVI3 TFN) [A]",
                           SensorFmt::F3);
        idxPctTdc_ = AddCol("pct.tdc", "CPU TDC Limit [%]", SensorFmt::PCT1);
        idxPctFast_ = AddCol("pct.pptfast", "CPU PPT FAST Limit [%]",
                             SensorFmt::PCT1);
        idxPctSlow_ = AddCol("pct.pptslow", "CPU PPT SLOW Limit [%]",
                             SensorFmt::PCT1);
        idxPctStapm_ = AddCol("pct.stapm", "APU STAPM Limit [%]",
                              SensorFmt::PCT1);
        idxPctThermal_ = AddCol("pct.thermal", "Thermal Limit [%]",
                                SensorFmt::PCT1);
    }

    // PMTable 偏移 -> Reading(NaN -> NA);仅 (f2) pmOk 帧调用。
    Reading PmAt(uint32_t off) const {
        const float v = pm_->At(off);
        return std::isnan(v) ? NA() : Ok(v);
    }

    // 全部 prev 计数器(读取失败保持 0,与既有实现一致)+ TSC 锚点;
    // 逐核 0xC001029A 与逐线程 RO APERF/MPERF 基线与采样同路径
    // (能量每核一个代表 LP,A/M 每线程各一份)。
    void ReadBaseline() {
        uint64_t v = 0;
        if (io_.ReadMsr(0, AMD_PKG_ENERGY_STAT, v)) prevPkg_ = v & 0xFFFFFFFFull;
        prevCoreE_.assign(cores_.size(), 0);
        staleCore_.assign(cores_.size(), false);
        for (size_t i = 0; i < cores_.size(); ++i)
            if (io_.ReadMsr(cores_[i].repLP, AMD_CORE_ENERGY_STAT, v))
                prevCoreE_[i] = v & 0xFFFFFFFFull;
        for (size_t i = 0; i < cores_.size(); ++i)
            for (const unsigned lp : cores_[i].threads) {
                if (lp >= nLP_ || lp >= lp_.size()) continue;
                uint64_t a = 0, m = 0;
                if (io_.ReadMsr(lp, AMD_RO_APERF, a) &&
                    io_.ReadMsr(lp, AMD_RO_MPERF, m)) {
                    lp_[lp].prevAperf = a;
                    lp_[lp].prevMperf = m;
                    lp_[lp].amBase = true;
                }
            }
        prevTsc_ = __rdtsc();
    }

    DriverIo& io_;
    PlatformCaps caps_;
    SensorTable table_;                    // 主数据源(Sample 自此派生)
    std::vector<CoreInfo> cores_;          // 规范化拓扑
    unsigned nLP_ = 0;
    unsigned family_ = 0;
    bool unitsOk_ = false;
    double energyUnit_ = 0.0;
    double tscHz_ = 0.0;                   // Hz(CalibrateTscHz 原样;eff 换算 /1e6)
    double busClock_ = 0.0;                // MHz;0 = 未知 -> bus/比率列 NA
    std::unique_ptr<UsageMonitor> usage_;
    std::vector<CoreState> coreSt_;
    std::vector<LpState> lp_;
    // PMTable 客户端(Task 6):null = SMU 握手失败(诚实降级);
    // pmKnown_ = 版本 == KpPm::kVersion(偏移可信,PM 列已建);
    // pmVersionMismatch_ = 有 SMU 但版本未知(仅记号,不建列)。
    std::unique_ptr<SmuPmTable> pm_;
    bool pmKnown_ = false;
    bool pmVersionMismatch_ = false;
    // 既有差分基线(pkg + 每核能量 + TSC;每核 stale 标记)
    uint64_t prevPkg_ = 0, prevTsc_ = 0;
    std::vector<uint64_t> prevCoreE_;      // 每核 0xC001029A 基线
    std::vector<bool> staleCore_;          // 上帧读取失败的核:恢复时只重置基线
    // 列索引(-1 = 该平台无此列)
    int idxAvgVid_ = -1, idxAvgClock_ = -1, idxClockBus_ = -1;
    int idxAvgEff_ = -1, idxAllEff_ = -1;
    int idxAvgUsage_ = -1, idxUsageMax_ = -1, idxUsageTotal_ = -1;
    int idxAvgUtil_ = -1, idxTotalUtil_ = -1;
    int idxAvgRatio_ = -1, idxAvgC0_ = -1;
    int idxTempTctl_ = -1;
    int idxPowerPkg_ = -1, idxPowerCoreAvg_ = -1;
    int idxEpp_ = -1;
    // PM 列索引(Task 6;-1 = 无 PMTable,整组缺席)
    int idxPmStapm_ = -1, idxPmTdc_ = -1, idxPmSoc_ = -1;
    int idxPctTdc_ = -1, idxPctFast_ = -1, idxPctSlow_ = -1;
    int idxPctStapm_ = -1, idxPctThermal_ = -1;
};

std::unique_ptr<IPlatformProbe> CreateAmdProbe(DriverIo& io,
                                               const PlatformInfo& info) {
    return std::make_unique<AmdProbe>(io, info);
}

} // namespace pd
