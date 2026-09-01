#pragma once
// PowerDash 功率参数类型体系 v2 —— spec 第 4 节的权威定义。
// 每个物理量都是 Reading:value 为 SI 单位,valid=false 表示平台不支持
// 或本帧读取失败(UI 隐藏区块、CSV 写空单元格)。
#include <cstdint>
#include <optional>
#include <string>

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
    double baseGHz = 0.0;        // CPUID 0x16;0=未知
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
