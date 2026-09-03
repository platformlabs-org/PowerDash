#pragma once
// PowerDashUi.h —— 面板渲染与参数解析,只消费统一模型(Sample v2 +
// PlatformCaps,见 PowerDashModel.h)。平台差异经 caps 控制区块显隐,
// 渲染层不出现任何 MSR/平台寄存器概念(spec 第 2/5 节)。CSV 写出走
// PowerDashSensors.h 的 v3 写出器(Task 7 起 v2 CsvHeader/CsvRow 已删)。
#include <cstddef>
#include <string>
#include <vector>

#include "PowerDashModel.h"

namespace pd {

struct MonitorOptions {
    double runSeconds = -1.0;
    std::string csvPath;
};

struct DashboardInfo {             // 平台无关的展示上下文
    std::string version;
    std::string cpuBrand;
    std::string codeName;
    int width = 80;
    bool csvActive = false;
    std::string csvName;
    bool ansi = false;
};

bool ParsePowerArguments(const std::vector<std::string>& args,
                         MonitorOptions& options, std::string& error);
std::size_t VisibleLength(const std::string& text);
std::string RenderLogo(int width, bool ansi);
std::string RenderDashboard(const DashboardInfo& info,
                            const PlatformCaps& caps,
                            const Sample& sample,
                            const std::vector<double>& history);

} // namespace pd
