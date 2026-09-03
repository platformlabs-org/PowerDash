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

class FixtureDriverIo : public pd::DriverIo {   // 可编程应答,probe 回放测试用
public:
    std::map<uint32_t, std::function<uint64_t()>> msr;   // msr -> 每次读取的值
    std::map<uint32_t, uint32_t> smn;                    // smn addr -> value
    // 可选:按 (core, msr) 脚本化读取失败(单核单次注入用)。真实硬件的
    // 计数器在读取失败期间照常前进,故钩子需自行推进 fixture 计数器。
    std::function<bool(unsigned core, uint32_t msr)> msrFailure;
    bool failAllMsrs = false;
    bool ReadMsr(unsigned core, uint32_t a, uint64_t& out) override {
        if (failAllMsrs) return false;
        if (msrFailure && msrFailure(core, a)) return false;
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
    bool WriteSmn(uint32_t, uint32_t) override { return false; }   // 后续任务扩展为可编程
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
    // 8 物理核(0xC001029A 按物理核计数,SMT 兄弟共享 -> 每核只读一个
    // 代表 LP)每帧各 +2048 raw -> 合计 8*2048 = 16384 raw = 1.0 W
    AmdCoreEnergy coreEnergy(8, 2048);
    io.msr[0xC001029A] = [&coreEnergy] { return coreEnergy(); };
    io.smn[0x59800] = 640u << 21;   // Tctl: 640 * 0.125 = 80.0 C(无 RANGE_SEL)
    pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
    info.cpuName = "AMD Ryzen 7 8845H  [Hawk Point]";
    info.logicalProcessors = 16; info.physicalCores = 8;   // 8C/16T
    info.coreLPs = {0, 2, 4, 6, 8, 10, 12, 14};            // Windows 相邻对
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
        // (nLP=4/physicalCores=4:无 SMT,代表集即全部 4 个 LP)
        FixtureDriverIo io;
        io.msr[0xC0010299] = [] { return 14ull << 8; };
        AmdCoreEnergy coreEnergy(4, 0x200, 0xFFFFFD00ull);
        io.msr[0xC001029A] = [&coreEnergy] { return coreEnergy(); };
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 4; info.physicalCores = 4;
        info.coreLPs = {0, 1, 2, 3};
        info.baseGHz = 3.8;
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

void TestAmdProbeCoreFailureBlanksDomainAndRebaselines() {
    // 终审修复:逐核 0xC001029A 任一核读取失败 -> coresW 整域 NA(对齐
    // IntelProbe 域语义,不输出残缺和);失败核标记 stale,恢复帧只刷新
    // 基线、跳过一次差分(防止陈旧 prev 造成跨帧累积尖峰)。
    FixtureDriverIo io;
    io.msr[0xC0010299] = [] { return 14ull << 8; };      // 1/16384 J
    uint64_t pkg = 0;   // pkg 每帧有效,保证 readSample 帧成立
    io.msr[0xC001029B] = [&pkg] { pkg += 32768; return pkg; };
    AmdCoreEnergy coreEnergy(4, 1024);   // 每帧每核 +1024 raw(4/4 无 SMT)
    io.msr[0xC001029A] = [&coreEnergy] { return coreEnergy(); };
    // core 1 的第 2 次读取(ctor 基线之后的首帧采样)失败;钩子先推进
    // 计数器再报失败,模拟真实硬件"读取失败但计数器照常前进"。调用序
    // 路由保持对齐。
    int core1Reads = 0;
    io.msrFailure = [&core1Reads, &coreEnergy](unsigned core, uint32_t) {
        if (core == 1 && ++core1Reads == 2) {
            coreEnergy();
            return true;
        }
        return false;
    };
    pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
    info.logicalProcessors = 4; info.physicalCores = 4;
    info.coreLPs = {0, 1, 2, 3};
    info.baseGHz = 3.8;
    auto probe = pd::CreateAmdProbe(io, info);

    // 基线:ctor 一圈后每核 prev = 1024。
    pd::Sample s1;
    Expect(probe->readSample(s1), "pkg carries the failing frame");
    Expect(!s1.coresW.valid,
           "any per-core read failure blanks the whole cores domain");

    // 恢复帧:core1 只重置基线(跳过差分),其余 3 核各 +1024 ->
    // sum = 3072 raw;若沿用陈旧 prev,core1 会贡献 2048(两帧累积)
    // -> 5120 raw = 0.3125 W 尖峰。
    pd::Sample s2;
    Expect(probe->readSample(s2), "recovery frame reads");
    Expect(s2.coresW.valid, "domain recovers once the failed core reads again");
    Expect(s2.coresW.valid &&
               std::abs(s2.coresW.value - (3072.0 / 16384.0)) < 0.0001,
           "recovery frame re-baselines the failed core (no spike)");

    // 稳态:4 核全部恢复单帧差分,sum = 4096 raw。
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
    // {0,2,4,6,8,10,12,14}),探针按 coreLPs 每物理核只读一个代表 LP。
    // fixture 提供 8 个独立计数器,每物理核每帧 +1024 raw -> 合计
    // 8*1024 = 8192 raw = 0.5 W;若实现遍历 16 个 LP,fixture 的调用
    // 路由被拉长一倍,读数翻倍,数值断言立即失败;oddLP 钩子另证
    // 0xC001029A 从不寻址奇数 LP(SMT 兄弟)。
    FixtureDriverIo io;
    io.msr[0xC0010299] = [] { return 14ull << 8; };       // 1/16384 J
    uint64_t pkg = 0;   // pkg 每帧有效,保证帧成立
    io.msr[0xC001029B] = [&pkg] { pkg += 32768; return pkg; };
    AmdCoreEnergy coreEnergy(8, 1024);
    io.msr[0xC001029A] = [&coreEnergy] { return coreEnergy(); };
    bool readOddLp = false;
    io.msrFailure = [&readOddLp](unsigned core, uint32_t msr) {
        if (msr == 0xC001029A && (core & 1)) readOddLp = true;
        return false;                                     // 只观测不注入
    };
    pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
    info.logicalProcessors = 16; info.physicalCores = 8;  // 8C/16T SMT
    info.coreLPs = {0, 2, 4, 6, 8, 10, 12, 14};
    info.baseGHz = 3.8;
    auto probe = pd::CreateAmdProbe(io, info);
    pd::Sample s;
    Expect(probe->readSample(s), "smt-dedup sample reads");
    Expect(s.coresW.valid && std::abs(s.coresW.value - 0.5) < 0.0001,
           "cores counts each physical core exactly once");
    Expect(!readOddLp, "per-core MSR never addresses an SMT sibling LP");

    // coreLPs 越界(> nLP)-> 整表弃用,退回全 LP 遍历(保底不残缺):
    // 16 LP fixture 下 cores 域读满 16 个计数器仍成立。
    FixtureDriverIo io2;
    io2.msr[0xC0010299] = [] { return 14ull << 8; };
    uint64_t pkg2 = 0;
    io2.msr[0xC001029B] = [&pkg2] { pkg2 += 32768; return pkg2; };
    AmdCoreEnergy coreEnergy2(16, 512);                   // 16*512 = 8192
    io2.msr[0xC001029A] = [&coreEnergy2] { return coreEnergy2(); };
    pd::PlatformInfo info2; info2.vendor = pd::Vendor::Amd;
    info2.logicalProcessors = 16; info2.physicalCores = 8;
    info2.coreLPs = {0, 2, 99};                           // 越界条目
    auto probe2 = pd::CreateAmdProbe(io2, info2);
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
        io.msr[0xC0010299] = [] { return 14ull << 8; };
        uint64_t pkg = 0;   // pkg 保帧
        io.msr[0xC001029B] = [&pkg] { pkg += 32768; return pkg; };
        io.smn[0x59800] = 0x510B0000u;                    // 实测 idle
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 16; info.physicalCores = 8;
        info.coreLPs = {0, 2, 4, 6, 8, 10, 12, 14};
        info.family = 0x1A;
        auto probe = pd::CreateAmdProbe(io, info);
        pd::Sample s;
        Expect(probe->readSample(s), "idle temp sample reads");
        Expect(s.tempC.valid && std::abs(s.tempC.value - 32.0) < 0.01,
               "RANGE_SEL temp decodes with -49 C (idle 0x510B0000)");
        io.smn[0x59800] = 0x7D8B0000u;                    // 实测 21 W 载荷
        pd::Sample s2;
        Expect(probe->readSample(s2), "load temp sample reads");
        Expect(s2.tempC.valid && std::abs(s2.tempC.value - 76.5) < 0.01,
               "RANGE_SEL temp decodes with -49 C (load 0x7D8B0000)");
    }
    {
        // 17h/19h 老式读数:bit19=0 且 TJ_SEL!=11 -> 保持原解码
        FixtureDriverIo io;
        io.msr[0xC0010299] = [] { return 14ull << 8; };
        uint64_t pkg = 0;
        io.msr[0xC001029B] = [&pkg] { pkg += 32768; return pkg; };
        io.smn[0x59800] = 640u << 21;                     // 80.0 C,无标志位
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 16; info.physicalCores = 8;
        info.coreLPs = {0, 2, 4, 6, 8, 10, 12, 14};
        info.family = 0x19;
        auto probe = pd::CreateAmdProbe(io, info);
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
        io.msr[0xC0010299] = [] { return 14ull << 8; };
        uint64_t pkg = 0;   // pkg 保帧
        io.msr[0xC001029B] = [&pkg] { pkg += 32768; return pkg; };
        io.msr[0xC0010064] = [] { return 0x334ull; };     // Zen5 P0
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 16; info.physicalCores = 8;
        info.coreLPs = {0, 2, 4, 6, 8, 10, 12, 14};
        info.family = 0x1A; info.baseGHz = 0.0;           // CPUID 0x16 缺席
        auto probe = pd::CreateAmdProbe(io, info);
        Expect(std::abs(probe->caps().baseGHz - 4.1) < 0.001,
               "zen5 base = CpuFid[11:0] * 5 MHz");
        // ctor 基线期 APERF/MPERF 未脚本化(prev=0);本帧 ΔA=1000,
        // ΔM=2000 -> 比值 0.5 -> 4.1 * 0.5 = 2.05 GHz
        uint64_t aperf = 0, mperf = 0;
        io.msr[0xE8] = [&aperf] { aperf += 1000; return aperf; };
        io.msr[0xE7] = [&mperf] { mperf += 2000; return mperf; };
        pd::Sample s;
        Expect(probe->readSample(s), "zen5 freq sample reads");
        Expect(s.freqGHz.valid && std::abs(s.freqGHz.value - 2.05) < 0.001,
               "zen5 freq = P0 base * aperf/mperf ratio");
    }
    {
        FixtureDriverIo io;
        io.msr[0xC0010299] = [] { return 14ull << 8; };
        uint64_t pkg = 0;
        io.msr[0xC001029B] = [&pkg] { pkg += 32768; return pkg; };
        io.msr[0xC0010064] = [] { return (8ull << 8) | 0xA8ull; };  // 168/8
        pd::PlatformInfo info; info.vendor = pd::Vendor::Amd;
        info.logicalProcessors = 8; info.physicalCores = 4;
        info.coreLPs = {0, 2, 4, 6};
        info.family = 0x19; info.baseGHz = 0.0;
        auto probe = pd::CreateAmdProbe(io, info);
        Expect(std::abs(probe->caps().baseGHz - 4.2) < 0.001,
               "zen4 base = CpuFid / CpuDfsId * 200 MHz");
        // 无 P-state(family 0x19 未脚本化 0xC0010064)时保持入口层
        // baseGHz(0),频率 NA —— 诚实降级
        FixtureDriverIo io2;
        io2.msr[0xC0010299] = [] { return 14ull << 8; };
        uint64_t pkg2 = 0;
        io2.msr[0xC001029B] = [&pkg2] { pkg2 += 32768; return pkg2; };
        pd::PlatformInfo info2; info2.vendor = pd::Vendor::Amd;
        info2.logicalProcessors = 8; info2.physicalCores = 4;
        info2.coreLPs = {0, 2, 4, 6};
        info2.family = 0x19; info2.baseGHz = 0.0;
        auto probe2 = pd::CreateAmdProbe(io2, info2);
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
    TestAmdProbeCoreFailureBlanksDomainAndRebaselines();
    TestAmdProbeScansPhysicalCoresOnly();
    TestAmdProbeTempRangeOffset();
    TestAmdProbePstateBaseClock();
    TestSamplerDrivesSinkAndFillsPlatformIndependentFields();
    TestSamplerExitsAfterFiveConsecutiveFailures();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All PowerDash UI tests passed\n";
    return EXIT_SUCCESS;
}
