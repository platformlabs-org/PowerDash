#include "PowerDashUi.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace pd {

namespace {

std::string Fixed(double value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

constexpr const char* RESET = "\x1b[0m";
constexpr const char* CYAN_BRIGHT = "\x1b[38;2;103;232;249m";
constexpr const char* CYAN = "\x1b[38;2;34;211;238m";
constexpr const char* BLUE = "\x1b[38;2;14;165;233m";
constexpr const char* BORDER = "\x1b[38;2;71;85;105m";
constexpr const char* WHITE = "\x1b[38;2;248;250;252m";
constexpr const char* MUTED = "\x1b[38;2;148;163;184m";
constexpr const char* GREEN = "\x1b[38;2;34;197;94m";
constexpr const char* YELLOW = "\x1b[38;2;250;204;21m";
constexpr const char* RED = "\x1b[38;2;239;68;68m";

std::string Color(bool ansi, const char* color, const std::string& text) {
    return ansi ? std::string(color) + text + RESET : text;
}

std::string CsvEscape(const std::string& value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos)
        return value;
    std::string escaped = "\"";
    for (char ch : value) {
        if (ch == '\"') escaped += '\"';
        escaped += ch;
    }
    escaped += '\"';
    return escaped;
}

std::string Fit(const std::string& text, std::size_t width) {
    if (VisibleLength(text) <= width)
        return text + std::string(width - VisibleLength(text), ' ');

    std::string result;
    std::size_t visible = 0;
    for (std::size_t i = 0; i < text.size() && visible < width; ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == 0x1b) {
            const std::size_t begin = i;
            while (i + 1 < text.size()) {
                ++i;
                char end = text[i];
                if ((end >= 'A' && end <= 'Z') ||
                    (end >= 'a' && end <= 'z'))
                    break;
            }
            result.append(text, begin, i - begin + 1);
        } else if (ch >= 0x80) {
            const std::size_t begin = i;
            while (i + 1 < text.size() &&
                   (static_cast<unsigned char>(text[i + 1]) & 0xc0) == 0x80)
                ++i;
            result.append(text, begin, i - begin + 1);
            ++visible;
        } else {
            result += text[i];
            ++visible;
        }
    }
    if (text.find('\x1b') != std::string::npos)
        result += RESET;
    return result;
}

std::string JoinSides(const std::string& left, const std::string& right,
                      std::size_t width) {
    const std::size_t leftWidth = VisibleLength(left);
    const std::size_t rightWidth = VisibleLength(right);
    if (leftWidth + rightWidth >= width)
        return Fit(left + " " + right, width);
    return left + std::string(width - leftWidth - rightWidth, ' ') + right;
}

std::string FormatElapsed(double seconds) {
    unsigned total = seconds > 0.0 ? static_cast<unsigned>(seconds) : 0;
    unsigned hours = total / 3600;
    unsigned minutes = (total / 60) % 60;
    unsigned secs = total % 60;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hours << ':'
        << std::setw(2) << minutes << ':' << std::setw(2) << secs;
    return out.str();
}

std::string ModeColor(bool ansi, const std::string& mode) {
    if (mode.find("Performance") != std::string::npos)
        return Color(ansi, YELLOW, mode);
    if (mode.find("Quiet") != std::string::npos)
        return Color(ansi, GREEN, mode);
    if (mode.find("Intelligent") != std::string::npos)
        return Color(ansi, CYAN, mode);
    return Color(ansi, MUTED, mode);
}

std::string SeverityColor(bool ansi, double value, double warning,
                          double danger, const std::string& text) {
    if (value >= danger) return Color(ansi, RED, text);
    if (value >= warning) return Color(ansi, YELLOW, text);
    return Color(ansi, GREEN, text);
}

} // namespace

bool ParsePowerArguments(const std::vector<std::string>& args,
                         MonitorOptions& options, std::string& error) {
    options = {};
    error.clear();
    bool durationSeen = false;
    bool csvSeen = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--csv") {
            if (csvSeen) {
                error = "--csv may only be specified once";
                return false;
            }
            if (++i >= args.size() || args[i].empty()) {
                error = "--csv requires a file path";
                return false;
            }
            csvSeen = true;
            options.csvPath = args[i];
            continue;
        }
        if (arg.rfind("--", 0) == 0) {
            error = "unknown option: " + arg;
            return false;
        }
        if (durationSeen) {
            error = "power accepts at most one duration";
            return false;
        }
        char* end = nullptr;
        const double seconds = std::strtod(arg.c_str(), &end);
        if (end == arg.c_str() || *end != '\0' || !std::isfinite(seconds) ||
            seconds <= 0.0) {
            error = "duration must be a positive number";
            return false;
        }
        durationSeen = true;
        options.runSeconds = seconds;
    }
    return true;
}

std::string CsvHeader() {
    return "timestamp,elapsed_s,pkg_w,ia_w,gt_w,sys_w,pl1_w,pl2_w,"
           "temp_c,freq_ghz,c0_pct,c2_pct,c6_pct,util_pct,smi_delta,mode";
}

std::string CsvRow(const PowerSample& sample) {
    std::ostringstream out;
    out << CsvEscape(sample.timestamp)
        << ',' << Fixed(sample.elapsedSeconds, 3)
        << ',' << Fixed(sample.pkgPower, 3)
        << ',' << Fixed(sample.iaPower, 3)
        << ',' << Fixed(sample.gtPower, 3)
        << ',' << Fixed(sample.sysPower, 3)
        << ',' << Fixed(sample.pl1Watt, 3)
        << ',' << Fixed(sample.pl2Watt, 3)
        << ',';
    if (sample.tempC >= 0) out << sample.tempC;
    out << ',';
    if (sample.freqGHz > 0.0) out << Fixed(sample.freqGHz, 3);
    out << ',' << Fixed(sample.c0Pct, 3)
        << ',' << Fixed(sample.c2Pct, 3)
        << ',' << Fixed(sample.c6Pct, 3)
        << ',' << Fixed(sample.utilPct, 3)
        << ',' << sample.smiDelta
        << ',' << CsvEscape(sample.mode);
    return out.str();
}

std::size_t VisibleLength(const std::string& text) {
    std::size_t visible = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == 0x1b) {
            while (i + 1 < text.size()) {
                ++i;
                char end = text[i];
                if ((end >= 'A' && end <= 'Z') ||
                    (end >= 'a' && end <= 'z'))
                    break;
            }
        } else if (ch >= 0x80) {
            while (i + 1 < text.size() &&
                   (static_cast<unsigned char>(text[i + 1]) & 0xc0) == 0x80)
                ++i;
            ++visible;
        } else {
            ++visible;
        }
    }
    return visible;
}

std::string RenderLogo(int width, bool ansi) {
    static const char* rows[] = {
        "░█▀█░█▀█░█░█░█▀▀░█▀▄░█▀▄░█▀█░█▀▀░█░█",
        "░█▀▀░█░█░█▄█░█▀▀░█▀▄░█░█░█▀█░▀▀█░█▀█",
        "░▀░░░▀▀▀░▀░▀░▀▀▀░▀░▀░▀▀░░▀░▀░▀▀▀░▀░▀"
    };
    static const char* colors[] = {CYAN_BRIGHT, CYAN, BLUE};
    const int logoWidth = static_cast<int>(VisibleLength(rows[0]));
    width = std::max(width, logoWidth);

    std::ostringstream out;
    for (int i = 0; i < 3; ++i) {
        if (i) out << '\n';
        const int left = (width - logoWidth) / 2;
        const int right = width - logoWidth - left;
        out << std::string(left, ' ')
            << Color(ansi, colors[i], rows[i])
            << std::string(right, ' ');
    }
    return out.str();
}

std::string RenderDashboard(const DashboardInfo& info,
                            const PowerSample& sample,
                            const std::vector<double>& history) {
    const int width = std::max(72, std::min(info.width, 120));
    const std::size_t inner = static_cast<std::size_t>(width - 2);
    const bool wide = width >= 92;
    const std::string borderText = "+" + std::string(inner, '-') + "+";
    const std::string border = Color(info.ansi, BORDER, borderText);
    std::vector<std::string> lines;

    {
        std::istringstream logo(RenderLogo(width, info.ansi));
        std::string line;
        /* full-width blank rows so every line keeps the exact panel width */
        lines.push_back(std::string(static_cast<std::size_t>(width), ' '));
        while (std::getline(logo, line)) lines.push_back(line);
        lines.push_back(std::string(static_cast<std::size_t>(width), ' '));
    }

    auto row = [&](const std::string& content) {
        lines.push_back(Color(info.ansi, BORDER, "|") + Fit(content, inner) +
                        Color(info.ansi, BORDER, "|"));
    };
    auto section = [&](const std::string& title) {
        const std::string label = "-- " + title + " ";
        const std::string content = Color(info.ansi, CYAN, label) +
            Color(info.ansi, BORDER,
                  std::string(inner - std::min(inner, VisibleLength(label)), '-'));
        row(content);
    };
    const std::size_t leftWidth = (inner - 1) / 2;
    const std::size_t rightWidth = inner - 1 - leftWidth;
    auto twoColumns = [&](const std::string& left, const std::string& right) {
        row(Fit(left, leftWidth) + Color(info.ansi, BORDER, "|") +
            Fit(right, rightWidth));
    };
    auto pairedSection = [&](const std::string& left,
                             const std::string& right) {
        auto label = [&](const std::string& title, std::size_t cellWidth) {
            const std::string text = "-- " + title + " ";
            return Color(info.ansi, CYAN, text) +
                   Color(info.ansi, BORDER,
                         std::string(cellWidth -
                             std::min(cellWidth, VisibleLength(text)), '-'));
        };
        twoColumns(label(left, leftWidth), label(right, rightWidth));
    };

    lines.push_back(border);
    const std::string elapsed = FormatElapsed(sample.elapsedSeconds);
    const std::string versionMode = " v" + info.version + "  Mode: " +
                                    ModeColor(info.ansi, sample.mode);
    const std::string timing = Color(info.ansi, MUTED,
        elapsed + "  Sample 1 s ");
    std::string recording;
    if (info.csvActive) {
        recording = Color(info.ansi, RED, " ● REC CSV") +
                    (info.csvName.empty() ? "" : "  " + info.csvName);
    }
    if (wide) {
        row(JoinSides(versionMode, recording + "  " + timing, inner));
    } else {
        row(JoinSides(versionMode, timing, inner));
        if (!recording.empty()) row(recording);
    }

    std::string cpu = " " + info.cpuBrand;
    if (!info.codeName.empty()) cpu += "  [" + info.codeName + "]";
    std::string cpuFacts = std::to_string(info.logicalProcessors) + " LPs";
    if (info.baseGHz > 0.0)
        cpuFacts += "  Base " + Fixed(info.baseGHz, 2) + " GHz";
    row(JoinSides(cpu, Color(info.ansi, MUTED, cpuFacts + " "), inner));

    /* RAPL hierarchy: PSYS ("SYSTEM") = PKG + off-package platform share
     * (memory, PCH, VR losses). The two bars decompose SYSTEM so their sum
     * always equals the title value; without a PSYS reading, REST falls
     * back to the package's internal remainder (PKG - IA - GT). Both bars
     * share one scale (0 .. PL2, or the thermal-spec fallback) and the
     * legend names the scale, the PL1 marker and the identity. */
    const bool psysKnown = sample.sysPower > 0.0;
    section(psysKnown
                ? "SYSTEM POWER · " + Fixed(sample.sysPower, 2) + " W"
                : "SYSTEM POWER");
    const double scale = sample.pl2Watt > 0.0 ? sample.pl2Watt
                       : (info.fallbackScaleW > 0.0 ? info.fallbackScaleW
                                                    : 60.0);
    const std::string scaleName = sample.pl2Watt > 0.0 ? "PL2" : "spec";
    const double fraction = std::max(0.0, sample.pkgPower / scale);
    const double restPower = psysKnown
        ? std::max(0.0, sample.sysPower - sample.pkgPower)
        : std::max(0.0, sample.pkgPower - sample.iaPower - sample.gtPower);
    const double restPkgPct = sample.pkgPower > 0.0
        ? restPower / sample.pkgPower * 100.0 : 0.0;

    /* fixed fields keep both bars the same length: " PKG    "/" REST   "
     * (8), right-aligned watt value (9), trailer (11: "100% of PL2") */
    auto valueField = [](double watt) {
        std::string v = Fixed(watt, 2) + " W";
        if (v.size() < 9) v = std::string(9 - v.size(), ' ') + v;
        return v;
    };
    auto padLeft = [](const std::string& text, std::size_t field) {
        const std::size_t visible = VisibleLength(text);
        return visible < field
            ? std::string(field - visible, ' ') + text : text;
    };
    const std::size_t labelField = 8;
    const std::size_t trailerField = 11;
    const int barWidth = std::max(10, static_cast<int>(inner) -
        static_cast<int>(labelField + 9 + 2 + 2 + 2 + trailerField));
    auto powerBar = [&](double watt, const char* fillColor,
                        bool withMarker) {
        int filled = static_cast<int>(
            std::max(0.0, watt / scale) * barWidth + 0.5);
        filled = std::max(0, std::min(filled, barWidth));
        int marker = -1;
        if (withMarker) {
            marker = static_cast<int>(
                sample.pl1Watt / scale * barWidth + 0.5);
            marker = std::max(0, std::min(marker, barWidth - 1));
        }
        std::string bar = Color(info.ansi, BORDER, "[");
        for (int i = 0; i < barWidth; ++i) {
            if (i == marker)
                bar += Color(info.ansi, YELLOW, "|");
            else if (i < filled)
                bar += Color(info.ansi, fillColor, "#");
            else
                bar += Color(info.ansi, MUTED, ".");
        }
        return bar + Color(info.ansi, BORDER, "]");
    };
    const char* pkgColor = sample.pkgPower > scale ? RED
                        : (sample.pkgPower > sample.pl1Watt &&
                           sample.pl1Watt > 0.0 ? YELLOW : GREEN);
    row(" PKG    " + valueField(sample.pkgPower) + "  " +
        powerBar(sample.pkgPower, pkgColor, true) + "  " +
        padLeft(SeverityColor(info.ansi, sample.pkgPower,
            sample.pl1Watt > 0.0 ? sample.pl1Watt : scale,
            scale, Fixed(fraction * 100.0, 0) + "% of " + scaleName),
            trailerField));
    row(" REST   " + valueField(restPower) + "  " +
        powerBar(restPower, CYAN, false) + "  " +
        padLeft(psysKnown
                    ? Fixed(restPower / sample.sysPower * 100.0, 0) +
                          "% of SYS"
                    : Fixed(restPkgPct, 0) + "% of PKG",
                trailerField));
    row(JoinSides("  0 W",
        "scale to " +
            Color(info.ansi, RED, scaleName + " " + Fixed(scale, 2) + " W") +
            "  ·  " +
            Color(info.ansi, YELLOW,
                  "| = PL1 " + Fixed(sample.pl1Watt, 2) + " W") +
            (psysKnown ? "  ·  PKG + REST = SYSTEM" : "") + " ",
        inner));

    /* IA/GT are parts of PKG (share of package); SYSTEM is the PSYS
     * platform superset that contains PKG, so it is annotated with PKG's
     * share of it instead of a % of PKG. REST OF PKG lives above, as a bar */
    const auto domainPct = [&](double value) {
        return sample.pkgPower > 0.0 ? value / sample.pkgPower * 100.0 : 0.0;
    };
    auto pctOfPkg = [&](double value) {
        std::string p = Fixed(domainPct(value), 0);
        return std::string(p.size() < 3 ? 3 - p.size() : 0, ' ') + p + "% PKG";
    };
    auto domainRow = [&](const std::string& label, double watt,
                         const std::string& pct) {
        return " " + label + std::string(13 - label.size(), ' ') +
               Fixed(watt, 2) + " W  " + pct;
    };
    const std::string ia = domainRow("IA", sample.iaPower,
                                     pctOfPkg(sample.iaPower));
    const std::string gt = domainRow("GT", sample.gtPower,
                                     pctOfPkg(sample.gtPower));
    std::string sys = " SYSTEM       " + Fixed(sample.sysPower, 2) + " W";
    if (sample.sysPower > 0.0)
        sys += "  PKG " + Fixed(sample.pkgPower / sample.sysPower * 100.0, 0) +
               "% of SYS";
    const int utilFill = std::min(20, std::max(0,
        static_cast<int>(sample.utilPct / 5.0 + 0.5)));
    const std::string utilGauge = "[" + std::string(utilFill, '#') +
                                  std::string(20 - utilFill, '.') + "]";
    const std::string util = " UTIL  " + Fixed(sample.utilPct, 0) + "%  " +
        SeverityColor(info.ansi, sample.utilPct, 80.0, 95.0, utilGauge);
    const std::string tempValue = sample.tempC >= 0
        ? SeverityColor(info.ansi, sample.tempC, 80.0, 95.0,
                        std::to_string(sample.tempC) + " C")
        : Color(info.ansi, MUTED, "N/A");
    std::string temp = " TEMP  " + tempValue;
    if (info.tjMaxC > 0) temp += " / TjMax " + std::to_string(info.tjMaxC) + " C";
    const std::string freq = " FREQ  " + (sample.freqGHz > 0.0
        ? Color(info.ansi, CYAN, Fixed(sample.freqGHz, 2) + " GHz")
        : Color(info.ansi, MUTED, "N/A"));

    std::string limits1 = " PL1 " + Fixed(sample.pl1Watt, 2) + " W";
    if (!sample.pl1Window.empty()) limits1 += " / " + sample.pl1Window;
    std::string limits2 = " PL2 " + Fixed(sample.pl2Watt, 2) + " W";
    if (!sample.pl2Window.empty()) limits2 += " / " + sample.pl2Window;
    if (sample.plLocked) limits2 += "  LOCKED";
    const std::string residency = " C0 " + Fixed(sample.c0Pct, 0) +
        "%   C2 " + Fixed(sample.c2Pct, 0) + "%   C6+ " +
        Fixed(sample.c6Pct, 0) + "%";
    const std::string smi = " SMI +" + std::to_string(sample.smiDelta);

    if (wide) {
        pairedSection("POWER DOMAINS", "THERMAL / PERFORMANCE");
        twoColumns(ia, util);
        twoColumns(gt, temp);
        twoColumns(sys, freq);
        pairedSection("CPU RESIDENCY", "POWER LIMITS");
        twoColumns(residency, limits1);
        twoColumns(smi, limits2);
    } else {
        section("POWER DOMAINS");
        row(ia + "    " + gt);
        row(sys);
        section("THERMAL / PERFORMANCE");
        row(util);
        row(temp + "    " + freq);
        section("CPU RESIDENCY");
        row(residency + "    " + smi);
        section("POWER LIMITS");
        row(limits1);
        row(limits2);
    }

    section("PACKAGE HISTORY · 60 s");
    const int historyWidth = std::max(8, std::min(60, width - 15));
    const char levels[] = "._:-=+*#@";
    std::string spark;
    const std::size_t first = history.size() > static_cast<std::size_t>(historyWidth)
                            ? history.size() - historyWidth : 0;
    for (std::size_t i = first; i < history.size(); ++i) {
        int level = static_cast<int>(std::max(0.0, history[i]) / scale * 8.0 + 0.5);
        level = std::max(0, std::min(level, 8));
        spark += levels[level];
    }
    row(" 0 W  " + Color(info.ansi, CYAN, spark) + "  " +
        Fixed(scale, 0) + " W");

    double minimum = 0.0, maximum = 0.0, average = 0.0;
    if (!history.empty()) {
        const std::size_t statsFirst = history.size() > 60
                                     ? history.size() - 60 : 0;
        minimum = maximum = history[statsFirst];
        for (std::size_t i = statsFirst; i < history.size(); ++i) {
            minimum = std::min(minimum, history[i]);
            maximum = std::max(maximum, history[i]);
            average += history[i];
        }
        average /= static_cast<double>(history.size() - statsFirst);
    }
    row(JoinSides(" MIN " + Fixed(minimum, 2) + " W",
        "AVG " + Fixed(average, 2) + " W    MAX " +
        Fixed(maximum, 2) + " W ", inner));
    lines.push_back(border);

    std::ostringstream out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out << '\n';
        out << lines[i];
    }
    return out.str();
}

} // namespace pd
