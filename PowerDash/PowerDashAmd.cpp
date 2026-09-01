// PowerDashAmd.cpp —— AMD Zen(family 17h/19h/1Ah)保底监控探针(Task 8)。
// 保底集 = RAPL 能量差分(pkg + 逐核 cores)+ SMn Tctl 温度 + APERF/MPERF
// 频率;其余能力位诚实降级(false/NA),不读 PMTable、不猜 PPT/EDC/TjMax。
// 结构照 PowerDashIntel.cpp:ctor 基线读取 + caps 构建,readSample 差分。
#include "PowerDashProbe.h"
#include <cmath>
#include <cstdint>
#include <intrin.h>
#include <vector>

namespace pd {
namespace {
// AMD RAPL 寄存器族(Zen 17h+,与 Intel RAPL MSR 位兼容 —— msr-index.h
// "MSR_AMD_RAPL_POWER_UNIT ... bit-compatible";turbostat 对 0xC0010299 同样
// 按 energy bits 8-12 解码:rapl_energy_units = ldexp(1, -(msr>>8 & 0x1f)))。
constexpr uint32_t AMD_RAPL_POWER_UNIT = 0xC0010299;
constexpr uint32_t AMD_CORE_ENERGY_STAT = 0xC001029A;   // 每核,32 位回绕
constexpr uint32_t AMD_PKG_ENERGY_STAT = 0xC001029B;    // 32 位回绕
constexpr uint32_t MSR_IA32_APERF = 0xE8, MSR_IA32_MPERF = 0xE7;
// SMN THM_TCTL:tempC = (raw >> 21) * 0.125(1/8 C 步进;经驱动原子
// IO_CTL_SMN_READ 读取,禁止用户态拆写 0x60/0x64)。
constexpr uint32_t SMN_THM_TCTL = 0x59800;

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
} // namespace

class AmdProbe : public IPlatformProbe {
public:
    AmdProbe(DriverIo& io, const PlatformInfo& info) : io_(io) {
        caps_ = BuildCaps(info);       // vendor/name/nLP/baseGHz + 保底能力位
        uint64_t u = 0;
        if (io_.ReadMsr(0, AMD_RAPL_POWER_UNIT, u)) {
            energyUnit_ = 1.0 / std::pow(2.0, DecodeEnergyBits(u));
            unitsOk_ = true;
        }
        ReadBaseline();                // pkg + 每 core 的 0xC001029A、A/M、TSC
    }

    const PlatformCaps& caps() const override { return caps_; }

    bool readSample(Sample& s) override {
        if (!unitsOk_) return false;
        bool anyPower = false;

        /* (a) pkg 能量差分 + 32 位回绕钳制(同 IntelProbe 语义)。
         * 与源一致按 1 s 采样窗口把能量差直接当功率数值。 */
        uint64_t curr = 0;
        if (io_.ReadMsr(0, AMD_PKG_ENERGY_STAT, curr)) {
            curr &= 0xFFFFFFFFull;
            if (curr < prevPkg_) curr += (1ULL << 32);
            s.pkgW = Ok((curr - prevPkg_) * energyUnit_);
            prevPkg_ = curr;
            anyPower = true;
        } else s.pkgW = NA();

        /* (b) cores:逐核读 0xC001029A(ReadMsr 的 core 形参正为此用),
         * 各核独立差分(独立回绕钳制)后求和。域语义对齐 IntelProbe:
         * 本帧任一核读取失败 -> 整域 NA(不输出残缺和,防静默少计);
         * 失败核标记 stale,恢复帧只刷新基线、跳过一次差分(陈旧 prev
         * 直接差分会把两帧能量算进一个采样窗口,造成单帧尖峰)。 */
        {
            uint64_t sum = 0;
            bool anyCore = false;
            bool anyFailed = false;
            for (unsigned core = 0; core < caps_.logicalProcessors; ++core) {
                uint64_t e = 0;
                if (!io_.ReadMsr(core, AMD_CORE_ENERGY_STAT, e)) {
                    staleCore_[core] = true;   // 下次成功读取只重置基线
                    anyFailed = true;
                    continue;
                }
                e &= 0xFFFFFFFFull;
                if (staleCore_[core]) {
                    staleCore_[core] = false;  // 恢复:刷新基线,跳过本帧差分
                    prevCoreE_[core] = e;
                    continue;
                }
                if (e < prevCoreE_[core]) e += (1ULL << 32);
                sum += e - prevCoreE_[core];
                prevCoreE_[core] = e;
                anyCore = true;
            }
            if (!anyFailed && anyCore) {
                s.coresW = Ok(sum * energyUnit_);
                anyPower = true;
            } else s.coresW = NA();
        }

        /* (c) 温度 —— SMN THM_TCTL:(raw >> 21) * 0.125 C;失败 -> NA。 */
        {
            uint32_t raw = 0;
            if (io_.ReadSmn(SMN_THM_TCTL, raw))
                s.tempC = Ok((double)(raw >> 21) * 0.125);
            else
                s.tempC = NA();
        }

        /* (d) 频率 —— APERF/MPERF 比值 * baseGHz(与 IntelProbe 同式,
         * 但 baseGHz 直接就是 GHz,无需 Intel 那步 /1000)。
         * baseGHz 来自 PlatformInfo(CPUID 0x16),0=未知 -> NA。 */
        {
            uint64_t aperf = 0, mperf = 0;
            if (io_.ReadMsr(0, MSR_IA32_APERF, aperf) &&
                io_.ReadMsr(0, MSR_IA32_MPERF, mperf)) {
                if (caps_.baseGHz > 0 && mperf > prevMperf_)
                    s.freqGHz = Ok(caps_.baseGHz * (double)(aperf - prevAperf_)
                                   / (double)(mperf - prevMperf_));
                else
                    s.freqGHz = NA();
                prevAperf_ = aperf;
                prevMperf_ = mperf;
            } else {
                s.freqGHz = NA();
            }
        }

        /* (e) 本平台不提供的域 —— 恒 NA/false(诚实降级,不猜)。 */
        s.gfxW = NA();
        s.platformW = NA();
        s.powerLimit.sustainedW = NA();
        s.powerLimit.burstW = NA();
        s.powerLimit.sustainedWindowS = NA();
        s.powerLimit.locked = false;
        s.c0Pct = NA(); s.c2Pct = NA(); s.c6Pct = NA();
        s.smiDelta = std::nullopt;

        return anyPower;   // 全部功率域失败才整体失败(熔断契约)
    }

private:
    // vendor/cpuName/logicalProcessors/baseGHz 来自 PlatformInfo;
    // 保底监控集:gfx(无 PP1 等价域)/platform(无 PSYS)/powerLimits
    // (PPT 未读)/residency/SMI 均不支持;budgetW=0(UI 走 spec fallback),
    // tjMaxC=0(AMD Tctl 偏移未知,UI 隐藏 TjMax)。
    static PlatformCaps BuildCaps(const PlatformInfo& info) {
        PlatformCaps c;
        c.vendor = info.vendor;
        c.cpuName = info.cpuName;
        c.logicalProcessors = info.logicalProcessors;
        c.baseGHz = info.baseGHz;
        return c;   // 其余字段默认 false/0 即诚实降级
    }

    // 全部 prev 计数器(读取失败保持 0,与 IntelProbe 一致)+ TSC 基线。
    // TSC 仅供未来驻留/窗口扩展锚定,v1 无消费方。
    void ReadBaseline() {
        uint64_t v = 0;
        if (io_.ReadMsr(0, AMD_PKG_ENERGY_STAT, v)) prevPkg_ = v & 0xFFFFFFFFull;
        prevCoreE_.assign(caps_.logicalProcessors, 0);
        staleCore_.assign(caps_.logicalProcessors, false);
        for (unsigned core = 0; core < caps_.logicalProcessors; ++core)
            if (io_.ReadMsr(core, AMD_CORE_ENERGY_STAT, v))
                prevCoreE_[core] = v & 0xFFFFFFFFull;
        io_.ReadMsr(0, MSR_IA32_APERF, prevAperf_);
        io_.ReadMsr(0, MSR_IA32_MPERF, prevMperf_);
        prevTsc_ = __rdtsc();
    }

    DriverIo& io_;
    PlatformCaps caps_;
    bool unitsOk_ = false;
    double energyUnit_ = 0.0;
    uint64_t prevPkg_ = 0;
    std::vector<uint64_t> prevCoreE_;   // 每核 0xC001029A 基线
    std::vector<bool> staleCore_;      // 上帧读取失败的核:恢复时只重置基线
    uint64_t prevAperf_ = 0, prevMperf_ = 0, prevTsc_ = 0;
};

std::unique_ptr<IPlatformProbe> CreateAmdProbe(DriverIo& io,
                                               const PlatformInfo& info) {
    return std::make_unique<AmdProbe>(io, info);
}

} // namespace pd
