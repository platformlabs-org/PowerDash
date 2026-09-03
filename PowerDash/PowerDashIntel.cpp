// PowerDashIntel.cpp —— Intel 平台探针宽表化(Task 4,v3)。
// SensorTable 为主数据源:ctor 拓扑规范化 + 能力探测 -> 构建 HWiNFO 命名
// 列集(spec §4.1);readSample 每帧先 SetInvalid 全表再按组回填(读取
// 失败留 NA,绝不残留旧值),Sample 从表内规范键派生 —— 同一物理量单条
// 写路径,CSV 与面板永不漂移(spec §1.3)。
// 解码依据 spec §1.1(公开数据源,LHM/turbostat 同式):
//   时钟/VID   0x198 EAX[15:8]×bus / EDX[15:0]÷8192(repLP 读)
//   bus        CalibrateTscHz() ÷ 0xCE[15:8](最大非睿频倍频;任一失败
//              -> 时钟/比率列恒 NA,不做 100 MHz 名义值兜底)
//   有效时钟   ΔAPERF×tscHz/ΔTSC(每线程,核 = 线程均值)
//   温度       0x19C bit31 门 + [22:16] 距 TjMax readout;TjMax=0x1A2[23:16]
//              (本核值,失败回退 LP0);包级 0x1B1 同式
//   降频 log   0x19C/0x1B1 位 1/5/11(thermal/critical/power-limit)
//   功率       RAPL 能量差分(既有 32 位回绕钳制,1 s 窗口约定)
//   PL         静态 0x610(PL1 [14:0]/tau [23:17]/PL2 [46:32]×powerUnit,
//              locked bit31)+ 动态 MMIO MCHBAR+0x59A0(×0.125,既有)
//   cTDP       0x64B[1:0];Clock Modulation 0x19A(bit4 使能+[3:0]×12.5%)
//   驻留       包级 0x60D/0x3F8/0x3F9/0x630/0x632、核级 0x660/0x3FD/0x3FE
//              (ctor 探测,可读才建列),÷ΔTSC×100;C0=ΔMPERF/ΔTSC×100
//   Limit Reasons 0x64F/0x650/0x651 log 位 16+i(整组探测,任一失败整组省略)
//   Usage      NtQuerySystemInformation 差分(UsageMonitor,首 2 帧暖机 NA);
//   Utility    busy×(ΔA/ΔM 钳 [0,4]) 钳 0-100(微软频率加权语义)
#include "PowerDashProbe.h"
#include "PowerDashSensors.h"
#include "PowerDashUsage.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <string>
#include <vector>

namespace pd {
namespace {

// ---- MSR 常量(spec §1.1;0x1B1 以 PowerDash.cpp:43 为准,brief 骨架
// 里的 0x1B2/0x613 均为笔误)----
constexpr uint32_t MSR_RAPL_POWER_UNIT = 0x606, MSR_PKG_POWER_INFO = 0x614;
constexpr uint32_t MSR_PKG_ENERGY_STATUS = 0x611, MSR_PP0_ENERGY_STATUS = 0x639;
constexpr uint32_t MSR_PP1_ENERGY_STATUS = 0x641, MSR_SYS_ENERGY_STATUS = 0x64D;
constexpr uint32_t MSR_PKG_POWER_LIMIT = 0x610, MSR_CONFIG_TDP_CONTROL = 0x64B;
constexpr uint32_t MSR_PLATFORM_INFO = 0xCE;      // [15:8] 最大非睿频倍频
constexpr uint32_t MSR_PERF_STATUS = 0x198;       // EAX[15:8] 倍频 / EDX[15:0] VID
constexpr uint32_t MSR_THERM_STATUS = 0x19C;      // bit31 门 + [22:16] + log 1/5/11
constexpr uint32_t MSR_PACKAGE_THERM_STATUS = 0x1B1, MSR_TEMPERATURE_TARGET = 0x1A2;
constexpr uint32_t MSR_IA32_APERF = 0xE8, MSR_IA32_MPERF = 0xE7;
constexpr uint32_t MSR_CLOCK_MODULATION = 0x19A;  // bit4 使能 + [3:0] 占空
constexpr uint32_t MSR_PKG_C2_RESIDENCY = 0x60D, MSR_PKG_C3_RESIDENCY = 0x3F8;
constexpr uint32_t MSR_PKG_C6_RESIDENCY = 0x3F9, MSR_PKG_C8_RESIDENCY = 0x630;
constexpr uint32_t MSR_PKG_C10_RESIDENCY = 0x632;
constexpr uint32_t MSR_CORE_C1_RESIDENCY = 0x660, MSR_CORE_C6_RESIDENCY = 0x3FD;
constexpr uint32_t MSR_CORE_C7_RESIDENCY = 0x3FE;
constexpr uint32_t MSR_SMI_COUNT = 0x34;
constexpr uint32_t MSR_IA_LIMIT_REASON = 0x64F, MSR_GT_LIMIT_REASON = 0x650;
constexpr uint32_t MSR_RING_LIMIT_REASON = 0x651;
// Uncore Ratio(0x620)无公开数据源解码(spec §1.1"实机核对后启用,否则
// 省略"),本版整列省略,实机验证任务补列。

// Limit Reasons 列名数组(log 位序:位 16+i 对应第 i 个;HWiNFO intel.CSV
// 原文,含双空格)。GT/RING 位序以 HWiNFO 列序为据,IA 与 SDM 一致。
constexpr const char* kIaReasons[11] = {
    "IA: PROCHOT", "IA: Thermal Event", "IA: Residency State Regulation",
    "IA: Running Average Thermal Limit", "IA: VR Thermal Alert", "IA: VR TDC",
    "IA: Electrical Design Point/Other (ICCmax PL4 SVID DDR RAPL)",
    "IA: Package-Level RAPL/PBM PL1", "IA: Package-Level RAPL/PBM PL2 PL3",
    "IA: Max Turbo Limit", "IA: Turbo Attenuation (MCT)" };
constexpr const char* kGtReasons[13] = {
    "GT: PROCHOT", "GT: Thermal Event", "GT: DDR RAPL",
    "GT: Residency State Regulation", "GT: Running Average Thermal Limit",
    "GT: VR Thermal Alert", "GT: VR TDC", "GT: Max VR Voltage  ICCmax  PL4",
    "GT: Domain-Level PBM PLGT", "GT: Package-Level RAPL/PBM PL1",
    "GT: Package-Level RAPL/PBM PL2 PL3", "GT: Inefficient Operation",
    "GT: Fuses limit" };
constexpr const char* kRingReasons[10] = {
    "RING: PROCHOT", "RING: Thermal Event", "RING: DDR RAPL",
    "RING: Residency State Regulation", "RING: Running Average Thermal Limit",
    "RING: VR Thermal Alert", "RING: VR TDC",
    "RING: Max VR Voltage  ICCmax  PL4", "RING: Package-Level RAPL/PBM PL1",
    "RING: Package-Level RAPL/PBM PL2 PL3" };

// 封装驻留 MSR 表(固定列序 C2/C3/C6/C8/C10,ctor 探测可读才建列)
constexpr struct { uint32_t msr; const char* key; const char* name; }
    kPkgRes[] = {
        {MSR_PKG_C2_RESIDENCY, "pkgres.c2", "Package C2 Residency [%]"},
        {MSR_PKG_C3_RESIDENCY, "pkgres.c3", "Package C3 Residency [%]"},
        {MSR_PKG_C6_RESIDENCY, "pkgres.c6", "Package C6 Residency [%]"},
        {MSR_PKG_C8_RESIDENCY, "pkgres.c8", "Package C8 Residency [%]"},
        {MSR_PKG_C10_RESIDENCY, "pkgres.c10", "Package C10 Residency [%]"},
    };

// HWiNFO 核名:effClass 0→"P-core n"、1→"E-core n"、≥2→"E-core (LP) n",
// 编号 = repLP(spec §1.1 GetLogicalProcessorInformationEx 命名约定)
std::string CoreName(const CoreInfo& c) {
    char buf[48];
    if (c.effClass == 0)
        snprintf(buf, sizeof(buf), "P-core %u", c.repLP);
    else if (c.effClass == 1)
        snprintf(buf, sizeof(buf), "E-core %u", c.repLP);
    else
        snprintf(buf, sizeof(buf), "E-core (LP) %u", c.repLP);
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

class IntelProbe : public IPlatformProbe {
public:
    IntelProbe(DriverIo& io, const PlatformInfo& info) : io_(io) {
        caps_ = BuildCaps(info);       // vendor/name/nLP/baseGHz(探测项随后回填)
        NormalizeTopology(info);       // cores 空/越界 -> 每 LP 合成单线程核
        uint64_t u = 0;                                          // :557-570
        if (io_.ReadMsr(0, MSR_RAPL_POWER_UNIT, u)) {
            energyUnit_ = 1.0 / std::pow(2.0, (u >> 8) & 0x1F);
            powerUnit_  = 1.0 / std::pow(2.0, (u >> 0) & 0x0F);
            timeUnitS_  = 1.0 / std::pow(2.0, (u >> 16) & 0x0F);
            unitsOk_ = true;
        }
        uint64_t spec = 0;                                       // :618-630
        if (unitsOk_ && io_.ReadMsr(0, MSR_PKG_POWER_INFO, spec))
            caps_.budgetW = ((spec >> 0) & 0x7FFF) * powerUnit_;  // thermal spec
        uint64_t tj = 0;                                         // :632-638
        if (io_.ReadMsr(0, MSR_TEMPERATURE_TARGET, tj) && ((tj >> 16) & 0xFF) != 0)
            caps_.tjMaxC = static_cast<int>((tj >> 16) & 0xFF);
        /* busClock = 实测 TSC ÷ 0xCE[15:8](LHM 同式);校准/寄存器任一
         * 失败 -> 0,时钟/比率列恒 NA(不做名义 100 MHz 兜底,准确度红线)。 */
        tscHz_ = CalibrateTscHz();
        if (tscHz_ > 0.0) {
            uint64_t ce = 0;
            if (io_.ReadMsr(0, MSR_PLATFORM_INFO, ce)) {
                const unsigned ratio = (unsigned)((ce >> 8) & 0xFF);
                if (ratio != 0) busClock_ = tscHz_ / (double)ratio / 1e6;  // MHz
            }
        }
        MapPlWindow();                                           // :572-594 MCHBAR+0x59A0
        ProbeCores();        // 每核 0x198/0x19C/0x1A2/0x660/0x3FD/0x3FE 探测
        ProbePackage();      // 封装驻留 MSR + Limit Reasons 三寄存器(整组)
        usage_ = std::make_unique<UsageMonitor>(
            std::make_unique<NtUsageSource>(nLP_));
        BuildColumns();      // HWiNFO 命名列集(键表见 task-4-brief Interfaces)
        caps_.residency = !pkgRes_.empty() || HasCoreResidencyColumn();
        ReadBaseline();      // 能量/SMI prev + TSC 锚点(A/M 基线见 lp_)
    }

    /* 源码只在 -setpl 路径解映射;Probe 拥有映射即负责释放。 */
    ~IntelProbe() override {
        if (map_) io_.UnmapPhys(map_);
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
        bool anyPower = false;

        /* (a) 能量差分 + 32 位回绕 —— 照抄 :760-779 的 if(curr<prev) += 1<<32;
         * 与源一致按 1 s 采样窗口把能量差直接当功率数值。 */
        uint64_t curr = 0;
        if (io_.ReadMsr(0, MSR_PKG_ENERGY_STATUS, curr)) {
            if (curr < prevPkg_) curr += (1ULL << 32);
            table_.Set(idxPower_[0], Ok((curr - prevPkg_) * energyUnit_));
            prevPkg_ = curr;
            anyPower = true;
        }
        if (io_.ReadMsr(0, MSR_PP0_ENERGY_STATUS, curr)) {
            if (curr < prevPp0_) curr += (1ULL << 32);
            table_.Set(idxPower_[1], Ok((curr - prevPp0_) * energyUnit_));
            prevPp0_ = curr;
            anyPower = true;
        }
        if (io_.ReadMsr(0, MSR_PP1_ENERGY_STATUS, curr)) {
            if (curr < prevPp1_) curr += (1ULL << 32);
            table_.Set(idxPower_[2], Ok((curr - prevPp1_) * energyUnit_));
            prevPp1_ = curr;
            anyPower = true;
        }
        if (io_.ReadMsr(0, MSR_SYS_ENERGY_STATUS, curr)) {
            if (curr < prevSys_) curr += (1ULL << 32);
            table_.Set(idxPower_[3], Ok((curr - prevSys_) * energyUnit_));
            prevSys_ = curr;
            anyPower = true;
        }

        /* (b) PL:静态 0x610(PL1 [14:0]、tau [23:17]、locked bit31、PL2
         * [46:32])+ 动态 MMIO(0.125 W 定标,既有窗口语义)+ cTDP。
         * tau/locked 恒取静态寄存器(动态窗口无 tau 字段,v2 同语义)。 */
        tauStatic_ = NA();
        lockedStatic_ = false;
        uint64_t pl = 0;
        if (io_.ReadMsr(0, MSR_PKG_POWER_LIMIT, pl)) {
            table_.Set(idxPl1S_, Ok((double)((pl >> 0) & 0x7FFF) * powerUnit_));
            table_.Set(idxPl2S_, Ok((double)((pl >> 32) & 0x7FFF) * powerUnit_));
            const uint64_t rawTau = (pl >> 17) & 0x7F;
            if (rawTau) tauStatic_ = Ok(rawTau * timeUnitS_);   // 0 = "n/a" -> NA
            lockedStatic_ = ((pl >> 31) & 1) != 0;
        }
        if (pl_) {
            table_.Set(idxPl1D_, Ok(((uint32_t)pl_[0] & 0x7FFF) * 0.125));
            table_.Set(idxPl2D_, Ok(((uint32_t)pl_[1] & 0x7FFF) * 0.125));
        }
        if (io_.ReadMsr(0, MSR_CONFIG_TDP_CONTROL, curr))
            table_.Set(idxCtdp_, Ok((double)(curr & 3)));

        /* (c) 温度/降频位 —— 每核 0x19C(repLP):bit31 门 + [22:16] readout
         * -> temp = TjMax−readout、distance = readout;log 位 1/5/11 恒随读
         * 取成功更新(粘性日志无需 bit31)。包级 0x1B1 同式(TjMax 取 LP0)。 */
        static const int thrBits[3] = {1, 5, 11};      // thermal/crit/plim log
        Mean tempAvg, distAvg;
        double tempMax = 0.0;
        bool anyTemp = false;
        unsigned thrOr[3] = {0, 0, 0};
        for (size_t i = 0; i < cores_.size(); ++i) {
            CoreState& cs = coreSt_[i];
            if (cs.idxDist < 0) continue;               // ctor 探测失败无列
            uint64_t th = 0;
            if (!io_.ReadMsr(cores_[i].repLP, MSR_THERM_STATUS, th)) continue;
            for (int k = 0; k < 3; ++k) {
                const unsigned bit = (unsigned)((th >> thrBits[k]) & 1);
                table_.Set(cs.idxThr[k], Ok((double)bit));
                thrOr[k] |= bit;
            }
            if ((th & (1ULL << 31)) == 0) continue;     // 温度字段无效
            const unsigned readout = (unsigned)((th >> 16) & 0x7F);
            table_.Set(cs.idxDist, Ok((double)readout));
            distAvg.Add((double)readout);
            const int tjMax = cs.tjMax > 0 ? cs.tjMax : caps_.tjMaxC;  // 回退 LP0
            if (tjMax > 0) {
                const double t = (double)(tjMax - (int)readout);
                table_.Set(cs.idxTemp, Ok(t));
                tempAvg.Add(t);
                if (!anyTemp || t > tempMax) { tempMax = t; anyTemp = true; }
            }
        }
        for (int k = 0; k < 3; ++k)                     // avg = OR(YESNO 语义)
            if (idxThrAvg_[k] >= 0)
                table_.Set(idxThrAvg_[k], Ok(thrOr[k] ? 1.0 : 0.0));
        {
            uint64_t th = 0;
            if (io_.ReadMsr(0, MSR_PACKAGE_THERM_STATUS, th)) {
                table_.Set(idxThrPkg_[0], Ok((double)((th >> 1) & 1)));
                table_.Set(idxThrPkg_[1], Ok((double)((th >> 5) & 1)));
                table_.Set(idxThrPkg_[2], Ok((double)((th >> 11) & 1)));
                if ((th & (1ULL << 31)) && caps_.tjMaxC > 0)
                    table_.Set(idxTempPkg_,
                               Ok((double)(caps_.tjMaxC -
                                           (int)((th >> 16) & 0x7F))));
            }
        }
        double v = 0.0;
        if (tempAvg.Get(v)) table_.Set(idxAvgTemp_, Ok(v));
        if (distAvg.Get(v)) table_.Set(idxAvgDist_, Ok(v));
        if (anyTemp) table_.Set(idxCoresMax_, Ok(tempMax));

        /* (d) 时钟/VID/比率 —— 每核 0x198(repLP):EAX[15:8]×bus、
         * EDX[15:0]÷8192;bus 未知(校准/0xCE 失败)-> 时钟/比率 NA,
         * VID 不依赖 bus 照常出。 */
        Mean clockAvg, vidAvg, ratioAvg;
        if (busClock_ > 0.0) table_.Set(idxClockBus_, Ok(busClock_));
        for (size_t i = 0; i < cores_.size(); ++i) {
            CoreState& cs = coreSt_[i];
            if (cs.idxClock < 0) continue;              // ctor 探测失败无列
            uint64_t perf = 0;
            if (!io_.ReadMsr(cores_[i].repLP, MSR_PERF_STATUS, perf)) continue;
            const unsigned ratio = (unsigned)((perf >> 8) & 0xFF);
            const double vid = (double)((perf >> 32) & 0xFFFF) / 8192.0;
            table_.Set(cs.idxVid, Ok(vid));
            vidAvg.Add(vid);
            if (busClock_ > 0.0) {
                const double mhz = (double)ratio * busClock_;
                table_.Set(cs.idxClock, Ok(mhz));
                clockAvg.Add(mhz);
                table_.Set(cs.idxRatio, Ok((double)ratio));   // = clock/bus
                ratioAvg.Add((double)ratio);
            }
        }
        if (clockAvg.Get(v)) table_.Set(idxAvgClock_, Ok(v));
        if (vidAvg.Get(v)) table_.Set(idxAvgVid_, Ok(v));
        if (ratioAvg.Get(v)) table_.Set(idxAvgRatio_, Ok(v));

        /* (e) On-Demand Clock Modulation(0x19A,LP0):bit4 未使能 -> 0%
         * (有效读数);使能时占空 = [3:0]×12.5%(brief/spec 定标)。 */
        if (io_.ReadMsr(0, MSR_CLOCK_MODULATION, curr))
            table_.Set(idxClockmod_, Ok((curr & 0x10)
                                            ? (double)(curr & 0xF) * 12.5
                                            : 0.0));

        /* (f) 有效时钟/Usage/Utility/C0 —— 每 LP APERF/MPERF 差分
         * (ctor 基线,读取失败保持旧基线);eff = ΔAPERF×tscHz/ΔTSC、
         * C0 = ΔMPERF/ΔTSC×100(turbostat Busy% 同式);核值 = 线程均值。
         * Usage 来自 UsageMonitor(真源首 2 帧暖期 ok=false -> NA);
         * utility = busy×(ΔA/ΔM 钳 [0,4]) 钳 0-100(微软频率加权语义)。 */
        const LpUsage lu = usage_->Read();
        std::vector<Mean> effCore(cores_.size()), usageCore(cores_.size()),
            utilCore(cores_.size());
        Mean effAll, utilAll, c0Avg;
        for (size_t i = 0; i < cores_.size(); ++i) {
            for (const unsigned lp : cores_[i].threads) {
                if (lp >= nLP_ || lp >= lp_.size()) continue;   // 越界防线
                LpState& L = lp_[lp];
                uint64_t a = 0, m = 0;
                if (!io_.ReadMsr(lp, MSR_IA32_APERF, a) ||
                    !io_.ReadMsr(lp, MSR_IA32_MPERF, m))
                    continue;                       // 一对完整才有差分窗口
                if (!L.amBase) {                    // 首个成功帧只建基线
                    L.prevAperf = a;
                    L.prevMperf = m;
                    L.amBase = true;
                    continue;
                }
                const double dA = (double)(a - L.prevAperf);
                const double dM = (double)(m - L.prevMperf);
                L.prevAperf = a;
                L.prevMperf = m;
                if (dtsc <= 0.0) continue;
                const double eff = dA * tscHz_ / dtsc;      // ΔAPERF/Δt = MHz
                const double c0 = dM / dtsc * 100.0;
                table_.Set(L.idxC0, Ok(c0));
                effCore[i].Add(eff);
                effAll.Add(eff);
                c0Avg.Add(c0);
                if (lu.ok && lp < lu.busyPct.size()) {
                    usageCore[i].Add(lu.busyPct[lp]);
                    if (dM > 0.0) {
                        double r = dA / dM;
                        if (r < 0.0) r = 0.0;
                        if (r > 4.0) r = 4.0;
                        double util = lu.busyPct[lp] * r;
                        if (util < 0.0) util = 0.0;
                        if (util > 100.0) util = 100.0;
                        utilCore[i].Add(util);
                        utilAll.Add(util);
                    }
                }
            }
            if (effCore[i].Get(v)) table_.Set(coreSt_[i].idxEff, Ok(v));
            if (usageCore[i].Get(v)) table_.Set(coreSt_[i].idxUsage, Ok(v));
            if (utilCore[i].Get(v)) table_.Set(coreSt_[i].idxUtil, Ok(v));
        }
        if (lu.ok) {
            table_.Set(idxUsageMax_, Ok(lu.maxPct));
            table_.Set(idxUsageTotal_, Ok(lu.totalPct));
        }
        Mean effAvgC, usageAvgC, utilAvgC;          // avg 列 = 核均值的均值
        for (size_t i = 0; i < cores_.size(); ++i) {
            if (effCore[i].Get(v)) effAvgC.Add(v);
            if (usageCore[i].Get(v)) usageAvgC.Add(v);
            if (utilCore[i].Get(v)) utilAvgC.Add(v);
        }
        if (effAvgC.Get(v)) table_.Set(idxAvgEff_, Ok(v));
        if (effAll.Get(v)) table_.Set(idxAllEff_, Ok(v));
        if (usageAvgC.Get(v)) table_.Set(idxAvgUsage_, Ok(v));
        if (utilAvgC.Get(v)) table_.Set(idxAvgUtil_, Ok(v));
        if (utilAll.Get(v)) table_.Set(idxTotalUtil_, Ok(v));
        if (c0Avg.Get(v)) table_.Set(idxAvgC0_, Ok(v));

        /* (g) 驻留 —— 包级(kPkgRes,ctor 探测过的)+ 每核 C1/C6/C7
         * (repLP,ctor 探测过的),÷ΔTSC×100;计数器读取失败保基线。 */
        for (PkgRes& pr : pkgRes_) {
            uint64_t r = 0;
            if (!io_.ReadMsr(0, pr.msr, r)) continue;
            if (dtsc > 0.0)
                table_.Set(pr.idx, Ok((double)(r - pr.prev) / dtsc * 100.0));
            pr.prev = r;
        }
        Mean c1Avg, c6Avg, c7Avg;
        for (size_t i = 0; i < cores_.size(); ++i) {
            CoreState& cs = coreSt_[i];
            const unsigned lp = cores_[i].repLP;
            uint64_t r = 0;
            if (cs.idxC1 >= 0 && io_.ReadMsr(lp, MSR_CORE_C1_RESIDENCY, r)) {
                if (dtsc > 0.0) {
                    const double p = (double)(r - cs.prevC1) / dtsc * 100.0;
                    table_.Set(cs.idxC1, Ok(p));
                    c1Avg.Add(p);
                }
                cs.prevC1 = r;
            }
            if (cs.idxC6 >= 0 && io_.ReadMsr(lp, MSR_CORE_C6_RESIDENCY, r)) {
                if (dtsc > 0.0) {
                    const double p = (double)(r - cs.prevC6) / dtsc * 100.0;
                    table_.Set(cs.idxC6, Ok(p));
                    c6Avg.Add(p);
                }
                cs.prevC6 = r;
            }
            if (cs.idxC7 >= 0 && io_.ReadMsr(lp, MSR_CORE_C7_RESIDENCY, r)) {
                if (dtsc > 0.0) {
                    const double p = (double)(r - cs.prevC7) / dtsc * 100.0;
                    table_.Set(cs.idxC7, Ok(p));
                    c7Avg.Add(p);
                }
                cs.prevC7 = r;
            }
        }
        if (c1Avg.Get(v)) table_.Set(idxAvgC1_, Ok(v));
        if (c6Avg.Get(v)) table_.Set(idxAvgC6_, Ok(v));
        if (c7Avg.Get(v)) table_.Set(idxAvgC7_, Ok(v));

        /* (h) Limit Reasons —— 0x64F/0x650/0x651 log 位 16+i;avg = OR。 */
        for (LimGroup& g : lims_) {
            uint64_t r = 0;
            if (!io_.ReadMsr(0, g.msr, r)) continue;
            unsigned orAll = 0;
            for (unsigned b = 0; b < g.count; ++b) {
                const unsigned bit = (unsigned)((r >> (16 + b)) & 1);
                table_.Set(g.idxBits[b], Ok((double)bit));
                orAll |= bit;
            }
            table_.Set(g.idxAvg, Ok((double)(orAll ? 1 : 0)));
        }

        /* (i) SMI —— 照抄 :817 的 smi - prev_smi(仅 Sample,非列)。 */
        {
            uint64_t smi = 0;
            if (io_.ReadMsr(0, MSR_SMI_COUNT, smi)) {
                s.smiDelta = smi - prevSmi_;
                prevSmi_ = smi;
            } else {
                s.smiDelta = std::nullopt;
            }
        }

        /* (j) Sample 派生 —— 全部经表 Lookup(键缺失/无效 -> NA,
         * UI 按既有 caps 逻辑降级,渲染层零改动)。 */
        s.pkgW = table_.Lookup("power.pkg");
        s.coresW = table_.Lookup("power.ia");
        s.gfxW = table_.Lookup("power.gt");
        s.platformW = table_.Lookup("power.sys");
        s.tempC = table_.Lookup("temp.pkg");
        const Reading clockAvgR = table_.Lookup("clock.avg");
        s.freqGHz = clockAvgR.valid ? Ok(clockAvgR.value / 1000.0) : NA();
        s.utilPct = table_.Lookup("usage.total");
        /* c0/c2/c6 保持既有包级语义:包级 0x60D[0:38]/0x3F9[0:38] 有效值
         * 是每核 TSC 份额之和(SDM),v2 既有口径不动 —— C6 钳制于 C2 内,
         * c2 字段存 C2−C6 中间值。 */
        const Reading c2 = table_.Lookup("pkgres.c2");
        const Reading c6 = table_.Lookup("pkgres.c6");
        if (c2.valid && c6.valid) {
            double c2pct = c2.value, c6pct = c6.value;
            if (c6pct > c2pct) c6pct = c2pct;      /* C6 counts within C2+ */
            double c0pct = 100.0 - c2pct; if (c0pct < 0) c0pct = 0;
            s.c0Pct = Ok(c0pct);
            s.c2Pct = Ok(c2pct - c6pct);
            s.c6Pct = Ok(c6pct);
        } else {
            s.c0Pct = NA();
            s.c2Pct = NA();
            s.c6Pct = NA();
        }
        /* PL:动态列优先、静态列兜底;tau/locked 恒取静态 0x610。 */
        const Reading pl1d = table_.Lookup("pl1.dynamic");
        const Reading pl1s = table_.Lookup("pl1.static");
        s.powerLimit.sustainedW = pl1d.valid ? pl1d : pl1s;
        const Reading pl2d = table_.Lookup("pl2.dynamic");
        const Reading pl2s = table_.Lookup("pl2.static");
        s.powerLimit.burstW = pl2d.valid ? pl2d : pl2s;
        s.powerLimit.sustainedWindowS = tauStatic_;
        s.powerLimit.locked = lockedStatic_;

        return anyPower;   // 全部功率域失败才整体失败(熔断契约)
    }

private:
    // 每核状态:探测结论 + 列索引(-1 = 无列)+ C-state 差分基线。
    struct CoreState {
        bool perfOk = false;     // 0x198 可读 -> 时钟/VID/比率列
        bool thermOk = false;    // 0x19C 可读 -> 温度/距离/降频列
        bool c1Ok = false, c6Ok = false, c7Ok = false;  // 驻留 MSR 可读性
        int tjMax = 0;           // 本核 0x1A2[23:16];0 -> 解码回退 caps_.tjMaxC
        uint64_t prevC1 = 0, prevC6 = 0, prevC7 = 0;
        int idxVid = -1, idxClock = -1, idxRatio = -1;
        int idxTemp = -1, idxDist = -1;
        int idxThr[3] = {-1, -1, -1};   // thermal/crit/plim log
        int idxEff = -1, idxUsage = -1, idxUtil = -1;
        int idxC1 = -1, idxC6 = -1, idxC7 = -1;
    };
    // 每 LP 状态:C0 列 + APERF/MPERF 基线(amBase=false 时下次成功只建基线)
    struct LpState {
        int idxC0 = -1;
        uint64_t prevAperf = 0, prevMperf = 0;
        bool amBase = false;
    };
    struct PkgRes { uint32_t msr; int idx; uint64_t prev; };
    struct LimGroup {
        uint32_t msr;
        const char* keyPrefix;            // "lim.ia" / "lim.gt" / "lim.ring"
        const char* avgName;              // "(avg) [Yes/No]" 汇总列名(HWiNFO 原文)
        const char* const* names;
        unsigned count;
        int idxAvg = -1;
        std::vector<int> idxBits;
    };

    // vendor/cpuName/logicalProcessors/baseGHz 来自 PlatformInfo;
    // gfx/platform 由 ctor 能量域基线探测回填,其余探测项就地置位。
    static PlatformCaps BuildCaps(const PlatformInfo& info) {
        PlatformCaps c;
        c.vendor = info.vendor;
        c.cpuName = info.cpuName;
        c.logicalProcessors = info.logicalProcessors;
        c.baseGHz = info.baseGHz;
        c.smi = true;                     // MSR_SMI_COUNT 家族恒在(既有语义)
        return c;
    }

    // 拓扑规范化:cores 空/repLP 或线程 LP 越界 = 拓扑未知,退回
    // "每 LP 一个单线程核"(全部 effClass 0,P-core 命名)—— 保底覆盖
    // 全部 LP,与 v2 全 LP 遍历语义一致(见 PowerDashModel.h cores 注释)。
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

    // 每核能力探测(各读一次 repLP;驻留计数器顺手当基线,瞬态寄存器
    // 只看可读性)—— 失败即不建对应列(准确度红线:无把握的列宁可省略)。
    void ProbeCores() {
        coreSt_.resize(cores_.size());
        for (size_t i = 0; i < cores_.size(); ++i) {
            CoreState& cs = coreSt_[i];
            const unsigned lp = cores_[i].repLP;
            uint64_t v = 0;
            cs.perfOk = io_.ReadMsr(lp, MSR_PERF_STATUS, v);
            cs.thermOk = io_.ReadMsr(lp, MSR_THERM_STATUS, v);
            if (io_.ReadMsr(lp, MSR_TEMPERATURE_TARGET, v))
                cs.tjMax = (int)((v >> 16) & 0xFF);
            if (io_.ReadMsr(lp, MSR_CORE_C1_RESIDENCY, v)) {
                cs.prevC1 = v;
                cs.c1Ok = true;
            }
            if (io_.ReadMsr(lp, MSR_CORE_C6_RESIDENCY, v)) {
                cs.prevC6 = v;
                cs.c6Ok = true;
            }
            if (io_.ReadMsr(lp, MSR_CORE_C7_RESIDENCY, v)) {
                cs.prevC7 = v;
                cs.c7Ok = true;
            }
        }
    }

    // 包级探测:封装驻留 MSR 逐个(可读才入表);Limit Reasons 三寄存器
    // 整组(任一失败整组省略,不猜 —— LNL 若无 0x64F 族自然缺列)。
    void ProbePackage() {
        for (const auto& p : kPkgRes) {
            uint64_t v = 0;
            if (io_.ReadMsr(0, p.msr, v))
                pkgRes_.push_back(PkgRes{p.msr, -1, v});   // idx 建列时回填
        }
        uint64_t v = 0;
        if (io_.ReadMsr(0, MSR_IA_LIMIT_REASON, v) &&
            io_.ReadMsr(0, MSR_GT_LIMIT_REASON, v) &&
            io_.ReadMsr(0, MSR_RING_LIMIT_REASON, v)) {
            lims_ = {
                {MSR_IA_LIMIT_REASON, "lim.ia", "IA Limit Reasons (avg) [Yes/No]",
                 kIaReasons, 11},
                {MSR_GT_LIMIT_REASON, "lim.gt", "GT Limit Reasons (avg) [Yes/No]",
                 kGtReasons, 13},
                {MSR_RING_LIMIT_REASON, "lim.ring", "Ring Limit Reasons (avg) [Yes/No]",
                 kRingReasons, 10},
            };
        }
    }

    // :572-594 —— PCI 0:0.0 reg 0x48 取 MCHBAR,+0x59A0 即 PL 窗口;
    // 失败则 caps_.powerLimits 保持 false(能力降级,不构造失败)。
    void MapPlWindow() {
        uint32_t mchbar = 0;
        if (!io_.ReadPciCfg(0, 0, 0, 0x48, mchbar) || (mchbar & 0x1) == 0)
            return;                                /* MCHBAR missing/disabled */
        uint64_t mmio_phys = static_cast<uint64_t>(mchbar & ~1u) + 0x59A0;
        uint64_t page = mmio_phys & ~0xFFFull;
        void* virt = nullptr;
        if (!io_.MapPhys(page, 0x1000, virt))
            return;
        map_ = virt;
        pl_ = reinterpret_cast<volatile uint32_t*>(
            reinterpret_cast<uintptr_t>(virt) + (mmio_phys & 0xFFF));
        caps_.powerLimits = true;
    }

    int AddCol(const char* key, const std::string& name, SensorFmt fmt) {
        return (int)table_.Add(key, name, fmt);
    }
    std::string SuffixName(const CoreInfo& c, const char* suffix) {
        return CoreName(c) + suffix;
    }

    // 构建列集:组序对齐 intel.CSV(电压→时钟→有效→Usage→Utility→Ratio→
    // 温度→降频→功率→限值→封装驻留→核驻留→Limit Reasons);键表见
    // task-4-brief Interfaces。探测失败的能力不建列(动态列集,同 HWiNFO)。
    void BuildColumns() {
        char key[48];
        const size_t n = cores_.size();
        coreSt_.resize(n);
        lp_.assign(nLP_, LpState{});

        /* 1 电压 */
        idxAvgVid_ = AddCol("vid.avg", "Core Voltages (avg) [V]", SensorFmt::F3);
        for (size_t i = 0; i < n; ++i)
            if (coreSt_[i].perfOk) {
                snprintf(key, sizeof(key), "vid.%zu", i);
                coreSt_[i].idxVid =
                    AddCol(key, SuffixName(cores_[i], " Voltage [V]"),
                           SensorFmt::F3);
            }

        /* 2 时钟 */
        idxAvgClock_ = AddCol("clock.avg", "Core Clocks (avg) [MHz]", SensorFmt::F1);
        for (size_t i = 0; i < n; ++i)
            if (coreSt_[i].perfOk) {
                snprintf(key, sizeof(key), "clock.%zu", i);
                coreSt_[i].idxClock =
                    AddCol(key, SuffixName(cores_[i], " Clock [MHz]"),
                           SensorFmt::F1);
            }
        idxClockBus_ = AddCol("clock.bus", "Bus Clock [MHz]", SensorFmt::F1);

        /* 3 有效时钟(eff.all = "Average Effective Clock",全机线程均值) */
        idxAvgEff_ = AddCol("eff.avg", "Core Effective Clocks (avg) [MHz]",
                            SensorFmt::F1);
        for (size_t i = 0; i < n; ++i) {
            snprintf(key, sizeof(key), "eff.%zu", i);
            coreSt_[i].idxEff =
                AddCol(key, SuffixName(cores_[i], " Effective Clock [MHz]"),
                       SensorFmt::F1);
        }
        idxAllEff_ = AddCol("eff.all", "Average Effective Clock [MHz]",
                            SensorFmt::F1);

        /* 4 Usage */
        idxAvgUsage_ = AddCol("usage.avg", "Core Usage (avg) [%]", SensorFmt::PCT1);
        for (size_t i = 0; i < n; ++i) {
            snprintf(key, sizeof(key), "usage.%zu", i);
            coreSt_[i].idxUsage =
                AddCol(key, SuffixName(cores_[i], " Usage [%]"), SensorFmt::PCT1);
        }
        idxUsageMax_ = AddCol("usage.max", "Max CPU/Thread Usage [%]",
                              SensorFmt::PCT1);
        idxUsageTotal_ = AddCol("usage.total", "Total CPU Usage [%]",
                                SensorFmt::PCT1);
        idxClockmod_ = AddCol("usage.clockmod", "On-Demand Clock Modulation [%]",
                              SensorFmt::PCT1);

        /* 5 Utility */
        idxAvgUtil_ = AddCol("util.avg", "Core Utility (avg) [%]", SensorFmt::PCT1);
        for (size_t i = 0; i < n; ++i) {
            snprintf(key, sizeof(key), "util.%zu", i);
            coreSt_[i].idxUtil =
                AddCol(key, SuffixName(cores_[i], " Utility [%]"), SensorFmt::PCT1);
        }
        idxTotalUtil_ = AddCol("util.total", "Total CPU Utility [%]",
                               SensorFmt::PCT1);

        /* 6 Ratio(Uncore Ratio 见文件头注释:无公开解码,省略) */
        idxAvgRatio_ = AddCol("ratio.avg", "Core Ratios (avg) [x]",
                              SensorFmt::RATIO2);
        for (size_t i = 0; i < n; ++i)
            if (coreSt_[i].perfOk) {
                snprintf(key, sizeof(key), "ratio.%zu", i);
                coreSt_[i].idxRatio =
                    AddCol(key, SuffixName(cores_[i], " Ratio [x]"),
                           SensorFmt::RATIO2);
            }

        /* 7 温度(HWiNFO 核温列名即 "<核名> [°C]",无 Temperature 字样) */
        idxAvgTemp_ = AddCol("temp.avg", "Core Temperatures (avg) [°C]",
                             SensorFmt::F1);
        for (size_t i = 0; i < n; ++i)
            if (coreSt_[i].thermOk) {
                snprintf(key, sizeof(key), "temp.%zu", i);
                coreSt_[i].idxTemp =
                    AddCol(key, SuffixName(cores_[i], " [°C]"), SensorFmt::F1);
            }
        idxAvgDist_ = AddCol("tjmax.avg", "Core Distance to TjMAX (avg) [°C]",
                             SensorFmt::F1);
        for (size_t i = 0; i < n; ++i)
            if (coreSt_[i].thermOk) {
                snprintf(key, sizeof(key), "tjmax.%zu", i);
                coreSt_[i].idxDist =
                    AddCol(key, SuffixName(cores_[i], " Distance to TjMAX [°C]"),
                           SensorFmt::F1);
            }
        idxTempPkg_ = AddCol("temp.pkg", "CPU Package [°C]", SensorFmt::F1);
        idxCoresMax_ = AddCol("temp.coremax", "Core Max [°C]", SensorFmt::F1);

        /* 8 降频位(per-core log 三族 + (avg)=OR + 包级三列;无任何核
         * 0x19C 列时 (avg) 列一并省略) */
        static const char* thrAvgKeys[3] = {"thr.avg.thermal", "thr.avg.crit",
                                            "thr.avg.plim"};
        static const char* thrAvgNames[3] = {
            "Core Thermal Throttling (avg) [Yes/No]",
            "Core Critical Temperature (avg) [Yes/No]",
            "Core Power Limit Exceeded (avg) [Yes/No]"};
        static const char* thrPerKeys[3] = {"thermal", "crit", "plim"};
        static const char* thrPerNames[3] = {
            " Thermal Throttling [Yes/No]", " Critical Temperature [Yes/No]",
            " Power Limit Exceeded [Yes/No]"};
        bool anyTherm = false;
        for (size_t i = 0; i < n; ++i)
            if (coreSt_[i].thermOk) anyTherm = true;
        for (int k = 0; k < 3; ++k) {
            if (anyTherm)
                idxThrAvg_[k] =
                    AddCol(thrAvgKeys[k], thrAvgNames[k], SensorFmt::YESNO);
            for (size_t i = 0; i < n; ++i)
                if (coreSt_[i].thermOk) {
                    snprintf(key, sizeof(key), "thr.%zu.%s", i, thrPerKeys[k]);
                    coreSt_[i].idxThr[k] =
                        AddCol(key, SuffixName(cores_[i], thrPerNames[k]),
                               SensorFmt::YESNO);
                }
        }
        static const char* thrPkgKeys[3] = {"thr.pkg.thermal", "thr.pkg.crit",
                                            "thr.pkg.plim"};
        static const char* thrPkgNames[3] = {
            "Package/Ring Thermal Throttling [Yes/No]",
            "Package/Ring Critical Temperature [Yes/No]",
            "Package/Ring Power Limit Exceeded [Yes/No]"};
        for (int k = 0; k < 3; ++k)
            idxThrPkg_[k] = AddCol(thrPkgKeys[k], thrPkgNames[k], SensorFmt::YESNO);

        /* 9 功率(四域恒建列;caps 位以 ctor 基线探测为准,帧读取失败 NA) */
        static const char* powerKeys[4] = {"power.pkg", "power.ia", "power.gt",
                                           "power.sys"};
        static const char* powerNames[4] = {"CPU Package Power [W]",
                                            "IA Cores Power [W]",
                                            "GT Cores Power [W]",
                                            "Total System Power [W]"};
        for (int k = 0; k < 4; ++k)
            idxPower_[k] = AddCol(powerKeys[k], powerNames[k], SensorFmt::F3);

        /* 10 限值(PL 静/动态 + cTDP;tau 不是列,仅入 Sample) */
        idxPl1S_ = AddCol("pl1.static", "PL1 Power Limit (Static) [W]",
                          SensorFmt::F3);
        idxPl1D_ = AddCol("pl1.dynamic", "PL1 Power Limit (Dynamic) [W]",
                          SensorFmt::F3);
        idxPl2S_ = AddCol("pl2.static", "PL2 Power Limit (Static) [W]",
                          SensorFmt::F3);
        idxPl2D_ = AddCol("pl2.dynamic", "PL2 Power Limit (Dynamic) [W]",
                          SensorFmt::F3);
        idxCtdp_ = AddCol("ctdp.level", "Current cTDP Level []", SensorFmt::TEXT);

        /* 11 封装驻留(ctor 探测过的才建列,kPkgRes 固定序) */
        for (PkgRes& pr : pkgRes_)
            for (const auto& p : kPkgRes)
                if (p.msr == pr.msr)
                    pr.idx = AddCol(p.key, p.name, SensorFmt::PCT1);

        /* 12 核驻留:C0(avg + 每线程 "<核> T<i> C0")+ C1/C6/C7
         * (avg + 每核,ctor 探测过的核才建列;avg 列 = 有任一核列即建) */
        idxAvgC0_ = AddCol("cores.c0.avg", "Core C0 Residency (avg) [%]",
                           SensorFmt::PCT1);
        for (size_t i = 0; i < n; ++i)
            for (size_t t = 0; t < cores_[i].threads.size(); ++t) {
                const unsigned lp = cores_[i].threads[t];
                snprintf(key, sizeof(key), "cores.c0.lp%u", lp);
                char tail[32];
                snprintf(tail, sizeof(tail), " T%zu C0 Residency [%%]", t);
                lp_[lp].idxC0 =
                    AddCol(key, SuffixName(cores_[i], tail), SensorFmt::PCT1);
            }
        struct CoreC { uint32_t msr; const char* key; const char* name;
                       const char* avgKey; const char* avgName;
                       bool CoreState::*ok; int CoreState::*idx; int* avgIdx; };
        CoreC coreCs[3] = {
            {MSR_CORE_C1_RESIDENCY, "c1", " C1 Residency [%]",
             "cores.c1.avg", "Core C1 Residency (avg) [%]",
             &CoreState::c1Ok, &CoreState::idxC1, &idxAvgC1_},
            {MSR_CORE_C6_RESIDENCY, "c6", " C6 Residency [%]",
             "cores.c6.avg", "Core C6 Residency (avg) [%]",
             &CoreState::c6Ok, &CoreState::idxC6, &idxAvgC6_},
            {MSR_CORE_C7_RESIDENCY, "c7", " C7 Residency [%]",
             "cores.c7.avg", "Core C7 Residency (avg) [%]",
             &CoreState::c7Ok, &CoreState::idxC7, &idxAvgC7_},
        };
        for (const CoreC& cc : coreCs) {
            bool any = false;
            for (size_t i = 0; i < n; ++i)
                if (coreSt_[i].*(cc.ok)) any = true;
            if (!any) continue;
            *(cc.avgIdx) = AddCol(cc.avgKey, cc.avgName, SensorFmt::PCT1);
            for (size_t i = 0; i < n; ++i)
                if (coreSt_[i].*(cc.ok)) {
                    snprintf(key, sizeof(key), "cores.%s.%zu", cc.key, i);
                    coreSt_[i].*(cc.idx) =
                        AddCol(key, SuffixName(cores_[i], cc.name),
                               SensorFmt::PCT1);
                }
        }

        /* 13 Limit Reasons(整组:avg + log 位序事件列,列名逐字符照 HWiNFO) */
        for (LimGroup& g : lims_) {
            g.idxAvg = AddCol((std::string(g.keyPrefix) + ".avg").c_str(),
                              g.avgName, SensorFmt::YESNO);
            g.idxBits.resize(g.count);
            for (unsigned b = 0; b < g.count; ++b) {
                char bitKey[48], bitName[96];
                snprintf(bitKey, sizeof(bitKey), "%s.%u", g.keyPrefix, b);
                snprintf(bitName, sizeof(bitName), "%s [Yes/No]", g.names[b]);
                g.idxBits[b] = AddCol(bitKey, bitName, SensorFmt::YESNO);
            }
        }
    }

    bool HasCoreResidencyColumn() const {
        for (const CoreState& cs : coreSt_)
            if (cs.idxC1 >= 0 || cs.idxC6 >= 0 || cs.idxC7 >= 0) return true;
        for (const LpState& l : lp_)
            if (l.idxC0 >= 0) return true;
        return false;
    }

    // :657-670 —— 能量/SMI prev 计数器(读取失败保持 0,与源一致)+
    // TSC 锚点;APERF/MPERF 基线在 ProbeCores 后逐 LP 就地建立。
    // gfx/platform 能力位 = 基线读取成功(诚实降级,MSR 缺席帧值恒 NA)。
    void ReadBaseline() {
        io_.ReadMsr(0, MSR_PKG_ENERGY_STATUS, prevPkg_);
        io_.ReadMsr(0, MSR_PP0_ENERGY_STATUS, prevPp0_);
        caps_.gfxPower = io_.ReadMsr(0, MSR_PP1_ENERGY_STATUS, prevPp1_);
        caps_.platformPower = io_.ReadMsr(0, MSR_SYS_ENERGY_STATUS, prevSys_);
        io_.ReadMsr(0, MSR_SMI_COUNT, prevSmi_);
        for (size_t i = 0; i < cores_.size(); ++i)
            for (const unsigned lp : cores_[i].threads) {
                if (lp >= nLP_ || lp >= lp_.size()) continue;
                uint64_t a = 0, m = 0;
                if (io_.ReadMsr(lp, MSR_IA32_APERF, a) &&
                    io_.ReadMsr(lp, MSR_IA32_MPERF, m)) {
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
    bool unitsOk_ = false;
    double energyUnit_ = 0.0, powerUnit_ = 0.0, timeUnitS_ = 0.0;
    double tscHz_ = 0.0, busClock_ = 0.0;  // MHz;0 = 未知 -> 时钟列 NA
    void* map_ = nullptr;
    volatile uint32_t* pl_ = nullptr;      // pl_[0]=PL1 raw, pl_[1]=PL2 raw
    std::unique_ptr<UsageMonitor> usage_;
    std::vector<CoreState> coreSt_;
    std::vector<LpState> lp_;
    std::vector<PkgRes> pkgRes_;
    std::vector<LimGroup> lims_;
    // 帧间 PL 静态寄存器暂存(Sample 派生用,非列)
    Reading tauStatic_;
    bool lockedStatic_ = false;
    // 既有差分基线(能量四域 + SMI + TSC)
    uint64_t prevPkg_ = 0, prevPp0_ = 0, prevPp1_ = 0, prevSys_ = 0;
    uint64_t prevSmi_ = 0, prevTsc_ = 0;
    // 列索引(-1 = 该平台无此列)
    int idxAvgVid_ = -1, idxAvgClock_ = -1, idxClockBus_ = -1;
    int idxAvgEff_ = -1, idxAllEff_ = -1;
    int idxAvgUsage_ = -1, idxUsageMax_ = -1, idxUsageTotal_ = -1;
    int idxClockmod_ = -1;
    int idxAvgUtil_ = -1, idxTotalUtil_ = -1;
    int idxAvgRatio_ = -1;
    int idxAvgTemp_ = -1, idxAvgDist_ = -1, idxTempPkg_ = -1, idxCoresMax_ = -1;
    int idxThrAvg_[3] = {-1, -1, -1}, idxThrPkg_[3] = {-1, -1, -1};
    int idxPower_[4] = {-1, -1, -1, -1};
    int idxPl1S_ = -1, idxPl1D_ = -1, idxPl2S_ = -1, idxPl2D_ = -1, idxCtdp_ = -1;
    int idxAvgC0_ = -1, idxAvgC1_ = -1, idxAvgC6_ = -1, idxAvgC7_ = -1;
};

std::unique_ptr<IPlatformProbe> CreateIntelProbe(DriverIo& io,
                                                 const PlatformInfo& info) {
    return std::make_unique<IntelProbe>(io, info);
}

} // namespace pd
