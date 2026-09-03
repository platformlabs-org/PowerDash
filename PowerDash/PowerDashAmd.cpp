// PowerDashAmd.cpp —— AMD Zen(family 17h/19h/1Ah)保底监控探针(Task 8)。
// 保底集 = RAPL 能量差分(pkg + 逐核 cores)+ SMn Tctl 温度 + APERF/MPERF
// 频率;其余能力位诚实降级(false/NA),不读 PMTable、不猜 PPT/EDC/TjMax。
// 结构照 PowerDashIntel.cpp:ctor 基线读取 + caps 构建,readSample 差分。
#include "PowerDashProbe.h"
#include <algorithm>
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
constexpr uint32_t AMD_PSTATE_0 = 0xC0010064;           // P-state 0(P0 标称)
constexpr uint32_t MSR_IA32_APERF = 0xE8, MSR_IA32_MPERF = 0xE7;
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
        // 0xC001029A 按物理核计数:SMT 兄弟 LP 共享同一计数器,遍历全部
        // nLP 会双计(tb16g7 实测:IA 一度读出 PKG 的 167%)。每个物理核
        // 只读一个代表 LP(入口层 v3 起从 GetLogicalProcessorInformationEx
        // 的核 mask 集齐兄弟、取 repLP;Windows 同核兄弟编号相邻,8C/16T
        // 代表集 = {0,2,4,6,8,10,12,14},实测 LP0/LP1 差分速率 877590/868173
        // raw/s 1% 内相等,证明确为同一计数器)。cores 空/repLP 越界 =
        // 拓扑未知,退回全 nLP 遍历(旧行为,保底不残缺)。
        for (const auto& c : info.cores)
            coreScanLPs_.push_back(c.repLP);
        const bool valid = !coreScanLPs_.empty() && std::all_of(
            coreScanLPs_.begin(), coreScanLPs_.end(),
            [n = caps_.logicalProcessors](unsigned lp) { return lp < n; });
        if (!valid) {
            coreScanLPs_.resize(caps_.logicalProcessors);
            for (unsigned lp = 0; lp < coreScanLPs_.size(); ++lp)
                coreScanLPs_[lp] = lp;
        }
        uint64_t u = 0;
        if (io_.ReadMsr(0, AMD_RAPL_POWER_UNIT, u)) {
            energyUnit_ = 1.0 / std::pow(2.0, DecodeEnergyBits(u));
            unitsOk_ = true;
        }
        // AMD 无 CPUID 0x16:入口层 PlatformInfo.baseGHz 恒 0(0xCE 兜底是
        // Intel 语义)。P0 标称频率改从 P-state MSR 解码,供 APERF/MPERF
        // 比值作基频;解码成功即覆盖(AMD 上这是权威来源)。
        {
            uint64_t p0 = 0;
            if (io_.ReadMsr(0, AMD_PSTATE_0, p0)) {
                const double mhz = DecodePstateCofMHz((uint32_t)p0,
                                                      info.family);
                if (mhz > 0.0) caps_.baseGHz = mhz / 1000.0;
            }
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

        /* (b) cores:逐物理核读 0xC001029A(coreScanLPs_ 每核一个代表 LP,
         * SMT 兄弟共享计数器 —— 见 ctor 注释),各核独立差分(独立回绕
         * 钳制)后求和。域语义对齐 IntelProbe:本帧任一核读取失败 ->
         * 整域 NA(不输出残缺和,防静默少计);失败核标记 stale,恢复帧
         * 只刷新基线、跳过一次差分(陈旧 prev 直接差分会把两帧能量算进
         * 一个采样窗口,造成单帧尖峰)。 */
        {
            uint64_t sum = 0;
            bool anyCore = false;
            bool anyFailed = false;
            for (unsigned i = 0; i < coreScanLPs_.size(); ++i) {
                const unsigned core = coreScanLPs_[i];
                uint64_t e = 0;
                if (!io_.ReadMsr(core, AMD_CORE_ENERGY_STAT, e)) {
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
                sum += e - prevCoreE_[i];
                prevCoreE_[i] = e;
                anyCore = true;
            }
            if (!anyFailed && anyCore) {
                s.coresW = Ok(sum * energyUnit_);
                anyPower = true;
            } else s.coresW = NA();
        }

        /* (c) 温度 —— SMN THM_TCTL,k10temp 语义(见常量区注解):
         * (raw >> 21) * 0.125 C,RANGE_SEL/TJ_SEL=11 时再 -49 C;失败 -> NA。 */
        {
            uint32_t raw = 0;
            if (io_.ReadSmn(SMN_THM_TCTL, raw)) {
                double t = (double)(raw >> 21) * 0.125;
                if ((raw & TEMP_RANGE_SEL) || (raw & TEMP_TJ_SEL) == TEMP_TJ_SEL)
                    t -= 49.0;
                s.tempC = Ok(t);
            } else
                s.tempC = NA();
        }

        /* (d) 频率 —— APERF/MPERF 比值 * baseGHz(与 IntelProbe 同式,
         * 但 baseGHz 直接就是 GHz,无需 Intel 那步 /1000)。
         * baseGHz 由 ctor 从 P-state P0(0xC0010064)CpuFid/CpuDfsId 解码
         * (AMD 无 CPUID 0x16),0=未知 -> NA。 */
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
    // 逐核基线与采样同扫 coreScanLPs_(每物理核一个代表 LP,见 ctor 注释)。
    // TSC 仅供未来驻留/窗口扩展锚定,v1 无消费方。
    void ReadBaseline() {
        uint64_t v = 0;
        if (io_.ReadMsr(0, AMD_PKG_ENERGY_STAT, v)) prevPkg_ = v & 0xFFFFFFFFull;
        prevCoreE_.assign(coreScanLPs_.size(), 0);
        staleCore_.assign(coreScanLPs_.size(), false);
        for (unsigned i = 0; i < coreScanLPs_.size(); ++i)
            if (io_.ReadMsr(coreScanLPs_[i], AMD_CORE_ENERGY_STAT, v))
                prevCoreE_[i] = v & 0xFFFFFFFFull;
        io_.ReadMsr(0, MSR_IA32_APERF, prevAperf_);
        io_.ReadMsr(0, MSR_IA32_MPERF, prevMperf_);
        prevTsc_ = __rdtsc();
    }

    DriverIo& io_;
    PlatformCaps caps_;
    std::vector<unsigned> coreScanLPs_;  // 逐核能量扫描的代表 LP 集(物理核去重)
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
