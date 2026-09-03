// PowerDashSensors.cpp —— SensorTable 宽表模型 + CSV v3 写出器(Task 2)。
// 数值单元格全部 snprintf 定点格式(不经 locale);YESNO 以 value>=0.5 判;
// invalid 一律空单元格。CsvEscape 自 PowerDashUi.cpp 复制为文件内 static
// (两处语义一致:v2 下线时原副本随之移除,本模块不对外暴露转义细节)。
#include "PowerDashSensors.h"

#include <cstdio>
#include <string>

namespace pd {
namespace {

std::string CsvEscape(const std::string& value) {   // 含逗号/引号/换行才加引号
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

std::string QuoteAlways(const std::string& value) { // 列名恒加引号(HWiNFO 全名
    std::string escaped = "\"";                     // 含 []/(),内嵌引号翻倍)
    for (char ch : value) {
        if (ch == '\"') escaped += '\"';
        escaped += ch;
    }
    escaped += '\"';
    return escaped;
}

} // namespace

unsigned SensorTable::Add(std::string key, std::string name, SensorFmt fmt) {
    cols_.push_back(SensorColumn{std::move(key), std::move(name), fmt});
    vals_.emplace_back();            // Reading 默认 NA;列/值恒等长
    return static_cast<unsigned>(cols_.size() - 1);
}

unsigned SensorTable::Count() const {
    return static_cast<unsigned>(cols_.size());
}

const SensorColumn& SensorTable::Column(unsigned i) const {
    return cols_[i];
}

void SensorTable::Set(unsigned idx, Reading r) {
    vals_[idx] = r;
}

const Reading& SensorTable::Get(unsigned idx) const {
    return vals_[idx];
}

int SensorTable::Find(const char* key) const {
    for (unsigned i = 0; i < cols_.size(); ++i)
        if (cols_[i].key == key) return static_cast<int>(i);
    return -1;
}

Reading SensorTable::Lookup(const char* key) const {
    int i = Find(key);
    return i < 0 ? NA() : vals_[i];
}

double SensorTable::LookupOr(const char* key, double fallback) const {
    int i = Find(key);
    return (i >= 0 && vals_[i].valid) ? vals_[i].value : fallback;
}

std::string FormatSensorCell(const Reading& r, SensorFmt fmt) {
    if (!r.valid) return std::string();              // invalid → 恒空单元格
    if (fmt == SensorFmt::YESNO) return r.value >= 0.5 ? "Yes" : "No";
    const char* pattern = "%.3f";                    // F3 保底,防漏 case
    switch (fmt) {
        case SensorFmt::F1:
        case SensorFmt::PCT1: pattern = "%.1f"; break;
        case SensorFmt::F2:
        case SensorFmt::RATIO2:
        case SensorFmt::PCT2: pattern = "%.2f"; break;
        case SensorFmt::F3:   pattern = "%.3f"; break;
        case SensorFmt::TEXT: pattern = "%g"; break; // 文本列占位(值仅为数值时)
        case SensorFmt::YESNO: break;                // 上面已提前返回
    }
    char text[64] = {};
    snprintf(text, sizeof(text), pattern, r.value);
    return text;
}

std::string CsvHeaderV3(const SensorTable& t) {
    std::string header = "Date,Time,\"Elapsed [s]\",\"Power Mode\"";
    for (unsigned i = 0; i < t.Count(); ++i)
        header += ',' + QuoteAlways(t.Column(i).name);
    return header;
}

std::string CsvRowV3(const SensorTable& t, const std::string& date,
                     const std::string& time, double elapsedS,
                     const std::string& mode) {
    char elapsed[32] = {};
    snprintf(elapsed, sizeof(elapsed), "%.3f", elapsedS);
    std::string row = date + ',' + time + ',' + elapsed + ',' + CsvEscape(mode);
    for (unsigned i = 0; i < t.Count(); ++i)
        row += ',' + FormatSensorCell(t.Get(i), t.Column(i).fmt);
    return row;
}

std::string FormatHwDate(unsigned d, unsigned mon, unsigned y) {
    char text[32] = {};
    snprintf(text, sizeof(text), "%u.%u.%u", d, mon, y);
    return text;
}

std::string FormatHwTime(unsigned h, unsigned m, unsigned s, unsigned ms) {
    char text[32] = {};
    snprintf(text, sizeof(text), "%u:%02u:%02u.%03u", h, m, s, ms);
    return text;
}

} // namespace pd
