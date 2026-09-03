#pragma once
// PowerDashSensors.h —— v3 宽表数据模型 + CSV v3 写出器。
// SensorTable 是 Task 4/5/7 的主数据模型:构造期按 HWiNFO 全名列出全部
// 传感器列(Add 定表),每帧 Set 填值;CSV v3 以 Date/Time/Elapsed/Power
// Mode 四前缀列对齐 HWiNFO 导出格式,invalid 读数一律写空单元格。
#include "PowerDashModel.h"
#include <string>
#include <vector>

namespace pd {

enum class SensorFmt { F1, F2, F3, RATIO2, PCT1, PCT2, YESNO, TEXT };

struct SensorColumn {
    std::string key;    // 代码内稳定键,如 "core.3.clock"(点分层,平台探针自定)
    std::string name;   // CSV 列名 = HWiNFO 全名,如 "E-core 3 Clock [MHz]"
    SensorFmt fmt;
};

class SensorTable {
public:
    unsigned Add(std::string key, std::string name, SensorFmt fmt);  // 构造期调用
    unsigned Count() const;
    const SensorColumn& Column(unsigned i) const;
    void Set(unsigned idx, Reading r);                               // 每帧
    void SetInvalid(unsigned idx) { Set(idx, NA()); }
    const Reading& Get(unsigned idx) const;
    int Find(const char* key) const;                                 // -1 = 无此列

    // Sample 派生:键缺失或 invalid → out NA(渲染层按 caps 降级)
    Reading Lookup(const char* key) const;
    double LookupOr(const char* key, double fallback) const;

private:
    std::vector<SensorColumn> cols_;
    std::vector<Reading> vals_;
};

// CSV v3(HWiNFO 对齐)。Date/Time/Elapsed/Mode 由调用方生成(入口层)。
std::string FormatSensorCell(const Reading& r, SensorFmt fmt);       // 单元格:invalid→""
std::string CsvHeaderV3(const SensorTable& t);
std::string CsvRowV3(const SensorTable& t, const std::string& date,
                     const std::string& time, double elapsedS,
                     const std::string& mode);

// HWiNFO 观测格式:d.m.yyyy / h:mm:ss.fff(日期与小时不补零)。三无符号
// 参数避免本模块引入 windows.h(SYSTEMTIME 由入口层拆开传入)。
std::string FormatHwDate(unsigned d, unsigned mon, unsigned y);
std::string FormatHwTime(unsigned h, unsigned m, unsigned s, unsigned ms);

} // namespace pd
