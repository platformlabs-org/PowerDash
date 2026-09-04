#pragma once
// PowerDashPmTable.h —— SMU PMTable 客户端(v3 Task 6;Task 10 架构修正)。
// ryzenAdj nb_smu_ops.c/api.c 协议(PSMU 默认邮箱,Krackan Point 实证):
// 邮箱寄存器在 SMN 空间,经用户态 ECAM(SmnEcam:MCFG 定位 + IO_CTL_MMAP
// 映射 B0:D0:F0 配置页,0xB8 地址/0xBC 数据单窗)访问 —— 驱动
// IO_CTL_SMN_READ/WRITE(Hal 0x60/0x64)读实证可用但数据口写被拒、
// CF8/CFC 端口 I/O 本平台异常,邮箱改用户态 MMIO(ryzen_smu 内核 ECAM
// 同机制);表内存在 SMU 指示的物理地址,经 IO_CTL_MMAP 映射后按 float
// 偏移读取。握手任一步失败 -> TryCreate 返 nullptr(诚实降级,探针不建
// 列、Sample 恒 NA)。实现见 PowerDashPmTable.cpp。
#include "PowerDashProbe.h"
#include <functional>
#include <memory>

namespace pd {

// 用户态 ECAM SMN 访问(MCFG 定位 + IO_CTL_MMAP 映射,寄存器 0xB8 地址/0xBC 数据)
class SmnEcam {
public:
    static constexpr uint32_t kAddrPort = 0xB8;  // SMN 地址口(B0:D0:F0 配置空间页内偏移)
    static constexpr uint32_t kDataPort = 0xBC;  // SMN 数据口(同窗;0x60/0x64 为驱动 Hal 窗,两窗独立闩锁)
    static bool Locate(uint64_t& ecamPhysBase);   // EnumSystemFirmwareTables('ACPI') 找 MCFG,
                                                  // 解析 segment 0 / 覆盖 bus 0 的条目 BaseAddress
    // 纯解析(单测注入缓冲):MCFG = 36 字节 ACPI 头 + N×16 字节条目
    //   {u64 BaseAddress; u16 PciSegmentGroup; u8 StartBus; u8 EndBus; u32 reserved},
    //   取 segment==0 且 StartBus<=0<=EndBus(即覆盖 bus 0)条目的 BaseAddress;
    //   截断/长度不符/坏签名/无匹配 -> false(诚实)。
    static bool ParseMcfg(const uint8_t* buf, size_t len, uint64_t& base);
    bool Init(DriverIo& io);                       // 定位+映射页(ECAM 物理页,0x1000),失败 false
    bool Read(uint32_t smnAddr, uint32_t& out);    // volatile: wr 0xB8=addr; rd 0xBC
    bool Write(uint32_t smnAddr, uint32_t value);  // wr 0xB8=addr; wr 0xBC=value
    // ECAM 页内直接偏移 volatile u32 读(SmuMsg 的 args 回读路径;测试假固件
    // 在页内镜像处写应答,客户端从页回读 —— 见 SmuPmTable::msgHook)。
    bool ReadPage(uint32_t pageOff, uint32_t& out);
    ~SmnEcam();                                    // UnmapPhys
    SmnEcam() = default;
    SmnEcam(const SmnEcam&) = delete;              // 持映射窗口,禁止拷贝(双解映射)
    SmnEcam& operator=(const SmnEcam&) = delete;
private:
    DriverIo* io_ = nullptr; void* map_ = nullptr; uint64_t phys_ = 0;
};

// Locate 测试注入点(TscCalibrationOverride 同约定):非空时 Locate 直接
// 调它(单测注入固定 ECAM 基址,免依赖测试机真固件);生产恒 nullptr
// -> 走 EnumSystemFirmwareTables 真固件路径。
extern bool (*SmnEcamLocateOverride)(uint64_t& ecamPhysBase);

// SmuPmTable::msgHook 的全局默认(测试装假固件;生产恒空)。TryCreate/
// Diagnose 内部构造的对象以它初始化 msgHook —— 测试拿不到内部对象,经此
// 注入;用毕即还原(TscCalibrationOverride 同纪律)。须先于 SmuPmTable
// 声明(其 ctor 用它初始化 msgHook)。
extern std::function<void(uint32_t msg, uint32_t(&args)[6])> SmuMsgHookDefault;

// Task 10:TryCreate 逐步诊断结果(--pmdump 握手失败时的取证输出)。
// failedStep 与 TryCreate 各步一一对应;字段保留到首败步为止(其后为 0)。
// step1 兼收 ECAM 初始化失败(MCFG 定位/页映射)—— 写自检无从发生。
struct SmuHandshakeTrace {   // TryCreate 各步结果(--pmdump 诊断输出)
    int failedStep = 0;      // 0=全成;1=写自检失败(含 ECAM 初始化);2=回读不符;
                            // 3=测试消息;4=版本消息;5=地址消息;6=地址为0;7=映射失败;
                            // 8=首次 transfer
    uint32_t argReadback = 0;     // step2 实际回读值
    uint32_t testRep = 0, versionRep = 0, addrRep = 0, transferRep = 0;
    uint32_t version = 0;
    uint64_t addr = 0;
};

class SmuPmTable {
public:
    // 邮箱寄存器 kReg 名(SMN 地址;公共常量 —— fixture 假固件据此在 ECAM
    // 页镜像处写应答,勿在别处重复字面量):
    static constexpr uint32_t kSmnMsg = 0x3B10a20;    // 消息寄存器(写消息号触发)
    static constexpr uint32_t kSmnRep = 0x3B10a80;    // 响应寄存器(0 = 处理中)
    static constexpr uint32_t kSmnArgs = 0x3B10a88;   // 参数区基址(args[i] @ +4*i)
    // 页内直接偏移(ECAM 页 = 假固件的 SMN 镜像;kSmnRep/kSmnArgs 低 12 位):
    // fixture 假固件把 response 写到窗口数据口 SmnEcam::kDataPort(客户端
    // 经 0xB8/0xBC 窗口轮询)、应答 args 写到 kArgsPageOff+4*i(客户端
    // ReadPage 回读)。
    static constexpr uint32_t kRepPageOff = kSmnRep & 0xFFFu;
    static constexpr uint32_t kArgsPageOff = kSmnArgs & 0xFFFu;

    static std::unique_ptr<SmuPmTable> TryCreate(DriverIo& io);  // 失败 nullptr(诚实降级)
    static SmuHandshakeTrace Diagnose(DriverIo& io);   // 逐步执行,不构造对象、不映射 PM 表页(仅 ECAM 页 + step8 transfer)
    ~SmuPmTable();                        // UnmapPhys 映射窗口
    bool Refresh();                       // 每帧:transfer 0x65(拒绝→10ms 重试一次)
    float At(uint32_t byteOff) const;     // float@偏移;越界/未刷新 NAN
    bool AtBits(uint32_t byteOff, uint32_t& out) const;  // 原始 4 字节@偏移;越界/未刷新 false(--pmdump 十六进制列)
    uint32_t version() const { return version_; }
    uint64_t addr() const { return addr_; }

    // TEST SEAM:消息寄存器写入**之后**立即调用(生产恒空)。fixture 假
    // 固件经它扮演 SMU:把 response 写到 ECAM 窗数据口(0xBC,客户端窗口
    // 轮询可读)、把应答 args 写到页内直接偏移(kArgsPageOff+4*i,客户端
    // 从页回读)—— 客户端读写仍全走映射页(真 ECAM 机制),不经钩子
    // 取值;钩子只扮演固件。args 以引用传入(钩子可观察请求参数)。
    std::function<void(uint32_t msg, uint32_t(&args)[6])> msgHook;

private:
    explicit SmuPmTable(DriverIo& io) : msgHook(SmuMsgHookDefault), io_(io) {}
    // 数组引用形参(不退化为指针):直接传给 msgHook(std::function 的
    // uint32_t(&)[6] 签名)。
    uint32_t SmuMsg(uint32_t msg, uint32_t (&args)[6]);   // 返回 response(0x1=OK)

    DriverIo& io_;
    SmnEcam smn_;                          // 用户态 ECAM SMN 访问(邮箱唯一通道)
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
