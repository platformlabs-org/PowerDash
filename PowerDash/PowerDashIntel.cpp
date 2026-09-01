// PowerDashIntel.cpp —— Intel RAPL 平台探针(Task 3)。
// 采样表达式自 PowerDash.cpp RunMonitor 逐段迁移,数值逻辑原样照搬,
// 仅把输出改写为 Sample v2 的 Reading(Ok/NA);原位置留待 Task 6 删除。
//   单位        PowerDash.cpp:557-570   thermal spec :618-630
//   MCHBAR PL   :572-594                TjMax       :632-638
//   基线计数    :657-670                每帧采样     :760-850
#include "PowerDashProbe.h"
#include <cmath>
#include <cstdint>
#include <intrin.h>

namespace pd {
namespace {
constexpr uint32_t MSR_RAPL_POWER_UNIT = 0x606, MSR_PKG_POWER_INFO = 0x614;
constexpr uint32_t MSR_PKG_ENERGY_STATUS = 0x611, MSR_PP0_ENERGY_STATUS = 0x639;
constexpr uint32_t MSR_PP1_ENERGY_STATUS = 0x641, MSR_SYS_ENERGY_STATUS = 0x64D;
/* 注意:源文件 PowerDash.cpp:43 定义 MSR_PACKAGE_THERM_STATUS = 0x1B1
 * (brief 骨架里的 0x1B2 与 fixture 的 0x613 均为笔误,以源为准)。 */
constexpr uint32_t MSR_PACKAGE_THERM_STATUS = 0x1B1, MSR_TEMPERATURE_TARGET = 0x1A2;
constexpr uint32_t MSR_IA32_APERF = 0xE8, MSR_IA32_MPERF = 0xE7;
constexpr uint32_t MSR_PKG_C2_RESIDENCY = 0x60D, MSR_PKG_C6_RESIDENCY = 0x3F9;
constexpr uint32_t MSR_SMI_COUNT = 0x34;
} // namespace

class IntelProbe : public IPlatformProbe {
public:
    IntelProbe(DriverIo& io, const PlatformInfo& info) : io_(io) {
        caps_ = BuildCaps(info);       // vendor/name/nLP/baseGHz + 探测项
        uint64_t u = 0;                                          // :557-570
        if (io_.ReadMsr(0, MSR_RAPL_POWER_UNIT, u)) {
            energyUnit_ = 1.0 / std::pow(2.0, (u >> 8) & 0x1F);
            powerUnit_  = 1.0 / std::pow(2.0, (u >> 0) & 0x0F);
            timeUnitS_  = 1.0 / std::pow(2.0, (u >> 16) & 0x0F);
            unitsOk_ = true;
        }
        uint64_t spec = 0;                                       // :618-630
        if (unitsOk_ && io_.ReadMsr(0, MSR_PKG_POWER_INFO, spec))
            caps_.budgetW = ((spec >> 0) & 0x7FFF) * powerUnit_;  // thermal spec,PL 兜底刻度
        uint64_t tj = 0;                                         // :632-638
        if (io_.ReadMsr(0, MSR_TEMPERATURE_TARGET, tj) && ((tj >> 16) & 0xFF) != 0)
            caps_.tjMaxC = static_cast<int>((tj >> 16) & 0xFF);
        MapPlWindow();                                           // :572-594 MCHBAR+0x59A0
        ReadBaseline();                                          // :657-670 prev 计数器
    }

    /* 源码只在 -setpl 路径解映射;Probe 拥有映射即负责释放。 */
    ~IntelProbe() override {
        if (map_) io_.UnmapPhys(map_);
    }

    const PlatformCaps& caps() const override { return caps_; }

    bool readSample(Sample& s) override {                        // :760-850 映射
        if (!unitsOk_) return false;
        bool anyPower = false;

        /* (a) 能量差分 + 32 位回绕 —— 照抄 :760-779 的 if(curr<prev) += 1<<32;
         * 与源一致按 1 s 采样窗口把能量差直接当功率数值。 */
        uint64_t curr = 0;
        if (io_.ReadMsr(0, MSR_PKG_ENERGY_STATUS, curr)) {
            if (curr < prevPkg_) curr += (1ULL << 32);
            s.pkgW = Ok((curr - prevPkg_) * energyUnit_);
            prevPkg_ = curr;
            anyPower = true;
        } else s.pkgW = NA();
        if (io_.ReadMsr(0, MSR_PP0_ENERGY_STATUS, curr)) {
            if (curr < prevPp0_) curr += (1ULL << 32);
            s.coresW = Ok((curr - prevPp0_) * energyUnit_);
            prevPp0_ = curr;
            anyPower = true;
        } else s.coresW = NA();
        if (io_.ReadMsr(0, MSR_PP1_ENERGY_STATUS, curr)) {
            if (curr < prevPp1_) curr += (1ULL << 32);
            s.gfxW = Ok((curr - prevPp1_) * energyUnit_);
            prevPp1_ = curr;
            anyPower = true;
        } else s.gfxW = NA();
        if (io_.ReadMsr(0, MSR_SYS_ENERGY_STATUS, curr)) {
            if (curr < prevSys_) curr += (1ULL << 32);
            s.platformW = Ok((curr - prevSys_) * energyUnit_);
            prevSys_ = curr;
            anyPower = true;
        } else s.platformW = NA();

        /* (b) PL(mmio 窗口可用时)—— 照抄 :781-785 与 :849-850。
         * v2 简化:tau 只保留 PL1 窗口(sustainedWindowS),PL2 的 tau 丢弃;
         * rawTau==0 与源 fmtTau 的 "n/a" 对应 -> NA。 */
        if (pl_) {
            uint32_t pl1_raw = pl_[0];
            uint32_t pl2_raw = pl_[1];
            s.powerLimit.sustainedW = Ok((pl1_raw & 0x7FFF) * 0.125);
            s.powerLimit.burstW     = Ok((pl2_raw & 0x7FFF) * 0.125);
            uint64_t rawTau = (pl1_raw >> 17) & 0x7F;
            s.powerLimit.sustainedWindowS = rawTau ? Ok(rawTau * timeUnitS_) : NA();
            s.powerLimit.locked = ((pl1_raw >> 31) & 1) != 0;
        } else {
            s.powerLimit.sustainedW = NA();
            s.powerLimit.burstW = NA();
            s.powerLimit.sustainedWindowS = NA();
            s.powerLimit.locked = false;
        }

        /* (c) 温度 —— 照抄 :787-795(package therm status:headroom below TjMax) */
        {
            uint64_t th = 0;
            if (caps_.tjMaxC > 0 &&
                io_.ReadMsr(0, MSR_PACKAGE_THERM_STATUS, th) &&
                (th & (1ULL << 31)))
                s.tempC = Ok((double)(caps_.tjMaxC - (int)((th >> 16) & 0x7F)));
            else
                s.tempC = NA();
        }

        /* (d) 频率 —— 照抄 :797-809(APERF/MPERF 比值 * base)。
         * baseMHz 来自 PlatformInfo(入口层 CPUID 0x16),0=未知 -> NA。 */
        {
            uint64_t aperf = 0, mperf = 0;
            if (io_.ReadMsr(0, MSR_IA32_APERF, aperf) &&
                io_.ReadMsr(0, MSR_IA32_MPERF, mperf)) {
                double baseMHz = caps_.baseGHz * 1000.0;
                if (baseMHz > 0 && mperf > prevMperf_)
                    s.freqGHz = Ok(baseMHz * (double)(aperf - prevAperf_)
                                   / (double)(mperf - prevMperf_) / 1000.0);
                else
                    s.freqGHz = NA();
                prevAperf_ = aperf;
                prevMperf_ = mperf;
            } else {
                s.freqGHz = NA();
            }
        }

        /* (e) 驻留 —— 照抄 :811-816(C6 钳制于 C2 内;c2 字段存 C2-C6 中间值) */
        {
            uint64_t c2 = 0, c6 = 0;
            if (io_.ReadMsr(0, MSR_PKG_C2_RESIDENCY, c2) &&
                io_.ReadMsr(0, MSR_PKG_C6_RESIDENCY, c6)) {
                uint64_t tsc = __rdtsc();
                double dtsc = (double)(tsc - prevTsc_);
                double c2pct = dtsc > 0 ? 100.0 * (double)(c2 - prevC2_) / dtsc : 0;
                double c6pct = dtsc > 0 ? 100.0 * (double)(c6 - prevC6_) / dtsc : 0;
                if (c6pct > c2pct) c6pct = c2pct;      /* C6 counts within C2+ */
                double c0pct = 100.0 - c2pct; if (c0pct < 0) c0pct = 0;
                double c2mid = c2pct - c6pct;
                s.c0Pct = Ok(c0pct);
                s.c2Pct = Ok(c2mid);
                s.c6Pct = Ok(c6pct);
                prevC2_ = c2; prevC6_ = c6; prevTsc_ = tsc;
            } else {
                s.c0Pct = NA(); s.c2Pct = NA(); s.c6Pct = NA();
            }
        }

        /* (f) SMI —— 照抄 :817 的 smi - prev_smi */
        {
            uint64_t smi = 0;
            if (io_.ReadMsr(0, MSR_SMI_COUNT, smi)) {
                s.smiDelta = smi - prevSmi_;
                prevSmi_ = smi;
            } else {
                s.smiDelta = std::nullopt;
            }
        }

        return anyPower;   // 全部功率域失败才整体失败
    }

private:
    // vendor/cpuName/logicalProcessors/baseGHz 来自 PlatformInfo;
    // Intel RAPL 家族恒提供 gfx(PP1)/platform(PSYS)/residency/SMI,
    // powerLimits 由 MapPlWindow 成功后置 true。
    static PlatformCaps BuildCaps(const PlatformInfo& info) {
        PlatformCaps c;
        c.vendor = info.vendor;
        c.cpuName = info.cpuName;
        c.logicalProcessors = info.logicalProcessors;
        c.baseGHz = info.baseGHz;
        c.gfxPower = true;
        c.platformPower = true;
        c.residency = true;
        c.smi = true;
        return c;
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

    // :657-670 —— 全部 prev 计数器(读取失败保持 0,与源一致)+ TSC 基线
    void ReadBaseline() {
        io_.ReadMsr(0, MSR_PKG_ENERGY_STATUS, prevPkg_);
        io_.ReadMsr(0, MSR_PP0_ENERGY_STATUS, prevPp0_);
        io_.ReadMsr(0, MSR_PP1_ENERGY_STATUS, prevPp1_);
        io_.ReadMsr(0, MSR_SYS_ENERGY_STATUS, prevSys_);
        io_.ReadMsr(0, MSR_IA32_APERF, prevAperf_);
        io_.ReadMsr(0, MSR_IA32_MPERF, prevMperf_);
        io_.ReadMsr(0, MSR_PKG_C2_RESIDENCY, prevC2_);
        io_.ReadMsr(0, MSR_PKG_C6_RESIDENCY, prevC6_);
        io_.ReadMsr(0, MSR_SMI_COUNT, prevSmi_);
        prevTsc_ = __rdtsc();
    }

    DriverIo& io_;
    PlatformCaps caps_;
    bool unitsOk_ = false;
    double energyUnit_ = 0.0, powerUnit_ = 0.0, timeUnitS_ = 0.0;
    void* map_ = nullptr;
    volatile uint32_t* pl_ = nullptr;      // pl_[0]=PL1 raw, pl_[1]=PL2 raw
    uint64_t prevPkg_ = 0, prevPp0_ = 0, prevPp1_ = 0, prevSys_ = 0;
    uint64_t prevAperf_ = 0, prevMperf_ = 0, prevC2_ = 0, prevC6_ = 0;
    uint64_t prevSmi_ = 0, prevTsc_ = 0;
};

std::unique_ptr<IPlatformProbe> CreateIntelProbe(DriverIo& io,
                                                 const PlatformInfo& info) {
    return std::make_unique<IntelProbe>(io, info);
}

} // namespace pd
