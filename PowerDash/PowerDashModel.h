#pragma once
// PowerDash 功率参数类型体系 v2 —— spec 第 4 节的权威定义。
// 每个物理量都是 Reading:value 为 SI 单位,valid=false 表示平台不支持
// 或本帧读取失败(UI 隐藏区块、CSV 写空单元格)。
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pd {

enum class Vendor { Intel, Amd };

struct Reading {
    double value = 0.0;   // W / A / 摄氏度 / GHz / %
    bool valid = false;
};

inline Reading Ok(double v) { Reading r; r.value = v; r.valid = true; return r; }
inline Reading NA() { return Reading{}; }

struct PowerLimit {              // 功率类限值 (W),跨平台统一
    Reading sustainedW;          // Intel PL1 <-> AMD PPT
    Reading burstW;              // Intel PL2 <-> AMD FPPT(无则 invalid)
    Reading sustainedWindowS;    // tau,秒
    bool locked = false;
};

struct CurrentLimit {            // 电流类限值 (A),AMD 专属
    Reading tdcA;
    Reading edcA;
};

struct Sample {
    std::string timestamp;
    double elapsedS = 0.0;
    Reading pkgW, coresW, gfxW, platformW;
    PowerLimit powerLimit;
    CurrentLimit currentLimit;
    Reading tempC, freqGHz, utilPct;
    Reading c0Pct, c2Pct, c6Pct;                 // 驻留率(Intel 专属)
    std::optional<std::uint64_t> smiDelta;       // Intel 专属
    std::string mode = "n/a";
};

struct PlatformCaps {
    Vendor vendor = Vendor::Intel;
    std::string cpuName;
    bool gfxPower = false;
    bool platformPower = false;
    bool powerLimits = false;
    bool residency = false;
    bool smi = false;
    double budgetW = 0.0;        // 预算刻度(PL2/FPPT);0=未知,UI 走 spec fallback
    int tjMaxC = 0;
    double baseGHz = 0.0;
    unsigned logicalProcessors = 0;
};

struct PlatformInfo {            // CPUID 静态信息,入口层计算后交给工厂
    Vendor vendor = Vendor::Intel;
    std::string cpuName;         // "brand  [codename]"
    unsigned logicalProcessors = 0;
    unsigned physicalCores = 0;  // GetLogicalProcessorInformation 统计;0=未知
                                 // (AMD 0xC001029A 按物理核计数,SMT 兄弟 LP
                                 //  共享同一计数器,遍历需去重;0 时退回 nLP)
    std::vector<unsigned> coreLPs;  // 每个物理核一个代表 LP(其 mask 最低
                                    // 置位位;Windows SMT 兄弟编号相邻,如
                                    // 8C/16T 为 {0,1}{2,3}…,代表集 =
                                    // {0,2,4,6,8,10,12,14})。空 = 拓扑未知,
                                    // 探针退回全 LP 遍历(旧保底行为)。
    double baseGHz = 0.0;        // CPUID 0x16;0=未知
    unsigned family = 0;         // CPUID family(AMD 探针按世代分 P-state/
                                 // SMN 解码;Intel 不消费)
};

struct Decomposition {
    std::string title;           // 顶区标题(不带数值,渲染层拼)
    Reading totalW;              // 标题总值
    std::string mainLabel = "PKG";
    Reading mainW;
    std::string restLabel = "REST";
    Reading restW;
    std::string identity;        // 图例恒等式
};

inline Decomposition Decompose(const Sample& s, const PlatformCaps& c) {
    Decomposition d;
    if (c.platformPower && s.platformW.valid && s.pkgW.valid) {
        d.title = "SYSTEM POWER";
        d.totalW = s.platformW;
        d.mainW = s.pkgW;
        d.restW = Ok(s.platformW.value - s.pkgW.value);
        if (d.restW.value < 0) d.restW = Ok(0);
        d.identity = "PKG + REST = SYSTEM";
    } else if (s.pkgW.valid) {
        d.title = "PACKAGE POWER";
        d.totalW = s.pkgW;
        d.mainW = s.pkgW;
        double rest = s.pkgW.value;
        if (s.coresW.valid) rest -= s.coresW.value;
        if (s.gfxW.valid) rest -= s.gfxW.value;
        d.restW = Ok(rest < 0 ? 0 : rest);
        d.identity = "CORES + GFX + REST = PKG";
    }
    return d;
}

} // namespace pd
