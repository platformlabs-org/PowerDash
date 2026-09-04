// PowerDashPmTable.cpp —— SMU PSMU 邮箱 PMTable 客户端实现(v3 Task 6;
// Task 10 架构修正:邮箱访问改用户态 ECAM)。
// 协议逐条对齐 ryzenAdj nb_smu_ops.c/api.c(上游核对;Krackan Point 走
// PSMU 默认邮箱):
//   寄存器    msg 0x3B10a20 / response 0x3B10a80 / args 0x3B10a88(+4*i,
//             i = 0..5)
//   通道      用户态 ECAM(SmnEcam):MCFG 定位 B0:D0:F0 配置页物理基址,
//             IO_CTL_MMAP 映射,0xB8 地址口/0xBC 数据口单窗 volatile 访问
//             —— 驱动 IO_CTL_SMN_READ/WRITE 的 Hal 0x60/0x64 读实证可用
//             但数据口写经 pci.sys 被拒,CF8/CFC 端口 I/O 本平台异常
//             (Krackan 三轮实机;Linux ryzen_smu 即 ECAM 机制)。
//   response  0x1 OK;其余非零一律按失败(0xFF 失败 / 0xFE 未知命令 /
//             0x80 前置条件拒绝 / 0x81 忙拒绝)
//   握手      (a) arg0 写 0x47 回读自检(不符 = ECAM 窗不可写,失败)
//             (b) 测试消息 0x1,response 须 0x1
//             (c) 消息 0x6 -> args[0] = 表版本(Krackan 0x00650005;
//                 其他版本继续握手但偏移不解读,由探针按版本门控)
//             (d) 消息 0x66 -> 表物理地址 = args[1]<<32 | args[0]
//             (e) 消息 0x65 transfer:0x80(前置拒绝)-> Sleep(10) 重试
//                 一次,仍失败即放弃;OK 后表内存即新鲜浮点
//   轮询      用户态有界忙等(≤1e6 次窗口读,无 sleep —— 总时长不得
//             卡 1 s 采样环),超时按 response 0 处理(调用方视为失败)
//   args 回读 SmuMsg 轮询命中后仍经 0xB8/0xBC 窗口逐字读 kSmnArgs+4*i
//             (邮箱寄存器是 SMN 地址,不是 ECAM 配置页偏移 —— 全路径
//             无页偏移捷径;测试假固件经 msgHook 把应答写进假寄存器堆,
//             客户端从窗口回读而非经钩子取值)
//   映射      表首所在页对齐,窗口 0x1000 字节(+ 页内偏移余量):
//             mapSize = 0x1000 + (addr & 0xFFF)、base = addr & ~0xFFF;
//             对象生存期持有,析构 UnmapPhys。At(byteOff) 读映射内
//             (addr & 0xFFF) + byteOff 处 4 字节 memcpy 位转换为 float
//             (避开 strict-aliasing UB);byteOff + 4 > 0x1000 或未刷新
//             -> NaN。
#include "PowerDashPmTable.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <vector>

namespace pd {

// 测试注入点(生产:Locate/Access 覆盖恒 nullptr / 钩子默认恒空)。
bool (*SmnEcamLocateOverride)(uint64_t&) = nullptr;
std::function<bool(bool, uint32_t, uint32_t&)> SmnEcamAccessOverride = nullptr;
std::function<void(uint32_t, uint32_t(&)[6])> SmuMsgHookDefault = nullptr;

namespace {
constexpr uint32_t kRepOk = 0x1;               // REP_MSG_OK
constexpr uint32_t kRepRejectedPrereq = 0x80;  // transfer 前置拒绝(重试一次)
constexpr uint32_t kMsgTest = 0x1;             // 握手自测消息
constexpr uint32_t kMsgVersion = 0x6;          // 取表版本 -> args[0]
constexpr uint32_t kMsgAddr = 0x66;            // 取表地址 -> args[0]/args[1]
constexpr uint32_t kMsgTransfer = 0x65;        // 传输(OK 后表内存新鲜)
constexpr uint32_t kArgSelftest = 0x47;        // SMN 窗可写自检魔数
constexpr uint32_t kWindow = 0x1000;           // 映射窗口(表大小上界)
constexpr uint32_t kPollLimit = 1000000u;      // 响应轮询上界(有界忙等)
constexpr uint32_t kArgCount = 6;
} // namespace

// ---- SmnEcam:用户态 ECAM SMN 访问 ----

/* MCFG 纯解析:36 字节 ACPI 头 + N×16 字节条目。条目布局(PCI Firmware
 * Spec MCFG):
 *   +0  u64 BaseAddress      该段 ECAM 基址(增强配置访问机制)
 *   +8  u16 PciSegmentGroup  PCI 段组号
 *   +10 u8  StartBus         覆盖起始总线号(含)
 *   +11 u8  EndBus           覆盖结束总线号(含)
 *   +12 u32 _reserved
 * 取 segment==0 且覆盖 bus 0(StartBus<=0<=EndBus;u8 即 StartBus==0)
 * 的条目 BaseAddress;截断/长度不符/坏签名/无匹配 -> false。 */
bool SmnEcam::ParseMcfg(const uint8_t* buf, size_t len, uint64_t& base) {
    base = 0;
    if (buf == nullptr || len < 36) return false;                 // 头截断
    if (std::memcmp(buf, "MCFG", 4) != 0) return false;           // 签字
    uint32_t tableLen = 0;
    std::memcpy(&tableLen, buf + 4, 4);                           // ACPI Length @ +4
    if (tableLen != static_cast<uint32_t>(len)) return false;     // 长度不符
    if ((len - 36) % 16 != 0) return false;                       // 条目截断
    const size_t entries = (len - 36) / 16;
    for (size_t i = 0; i < entries; ++i) {
        const uint8_t* e = buf + 36 + i * 16;
        uint16_t seg = 0;
        std::memcpy(&seg, e + 8, 2);
        if (seg != 0) continue;                                   // 非 segment 0
        if (e[10] > 0) continue;          // StartBus>0:u8 下覆盖 bus 0 即 StartBus==0
        std::memcpy(&base, e, 8);
        return true;
    }
    return false;                         // 无覆盖 bus 0 的 segment 0 条目
}

/* 定位 ECAM 基址:EnumSystemFirmwareTables('ACPI') 枚举固件表 id,取
 * 'MCFG' 缓冲走纯解析。API 动态解析(老内核无此导出)-> nullptr 即诚实
 * false;无 MCFG / 无匹配条目同样 false(调用方 Init 失败降级)。 */
bool SmnEcam::Locate(uint64_t& ecamPhysBase) {
    ecamPhysBase = 0;
    if (SmnEcamLocateOverride) return SmnEcamLocateOverride(ecamPhysBase);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) return false;
    using EnumFn = UINT(WINAPI*)(DWORD, void*, DWORD);
    using GetFn = UINT(WINAPI*)(DWORD, DWORD, void*, DWORD);
    auto enumFn = reinterpret_cast<EnumFn>(GetProcAddress(k32, "EnumSystemFirmwareTables"));
    auto getFn = reinterpret_cast<GetFn>(GetProcAddress(k32, "GetSystemFirmwareTable"));
    if (enumFn == nullptr || getFn == nullptr) return false;      // API 不可用
    const DWORD acpi = (DWORD)'A' | ((DWORD)'C' << 8) | ((DWORD)'P' << 16) |
                       ((DWORD)'I' << 24);
    const UINT needed = enumFn(acpi, nullptr, 0);
    if (needed == 0 || needed % 4 != 0) return false;             // 无表/id 非法
    std::vector<uint8_t> ids(needed);
    if (enumFn(acpi, ids.data(), needed) != needed) return false;
    for (UINT off = 0; off + 4 <= needed; off += 4) {
        DWORD id = 0;
        std::memcpy(&id, ids.data() + off, 4);
        const char* c = reinterpret_cast<const char*>(&id);
        if (c[0] != 'M' || c[1] != 'C' || c[2] != 'F' || c[3] != 'G') continue;
        const UINT size = getFn(acpi, id, nullptr, 0);
        if (size == 0) continue;
        std::vector<uint8_t> table(size);
        if (getFn(acpi, id, table.data(), size) != size) continue;
        uint64_t base = 0;
        if (ParseMcfg(table.data(), table.size(), base)) {
            ecamPhysBase = base;                                  // B0:D0:F0 页内偏移 = 0(bus0/dev0/fn0)
            return true;
        }
    }
    return false;
}

/* 定位 + 映射 B0:D0:F0 所在 ECAM 页(0x1000 + 页内未对齐余量;MCFG 基址
 * 常为 MB 对齐,余量通常 0 —— 仍统一计算)。失败 false(io_/map_ 不落)。 */
bool SmnEcam::Init(DriverIo& io) {
    uint64_t phys = 0;
    if (!Locate(phys)) return false;
    const uint64_t page = phys & ~0xFFFull;
    const size_t len = 0x1000 + static_cast<size_t>(phys & 0xFFFull);
    void* virt = nullptr;
    if (!io.MapPhys(page, len, virt) || virt == nullptr) return false;
    io_ = &io;
    phys_ = phys;
    map_ = virt;
    return true;
}

/* SMN 写:0xB8 口闩 SMN 地址 -> 0xBC 口写数据(volatile,驱动 IO_CTL_MMAP
 * 映射的 MMIO;页内偏移 = (phys_ & 0xFFF) + reg,页对齐时前者为 0)。
 * 测试覆盖非空时整体改道假寄存器堆(生产恒 nullptr,volatile 路径原样)。 */
bool SmnEcam::Write(uint32_t smnAddr, uint32_t value) {
    if (SmnEcamAccessOverride) return SmnEcamAccessOverride(true, smnAddr, value);
    if (map_ == nullptr) return false;
    auto* p = static_cast<volatile uint8_t*>(map_) + (phys_ & 0xFFFull);
    *reinterpret_cast<volatile uint32_t*>(p + kAddrPort) = smnAddr;
    *reinterpret_cast<volatile uint32_t*>(p + kDataPort) = value;
    return true;
}

/* SMN 读:0xB8 口闩 SMN 地址 -> 0xBC 口读数据。 */
bool SmnEcam::Read(uint32_t smnAddr, uint32_t& out) {
    if (SmnEcamAccessOverride) return SmnEcamAccessOverride(false, smnAddr, out);
    out = 0;
    if (map_ == nullptr) return false;
    auto* p = static_cast<volatile uint8_t*>(map_) + (phys_ & 0xFFFull);
    *reinterpret_cast<volatile uint32_t*>(p + kAddrPort) = smnAddr;
    out = *reinterpret_cast<volatile uint32_t*>(p + kDataPort);
    return true;
}

SmnEcam::~SmnEcam() {
    if (map_ != nullptr && io_ != nullptr) io_->UnmapPhys(map_);
}

// ---- SmuPmTable ----

SmuPmTable::~SmuPmTable() {
    if (map_) io_.UnmapPhys(map_);
}

/* 单条 SMU 消息:清 response -> 写 args[0..5] -> 写 msg(均经 0xB8/0xBC
 * 窗口,即实机触发 SMU 的真写路径)-> msgHook(测试 seam:假固件此刻把
 * response/应答 args 写进寄存器堆)-> 窗口轮询 response 至非零(≤1e6
 * 次,超时返回 0)-> 窗口逐字回读 args[0..5]。
 * 任一步窗口访问失败同样返回 0(调用方只认 0x1 = OK)。 */
uint32_t SmuPmTable::SmuMsg(uint32_t msg, uint32_t (&args)[kArgCount]) {
    if (!smn_.Write(kSmnRep, 0)) return 0;
    for (uint32_t i = 0; i < kArgCount; ++i)
        if (!smn_.Write(kSmnArgs + 4u * i, args[i])) return 0;
    if (!smn_.Write(kSmnMsg, msg)) return 0;
    if (msgHook) msgHook(msg, args);
    uint32_t rep = 0;
    for (uint32_t n = 0; n < kPollLimit; ++n) {
        if (!smn_.Read(kSmnRep, rep)) return 0;
        if (rep != 0) break;
    }
    if (rep == 0) return 0;                    // 超时:SMU 未应答
    for (uint32_t i = 0; i < kArgCount; ++i)
        if (!smn_.Read(kSmnArgs + 4u * i, args[i])) return 0;
    return rep;
}

/* 每帧刷新:transfer 0x65。0x80(前置拒绝,如上一帧传输尚未完成)->
 * Sleep(10) 重试一次,再失败即放弃(false,本帧 PM 列全 NA);其余非
 * OK 码直接失败。成功后表内存即本帧新鲜浮点(At 解禁)。 */
bool SmuPmTable::Refresh() {
    uint32_t args[kArgCount] = {};
    uint32_t rep = SmuMsg(kMsgTransfer, args);
    if (rep == kRepRejectedPrereq) {
        Sleep(10);
        rep = SmuMsg(kMsgTransfer, args);
    }
    if (rep != kRepOk) return false;
    refreshed_ = true;
    return true;
}

/* 表内 float 读取:越界(byteOff + 4 > 窗口)/未刷新/未映射 -> NaN。 */
float SmuPmTable::At(uint32_t byteOff) const {
    if (map_ == nullptr || !refreshed_) return std::nanf("");
    if (size_ < 4 || byteOff > size_ - 4u) return std::nanf("");
    const auto* p = static_cast<const uint8_t*>(map_) +
                    (addr_ & 0xFFFull) + byteOff;
    float f = 0.0f;
    std::memcpy(&f, p, 4);                     // 4 字节位转换(无别名 UB)
    return f;
}

/* 表内原始 4 字节读取(--pmdump 十六进制列):与 At 同一越界/刷新/映射
 * 语义,仅不做位转换 —— false = 该行不在窗口/未刷新(调用方跳过该行,
 * 不猜 0)。 */
bool SmuPmTable::AtBits(uint32_t byteOff, uint32_t& out) const {
    out = 0;
    if (map_ == nullptr || !refreshed_) return false;
    if (size_ < 4 || byteOff > size_ - 4u) return false;
    const auto* p = static_cast<const uint8_t*>(map_) +
                    (addr_ & 0xFFFull) + byteOff;
    std::memcpy(&out, p, 4);                    // 4 字节位拷贝(无别名 UB)
    return true;
}

/* 握手 + 映射 + 首帧刷新;任一步失败 nullptr(调用方诚实降级,不猜)。 */
std::unique_ptr<SmuPmTable> SmuPmTable::TryCreate(DriverIo& io) {
    std::unique_ptr<SmuPmTable> t(new SmuPmTable(io));
    // (0) 用户态 ECAM 初始化:MCFG 定位 + B0:D0:F0 配置页映射(此后邮箱
    //     全部读写走 smn_,不再触驱动 IO_CTL_SMN_*)。
    if (!t->smn_.Init(io)) return nullptr;
    // (a) arg0 写 0x47 回读自检:ECAM 窗不可写(定位/映射错)即失败。
    if (!t->smn_.Write(kSmnArgs, kArgSelftest)) return nullptr;
    uint32_t readback = 0;
    if (!t->smn_.Read(kSmnArgs, readback) || readback != kArgSelftest)
        return nullptr;
    // (b) 测试消息:response 必须 OK。
    uint32_t args[kArgCount] = {};
    if (t->SmuMsg(kMsgTest, args) != kRepOk) return nullptr;
    // (c) 版本:非 Krackan 0x00650005 仍继续(记录版本;偏移解读由探针
    //     按版本门控,本层只读不解读)。
    if (t->SmuMsg(kMsgVersion, args) != kRepOk) return nullptr;
    t->version_ = args[0];
    // (d) 表物理地址(arg1 << 32 | arg0)。
    if (t->SmuMsg(kMsgAddr, args) != kRepOk) return nullptr;
    t->addr_ = (static_cast<uint64_t>(args[1]) << 32) | args[0];
    if (t->addr_ == 0) return nullptr;
    // (e) 页对齐映射:窗口 0x1000 字节 + 表首页内偏移余量(表可能跨页)。
    t->size_ = kWindow;
    const uint64_t page = t->addr_ & ~0xFFFull;
    const size_t mapLen = kWindow + static_cast<size_t>(t->addr_ & 0xFFFull);
    void* virt = nullptr;
    if (!io.MapPhys(page, mapLen, virt)) return nullptr;
    t->map_ = virt;
    // (f) 首次 transfer:此刻起表内存新鲜(失败 = SMU 拒绝服务,放弃)。
    if (!t->Refresh()) return nullptr;
    return t;
}

/* Task 10:握手逐步诊断(--pmdump TryCreate 失败后的取证)。与 TryCreate
 * 同一序列逐步执行,记录每步中间量(响应码/回读/版本/地址),首败即停
 * (其后字段保持 0)。与 TryCreate 的两点刻意差异:step7 不做 PM 表页
 * 映射(诊断不占用/泄漏表映射窗口;ECAM 页是邮箱自身必需,仍映射 ——
 * failedStep=7 保留枚举语义但本函数不会产生),step8 只发一条 0x65 记录
 * 响应码(不走 Refresh 的 0x80 重试)。内部临时实例仅为复用私有 SmuMsg
 * (表页 map_ 恒空,析构仅释放 ECAM 页),不产出对象状态。
 * Krackan 实机:TryCreate 报 handshake failed 而邮箱终态 msg=0x65/rep=0x1,
 * 本函数给出精确失败步与固件版本应答。 */
SmuHandshakeTrace SmuPmTable::Diagnose(DriverIo& io) {
    SmuHandshakeTrace tr;
    SmuPmTable tmp(io);                    // 无状态临时实例:仅借 SmuMsg
    // (0) ECAM 初始化失败并入 step1(写自检无从发生)。
    if (!tmp.smn_.Init(io)) { tr.failedStep = 1; return tr; }
    // (a1) 写自检魔数:写不进 = ECAM 窗/映射拒绝。
    if (!tmp.smn_.Write(kSmnArgs, kArgSelftest)) { tr.failedStep = 1; return tr; }
    // (a2) 回读自检:记下实际值(不符 = 窗不可写/被覆盖)。
    uint32_t readback = 0;
    if (!tmp.smn_.Read(kSmnArgs, readback)) { tr.failedStep = 2; return tr; }
    tr.argReadback = readback;
    if (readback != kArgSelftest) { tr.failedStep = 2; return tr; }
    // (b) 测试消息。
    uint32_t args[kArgCount] = {};
    tr.testRep = tmp.SmuMsg(kMsgTest, args);
    if (tr.testRep != kRepOk) { tr.failedStep = 3; return tr; }
    // (c) 版本消息(0xFE = 未知命令:固件不识此邮箱消息号)。
    tr.versionRep = tmp.SmuMsg(kMsgVersion, args);
    if (tr.versionRep != kRepOk) { tr.failedStep = 4; return tr; }
    tr.version = args[0];
    // (d) 地址消息。
    tr.addrRep = tmp.SmuMsg(kMsgAddr, args);
    if (tr.addrRep != kRepOk) { tr.failedStep = 5; return tr; }
    tr.addr = (static_cast<uint64_t>(args[1]) << 32) | args[0];
    if (tr.addr == 0) { tr.failedStep = 6; return tr; }
    // (e) 映射步:跳过(见上);(f) 首次 transfer:单发 0x65 只看响应码。
    uint32_t targs[kArgCount] = {};
    tr.transferRep = tmp.SmuMsg(kMsgTransfer, targs);
    if (tr.transferRep != kRepOk) tr.failedStep = 8;
    return tr;
}

} // namespace pd
