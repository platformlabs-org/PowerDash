#pragma once
// PowerDashPmTable.h —— SMU PMTable 客户端(v3 Task 6)。
// ryzenAdj nb_smu_ops.c/api.c 协议(PSMU 默认邮箱,Krackan Point 实证):
// 邮箱寄存器在 SMN 空间,经驱动 IO_CTL_SMN_READ/WRITE 原子访问(禁止
// 用户态拆写 0x60/0x64);表内存在 SMU 指示的物理地址,经 IO_CTL_MMAP
// 映射后按 float 偏移读取。握手任一步失败 -> TryCreate 返 nullptr
// (诚实降级,探针不建列、Sample 恒 NA)。实现见 PowerDashPmTable.cpp。
#include "PowerDashProbe.h"
#include <memory>

namespace pd {

class SmuPmTable {
public:
    static std::unique_ptr<SmuPmTable> TryCreate(DriverIo& io);  // 失败 nullptr(诚实降级)
    ~SmuPmTable();                        // UnmapPhys 映射窗口
    bool Refresh();                       // 每帧:transfer 0x65(拒绝→10ms 重试一次)
    float At(uint32_t byteOff) const;     // float@偏移;越界/未刷新 NAN
    bool AtBits(uint32_t byteOff, uint32_t& out) const;  // 原始 4 字节@偏移;越界/未刷新 false(--pmdump 十六进制列)
    uint32_t version() const { return version_; }
    uint64_t addr() const { return addr_; }

private:
    explicit SmuPmTable(DriverIo& io) : io_(io) {}
    uint32_t SmuMsg(uint32_t msg, uint32_t args[6]);   // 返回 response(0x1=OK)

    DriverIo& io_;
    void* map_ = nullptr;                 // MMAP 视图(对象生存期持有)
    uint32_t version_ = 0, size_ = 0;     // size_ = 0x1000 映射窗口
    uint64_t addr_ = 0;                   // PMTable 物理地址(msg 0x66)
    bool refreshed_ = false;              // transfer 成功后 At 才出值
};

// Krackan Point PMTable 版本 0x00650005 已知偏移(ryzenAdj api.c)。
// 单位:W / A / °C(浮点),STAPM/slow 时间窗为秒。偏移仅对本版本成立;
// 其他版本 At() 只读不解读(探针按版本门控建列)。
struct KpPm {
    static constexpr uint32_t kVersion = 0x00650005;
    static constexpr uint32_t StapmLimit = 0x00, StapmValue = 0x04;
    static constexpr uint32_t FastLimit = 0x08, FastValue = 0x0C;   // FPPT/PPT FAST
    static constexpr uint32_t SlowLimit = 0x10, SlowValue = 0x14;   // SPPT/PPT SLOW
    static constexpr uint32_t ApuSlowLimit = 0x18, ApuSlowValue = 0x1C;
    static constexpr uint32_t TdcLimit = 0x30, TdcValue = 0x34;
    static constexpr uint32_t SocCurLimit = 0x38, SocCurValue = 0x3C;
    static constexpr uint32_t TctlLimit = 0x40, TctlValue = 0x44;
    static constexpr uint32_t StapmTimeS = 0x90C, SlowTimeS = 0x910;
};

} // namespace pd
