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

// ---- Task 8: AmdProbe(保底监控集)----

// AMD 逐核能量 fixture:实现按"每帧遍历 core 0..nLP-1 各读一次
// 0xC001029A、各自独立差分"的语义采样;fixture 依调用序号把读数路由到
// perCore[c],模拟每个核自己的 32 位计数器(ctor 基线一圈、readSample 一圈)。
// 若实现误用单一共享计数器,基线在圈中间被取走,每核差分会放大 n 倍。
struct AmdCoreEnergy {
    AmdCoreEnergy(unsigned n, uint64_t step, uint64_t seed = 0)
        : n_(n), step_(step), v_(n, seed) {}
    uint64_t operator()() {
        unsigned c = calls_++ % n_;
        v_[c] = (v_[c] + step_) & 0xFFFFFFFFull;   // 32 位计数器回绕语义
        return v_[c];
    }
    unsigned n_;
    unsigned calls_ = 0;
    uint64_t step_;
    std::vector<uint64_t> v_;
};

void TestAmdProbeReplay() {
    FixtureDriverIo io;
    // 单位寄存器 0xC0010299 与 Intel 0x606 位兼容(turbostat: energy bits
    // 8-12):energy bits=14 -> 1/16384 J
    io.msr[0xC0010299] = [] { return 14ull << 8; };
    uint64_t pkg = 0;   // 每读 +32768 raw = 2.0 J -> 2.0 W
    io.msr[0xC001029B] = [&pkg] { pkg += 32768; return pkg; };
    // 16 核每帧各 +1024 raw -> 合计 16384 raw = 1.0 W
    AmdCoreEnergy coreEnergy(16, 1024);
    io.msr[0xC001029A] = [&coreEnergy] { return coreEnergy(); };
    io.smn[0x59800] = 640u << 21;   // Tctl: 640 * 0.125 = 80.0 C
    pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
    info.cpuName = "AMD Ryzen 7 8845H  [Hawk Point]";
    info.logicalProcessors = 16; info.baseGHz = 3.8;
    auto probe = pd::CreateAmdProbe(io, info);
    Expect(probe != nullptr, "amd probe constructs");
    pd::PlatformCaps c = probe->caps();
    Expect(!c.gfxPower && !c.platformPower && !c.powerLimits && !c.residency &&
           !c.smi, "amd caps degrade honestly");
    Expect(c.vendor == pd::Vendor::Amd && c.logicalProcessors == 16 &&
           c.cpuName == "AMD Ryzen 7 8845H  [Hawk Point]" &&
           std::abs(c.baseGHz - 3.8) < 0.01, "amd caps carry identity");
    Expect(c.tjMaxC == 0 && c.budgetW == 0.0, "amd leaves tjMax/budget unknown");
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
        io.msr[0xC0010299] = [] { return 14ull << 8; };   // 1/16384 J
        uint64_t pkg = 0xFFFFFD00ull;   // 模拟 32 位能量计数器跨 2^32 回绕
        io.msr[0xC001029B] = [&pkg] { pkg = (pkg + 0x200) & 0xFFFFFFFFull; return pkg; };
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;   // nLP=0
        auto probe = pd::CreateAmdProbe(io, info);
        pd::Sample s;
        Expect(probe->readSample(s), "pkg-only sample reads");
        // ctor 基线 prev = 0xFFFFFF00;本帧回绕到 0x100 < prev,
        // 走 curr += 1<<32 分支后差分 = 0x200 = 512 raw -> 512/16384 W
        Expect(s.pkgW.valid && std::abs(s.pkgW.value - (512.0 / 16384.0)) < 0.0001,
               "32-bit pkg wraparound adds 1<<32 to delta");
        Expect(!s.coresW.valid, "zero logical processors leaves cores NA");
    }
    {   // 逐核独立回绕:4 核全部从 2^32-512 起步,每读 +0x200
        FixtureDriverIo io;
        io.msr[0xC0010299] = [] { return 14ull << 8; };
        AmdCoreEnergy coreEnergy(4, 0x200, 0xFFFFFD00ull);
        io.msr[0xC001029A] = [&coreEnergy] { return coreEnergy(); };
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 4; info.baseGHz = 3.8;
        auto probe = pd::CreateAmdProbe(io, info);
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
        io.msr[0xC0010299] = [] { return 14ull << 8; };   // 单位可用
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
        io.msr[0xC0010299] = [] { return 14ull << 8; };
        uint64_t pkg = 0;
        io.msr[0xC001029B] = [&pkg] { pkg += 32768; return pkg; };
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 16; info.baseGHz = 3.8;
        auto probe = pd::CreateAmdProbe(io, info);
        pd::Sample s;
        Expect(probe->readSample(s), "frame survives missing temp/freq");
        Expect(s.pkgW.valid && !s.tempC.valid && !s.freqGHz.valid,
               "temp/freq degrade to NA independently");
        // ctor 基线期未脚本化(prev=0);本帧 ΔA=1000, ΔM=2000 -> 3.8*0.5
        uint64_t aperf = 0, mperf = 0;
        io.msr[0xE8] = [&aperf] { aperf += 1000; return aperf; };
        io.msr[0xE7] = [&mperf] { mperf += 2000; return mperf; };
        pd::Sample s2;
        Expect(probe->readSample(s2), "second frame reads");
        Expect(s2.freqGHz.valid && std::abs(s2.freqGHz.value - 1.9) < 0.001,
               "freq = baseGHz * aperf/mperf ratio (3.8 * 0.5)");
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
    TestAmdProbeReplay();
    TestAmdProbeEnergyWraparound();
    TestAmdProbeDegradesAndFuses();
    TestSamplerDrivesSinkAndFillsPlatformIndependentFields();
    TestSamplerExitsAfterFiveConsecutiveFailures();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All PowerDash UI tests passed\n";
    return EXIT_SUCCESS;
}
