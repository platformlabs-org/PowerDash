#pragma once
// PowerDashPmTable.h —— SMU PMTable 客户端(v3 Task 6;Task 10 回切驱动通道)。
// ryzenAdj nb_smu_ops.c/api.c 协议(PSMU 默认邮箱,Krackan Point 实证):
// 邮箱寄存器在 SMN 空间,经驱动 IO_CTL_SMN_READ/WRITE(Hal 0x60 地址/
// 0x64 数据窗,驱动内互斥)原子访问;表内存在 SMU 指示的物理地址,经
// IO_CTL_MMAP 映射后按 float 偏移读取。握手任一步失败 -> TryCreate 返
// nullptr(诚实降级,探针不建列、Sample 恒 NA)。实现见 PowerDashPmTable.cpp。
//
// 通道结论留档(防回归,labs-tb16g7 实证):驱动 Hal 0x60/0x64 SMN 读与
// 写均可用(0x64 写回同值测试通过,机器稳定)—— 此前"写被 pci.sys 拒"
// 系 deviceControl 输出缓冲前置门 bug 所致误诊(55ba422 已修),非 Hal
// 写过滤。用户态 ECAM 绕道(MCFG 定位 + IO_CTL_MMAP 映射 B0:D0:F0 配置
// 页,经 volatile 0x60/0x64)已删:\Device\PhysicalMemory 映射为
// WRITE-BACK 缓存,经缓存映射访问 MMIO 配置空间属未定义行为,实机三次
// 触发 bugcheck(两次 0xB8/0xBC 裸写触碰未知 DF 寄存器,一次 0x60/0x64
// 经缓存映射)。PM 表页映射不受影响:表在 DRAM(非 MMIO),缓存映射
// DRAM 安全,该路径保留。
#include "PowerDashProbe.h"
#include <memory>

namespace pd {

// Task 10:TryCreate 逐步诊断结果(--pmdump 握手失败时的取证输出)。
// failedStep 与 TryCreate 各步一一对应;字段保留到首败步为止(其后为 0)。
struct SmuHandshakeTrace {   // TryCreate 各步结果(--pmdump 诊断输出)
    int failedStep = 0;      // 0=全成;1=写自检失败;2=回读不符;3=测试消息;
                            // 4=版本消息;5=地址消息;6=地址为0;7=映射失败;
                            // 8=首次 transfer
    uint32_t argReadback = 0;     // step2 实际回读值
    uint32_t testRep = 0, versionRep = 0, addrRep = 0, transferRep = 0;
    uint32_t version = 0;
    uint64_t addr = 0;
};

class SmuPmTable {
public:
    // 邮箱寄存器(SMN 地址,经驱动 IO_CTL_SMN_READ/WRITE 原子访问;
    // 公共常量,fixture 假固件据此向假寄存器堆写应答,勿在别处重复字面量):
    static constexpr uint32_t kSmnMsg = 0x3B10a20;    // 消息寄存器(写消息号触发)
    static constexpr uint32_t kSmnRep = 0x3B10a80;    // 响应寄存器(0 = 处理中)
    static constexpr uint32_t kSmnArgs = 0x3B10a88;   // 参数区基址(args[i] @ +4*i)

    static std::unique_ptr<SmuPmTable> TryCreate(DriverIo& io);  // 失败 nullptr(诚实降级)
    static SmuHandshakeTrace Diagnose(DriverIo& io);   // 逐步执行,不构造对象、不映射(到 step7 为止的只读诊断 + step8 transfer)
    ~SmuPmTable();                        // UnmapPhys 映射窗口
    bool Refresh();                       // 每帧:transfer 0x65(拒绝→10ms 重试一次)
    float At(uint32_t byteOff) const;     // float@偏移;越界/未刷新 NAN
    bool AtBits(uint32_t byteOff, uint32_t& out) const;  // 原始 4 字节@偏移;越界/未刷新 false(--pmdump 十六进制列)
    uint32_t version() const { return version_; }
    uint64_t addr() const { return addr_; }

private:
    explicit SmuPmTable(DriverIo& io) : io_(io) {}
    // 数组引用形参(不退化为指针)。
    uint32_t SmuMsg(uint32_t msg, uint32_t (&args)[6]);   // 返回 response(0x1=OK)

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
