#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pd {

struct MonitorOptions {
    double runSeconds = -1.0;
    std::string csvPath;
};

struct PowerSample {
    std::string timestamp;
    double elapsedSeconds = 0.0;
    double pkgPower = 0.0;
    double iaPower = 0.0;
    double gtPower = 0.0;
    double sysPower = 0.0;
    double pl1Watt = 0.0;
    double pl2Watt = 0.0;
    std::string pl1Window;
    std::string pl2Window;
    bool plLocked = false;
    int tempC = -1;
    double freqGHz = 0.0;
    double c0Pct = 0.0;
    double c2Pct = 0.0;
    double c6Pct = 0.0;
    double utilPct = 0.0;
    std::uint64_t smiDelta = 0;
    std::string mode = "n/a";
};

struct DashboardInfo {
    std::string version;
    std::string cpuBrand;
    std::string codeName;
    unsigned logicalProcessors = 0;
    double baseGHz = 0.0;
    int tjMaxC = 0;
    int width = 80;
    double fallbackScaleW = 60.0;
    bool csvActive = false;
    std::string csvName;
    bool ansi = false;
};

bool ParsePowerArguments(const std::vector<std::string>& args,
                         MonitorOptions& options, std::string& error);
std::string CsvHeader();
std::string CsvRow(const PowerSample& sample);
std::size_t VisibleLength(const std::string& text);
std::string RenderLogo(int width, bool ansi);
std::string RenderDashboard(const DashboardInfo& info,
                            const PowerSample& sample,
                            const std::vector<double>& history);

} // namespace pd
