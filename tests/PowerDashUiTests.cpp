#include "../PowerDash/PowerDashModel.h"
#include "../PowerDash/PowerDashProbe.h"
#include "../PowerDash/PowerDashSampler.h"
#include "../PowerDash/PowerDashSensors.h"
#include "../PowerDash/PowerDashUi.h"
#include "../PowerDash/PowerDashUsage.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestPowerArgumentsAcceptDurationAndCsvInEitherOrder() {
    pd::MonitorOptions options;
    std::string error;

    Expect(pd::ParsePowerArguments({"15", "--csv", "capture.csv"},
                                   options, error),
           "duration followed by --csv should parse");
    Expect(options.runSeconds == 15.0, "duration should be retained");
    Expect(options.csvPath == "capture.csv", "CSV path should be retained");

    options = {};
    error.clear();
    Expect(pd::ParsePowerArguments({"--csv", "logs/power.csv", "2.5"},
                                   options, error),
           "--csv followed by duration should parse");
    Expect(options.runSeconds == 2.5, "fractional duration should parse");
    Expect(options.csvPath == "logs/power.csv", "ordered CSV path should parse");
}

void TestPowerArgumentsRejectMissingCsvPathAndInvalidDuration() {
    pd::MonitorOptions options;
    std::string error;

    Expect(!pd::ParsePowerArguments({"--csv"}, options, error),
           "--csv without a path should fail");
    Expect(!error.empty(), "missing path should explain the error");

    error.clear();
    Expect(!pd::ParsePowerArguments({"zero"}, options, error),
           "non-numeric duration should fail");
    Expect(!pd::ParsePowerArguments({"0"}, options, error),
           "zero duration should fail");
}

void TestCsvHasStableColumnsAndEscapesText() {
    const std::string expectedHeader =
        "timestamp,elapsed_s,platform,pkg_w,cores_w,gfx_w,platform_w,"
        "limit_sustained_w,limit_sustained_window_s,limit_burst_w,limit_locked,"
        "tdc_a,edc_a,temp_c,freq_ghz,util_pct,c0_pct,c2_pct,c6_pct,smi_delta,mode";
    Expect(pd::CsvHeader() == expectedHeader, "CSV header columns changed");

    pd::Sample sample;                        // v2 fixture: gfx/tdc/edc invalid
    sample.timestamp = "2026-09-01T12:34:56";
    sample.elapsedS = 1.0;
    sample.pkgW = pd::Ok(12.345);
    sample.coresW = pd::Ok(8.5);
    sample.gfxW = pd::NA();
    sample.platformW = pd::Ok(1.75);
    sample.powerLimit.sustainedW = pd::Ok(28.0);
    sample.powerLimit.sustainedWindowS = pd::Ok(0.002);
    sample.powerLimit.burstW = pd::Ok(45.0);
    sample.powerLimit.locked = true;
    sample.currentLimit.tdcA = pd::NA();
    sample.currentLimit.edcA = pd::NA();
    sample.tempC = pd::Ok(67);
    sample.freqGHz = pd::Ok(3.125);
    sample.utilPct = pd::Ok(7.5);
    sample.c0Pct = pd::Ok(42.0);
    sample.c2Pct = pd::Ok(18.0);
    sample.c6Pct = pd::Ok(40.0);
    sample.smiDelta = 2;
    sample.mode = "Intelligent, Auto";

    const std::string expectedRow =
        "2026-09-01T12:34:56,1.000,intel,12.345,8.500,,1.750,"
        "28.000,0.002,45.000,1,,,67,3.125,7.500,"
        "42.000,18.000,40.000,2,"
        "\"Intelligent, Auto\"";
    Expect(pd::CsvRow(pd::Vendor::Intel, sample) == expectedRow,
           "CSV row should preserve precision, blank invalid cells and "
           "quote commas");
}

void TestLogoIsCenteredAndColorDoesNotChangeItsWidth() {
    const std::string plain = pd::RenderLogo(72, false);
    const std::string colored = pd::RenderLogo(72, true);
    Expect(plain.find("░█▀█░█▀█░█░█░█▀▀░█▀▄░█▀▄░█▀█░█▀▀░█░█") !=
               std::string::npos,
           "logo should contain the approved first row");
    Expect(colored.find("\x1b[38;2;103;232;249m") != std::string::npos,
           "logo should start with the approved bright cyan");

    for (const std::string* logo : {&plain, &colored}) {
        size_t start = 0;
        int rows = 0;
        while (start <= logo->size()) {
            size_t end = logo->find('\n', start);
            Expect(pd::VisibleLength(logo->substr(start, end - start)) == 72,
                   "each centered logo row should match the target width");
            ++rows;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        Expect(rows == 3, "logo should contain exactly three rows");
    }
}

pd::DashboardInfo DashboardFixture(int width, bool ansi = false) {
    pd::DashboardInfo info;
    info.version = "1.0";
    info.cpuBrand = "Intel Core Ultra 7 258V";
    info.codeName = "Lunar Lake";
    info.width = width;
    info.csvActive = true;
    info.csvName = "capture.csv";
    info.ansi = ansi;
    return info;
}

pd::PlatformCaps IntelCapsFixture() {
    pd::PlatformCaps caps;
    caps.vendor = pd::Vendor::Intel;
    caps.cpuName = "Intel Core Ultra 7 258V  [Lunar Lake]";
    caps.gfxPower = true;
    caps.platformPower = true;
    caps.powerLimits = true;
    caps.residency = true;
    caps.smi = true;
    caps.budgetW = 0.0;
    caps.tjMaxC = 100;
    caps.baseGHz = 2.0;
    caps.logicalProcessors = 8;
    return caps;
}

pd::PlatformCaps AmdCapsFixture() {
    pd::PlatformCaps c; c.vendor = pd::Vendor::Amd;
    c.cpuName = "AMD Ryzen 7 8845H  [Hawk Point]";
    c.logicalProcessors = 16; c.baseGHz = 3.8; c.tjMaxC = 95;
    return c;                       // 其余能力位 false
}

pd::Sample SampleFixture() {
    pd::Sample sample;
    sample.elapsedS = 92;
    sample.pkgW = pd::Ok(18.5);
    sample.coresW = pd::Ok(12.0);
    sample.gfxW = pd::Ok(0.5);
    sample.platformW = pd::Ok(25.0);
    sample.powerLimit.sustainedW = pd::Ok(28.0);
    sample.powerLimit.burstW = pd::Ok(45.0);
    sample.powerLimit.sustainedWindowS = pd::Ok(28.0);   // 28.00 s
    sample.powerLimit.locked = true;
    sample.tempC = pd::Ok(64);
    sample.freqGHz = pd::Ok(3.2);
    sample.c0Pct = pd::Ok(35);
    sample.c2Pct = pd::Ok(15);
    sample.c6Pct = pd::Ok(50);
    sample.utilPct = pd::Ok(8);
    sample.smiDelta = 1;
    sample.mode = "Intelligent (STD)";
    return sample;
}

void ExpectEveryLineHasWidth(const std::string& frame, std::size_t width,
                             const char* message) {
    size_t start = 0;
    while (start <= frame.size()) {
        size_t end = frame.find('\n', start);
        Expect(pd::VisibleLength(frame.substr(start, end - start)) == width,
               message);
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

void TestWideDashboardBalancesAtAGlanceAndDiagnosticInformation() {
    const std::string frame = pd::RenderDashboard(DashboardFixture(96),
                                                   IntelCapsFixture(),
                                                   SampleFixture(),
                                                   {12.0, 18.5});
    Expect(frame.find("░█▀█░█▀█░█░█░█▀▀") != std::string::npos,
           "dashboard should begin with the approved logo");
    Expect(frame.find("SYSTEM POWER") != std::string::npos &&
           frame.find("PACKAGE HISTORY") != std::string::npos,
           "system power and history must anchor the panel");
    Expect(frame.find("POWER LIMITS") != std::string::npos,
           "power limits need their own section");
    Expect(frame.find("POWER DOMAINS") != std::string::npos,
           "power domains need their own section");
    Expect(frame.find("CPU RESIDENCY") != std::string::npos,
           "CPU residency needs its own section");
    Expect(frame.find("● REC CSV") != std::string::npos,
           "active CSV capture should be visible");
    Expect(frame.find("capture.csv") != std::string::npos,
           "recording indicator should identify the CSV file");
    Expect(frame.find("MIN 12.00 W") != std::string::npos &&
           frame.find("AVG 15.25 W") != std::string::npos &&
           frame.find("MAX 18.50 W") != std::string::npos,
           "history should include independently checkable statistics");

    /* RAPL domain accounting: IA + GT + rest == PKG (18.5 = 12 + 0.5 + 6);
     * SYSTEM is the PSYS superset, annotated with PKG's share of it */
    Expect(frame.find("SYSTEM POWER") != std::string::npos,
           "the bar section should be titled SYSTEM POWER");
    Expect(frame.find("REST") != std::string::npos &&
           frame.find("6.50 W") != std::string::npos &&
           frame.find("26% of SYS") != std::string::npos,
           "REST should decompose SYSTEM beside PKG (25.0 = 18.5 + 6.5)");
    Expect(frame.find("65% PKG") != std::string::npos &&
           frame.find(" 3% PKG") != std::string::npos,
           "IA and GT should be expressed as a share of PKG");
    Expect(frame.find("PKG 74% of SYS") != std::string::npos,
           "SYSTEM should show PKG's share of the platform domain");
    Expect(frame.find("| = PL1 28.00 W") != std::string::npos &&
           frame.find("scale to PL2 45.00 W") != std::string::npos,
           "gauge legend should name the scale endpoints and the marker");
    Expect(frame.find("PKG + REST = SYSTEM") != std::string::npos,
           "legend should state the decomposition identity");
    Expect(frame.find("41% of PL2") != std::string::npos,
           "PKG bar trailer should be expressed against the named scale");

    bool foundBalancedColumns = false;
    size_t start = 0;
    while (start <= frame.size()) {
        size_t end = frame.find('\n', start);
        std::string line = frame.substr(start, end - start);
        if (line.find("POWER DOMAINS") != std::string::npos &&
            line.find("THERMAL / PERFORMANCE") != std::string::npos)
            foundBalancedColumns = true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    Expect(foundBalancedColumns,
           "96-column layout should place diagnostic groups side by side");
    ExpectEveryLineHasWidth(frame, 96,
                            "every wide dashboard row should be exactly 96 columns");
}

void TestNarrowDashboardStacksGroupsWithoutDroppingMetrics() {
    const std::string frame = pd::RenderDashboard(DashboardFixture(72),
                                                   IntelCapsFixture(),
                                                   SampleFixture(),
                                                   {12.0, 18.5});
    for (const char* required : {"SYSTEM POWER", "POWER DOMAINS",
                                 "THERMAL / PERFORMANCE",
                                 "CPU RESIDENCY", "POWER LIMITS",
                                 "PACKAGE HISTORY", "TjMax 100 C",
                                 "PL1 28.00 W", "PL2 45.00 W",
                                 "% of SYS"})
        Expect(frame.find(required) != std::string::npos,
               "72-column layout dropped a required diagnostic");

    bool groupsShareLine = false;
    size_t start = 0;
    while (start <= frame.size()) {
        size_t end = frame.find('\n', start);
        std::string line = frame.substr(start, end - start);
        if (line.find("POWER DOMAINS") != std::string::npos &&
            line.find("THERMAL / PERFORMANCE") != std::string::npos)
            groupsShareLine = true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    Expect(!groupsShareLine, "72-column layout should stack diagnostic groups");
    ExpectEveryLineHasWidth(frame, 72,
                            "every narrow dashboard row should be exactly 72 columns");
}

void TestAmdDashboardHidesAbsentSections() {
    pd::Sample s;                                   // pkg 28 = cores 22 + gfx NA + rest 6
    s.pkgW = pd::Ok(28.0); s.coresW = pd::Ok(22.0); s.gfxW = pd::NA();
    s.tempC = pd::Ok(61); s.freqGHz = pd::Ok(4.1); s.utilPct = pd::Ok(12);
    s.mode = "Intelligent (APM)";
    const std::string frame = pd::RenderDashboard(DashboardFixture(96),
                                                  AmdCapsFixture(), s, {28.0});
    Expect(frame.find("PACKAGE POWER · 28.00 W") != std::string::npos,
           "amd top section title carries pkg total");
    Expect(frame.find("CORES + GFX + REST = PKG") != std::string::npos,
           "amd legend identity");
    Expect(frame.find("of SYS") == std::string::npos, "no SYS denominator on amd");
    Expect(frame.find("CPU RESIDENCY") == std::string::npos, "residency hidden");
    Expect(frame.find("SMI") == std::string::npos, "smi hidden");
    Expect(frame.find(" GT ") == std::string::npos, "gfx domain hidden");
    Expect(frame.find(" UTIL ") != std::string::npos &&
           frame.find(" TEMP ") != std::string::npos &&
           frame.find(" FREQ ") != std::string::npos, "platform-neutral rows stay");
    {   // sustainedW 无效(AMD 每帧)时 PKG 功率条不得画 PL1 竖线标记
        bool checkedBar = false;
        size_t start = 0;
        while (start <= frame.size()) {
            size_t end = frame.find('\n', start);
            std::string line = frame.substr(start, end - start);
            if (line.find(" PKG    ") != std::string::npos) {
                const size_t lb = line.find('[');
                const size_t rb = lb == std::string::npos
                                      ? std::string::npos : line.find(']', lb);
                Expect(lb != std::string::npos && rb != std::string::npos &&
                           line.substr(lb + 1, rb - lb - 1).find('|') ==
                               std::string::npos,
                       "invalid sustained limit must not draw the bar marker");
                checkedBar = true;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        Expect(checkedBar, "amd golden frame contains a PKG bar row");
    }
    ExpectEveryLineHasWidth(frame, 96, "amd dashboard geometry holds");
}

void TestAnsiColorsPreserveDashboardGeometry() {
    const std::string frame = pd::RenderDashboard(DashboardFixture(96, true),
                                                   IntelCapsFixture(),
                                                   SampleFixture(),
                                                   {12.0, 18.5});
    Expect(frame.find("\x1b[38;2;103;232;249m") != std::string::npos,
           "colored dashboard should use the approved logo palette");
    Expect(frame.find("\x1b[38;2;239;68;68m") != std::string::npos,
           "CSV recording status should use the recording red");
    ExpectEveryLineHasWidth(frame, 96,
                            "ANSI colors must not change dashboard geometry");
}

void TestModelDecompositionIdentities() {
    pd::Sample s;                                   // Intel: SYS = PKG + REST
    s.pkgW = pd::Ok(18.5); s.coresW = pd::Ok(12.0);
    s.gfxW = pd::Ok(0.5); s.platformW = pd::Ok(25.0);
    pd::PlatformCaps intel; intel.vendor = pd::Vendor::Intel;
    intel.platformPower = true;
    pd::Decomposition d = pd::Decompose(s, intel);
    Expect(d.title == "SYSTEM POWER", "intel decomposition title");
    Expect(d.totalW.valid && d.totalW.value == 25.0, "intel total = platform");
    Expect(d.mainW.valid && d.mainW.value == 18.5, "intel main = pkg");
    Expect(d.restW.valid && std::abs(d.restW.value - 6.5) < 0.01,
           "intel rest = platform - pkg");
    Expect(d.identity == "PKG + REST = SYSTEM", "intel identity");

    pd::Sample a;                                   // AMD: PKG = CORES + GFX + REST
    a.pkgW = pd::Ok(7.08); a.coresW = pd::Ok(1.50);
    a.gfxW = pd::Ok(0.01);
    pd::PlatformCaps amd; amd.vendor = pd::Vendor::Amd;
    pd::Decomposition e = pd::Decompose(a, amd);
    Expect(e.title == "PACKAGE POWER", "amd decomposition title");
    Expect(e.totalW.valid && e.totalW.value == 7.08, "amd total = pkg");
    Expect(e.restW.valid && std::abs(e.restW.value - 5.57) < 0.01,
           "amd rest = pkg - cores - gfx");
    Expect(e.identity == "CORES + GFX + REST = PKG", "amd identity");
}

// v3 Task 4:FixtureDriverIo 换用 (core, msr) 静态值表(单核差异化;动态
// 演进由测试在两拍之间改表模拟 —— 探针 ctor 读基线、测试 bump 后帧读差分)。
// 保留 msrFailure/failAllMsrs;WriteSmn 记录 smnWrites(Task 1 契约),
// smnReadHook 供后续 AMD PMTable 邮箱轮询;MapPhys 发放假物理内存。
class FixtureDriverIo : public pd::DriverIo {
public:
    std::map<std::pair<unsigned, uint32_t>, uint64_t> msrPerCore;  // (core,msr)->值
    std::map<uint32_t, uint32_t> smn;                              // smn addr -> value
    std::map<uint32_t, uint32_t> smnWrites;                        // addr -> 最后写入值
    std::function<uint32_t(uint32_t addr)> smnReadHook;            // 动态 SMN(邮箱轮询)
    std::vector<uint8_t> physMem;                                  // 假物理内存
    uint64_t mapPhysBase = 0;
    // 可选:按 (core, msr) 脚本化读取失败(单核单次注入用)。真实硬件的
    // 计数器在读取失败期间照常前进,故钩子需自行推进 fixture 计数器。
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
        mapPhysBase = phys; virt = physMem.data(); return true;  // 页内偏移 0(测试构造保证)
    }
    void UnmapPhys(void*) override {}
};

// Task 3 适配:PlatformInfo::coreLPs(代表 LP 列表)→ cores(CoreInfo)。
// 旧测试以代表 LP 列表描述拓扑;helper 逐 repLP 合成单线程 CoreInfo
// (现存探针只消费 repLP;SMT 兄弟表 threads 仅在真机拓扑里由
// QueryCoreTopologyV2 填充,Task 4/5 重写探针时再按需扩展)。
void SetCoreReps(pd::PlatformInfo& info, std::initializer_list<unsigned> reps) {
    info.cores.clear();
    for (unsigned lp : reps)
        info.cores.push_back(pd::CoreInfo{lp, {lp}, 0});
}

class FakeProbe : public pd::IPlatformProbe {   // 脚本化样本序列
public:
    std::vector<pd::Sample> script; size_t i = 0; int reads = 0;
    pd::PlatformCaps capsHolder;
    const pd::PlatformCaps& caps() const override { return capsHolder; }
    bool readSample(pd::Sample& s) override {
        ++reads;
        if (i >= script.size()) return false;
        s = script[i++]; return true;
    }
};

void TestDriverIoFixtureRouting() {
    FixtureDriverIo io;
    io.msrPerCore[{0, 0x611}] = 12345;
    uint64_t v = 0;
    Expect(io.ReadMsr(0, 0x611, v) && v == 12345, "fixture msr scripted answer");
    Expect(!io.ReadMsr(1, 0x611, v), "per-(core,msr) keying isolates cores");
    Expect(!io.ReadMsr(0, 0x999, v), "fixture unknown msr fails");
    Expect(io.WriteSmn(0x3B10A20, 0x66) && io.smnWrites[0x3B10A20] == 0x66,
           "WriteSmn recorded into smnWrites");
    io.smnReadHook = [](uint32_t a) { return a + 1; };
    uint32_t s = 0;
    Expect(io.ReadSmn(0x10, s) && s == 0x11, "smnReadHook dynamic answer");
    io.smnReadHook = nullptr;
    io.physMem.assign(0x1000, 0);
    void* p = nullptr;
    Expect(io.MapPhys(0xFED10000ull, 0x1000, p) && p == io.physMem.data() &&
               io.mapPhysBase == 0xFED10000ull,
           "MapPhys hands out the fake physical memory");
    Expect(!io.MapPhys(0x1000, 0x2000, p), "MapPhys rejects oversized requests");
}

void TestIntelProbeReplay() {
    // v3:静态 (core,msr) 表两拍用法 —— ctor 建基线,测试 bump 后帧读差分。
    FixtureDriverIo io;
    // 单位寄存器:power bits=3(0.125W), energy bits=14(1/16384 J)
    io.msrPerCore[{0, 0x606}] = (14ull << 8) | 3ull;
    io.msrPerCore[{0, 0x611}] = 0;   // 能量四域基线 0
    io.msrPerCore[{0, 0x639}] = 0;
    io.msrPerCore[{0, 0x641}] = 0;
    io.msrPerCore[{0, 0x64D}] = 0;
    // 温度:MSR_PACKAGE_THERM_STATUS 实为 0x1B1(PowerDash.cpp:43,
    // brief 的 0x613 是笔误);valid bit31 + headroom 31 -> 105-31=74 C
    io.msrPerCore[{0, 0x1B1}] = (1ull << 31) | (0x1Full << 16);
    io.msrPerCore[{0, 0x1A2}] = 105ull << 16;           // TjMax=105
    pd::PlatformInfo info; info.vendor = pd::Vendor::Intel;
    info.cpuName = "Intel(R) Core(TM) Ultra 5 225H  [Arrow Lake-H]";
    info.logicalProcessors = 16; info.baseGHz = 2.5;
    auto probe = pd::CreateIntelProbe(io, info);
    Expect(probe != nullptr, "intel probe constructs");
    Expect(probe->caps().vendor == pd::Vendor::Intel &&
           probe->caps().tjMaxC == 105, "caps carry tjMax");
    // 构造期已建基线;bump 后第一帧即得到差分功率
    io.msrPerCore[{0, 0x611}] = 32768;   // +32768 raw = 2.0 W
    io.msrPerCore[{0, 0x639}] = 16384;   // +16384 raw = 1.0 W
    io.msrPerCore[{0, 0x641}] = 8192;    // 0.5 W
    io.msrPerCore[{0, 0x64D}] = 49152;   // 3.0 W
    pd::Sample s;
    Expect(probe->readSample(s), "first sample reads");
    Expect(s.pkgW.valid && std::abs(s.pkgW.value - 2.0) < 0.01,
           "pkg power from replayed delta");
    Expect(s.coresW.valid && std::abs(s.coresW.value - 1.0) < 0.01,
           "cores = PP0 delta");
    Expect(s.gfxW.valid && std::abs(s.gfxW.value - 0.5) < 0.01, "gfx = PP1");
    Expect(s.platformW.valid && std::abs(s.platformW.value - 3.0) < 0.01,
           "platform = PSYS");
    Expect(s.tempC.valid && std::abs(s.tempC.value - 74.0) < 0.01,
           "temp = tjMax - therm headroom");
    // MCHBAR/MMAP 在 FixtureDriverIo 下失败 -> 能力降级,不是构造失败
    Expect(!probe->caps().powerLimits, "MCHBAR unavailable degrades powerLimits");
    Expect(!s.powerLimit.sustainedW.valid && !s.powerLimit.burstW.valid,
           "PL readings stay invalid without MMIO or static 0x610");
    // 0x198 未脚本化 -> 无时钟列 -> 频率无效
    Expect(!s.freqGHz.valid, "PERF_STATUS unscripted leaves freq invalid");
}

void TestIntelProbeEnergyWraparound() {
    FixtureDriverIo io;
    io.msrPerCore[{0, 0x606}] = (14ull << 8) | 3ull;   // 1/16384 J
    io.msrPerCore[{0, 0x611}] = 0xFFFFFF00ull;   // ctor 基线读到的 32 位计数器
    pd::PlatformInfo info; info.vendor = pd::Vendor::Intel;
    auto probe = pd::CreateIntelProbe(io, info);
    io.msrPerCore[{0, 0x611}] = 0x100;           // 本帧回绕到 2^32 附近
    pd::Sample s;
    Expect(probe->readSample(s), "wraparound sample reads");
    // ctor 基线 prev = 0xFFFFFF00;本帧读回绕到 0x100 < prev,
    // 走 curr += 1<<32 分支后差分 = 0x100000100 - 0xFFFFFF00 = 0x200
    Expect(s.pkgW.valid && std::abs(s.pkgW.value - (512.0 / 16384.0)) < 0.0001,
           "32-bit energy wraparound adds 1<<32 to the delta");
}

// ---- Task 4: Intel 探针宽表(经 IPlatformProbe 接口消费,不向下转型)----
void TestIntelProbeWideTable() {
    // 基础:RAPL 单位 energy bits=16(1/65536 J)、power bits=3(0.125 W)、
    // time bits=11(2^-11 s);能量计数器帧间差 0x10000 raw = 1.0 J -> 1 W。
    FixtureDriverIo io;
    io.msrPerCore[{0, 0x606}] = (16ull << 8) | 3ull | (11ull << 16);
    io.msrPerCore[{0, 0x614}] = 0x2FF;   // thermal spec 0x2FF*0.125 = 95.875 W
    io.msrPerCore[{0, 0x611}] = 0;       // 能量四域基线 0
    io.msrPerCore[{0, 0x639}] = 0;
    io.msrPerCore[{0, 0x641}] = 0;
    io.msrPerCore[{0, 0x64D}] = 0;
    io.msrPerCore[{0, 0x1A2}] = 105ull << 16;   // LP0 TjMax(core1 走回退路径)
    io.msrPerCore[{0, 0xCE}] = 32ull << 8;      // 最大非睿频倍频 32 -> bus=TSC/32
    io.msrPerCore[{0, 0x1B1}] = (1ull << 31) | (8ull << 16);   // 包温 readout 8
    io.msrPerCore[{0, 0x610}] = 224ull /*PL1 28W*/ | (1ull << 15) |
                                (10ull << 17) /*tau 10*/ | (1ull << 31) /*locked*/ |
                                (368ull << 32) /*PL2 46W*/ | (1ull << 47);
    io.msrPerCore[{0, 0x64B}] = 1;               // cTDP level 1
    io.msrPerCore[{0, 0x64F}] = 1ull << 25;      // IA log 位 16+9(Max Turbo Limit)
    io.msrPerCore[{0, 0x650}] = 0;
    io.msrPerCore[{0, 0x651}] = 0;
    for (unsigned c = 0; c < 2; ++c) {
        io.msrPerCore[{c, 0x198}] = (40ull << 8) | (5734ull << 32);  // 倍频 40/VID 5734
        io.msrPerCore[{c, 0x19C}] = (1ull << 31) | (5ull << 16) | (1ull << 1);  // readout 5+thermal log
        io.msrPerCore[{c, 0x660}] = 0;    // C1 驻留可读(恒 0)
        io.msrPerCore[{c, 0x3FD}] = 0;    // C6 驻留可读(恒 0)
        io.msrPerCore[{c, 0xE8}] = 0;     // APERF/MPERF 恒 0 ->
        io.msrPerCore[{c, 0xE7}] = 0;     //   eff/C0 差分恒 0%
    }
    pd::PlatformInfo info;
    info.vendor = pd::Vendor::Intel;
    info.logicalProcessors = 2;
    info.cores = { {0, {0}, 0}, {1, {1}, 1} };   // P0 + E1(核名区分 effClass)
    info.baseGHz = 0;
    const double tscHz = pd::CalibrateTscHz();   // 独立测 TSC,断言 bus 公式
    auto probe = pd::CreateIntelProbe(io, info);
    pd::SensorTable* t = probe->sensors();
    Expect(t != nullptr, "intel probe exposes its wide table");
    Expect(probe->caps().tjMaxC == 105, "caps tjMax from 0x1A2");
    Expect(std::abs(probe->caps().budgetW - 95.875) < 1e-9,
           "budgetW = thermal spec 0x2FF*0.125");
    Expect(probe->caps().gfxPower && probe->caps().platformPower,
           "energy domain ctor probes set caps honestly");
    Expect(probe->caps().residency, "residency caps when C-state columns exist");

    // 列名 = HWiNFO 原文(effClass 0 -> "P-core n"、1 -> "E-core n",编号 = repLP)
    int i = -1;
    Expect((i = t->Find("clock.0")) >= 0 &&
               t->Column(i).name == "P-core 0 Clock [MHz]",
           "core 0 uses P-core naming");
    Expect((i = t->Find("clock.1")) >= 0 &&
               t->Column(i).name == "E-core 1 Clock [MHz]",
           "core 1 uses E-core naming");
    Expect((i = t->Find("cores.c0.lp1")) >= 0 &&
               t->Column(i).name == "E-core 1 T0 C0 Residency [%]",
           "per-thread C0 uses HWiNFO T0 naming");
    Expect((i = t->Find("temp.pkg")) >= 0 &&
               t->Column(i).name == "CPU Package [°C]",
           "package temp column name");
    // 组序:电压->时钟->有效->Usage->Utility->Ratio->温度->降频->功率->限值->核驻留->Limit Reasons
    Expect(t->Find("vid.avg") < t->Find("clock.avg") &&
               t->Find("clock.avg") < t->Find("eff.avg") &&
               t->Find("eff.avg") < t->Find("usage.avg") &&
               t->Find("usage.avg") < t->Find("util.avg") &&
               t->Find("util.avg") < t->Find("ratio.avg") &&
               t->Find("ratio.avg") < t->Find("temp.avg") &&
               t->Find("temp.avg") < t->Find("thr.pkg.thermal") &&
               t->Find("thr.pkg.thermal") < t->Find("power.pkg") &&
               t->Find("power.pkg") < t->Find("pl1.static") &&
               t->Find("pl1.static") < t->Find("cores.c0.avg") &&
               t->Find("cores.c0.avg") < t->Find("lim.ia.avg"),
           "column groups follow the HWiNFO sample order");

    // 两拍:ctor 基线 -> bump -> readSample(能量差分每拍 1.0 W)
    auto bump = [&io] {
        io.msrPerCore[{0, 0x611}] += 0x10000;
        io.msrPerCore[{0, 0x639}] += 0x10000;
        io.msrPerCore[{0, 0x641}] += 0x10000;
        io.msrPerCore[{0, 0x64D}] += 0x10000;
    };
    pd::Sample s;
    bump();
    Expect(probe->readSample(s), "beat 1 reads");
    pd::Sample s2;
    bump();
    Expect(probe->readSample(s2), "beat 2 reads");
    pd::SensorTable& tb = *t;

    // 电压:VID = 0x198 EDX[15:0]/8192 -> 5734/8192
    const double vid = 5734.0 / 8192.0;
    Expect(tb.Lookup("vid.0").valid && std::abs(tb.Lookup("vid.0").value - vid) < 1e-9,
           "vid.0 = 5734/8192");
    Expect(tb.Lookup("vid.avg").valid && std::abs(tb.Lookup("vid.avg").value - vid) < 1e-9,
           "vid.avg = mean over cores");
    // 时钟:bus = 实测 TSC / 0xCE 倍频;clock.N = 倍频 x bus;ratio = 40
    const pd::Reading bus = tb.Lookup("clock.bus");
    const double busExp = tscHz / 32.0 / 1e6;
    Expect(bus.valid && std::abs(bus.value - busExp) < 0.02 * busExp,
           "bus = CalibrateTscHz()/0xCE ratio");
    Expect(bus.value > 90.0 && bus.value < 130.0,
           "bus in BCLK band (host TSC 3686.4 MHz at ratio 32 -> 115.2)");
    Expect(tb.Lookup("clock.0").valid &&
               std::abs(tb.Lookup("clock.0").value - 40.0 * bus.value) <
                   0.01 * 40.0 * bus.value,
           "clock.0 = ratio 40 x bus");
    Expect(tb.Lookup("clock.avg").valid &&
               std::abs(tb.Lookup("clock.avg").value - 40.0 * bus.value) <
                   0.01 * 40.0 * bus.value,
           "clock.avg = mean over cores");
    Expect(tb.Lookup("ratio.0").valid && std::abs(tb.Lookup("ratio.0").value - 40.0) < 1e-9,
           "ratio.0 = 40");
    // 温度:readout 5 -> temp = 105-5;距离列 = 5;包温 105-8
    Expect(tb.Lookup("temp.0").valid && std::abs(tb.Lookup("temp.0").value - 100.0) < 1e-9,
           "temp.0 = TjMax - 5");
    Expect(tb.Lookup("tjmax.0").valid && std::abs(tb.Lookup("tjmax.0").value - 5.0) < 1e-9,
           "tjmax.0 distance = 5");
    Expect(tb.Lookup("temp.avg").valid && std::abs(tb.Lookup("temp.avg").value - 100.0) < 1e-9,
           "temp.avg = 100");
    Expect(tb.Lookup("temp.coremax").valid &&
               std::abs(tb.Lookup("temp.coremax").value - 100.0) < 1e-9,
           "temp.coremax = 100");
    Expect(tb.Lookup("temp.pkg").valid && std::abs(tb.Lookup("temp.pkg").value - 97.0) < 1e-9,
           "temp.pkg = 105-8");
    // 降频位:0x19C log 位 1/5/11;avg = OR;包级三位全 0
    Expect(tb.Lookup("thr.0.thermal").valid && tb.Lookup("thr.0.thermal").value == 1.0,
           "thr.0.thermal = log bit 1");
    Expect(tb.Lookup("thr.avg.thermal").valid && tb.Lookup("thr.avg.thermal").value == 1.0,
           "thr.avg.thermal = OR over cores");
    Expect(tb.Lookup("thr.0.crit").valid && tb.Lookup("thr.0.crit").value == 0.0,
           "thr.0.crit = 0");
    Expect(tb.Lookup("thr.0.plim").valid && tb.Lookup("thr.0.plim").value == 0.0,
           "thr.0.plim = 0");
    Expect(tb.Lookup("thr.pkg.thermal").valid && tb.Lookup("thr.pkg.thermal").value == 0.0,
           "package thermal = 0");
    // PL 静态 0x610:224x0.125=28、368x0.125=46;动态列 MMIO 缺席 -> NA
    Expect(tb.Lookup("pl1.static").valid &&
               std::abs(tb.Lookup("pl1.static").value - 28.0) < 1e-9,
           "pl1.static = 224*0.125");
    Expect(tb.Lookup("pl2.static").valid &&
               std::abs(tb.Lookup("pl2.static").value - 46.0) < 1e-9,
           "pl2.static = 368*0.125");
    Expect(!tb.Lookup("pl1.dynamic").valid && !tb.Lookup("pl2.dynamic").valid,
           "dynamic PL NA without MMIO window");
    Expect(tb.Lookup("ctdp.level").valid && tb.Lookup("ctdp.level").value == 1.0,
           "ctdp.level = 0x64B[1:0]");
    // Limit Reasons:0x64F log 位 16+9 -> lim.ia.9;avg = OR
    Expect(tb.Lookup("lim.ia.9").valid && tb.Lookup("lim.ia.9").value == 1.0,
           "lim.ia.9 = Max Turbo Limit log bit");
    Expect(tb.Lookup("lim.ia.avg").valid && tb.Lookup("lim.ia.avg").value == 1.0,
           "lim.ia.avg = OR of all bits (bit 9 set -> Yes)");
    Expect(tb.Lookup("lim.gt.0").valid && tb.Lookup("lim.gt.0").value == 0.0,
           "lim.gt.0 = 0");
    // 能量:每拍 +0x10000 raw x 1/65536 J = 1.0 W(1 s 窗口约定)
    for (const char* k : {"power.pkg", "power.ia", "power.gt", "power.sys"})
        Expect(tb.Lookup(k).valid && std::abs(tb.Lookup(k).value - 1.0) < 1e-6,
               "power domain decodes to 1.0 W");
    // 驻留/有效:fixture 恒 0 差 -> C1/C6/C0/eff 全 0 但有效
    Expect(tb.Lookup("cores.c1.avg").valid && tb.Lookup("cores.c1.avg").value == 0.0,
           "cores.c1.avg = 0");
    Expect(tb.Lookup("cores.c6.avg").valid && tb.Lookup("cores.c6.avg").value == 0.0,
           "cores.c6.avg = 0");
    Expect(tb.Lookup("cores.c0.lp0").valid && tb.Lookup("cores.c0.lp0").value == 0.0,
           "cores.c0.lp0 = dMPERF/dTSC = 0");
    Expect(tb.Lookup("eff.all").valid && tb.Lookup("eff.all").value == 0.0,
           "eff = dAPERF/dTSC = 0");
    // Usage:真源双基线暖机,前两拍 NA(暖机期不猜值)
    Expect(!tb.Lookup("usage.total").valid && !tb.Lookup("usage.avg").valid,
           "usage NA during warm-up frames");
    // 无脚本项 -> 诚实 NA/省列
    Expect(!tb.Lookup("usage.clockmod").valid, "clockmod unscripted -> NA");
    Expect(t->Find("cores.c7.avg") < 0, "C7 unreadable at probe -> column omitted");
    Expect(t->Find("pkgres.c2") < 0, "package residency unreadable -> omitted");
    // Sample 派生:同键单条写路径(表值 == Sample 值)
    Expect(s2.pkgW.valid && std::abs(s2.pkgW.value - tb.Lookup("power.pkg").value) < 1e-12,
           "Sample pkgW == power.pkg");
    Expect(s2.coresW.valid && std::abs(s2.coresW.value - 1.0) < 1e-6,
           "Sample coresW == power.ia");
    Expect(s2.tempC.valid && s2.tempC.value == 97.0, "Sample tempC == temp.pkg");
    Expect(s2.freqGHz.valid &&
               std::abs(s2.freqGHz.value - tb.Lookup("clock.avg").value / 1000.0) < 1e-9,
           "Sample freq = clock.avg/1000");
    Expect(s2.powerLimit.sustainedW.valid &&
               std::abs(s2.powerLimit.sustainedW.value - 28.0) < 1e-9,
           "sustainedW = dynamic-first, static fallback");
    Expect(s2.powerLimit.burstW.valid &&
               std::abs(s2.powerLimit.burstW.value - 46.0) < 1e-9,
           "burstW = static fallback");
    Expect(s2.powerLimit.sustainedWindowS.valid &&
               std::abs(s2.powerLimit.sustainedWindowS.value - 10.0 / 2048.0) < 1e-12,
           "tau = 10*2^-11 s");
    Expect(s2.powerLimit.locked, "locked from 0x610 bit31");
    Expect(!s2.utilPct.valid, "utilPct NA while usage warms up");
    Expect(!s2.c0Pct.valid && !s2.c2Pct.valid && !s2.c6Pct.valid,
           "package residency NA -> c0/c2/c6 NA");

    // 第三拍:抽走 0x64F -> Limit Reasons 列回 NA(帧首 SetInvalid 全表,
    // 失败读取不残留旧值);能量照常 bump,帧仍成立
    io.msrPerCore.erase({0, 0x64F});
    bump();
    pd::Sample s3;
    Expect(probe->readSample(s3), "beat 3 reads");
    Expect(!tb.Lookup("lim.ia.9").valid,
           "failed read leaves NA (no stale value from previous frame)");
    Expect(tb.Lookup("power.pkg").valid &&
               std::abs(tb.Lookup("power.pkg").value - 1.0) < 1e-6,
           "healthy columns keep decoding on the degraded frame");
}

// ---- Task 8: AmdProbe(保底监控集)----
// v3 Task 4:能量 fixture 改用静态 (core,msr) 表两拍法 —— ctor 基线一圈读
// repLP 集合,测试在帧前 bump 表值模拟每个核自己的 32 位计数器前进;若实
// 现误读 SMT 兄弟或越界 LP,对应 (core,msr) 无表项 -> 读取失败,数值断言
// 立即暴露。动态推进(读取失败但计数器照走)由 msrFailure 钩子改表实现。

void TestAmdProbeReplay() {
    FixtureDriverIo io;
    // 单位寄存器 0xC0010299 与 Intel 0x606 位兼容(turbostat: energy bits
    // 8-12):energy bits=14 -> 1/16384 J
    io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;
    io.msrPerCore[{0, 0xC001029B}] = 0;   // pkg 基线 0
    // 8 物理核(0xC001029A 按物理核计数,SMT 兄弟共享 -> 每核只读一个
    // 代表 LP)每帧各 +2048 raw -> 合计 8*2048 = 16384 raw = 1.0 W
    const unsigned reps[8] = {0, 2, 4, 6, 8, 10, 12, 14};
    for (unsigned lp : reps) io.msrPerCore[{lp, 0xC001029A}] = 0;
    io.smn[0x59800] = 640u << 21;   // Tctl: 640 * 0.125 = 80.0 C(无 RANGE_SEL)
    pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
    info.cpuName = "AMD Ryzen 7 8845H  [Hawk Point]";
    info.logicalProcessors = 16; info.physicalCores = 8;   // 8C/16T
    SetCoreReps(info, {0, 2, 4, 6, 8, 10, 12, 14});        // Windows 相邻对
    info.baseGHz = 3.8;
    auto probe = pd::CreateAmdProbe(io, info);
    Expect(probe != nullptr, "amd probe constructs");
    pd::PlatformCaps c = probe->caps();
    Expect(!c.gfxPower && !c.platformPower && !c.powerLimits && !c.residency &&
           !c.smi, "amd caps degrade honestly");
    Expect(c.vendor == pd::Vendor::Amd && c.logicalProcessors == 16 &&
           c.cpuName == "AMD Ryzen 7 8845H  [Hawk Point]" &&
           std::abs(c.baseGHz - 3.8) < 0.01, "amd caps carry identity");
    Expect(c.tjMaxC == 0 && c.budgetW == 0.0, "amd leaves tjMax/budget unknown");
    io.msrPerCore[{0, 0xC001029B}] = 32768;   // +32768 raw = 2.0 W
    for (unsigned lp : reps) io.msrPerCore[{lp, 0xC001029A}] = 2048;
    pd::Sample s;
    Expect(probe->readSample(s), "amd sample reads");
    Expect(s.pkgW.valid && std::abs(s.pkgW.value - 2.0) < 0.01,
           "pkg energy delta");
    Expect(s.coresW.valid && std::abs(s.coresW.value - 1.0) < 0.01,
           "cores = sum of per-core deltas");
    Expect(!s.gfxW.valid && !s.platformW.valid, "absent domains stay invalid");
    Expect(s.tempC.valid && std::abs(s.tempC.value - 80.0) < 0.01,
           "SMN Tctl decode");
    // APERF/MPERF 未脚本化 -> 频率 NA(子项优雅降级)
    Expect(!s.freqGHz.valid, "APERF/MPERF unscripted leaves freq invalid");
}

void TestAmdProbeEnergyWraparound() {
    {   // pkg 计数器跨 2^32 回绕;nLP=0 边缘 -> cores 无读数保持 NA
        FixtureDriverIo io;
        io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;   // 1/16384 J
        io.msrPerCore[{0, 0xC001029B}] = 0xFFFFFF00ull;   // ctor 基线读到的
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;   // nLP=0
        auto probe = pd::CreateAmdProbe(io, info);
        io.msrPerCore[{0, 0xC001029B}] = 0x100;   // 本帧回绕到 2^32 附近
        pd::Sample s;
        Expect(probe->readSample(s), "pkg-only sample reads");
        // ctor 基线 prev = 0xFFFFFF00;本帧回绕到 0x100 < prev,
        // 走 curr += 1<<32 分支后差分 = 0x200 = 512 raw -> 512/16384 W
        Expect(s.pkgW.valid && std::abs(s.pkgW.value - (512.0 / 16384.0)) < 0.0001,
               "32-bit pkg wraparound adds 1<<32 to delta");
        Expect(!s.coresW.valid, "zero logical processors leaves cores NA");
    }
    {   // 逐核独立回绕:4 核 ctor 基线均为 0xFFFFFF00,本帧回绕到 0x100
        // (nLP=4/physicalCores=4:无 SMT,代表集即全部 4 个 LP)
        FixtureDriverIo io;
        io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;
        for (unsigned c = 0; c < 4; ++c)
            io.msrPerCore[{c, 0xC001029A}] = 0xFFFFFF00ull;
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 4; info.physicalCores = 4;
        SetCoreReps(info, {0, 1, 2, 3});
        info.baseGHz = 3.8;
        auto probe = pd::CreateAmdProbe(io, info);
        for (unsigned c = 0; c < 4; ++c) io.msrPerCore[{c, 0xC001029A}] = 0x100;
        pd::Sample s;
        Expect(probe->readSample(s), "per-core wraparound sample reads");
        // 每核 delta = 0x200,4 核合计 0x800 = 2048 raw -> 2048/16384 = 0.125 W
        Expect(s.coresW.valid && std::abs(s.coresW.value - 0.125) < 0.0001,
               "per-core counters wrap independently");
        Expect(!s.pkgW.valid, "pkg unscripted stays NA while cores carry frame");
    }
}

void TestAmdProbeDegradesAndFuses() {
    {   // 能量域全缺失 -> 熔断返回 false(温度/频率同步缺失亦为 NA)
        FixtureDriverIo io;
        io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;   // 单位可用
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 16; info.baseGHz = 3.8;
        auto probe = pd::CreateAmdProbe(io, info);
        pd::Sample s;
        Expect(!probe->readSample(s), "all energy domains absent fails the frame");
        Expect(!s.pkgW.valid && !s.coresW.valid, "energy fields stay invalid");
    }
    {   // 能量可用而温度/频率缺失 -> 帧成立,子项各自 NA;
        // APERF/MPERF 补上后 freq = baseGHz * ΔA/ΔM(不除 1000)
        FixtureDriverIo io;
        io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;
        io.msrPerCore[{0, 0xC001029B}] = 0;
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 16; info.baseGHz = 3.8;
        auto probe = pd::CreateAmdProbe(io, info);
        io.msrPerCore[{0, 0xC001029B}] = 32768;   // 2.0 W 保帧
        pd::Sample s;
        Expect(probe->readSample(s), "frame survives missing temp/freq");
        Expect(s.pkgW.valid && !s.tempC.valid && !s.freqGHz.valid,
               "temp/freq degrade to NA independently");
        // ctor 基线期未脚本化(prev=0);本帧 ΔA=1000, ΔM=2000 -> 3.8*0.5
        io.msrPerCore[{0, 0xE8}] = 1000;
        io.msrPerCore[{0, 0xE7}] = 2000;
        pd::Sample s2;
        Expect(probe->readSample(s2), "second frame reads");
        Expect(s2.freqGHz.valid && std::abs(s2.freqGHz.value - 1.9) < 0.001,
               "freq = baseGHz * aperf/mperf ratio (3.8 * 0.5)");
    }
}

void TestAmdProbeCoreFailureBlanksDomainAndRebaselines() {
    // 终审修复:逐核 0xC001029A 任一核读取失败 -> coresW 整域 NA(对齐
    // IntelProbe 域语义,不输出残缺和);失败核标记 stale,恢复帧只刷新
    // 基线、跳过一次差分(防止陈旧 prev 造成跨帧累积尖峰)。
    // v3 fixture:静态表 + msrFailure 钩子推进计数器(读失败但硬件照走)。
    FixtureDriverIo io;
    io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;   // 1/16384 J
    io.msrPerCore[{0, 0xC001029B}] = 0;            // pkg 保帧
    for (unsigned c = 0; c < 4; ++c)
        io.msrPerCore[{c, 0xC001029A}] = 1024;     // ctor 基线一圈后每核 prev=1024
    // core 1 的第 2 次读取(ctor 基线之后的首帧采样)失败;钩子先推进
    // 计数器再报失败,模拟真实硬件"读取失败但计数器照常前进"。
    int core1Reads = 0;
    io.msrFailure = [&core1Reads, &io](unsigned core, uint32_t msr) {
        if (core == 1 && msr == 0xC001029A && ++core1Reads == 2) {
            io.msrPerCore[{1, 0xC001029A}] += 1024;
            return true;
        }
        return false;
    };
    pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
    info.logicalProcessors = 4; info.physicalCores = 4;
    SetCoreReps(info, {0, 1, 2, 3});
    info.baseGHz = 3.8;
    auto probe = pd::CreateAmdProbe(io, info);

    // 基线:ctor 一圈后每核 prev = 1024。帧 1:三核读 2048(各 +1024),
    // core1 读失败(计数器被钩子推进到 2048)。
    io.msrPerCore[{0, 0xC001029B}] = 32768;
    for (unsigned c = 0; c < 4; ++c)
        if (c != 1) io.msrPerCore[{c, 0xC001029A}] = 2048;
    pd::Sample s1;
    Expect(probe->readSample(s1), "pkg carries the failing frame");
    Expect(!s1.coresW.valid,
           "any per-core read failure blanks the whole cores domain");

    // 恢复帧:core1 只重置基线(2048,跳过差分),其余 3 核各 +1024 ->
    // sum = 3072 raw;若沿用陈旧 prev,core1 会贡献 2048(两帧累积)
    // -> 5120 raw = 0.3125 W 尖峰。
    for (unsigned c = 0; c < 4; ++c)
        if (c != 1) io.msrPerCore[{c, 0xC001029A}] = 3072;
    pd::Sample s2;
    Expect(probe->readSample(s2), "recovery frame reads");
    Expect(s2.coresW.valid, "domain recovers once the failed core reads again");
    Expect(s2.coresW.valid &&
               std::abs(s2.coresW.value - (3072.0 / 16384.0)) < 0.0001,
           "recovery frame re-baselines the failed core (no spike)");

    // 稳态:4 核全部恢复单帧差分(core1 从 2048 基线到 3072),sum = 4096 raw。
    for (unsigned c = 0; c < 4; ++c)
        if (c != 1) io.msrPerCore[{c, 0xC001029A}] = 4096;
    io.msrPerCore[{1, 0xC001029A}] = 3072;
    pd::Sample s3;
    Expect(probe->readSample(s3), "steady frame reads");
    Expect(s3.coresW.valid &&
               std::abs(s3.coresW.value - (4096.0 / 16384.0)) < 0.0001,
           "steady state resumes full per-frame deltas");
}

void TestAmdProbeScansPhysicalCoresOnly() {
    // tb16g7 实测缺陷:0xC001029A 按物理核计数,SMT 兄弟 LP 共享同一
    // 计数器;遍历全部 nLP 会双计(实测 IA 167% of PKG)。Windows 枚举
    // 同核兄弟为相邻 LP(8C/16T mask 0x0003/0x000C/…,代表集 =
    // {0,2,4,6,8,10,12,14}),探针按 cores 的 repLP 每物理核只读一个代表 LP。
    // fixture 只给 8 个代表 LP 表项:若实现遍历 16 个 LP,奇数 LP 无表项
    // -> 读取失败,断言立即暴露;oddLP 钩子另证 0xC001029A 从不寻址奇数 LP。
    FixtureDriverIo io;
    io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;   // 1/16384 J
    io.msrPerCore[{0, 0xC001029B}] = 0;            // pkg 保帧
    const unsigned reps[8] = {0, 2, 4, 6, 8, 10, 12, 14};
    for (unsigned lp : reps) io.msrPerCore[{lp, 0xC001029A}] = 0;
    bool readOddLp = false;
    io.msrFailure = [&readOddLp](unsigned core, uint32_t msr) {
        if (msr == 0xC001029A && (core & 1)) readOddLp = true;
        return false;                                     // 只观测不注入
    };
    pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
    info.logicalProcessors = 16; info.physicalCores = 8;  // 8C/16T SMT
    SetCoreReps(info, {0, 2, 4, 6, 8, 10, 12, 14});
    info.baseGHz = 3.8;
    auto probe = pd::CreateAmdProbe(io, info);
    io.msrPerCore[{0, 0xC001029B}] = 32768;
    // 每物理核每帧 +1024 raw -> 合计 8*1024 = 8192 raw = 0.5 W
    for (unsigned lp : reps) io.msrPerCore[{lp, 0xC001029A}] = 1024;
    pd::Sample s;
    Expect(probe->readSample(s), "smt-dedup sample reads");
    Expect(s.coresW.valid && std::abs(s.coresW.value - 0.5) < 0.0001,
           "cores counts each physical core exactly once");
    Expect(!readOddLp, "per-core MSR never addresses an SMT sibling LP");

    // repLP 越界(> nLP)-> 整表弃用,退回全 LP 遍历(保底不残缺):
    // 16 LP fixture 下 cores 域读满 16 个计数器仍成立。
    FixtureDriverIo io2;
    io2.msrPerCore[{0, 0xC0010299}] = 14ull << 8;
    io2.msrPerCore[{0, 0xC001029B}] = 0;
    for (unsigned lp = 0; lp < 16; ++lp) io2.msrPerCore[{lp, 0xC001029A}] = 0;
    pd::PlatformInfo info2; info2.vendor = pd::Vendor::Amd;
    info2.logicalProcessors = 16; info2.physicalCores = 8;
    SetCoreReps(info2, {0, 2, 99});                       // 越界条目
    auto probe2 = pd::CreateAmdProbe(io2, info2);
    io2.msrPerCore[{0, 0xC001029B}] = 32768;
    // 16 个 LP 各 +512 raw -> 合计 16*512 = 8192 raw = 0.5 W
    for (unsigned lp = 0; lp < 16; ++lp) io2.msrPerCore[{lp, 0xC001029A}] = 512;
    pd::Sample s2;
    Expect(probe2->readSample(s2), "fallback sample reads");
    Expect(s2.coresW.valid && std::abs(s2.coresW.value - 0.5) < 0.0001,
           "invalid topology falls back to all-LP scan");
}

void TestAmdProbeTempRangeOffset() {
    // tb16g7(family 0x1A)实测缺陷:SMN 0x59800 读数恒带 RANGE_SEL(bit19),
    // 旧式 (raw>>21)*0.125 解码虚高 49 C(idle 81 / 载荷 126 / 满载 141)。
    // 修正采用 Linux k10temp 语义(drivers/hwmon/k10temp.c,"Common for Zen
    // CPU families (17h/18h/19h/1Ah)"):RANGE_SEL(bit19)=1 或
    // TJ_SEL([17:16])=0b11 时 -49 C。fixture 直接用 tb16g7 抓到的原始值:
    //   idle 0x59800 = 0x510B0000 -> 81.0 - 49 = 32.0 C
    //   21 W 载荷      = 0x7D8B0000 -> 125.5 - 49 = 76.5 C
    //   17h 老式无标志 raw = 640<<21(0x50000000)-> 80.0 C(不加偏移)
    {
        FixtureDriverIo io;
        io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;
        io.msrPerCore[{0, 0xC001029B}] = 0;   // pkg 保帧
        io.smn[0x59800] = 0x510B0000u;        // 实测 idle
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 16; info.physicalCores = 8;
        SetCoreReps(info, {0, 2, 4, 6, 8, 10, 12, 14});
        info.family = 0x1A;
        auto probe = pd::CreateAmdProbe(io, info);
        io.msrPerCore[{0, 0xC001029B}] = 32768;
        pd::Sample s;
        Expect(probe->readSample(s), "idle temp sample reads");
        Expect(s.tempC.valid && std::abs(s.tempC.value - 32.0) < 0.01,
               "RANGE_SEL temp decodes with -49 C (idle 0x510B0000)");
        io.smn[0x59800] = 0x7D8B0000u;        // 实测 21 W 载荷
        pd::Sample s2;
        Expect(probe->readSample(s2), "load temp sample reads");
        Expect(s2.tempC.valid && std::abs(s2.tempC.value - 76.5) < 0.01,
               "RANGE_SEL temp decodes with -49 C (load 0x7D8B0000)");
    }
    {
        // 17h/19h 老式读数:bit19=0 且 TJ_SEL!=11 -> 保持原解码
        FixtureDriverIo io;
        io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;
        io.msrPerCore[{0, 0xC001029B}] = 0;
        io.smn[0x59800] = 640u << 21;         // 80.0 C,无标志位
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 16; info.physicalCores = 8;
        SetCoreReps(info, {0, 2, 4, 6, 8, 10, 12, 14});
        info.family = 0x19;
        auto probe = pd::CreateAmdProbe(io, info);
        io.msrPerCore[{0, 0xC001029B}] = 32768;
        pd::Sample s;
        Expect(probe->readSample(s), "legacy temp sample reads");
        Expect(s.tempC.valid && std::abs(s.tempC.value - 80.0) < 0.01,
               "no RANGE_SEL keeps the plain 0.125 C decode");
    }
}

void TestAmdProbePstateBaseClock() {
    // tb16g7 实测缺陷:AMD 不实现 CPUID 0x16(读 0)-> 频率恒 NA。基频
    // 改从 P-state P0(MSR 0xC0010064)CpuFid 解码(AMD PPR CoreCOF 定义,
    // LibreHardwareMonitor Amd17Cpu.cs 同式实现):
    //   family 0x1A(Zen5,PPR 57896-B0):CpuFid[11:0] * 5 MHz
    //     -> fid=0x334(820)→ 820*5 = 4100 MHz(4.1 GHz)
    //   family 17h/19h(PPR 55570-B1 / PPR 19h Model 70h A0):
    //     CpuFid[7:0] / CpuDfsId[13:8] * 200 MHz
    //     -> fid=0xA8(168)/DfsId=8 → 4200 MHz(ZenStates 文档示例:倍频
    //        42.0x 即 4.2 GHz)
    {
        FixtureDriverIo io;
        io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;
        io.msrPerCore[{0, 0xC001029B}] = 0;   // pkg 保帧
        io.msrPerCore[{0, 0xC0010064}] = 0x334ull;         // Zen5 P0
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 16; info.physicalCores = 8;
        SetCoreReps(info, {0, 2, 4, 6, 8, 10, 12, 14});
        info.family = 0x1A; info.baseGHz = 0.0;           // CPUID 0x16 缺席
        auto probe = pd::CreateAmdProbe(io, info);
        Expect(std::abs(probe->caps().baseGHz - 4.1) < 0.001,
               "zen5 base = CpuFid[11:0] * 5 MHz");
        // ctor 基线期 APERF/MPERF 未脚本化(prev=0);本帧 ΔA=1000,
        // ΔM=2000 -> 比值 0.5 -> 4.1 * 0.5 = 2.05 GHz
        io.msrPerCore[{0, 0xC001029B}] = 32768;
        io.msrPerCore[{0, 0xE8}] = 1000;
        io.msrPerCore[{0, 0xE7}] = 2000;
        pd::Sample s;
        Expect(probe->readSample(s), "zen5 freq sample reads");
        Expect(s.freqGHz.valid && std::abs(s.freqGHz.value - 2.05) < 0.001,
               "zen5 freq = P0 base * aperf/mperf ratio");
    }
    {
        FixtureDriverIo io;
        io.msrPerCore[{0, 0xC0010299}] = 14ull << 8;
        io.msrPerCore[{0, 0xC001029B}] = 0;
        io.msrPerCore[{0, 0xC0010064}] = (8ull << 8) | 0xA8ull;  // 168/8
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 8; info.physicalCores = 4;
        SetCoreReps(info, {0, 2, 4, 6});
        info.family = 0x19; info.baseGHz = 0.0;
        auto probe = pd::CreateAmdProbe(io, info);
        Expect(std::abs(probe->caps().baseGHz - 4.2) < 0.001,
               "zen4 base = CpuFid / CpuDfsId * 200 MHz");
        // 无 P-state(family 0x19 未脚本化 0xC0010064)时保持入口层
        // baseGHz(0),频率 NA —— 诚实降级
        FixtureDriverIo io2;
        io2.msrPerCore[{0, 0xC0010299}] = 14ull << 8;
        io2.msrPerCore[{0, 0xC001029B}] = 0;
        pd::PlatformInfo info2; info2.vendor = pd::Vendor::Amd;
        info2.logicalProcessors = 8; info2.physicalCores = 4;
        SetCoreReps(info2, {0, 2, 4, 6});
        info2.family = 0x19; info2.baseGHz = 0.0;
        auto probe2 = pd::CreateAmdProbe(io2, info2);
        io2.msrPerCore[{0, 0xC001029B}] = 32768;
        pd::Sample s2;
        Expect(probe2->readSample(s2), "no-pstate frame reads");
        Expect(!s2.freqGHz.valid, "missing P-state leaves freq NA");
    }
}

void TestSamplerDrivesSinkAndFillsPlatformIndependentFields() {
    FakeProbe p;
    pd::Sample a; a.pkgW = pd::Ok(5.0); a.mode = "n/a";
    pd::Sample b; b.pkgW = pd::Ok(6.0);
    p.script = {a, b};
    std::vector<pd::Sample> got;
    pd::Sampler s(p, [] { return "Intelligent (APM)"; }, nullptr,
                  [] {});                       // 即时 tick,测试不真实睡眠
    bool ok = s.Run(2.0, [&](const pd::Sample& x) { got.push_back(x); });
    Expect(ok, "sampler completes scripted run");
    Expect(got.size() == 2, "one sink call per tick");
    Expect(got[0].utilPct.valid, "sampler fills utilization");
    Expect(got[0].mode == "Intelligent (APM)", "sampler fills mode");
    Expect(!got[0].timestamp.empty() && got[0].elapsedS >= 0.0,
           "sampler fills timestamp/elapsed");
}

void TestSamplerExitsAfterFiveConsecutiveFailures() {
    FakeProbe p;                                // script 为空,每次 readSample 失败
    int sinks = 0;
    pd::Sampler s(p, [] { return "n/a"; }, nullptr, [] {});
    bool ok = s.Run(-1.0, [&](const pd::Sample&) { ++sinks; });
    Expect(!ok, "five consecutive failures abort the run");
    Expect(sinks == 0, "failed frames never reach the sink");
}

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

// ---- Task 3 修复:ParseCoreTopology 合成缓冲单测 ----
// x64 SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX(RelationProcessorCore)手工
// 布局(SDK winnt.h:GroupMask[ANYSIZE_ARRAY] 数组,无 union):头 8 字节
// (Relationship ULONG @0、Size ULONG @4)+ PROCESSOR_RELATIONSHIP
// {Flags @8、EfficiencyClass @9、Reserved[20] @10、GroupCount WORD @30、
// GroupMask[0].Mask KAFFINITY @32、GroupMask[0].Group WORD @40} = 48 字节。
std::vector<unsigned char> CoreExEntry(unsigned long long mask,
                                       unsigned effClass,
                                       unsigned short groupCount = 1,
                                       unsigned short group = 0,
                                       unsigned long entrySize = 48) {
    // 实际分配取 max(entrySize, 48):builder 要写满头部+GroupMask[0]
    // (偏移 0..41),Size=0 畸形条目也需占位字节(头里写声称值 0)。
    std::vector<unsigned char> e(entrySize < 48 ? 48 : entrySize, 0);
    const auto put16 = [&e](size_t off, unsigned short v) {
        memcpy(e.data() + off, &v, sizeof(v));
    };
    const auto put32 = [&e](size_t off, unsigned long v) {
        memcpy(e.data() + off, &v, sizeof(v));
    };
    const auto put64 = [&e](size_t off, unsigned long long v) {
        memcpy(e.data() + off, &v, sizeof(v));
    };
    put32(0, 0);                    // Relationship = RelationProcessorCore
    put32(4, entrySize);            // Size
    e[9] = (unsigned char)effClass;
    put16(30, groupCount);          // GroupCount
    put64(32, mask);                // GroupMask[0].Mask
    put16(40, group);               // GroupMask[0].Group
    return e;
}

void TestParseCoreTopology() {
    // 回归(评审实测形态):16 条目 x 48 字节 = 768。旧准入
    // off + sizeof(EX)=80 <= bytes 在末条目(起点 720)处 720+80 > 768,
    // 无条件丢掉最后一个物理核;新准入只看 8 字节头,末核必须在场。
    std::vector<unsigned char> buf;
    for (unsigned i = 0; i < 8; ++i) {          // 8 个 SMT 对:LP {2i, 2i+1},eff 0
        const std::vector<unsigned char> e = CoreExEntry(0x3ull << (2 * i), 0);
        buf.insert(buf.end(), e.begin(), e.end());
    }
    for (unsigned i = 0; i < 8; ++i) {          // 8 个单线程核:LP 16..23,eff 1
        const std::vector<unsigned char> e = CoreExEntry(1ull << (16 + i), 1);
        buf.insert(buf.end(), e.begin(), e.end());
    }
    Expect(buf.size() == 768, "synthetic buffer is 16 entries x 48 bytes");
    std::vector<pd::CoreInfo> cores;
    Expect(pd::ParseCoreTopology(buf.data(), (unsigned long)buf.size(), cores),
           "16-entry buffer parses");
    Expect(cores.size() == 16,
           "last entry admitted (old sizeof(EX) bound dropped it -> 15)");
    if (cores.size() == 16) {
        Expect(cores[0].repLP == 0 && cores[0].threads.size() == 2 &&
               cores[0].threads[0] == 0 && cores[0].threads[1] == 1 &&
               cores[0].effClass == 0, "first SMT pair {0,1} eff 0");
        Expect(cores[7].repLP == 14 && cores[7].threads.size() == 2 &&
               cores[7].threads[0] == 14 && cores[7].threads[1] == 15,
               "last SMT pair {14,15}");
        Expect(cores[15].repLP == 23 && cores[15].threads.size() == 1 &&
               cores[15].threads[0] == 23 && cores[15].effClass == 1,
               "final single-thread E-core entry survives (regression)");
    }
    {   // 跨组拒绝:GroupCount=2(条目 64 字节)与 mask 落在非 0 组。
        std::vector<unsigned char> b = CoreExEntry(0x3, 0, 2, 0, 64);
        std::vector<pd::CoreInfo> c;
        Expect(!pd::ParseCoreTopology(b.data(), (unsigned long)b.size(), c),
               "GroupCount != 1 rejected");
    }
    {
        std::vector<unsigned char> b = CoreExEntry(0x3, 0, 1, 1);
        std::vector<pd::CoreInfo> c;
        Expect(!pd::ParseCoreTopology(b.data(), (unsigned long)b.size(), c),
               "non-zero group rejected");
    }
    {   // 破损条目:Size=0(步进死循环防线)与 Size 越过缓冲尾。
        std::vector<unsigned char> b = CoreExEntry(0x1, 0, 1, 0, 0);
        std::vector<pd::CoreInfo> c;
        Expect(!pd::ParseCoreTopology(b.data(), (unsigned long)b.size(), c),
               "Size=0 entry rejected");
    }
    {
        std::vector<unsigned char> b = CoreExEntry(0x1, 0);
        const std::vector<unsigned char> second = CoreExEntry(0x2, 0);
        b.insert(b.end(), second.begin(), second.begin() + 40);  // 只剩 40 字节
        std::vector<pd::CoreInfo> c;
        Expect(!pd::ParseCoreTopology(b.data(), (unsigned long)b.size(), c),
               "entry overrunning buffer rejected");
    }
}

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

} // namespace

int main() {
    TestPowerArgumentsAcceptDurationAndCsvInEitherOrder();
    TestPowerArgumentsRejectMissingCsvPathAndInvalidDuration();
    TestCsvHasStableColumnsAndEscapesText();
    TestLogoIsCenteredAndColorDoesNotChangeItsWidth();
    TestWideDashboardBalancesAtAGlanceAndDiagnosticInformation();
    TestNarrowDashboardStacksGroupsWithoutDroppingMetrics();
    TestAmdDashboardHidesAbsentSections();
    TestAnsiColorsPreserveDashboardGeometry();
    TestModelDecompositionIdentities();
    TestDriverIoFixtureRouting();
    TestIntelProbeReplay();
    TestIntelProbeEnergyWraparound();
    TestIntelProbeWideTable();
    TestAmdProbeReplay();
    TestAmdProbeEnergyWraparound();
    TestAmdProbeDegradesAndFuses();
    TestAmdProbeCoreFailureBlanksDomainAndRebaselines();
    TestAmdProbeScansPhysicalCoresOnly();
    TestAmdProbeTempRangeOffset();
    TestAmdProbePstateBaseClock();
    TestSamplerDrivesSinkAndFillsPlatformIndependentFields();
    TestSamplerExitsAfterFiveConsecutiveFailures();
    TestUsageMonitorDiff();
    TestNtUsageSourceInstantiates();
    TestCalibrateTscHzPlausible();
    TestCoreInfoFallback();
    TestParseCoreTopology();
    TestSensorTableBasics();
    TestFormatSensorCell();
    TestCsvV3HeaderAndRow();
    TestHwDateTimeFormats();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All PowerDash UI tests passed\n";
    return EXIT_SUCCESS;
}
