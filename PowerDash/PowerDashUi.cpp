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

/* RAPL tau presentation, identical to the original PowerDash.cpp fmtTau:
 * >= 1 s -> "x.xx s", >= 1 ms -> "x.xx ms", else "x.xx us". The
 * rawTau == 0 -> "n/a" case is expressed by an invalid Reading at the
 * call site (window text then stays "n/a"). */
std::string FormatWindow(double seconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    if (seconds >= 1.0)
        out << seconds << " s";
    else if (seconds >= 0.001)
        out << seconds * 1e3 << " ms";
    else
        out << seconds * 1e6 << " us";
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
    return "timestamp,elapsed_s,platform,pkg_w,cores_w,gfx_w,platform_w,"
           "limit_sustained_w,limit_sustained_window_s,limit_burst_w,limit_locked,"
           "tdc_a,edc_a,temp_c,freq_ghz,util_pct,c0_pct,c2_pct,c6_pct,smi_delta,mode";
}

/* CSV v2: invalid Reading -> EMPTY cell (never "0"), limit_locked 0/1,
 * platform in {intel, amd} (spec section 6). */
std::string CsvRow(Vendor vendor, const Sample& sample) {
    auto cell = [](const Reading& r, int precision) {
        return r.valid ? Fixed(r.value, precision) : std::string();
    };
    std::ostringstream out;
    out << CsvEscape(sample.timestamp)
        << ',' << Fixed(sample.elapsedS, 3)
        << ',' << (vendor == Vendor::Amd ? "amd" : "intel")
        << ',' << cell(sample.pkgW, 3)
        << ',' << cell(sample.coresW, 3)
        << ',' << cell(sample.gfxW, 3)
        << ',' << cell(sample.platformW, 3)
        << ',' << cell(sample.powerLimit.sustainedW, 3)
        << ',' << cell(sample.powerLimit.sustainedWindowS, 3)
        << ',' << cell(sample.powerLimit.burstW, 3)
        << ',' << (sample.powerLimit.locked ? 1 : 0)
        << ',' << cell(sample.currentLimit.tdcA, 3)
        << ',' << cell(sample.currentLimit.edcA, 3)
        << ',' << cell(sample.tempC, 0)
        << ',' << cell(sample.freqGHz, 3)
        << ',' << cell(sample.utilPct, 3)
        << ',' << cell(sample.c0Pct, 3)
        << ',' << cell(sample.c2Pct, 3)
        << ',' << cell(sample.c6Pct, 3)
        << ',';
    if (sample.smiDelta.has_value()) out << *sample.smiDelta;
    out << ',' << CsvEscape(sample.mode);
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
                            const PlatformCaps& caps,
                            const Sample& sample,
                            const std::vector<double>& history) {
    const bool intel = caps.vendor != Vendor::Amd;
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
    const std::string elapsed = FormatElapsed(sample.elapsedS);
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
    std::string cpuFacts = std::to_string(caps.logicalProcessors) + " LPs";
    if (caps.baseGHz > 0.0)
        cpuFacts += "  Base " + Fixed(caps.baseGHz, 2) + " GHz";
    row(JoinSides(cpu, Color(info.ansi, MUTED, cpuFacts + " "), inner));

    /* Power hierarchy via Decompose (spec section 5): the two bars decompose
     * the title total so their sum always equals it - Intel PSYS
     * ("SYSTEM" = PKG + off-package platform share) or, without a platform
     * reading, the package's internal remainder (PKG - CORES - GFX). Both
     * bars share one scale (0 .. burst limit, or the budget/spec fallback)
     * and the legend names the scale, the sustained marker and the identity. */
    const Decomposition d = Decompose(sample, caps);
    section(d.totalW.valid
                ? d.title + " · " + Fixed(d.totalW.value, 2) + " W"
                : (caps.platformPower ? "SYSTEM POWER" : "PACKAGE POWER"));
    const bool sysDecomposition = caps.platformPower &&
        sample.platformW.valid && sample.pkgW.valid;
    const double mainPower = d.mainW.valid ? d.mainW.value : 0.0;
    const double restPower = d.restW.valid ? d.restW.value : 0.0;
    /* burst == 0 (register reads zero) keeps the v1 "not known" handling:
     * fall to the probe budget or the 60 W spec default */
    const bool burstKnown = sample.powerLimit.burstW.valid &&
                            sample.powerLimit.burstW.value > 0.0;
    const double scale = burstKnown ? sample.powerLimit.burstW.value
                       : (caps.budgetW > 0.0 ? caps.budgetW : 60.0);
    const std::string scaleName = burstKnown
        ? (intel ? "PL2" : "FPPT") : std::string("spec");
    const double sustainedW = sample.powerLimit.sustainedW.valid
        ? sample.powerLimit.sustainedW.value : 0.0;
    const double fraction = std::max(0.0, mainPower / scale);
    const double restPkgPct = mainPower > 0.0
        ? restPower / mainPower * 100.0 : 0.0;

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
                sustainedW / scale * barWidth + 0.5);
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
    const char* pkgColor = mainPower > scale ? RED
                        : (mainPower > sustainedW &&
                           sustainedW > 0.0 ? YELLOW : GREEN);
    /* the sustained-limit marker is only drawn when the reading is valid -
     * otherwise (every AMD frame) a stray '|' would sit at bar column 0
     * while the legend omits the marker text */
    row(" PKG    " + valueField(mainPower) + "  " +
        powerBar(mainPower, pkgColor, sample.powerLimit.sustainedW.valid) + "  " +
        padLeft(SeverityColor(info.ansi, mainPower,
            sustainedW > 0.0 ? sustainedW : scale,
            scale, Fixed(fraction * 100.0, 0) + "% of " + scaleName),
            trailerField));
    row(" REST   " + valueField(restPower) + "  " +
        powerBar(restPower, CYAN, false) + "  " +
        padLeft(sysDecomposition && d.totalW.value > 0.0
                    ? Fixed(restPower / d.totalW.value * 100.0, 0) +
                          "% of SYS"
                    : Fixed(restPkgPct, 0) + "% of PKG",
                trailerField));
    row(JoinSides("  0 W",
        "scale to " +
            Color(info.ansi, RED, scaleName + " " + Fixed(scale, 2) + " W") +
            (sample.powerLimit.sustainedW.valid
                ? "  ·  " + Color(info.ansi, YELLOW,
                      "| = " + std::string(intel ? "PL1" : "PPT") + " " +
                      Fixed(sustainedW, 2) + " W")
                : "") +
            (!d.identity.empty() ? "  ·  " + d.identity : "") + " ",
        inner));

    /* CORES/GFX are parts of PKG (share of package); SYSTEM is the platform
     * superset that contains PKG, so it is annotated with PKG's share of it
     * instead of a % of PKG. REST OF PKG lives above, as a bar. Rows for
     * unsupported/unreadable domains are hidden entirely (spec rule 3). */
    const double pkgBase = sample.pkgW.valid ? sample.pkgW.value : 0.0;
    const auto domainPct = [&](double value) {
        return pkgBase > 0.0 ? value / pkgBase * 100.0 : 0.0;
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
    const bool showCores = sample.coresW.valid;
    const bool showGfx = caps.gfxPower && sample.gfxW.valid;
    const bool showSystem = caps.platformPower && sample.platformW.valid;
    const std::string ia = domainRow("IA", sample.coresW.value,
                                     pctOfPkg(sample.coresW.value));
    const std::string gt = domainRow("GT", sample.gfxW.value,
                                     pctOfPkg(sample.gfxW.value));
    std::string sys = " SYSTEM       " + Fixed(sample.platformW.value, 2) + " W";
    if (showSystem)
        sys += "  PKG " + Fixed(pkgBase / sample.platformW.value * 100.0, 0) +
               "% of SYS";
    const double utilPct = sample.utilPct.valid ? sample.utilPct.value : 0.0;
    const int utilFill = std::min(20, std::max(0,
        static_cast<int>(utilPct / 5.0 + 0.5)));
    const std::string utilGauge = "[" + std::string(utilFill, '#') +
                                  std::string(20 - utilFill, '.') + "]";
    const std::string util = " UTIL  " + Fixed(utilPct, 0) + "%  " +
        SeverityColor(info.ansi, utilPct, 80.0, 95.0, utilGauge);
    const std::string tempValue = sample.tempC.valid
        ? SeverityColor(info.ansi, sample.tempC.value, 80.0, 95.0,
                        Fixed(sample.tempC.value, 0) + " C")
        : Color(info.ansi, MUTED, "N/A");
    std::string temp = " TEMP  " + tempValue;
    if (caps.tjMaxC > 0) temp += " / TjMax " + std::to_string(caps.tjMaxC) + " C";
    const std::string freq = " FREQ  " + (sample.freqGHz.valid
        ? Color(info.ansi, CYAN, Fixed(sample.freqGHz.value, 2) + " GHz")
        : Color(info.ansi, MUTED, "N/A"));

    /* POWER LIMITS: Intel two rows (PL1/PL2 + tau + LOCKED); AMD three rows
     * (PPT/FPPT, TDC, EDC) with explicit inline units (spec rule 4). The
     * sustained tau stays "n/a" when the window Reading is invalid, exactly
     * like the original fmtTau(0). */
    std::vector<std::string> limitRows;
    if (caps.powerLimits) {
        const std::string tauText =
            sample.powerLimit.sustainedWindowS.valid
                ? FormatWindow(sample.powerLimit.sustainedWindowS.value)
                : std::string("n/a");
        const double burstW = sample.powerLimit.burstW.valid
            ? sample.powerLimit.burstW.value : 0.0;
        if (intel) {
            limitRows.push_back(
                " PL1 " + Fixed(sustainedW, 2) + " W / " + tauText);
            limitRows.push_back(
                " PL2 " + Fixed(burstW, 2) + " W" +
                (sample.powerLimit.locked ? "  LOCKED" : ""));
        } else {
            std::string ppt = " PPT " + Fixed(sustainedW, 2) + " W";
            if (sample.powerLimit.burstW.valid)
                ppt += "  FPPT " + Fixed(burstW, 2) + " W";
            limitRows.push_back(ppt);
            if (sample.currentLimit.tdcA.valid)
                limitRows.push_back(
                    " TDC " + Fixed(sample.currentLimit.tdcA.value, 2) + " A");
            if (sample.currentLimit.edcA.valid)
                limitRows.push_back(
                    " EDC " + Fixed(sample.currentLimit.edcA.value, 2) + " A");
        }
    }

    const std::string residency = " C0 " +
        Fixed(sample.c0Pct.valid ? sample.c0Pct.value : 0.0, 0) +
        "%   C2 " + Fixed(sample.c2Pct.valid ? sample.c2Pct.value : 0.0, 0) +
        "%   C6+ " +
        Fixed(sample.c6Pct.valid ? sample.c6Pct.value : 0.0, 0) + "%";
    const std::string smi = " SMI +" +
        std::to_string(sample.smiDelta.value_or(0));

    if (wide) {
        pairedSection("POWER DOMAINS", "THERMAL / PERFORMANCE");
        const std::vector<std::string> domains = {
            showCores ? ia : std::string(), showGfx ? gt : std::string(),
            showSystem ? sys : std::string()};
        const std::vector<std::string> thermo = {util, temp, freq};
        std::size_t pair = 0;
        for (; pair < domains.size() && pair < thermo.size(); ++pair)
            if (domains[pair].empty()) row(thermo[pair]);
            else twoColumns(domains[pair], thermo[pair]);
        for (; pair < domains.size(); ++pair)
            if (!domains[pair].empty()) row(domains[pair]);
        for (; pair < thermo.size(); ++pair) row(thermo[pair]);

        /* paired section degrades to a single full-width section when the
         * platform has no residency metrics (AMD): limits render alone */
        const std::vector<std::string> resLeft = {
            caps.residency ? residency : std::string(),
            caps.smi && sample.smiDelta.has_value() ? smi : std::string()};
        if (!resLeft[0].empty() || !resLeft[1].empty()) {
            if (limitRows.empty()) {
                section("CPU RESIDENCY");
            } else {
                pairedSection("CPU RESIDENCY", "POWER LIMITS");
            }
        } else if (!limitRows.empty()) {
            section("POWER LIMITS");
        }
        std::size_t limitPair = 0;
        for (const std::string& left : resLeft) {
            if (left.empty()) continue;
            if (limitPair < limitRows.size()) {
                twoColumns(left, limitRows[limitPair++]);
            } else {
                row(left);
            }
        }
        for (; limitPair < limitRows.size(); ++limitPair)
            row(limitRows[limitPair]);
    } else {
        section("POWER DOMAINS");
        if (showCores && showGfx) row(ia + "    " + gt);
        else if (showCores) row(ia);
        else if (showGfx) row(gt);
        if (showSystem) row(sys);
        section("THERMAL / PERFORMANCE");
        row(util);
        row(temp + "    " + freq);
        if (caps.residency) {
            section("CPU RESIDENCY");
            row(caps.smi && sample.smiDelta.has_value()
                    ? residency + "    " + smi : residency);
        }
        if (!limitRows.empty()) {
            section("POWER LIMITS");
            for (const std::string& limit : limitRows) row(limit);
        }
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
