#include "../PowerDash/PowerDashModel.h"
#include "../PowerDash/PowerDashProbe.h"
#include "../PowerDash/PowerDashSampler.h"
#include "../PowerDash/PowerDashUi.h"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
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
        "timestamp,elapsed_s,pkg_w,ia_w,gt_w,sys_w,pl1_w,pl2_w,"
        "temp_c,freq_ghz,c0_pct,c2_pct,c6_pct,util_pct,smi_delta,mode";
    Expect(pd::CsvHeader() == expectedHeader, "CSV header columns changed");

    pd::PowerSample sample;
    sample.timestamp = "2026-09-01T12:34:56";
    sample.elapsedSeconds = 1.0;
    sample.pkgPower = 12.345;
    sample.iaPower = 8.5;
    sample.gtPower = 0.25;
    sample.sysPower = 1.75;
    sample.pl1Watt = 28.0;
    sample.pl2Watt = 45.0;
    sample.tempC = 67;
    sample.freqGHz = 3.125;
    sample.c0Pct = 42.0;
    sample.c2Pct = 18.0;
    sample.c6Pct = 40.0;
    sample.utilPct = 7.5;
    sample.smiDelta = 2;
    sample.mode = "Intelligent, Auto";

    const std::string expectedRow =
        "2026-09-01T12:34:56,1.000,12.345,8.500,0.250,1.750,"
        "28.000,45.000,67,3.125,42.000,18.000,40.000,7.500,2,"
        "\"Intelligent, Auto\"";
    Expect(pd::CsvRow(sample) == expectedRow,
           "CSV row should preserve precision and quote commas");
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
    info.logicalProcessors = 8;
    info.baseGHz = 2.0;
    info.tjMaxC = 100;
    info.width = width;
    info.csvActive = true;
    info.csvName = "capture.csv";
    info.ansi = ansi;
    return info;
}

pd::PowerSample SampleFixture() {
    pd::PowerSample sample;
    sample.elapsedSeconds = 92;
    sample.pkgPower = 18.5;
    sample.iaPower = 12.0;
    sample.gtPower = 0.5;
    sample.sysPower = 25.0;
    sample.pl1Watt = 28.0;
    sample.pl2Watt = 45.0;
    sample.pl1Window = "28.00 s";
    sample.pl2Window = "2.00 ms";
    sample.plLocked = true;
    sample.tempC = 64;
    sample.freqGHz = 3.2;
    sample.c0Pct = 35;
    sample.c2Pct = 15;
    sample.c6Pct = 50;
    sample.utilPct = 8;
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

void TestAnsiColorsPreserveDashboardGeometry() {
    const std::string frame = pd::RenderDashboard(DashboardFixture(96, true),
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

class FixtureDriverIo : public pd::DriverIo {   // 可编程应答,probe 回放测试用
public:
    std::map<uint32_t, std::function<uint64_t()>> msr;   // msr -> 每次读取的值
    std::map<uint32_t, uint32_t> smn;                    // smn addr -> value
    bool failAllMsrs = false;
    bool ReadMsr(unsigned, uint32_t a, uint64_t& out) override {
        if (failAllMsrs) return false;
        auto it = msr.find(a);
        if (it == msr.end()) return false;
        out = it->second();
        return true;
    }
    bool ReadPciCfg(unsigned, unsigned, unsigned, unsigned, uint32_t&) override { return false; }
    bool WritePciCfg(unsigned, unsigned, unsigned, unsigned, uint32_t) override { return false; }
    bool ReadSmn(uint32_t a, uint32_t& out) override {
        auto it = smn.find(a);
        if (it == smn.end()) return false;
        out = it->second; return true;
    }
    bool MapPhys(uint64_t, size_t, void*&) override { return false; }
    void UnmapPhys(void*) override {}
};

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
    io.msr[0x611] = [] { return 12345ull; };
    uint64_t v = 0;
    Expect(io.ReadMsr(0, 0x611, v) && v == 12345, "fixture msr scripted answer");
    Expect(!io.ReadMsr(0, 0x999, v), "fixture unknown msr fails");
}

void TestIntelProbeReplay() {
    FixtureDriverIo io;
    // 单位寄存器:power bits=3(0.125W), energy bits=14(1/16384 J), time bits=10
    io.msr[0x606] = [] { return (14ull << 8) | 3ull; };
    uint64_t pkg = 0;   // 每 tick 增 32768 raw = 2.0 J -> 2 W
    io.msr[0x611] = [&pkg] { pkg += 32768; return pkg; };
    uint64_t pp0 = 0;   // 每 tick 增 16384 raw = 1.0 J -> 1 W
    io.msr[0x639] = [&pp0] { pp0 += 16384; return pp0; };
    uint64_t pp1 = 0, sys = 0;
    io.msr[0x641] = [&pp1] { pp1 += 8192; return pp1; };   // 0.5 W
    io.msr[0x64D] = [&sys] { sys += 49152; return sys; };  // 3.0 W
    // 温度:MSR_PACKAGE_THERM_STATUS 实为 0x1B1(PowerDash.cpp:43,
    // brief 的 0x613 是笔误);valid bit31 + headroom 31 -> 105-31=74 C
    io.msr[0x1B1] = [] { return (1ull << 31) | (0x1Full << 16); };
    io.msr[0x1A2] = [] { return 105ull << 16; };           // TjMax=105
    pd::PlatformInfo info; info.vendor = pd::Vendor::Intel;
    info.cpuName = "Intel(R) Core(TM) Ultra 5 225H  [Arrow Lake-H]";
    info.logicalProcessors = 16; info.baseGHz = 2.5;
    auto probe = pd::CreateIntelProbe(io, info);
    Expect(probe != nullptr, "intel probe constructs");
    Expect(probe->caps().vendor == pd::Vendor::Intel &&
           probe->caps().tjMaxC == 105, "caps carry tjMax");
    // 构造期已消费一次 prev;第一帧即得到差分功率
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
           "PL readings stay invalid without the MMIO window");
    // APERF/MPERF 未脚本化 -> 频率无效
    Expect(!s.freqGHz.valid, "APERF/MPERF unscripted leaves freq invalid");
}

void TestIntelProbeEnergyWraparound() {
    FixtureDriverIo io;
    io.msr[0x606] = [] { return (14ull << 8) | 3ull; };   // 1/16384 J
    uint64_t pkg = 0xFFFFFD00ull;   // 模拟 32 位能量计数器跨 2^32 回绕
    io.msr[0x611] = [&pkg] { pkg = (pkg + 0x200) & 0xFFFFFFFFull; return pkg; };
    pd::PlatformInfo info; info.vendor = pd::Vendor::Intel;
    auto probe = pd::CreateIntelProbe(io, info);
    pd::Sample s;
    Expect(probe->readSample(s), "wraparound sample reads");
    // ctor 基线读得 prev = 0xFFFFFF00;本帧读回绕到 0x100 < prev,
    // 走 curr += 1<<32 分支后差分 = 0x100000100 - 0xFFFFFF00 = 0x200
    Expect(s.pkgW.valid && std::abs(s.pkgW.value - (512.0 / 16384.0)) < 0.0001,
           "32-bit energy wraparound adds 1<<32 to the delta");
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

} // namespace

int main() {
    TestPowerArgumentsAcceptDurationAndCsvInEitherOrder();
    TestPowerArgumentsRejectMissingCsvPathAndInvalidDuration();
    TestCsvHasStableColumnsAndEscapesText();
    TestLogoIsCenteredAndColorDoesNotChangeItsWidth();
    TestWideDashboardBalancesAtAGlanceAndDiagnosticInformation();
    TestNarrowDashboardStacksGroupsWithoutDroppingMetrics();
    TestAnsiColorsPreserveDashboardGeometry();
    TestModelDecompositionIdentities();
    TestDriverIoFixtureRouting();
    TestIntelProbeReplay();
    TestIntelProbeEnergyWraparound();
    TestSamplerDrivesSinkAndFillsPlatformIndependentFields();
    TestSamplerExitsAfterFiveConsecutiveFailures();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All PowerDash UI tests passed\n";
    return EXIT_SUCCESS;
}
