// PowerDashPmTable.cpp —— SMU PSMU 邮箱 PMTable 客户端实现(v3 Task 6;
// Task 10 回切驱动通道)。
// 协议逐条对齐 ryzenAdj nb_smu_ops.c/api.c(上游核对;Krackan Point 走
// PSMU 默认邮箱):
//   寄存器    msg 0x3B10a20 / response 0x3B10a80 / args 0x3B10a88(+4*i,
//             i = 0..5)
//   通道      驱动 IO_CTL_SMN_READ/WRITE(Hal 0x60 地址/0x64 数据窗,
//             驱动内互斥原子)。labs-tb16g7 实证:读与写均可用(0x64 写回
//             同值测试通过,机器稳定)—— 此前"写被拒"系 deviceControl
//             输出缓冲前置门 bug 误诊(55ba422 已修)。用户态 ECAM 绕道
//             (MCFG + IO_CTL_MMAP 映射配置页)已删且禁止回归:
//             \Device\PhysicalMemory 映射为 WRITE-BACK 缓存,经缓存映射
//             访问 MMIO 属未定义行为,实机三次 bugcheck(两次 0xB8/0xBC
//             裸写触碰未知 DF 寄存器,一次 0x60/0x64 经缓存映射)。
//   response  0x1 OK;其余非零一律按失败(0xFF 失败 / 0xFE 未知命令 /
//             0x80 前置条件拒绝 / 0x81 忙拒绝)
//   握手      (a) arg0 写 0x47 回读自检(不符 = SMN 窗不可写,失败)
//             (b) 测试消息 0x1,response 须 0x1
//             (c) 消息 0x6 -> args[0] = 表版本(Krackan 0x00650005;
//                 其他版本继续握手但偏移不解读,由探针按版本门控)
//             (d) 消息 0x66 -> 表物理地址 = args[1]<<32 | args[0]
//             (e) 消息 0x65 transfer:0x80(前置拒绝)-> Sleep(10) 重试
//                 一次,仍失败即放弃;OK 后表内存即新鲜浮点
//   轮询      用户态有界忙等(≤1e6 次 ReadSmn,无 sleep —— 总时长不得
//             卡 1 s 采样环),超时按 response 0 处理(调用方视为失败)
//   映射      表首所在页对齐,窗口 0x1000 字节(+ 页内偏移余量):
//             mapSize = 0x1000 + (addr & 0xFFF)、base = addr & ~0xFFF;
//             对象生存期持有,析构 UnmapPhys。表在 DRAM(非 MMIO),缓存
//             映射安全(与已删的 ECAM 配置页映射不同)。At(byteOff) 读
//             映射内 (addr & 0xFFF) + byteOff 处 4 字节 memcpy 位转换为
//             float(避开 strict-aliasing UB);byteOff + 4 > 0x1000 或未
//             刷新 -> NaN。
#include "PowerDashPmTable.h"
#include <windows.h>
#include <cmath>
#include <cstring>

namespace pd {
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

SmuPmTable::~SmuPmTable() {
    if (map_) io_.UnmapPhys(map_);
}

/* 单条 SMU 消息:清 response -> 写 args[0..5] -> 写 msg(均经驱动
 * IO_CTL_SMN_WRITE,即实机触发 SMU 的真写路径)-> 轮询 response 至非零
 * (≤1e6 次,超时返回 0)-> 回读 args[0..5]。
 * 任一步 IO 失败同样返回 0(调用方只认 0x1 = OK)。 */
uint32_t SmuPmTable::SmuMsg(uint32_t msg, uint32_t (&args)[kArgCount]) {
    if (!io_.WriteSmn(kSmnRep, 0)) return 0;
    for (uint32_t i = 0; i < kArgCount; ++i)
        if (!io_.WriteSmn(kSmnArgs + 4u * i, args[i])) return 0;
    if (!io_.WriteSmn(kSmnMsg, msg)) return 0;
    uint32_t rep = 0;
    for (uint32_t n = 0; n < kPollLimit; ++n) {
        if (!io_.ReadSmn(kSmnRep, rep)) return 0;
        if (rep != 0) break;
    }
    if (rep == 0) return 0;                    // 超时:SMU 未应答
    for (uint32_t i = 0; i < kArgCount; ++i)
        if (!io_.ReadSmn(kSmnArgs + 4u * i, args[i])) return 0;
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
    /* Krackan 实测:rep=OK 后表并非立即可读 —— 立即读全零(60 帧采集
     * 全 0.000 的实机证据),SMU 异步填充;等待后再放行读路径。 */
    Sleep(10);
    refreshed_ = true;
    return true;
}

/* --pmxfer 取证:Raven 老流程的 0x66/0x65 都带表选择子 arg0(TA=3,
 * TABLE_MOMENTARY_PM=5);Renoir+ 改裸调。Krackan 裸调 rep=OK 但表不落
 * 0x66 所报地址 —— 试验带选择子的 0x65 是否触发填充。锚点取
 * 0x00/0x30/0x40/0x9D8(STAPM/Tdc/Tctl/Strix 系 CorePower[0])。 */
uint32_t SmuPmTable::TransferProbe(uint32_t tableId, float anchors[4]) {
    uint32_t args[kArgCount] = {};
    args[0] = tableId;
    const uint32_t rep = SmuMsg(kMsgTransfer, args);
    if (rep == kRepOk) Sleep(20);
    refreshed_ = true;                     // 允许 At 读(取证不设防)
    anchors[0] = At(0x00);
    anchors[1] = At(0x30);
    anchors[2] = At(0x40);
    anchors[3] = At(0x9D8);
    return rep;
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
    // (a) arg0 写 0x47 回读自检:SMN 窗不可写(地址错/驱动拒绝)即失败。
    if (!io.WriteSmn(kSmnArgs, kArgSelftest)) return nullptr;
    uint32_t readback = 0;
    if (!io.ReadSmn(kSmnArgs, readback) || readback != kArgSelftest)
        return nullptr;
    // (b) 测试消息:response 必须 OK。
    uint32_t args[kArgCount] = {};
    if (t->SmuMsg(kMsgTest, args) != kRepOk) return nullptr;
    // (c) 版本:非 Krackan 0x00650005 仍继续(记录版本;偏移解读由探针
    //     按版本门控,本层只读不解读)。
    if (t->SmuMsg(kMsgVersion, args) != kRepOk) return nullptr;
    t->version_ = args[0];
    // (d) 表物理地址。首取 arg0(32 位):实测 Krackan(labs-tb16g7,
    // 23.3GB RAM)固件 arg0=0x5E280000、arg1=0x6 —— arg1 非地址高位,
    // 按 ryzenAdj 的 arg1<<32|arg0 拼出 0x65E280000(27.4GB)超出物理
    // 内存,映射必败。回退序:arg0 -> (arg1<<32)|arg0,映射成功即用。
    if (t->SmuMsg(kMsgAddr, args) != kRepOk) return nullptr;
    if (args[0] == 0 && args[1] == 0) return nullptr;
    // (e) 页对齐映射:窗口 0x1000 字节 + 表首页内偏移余量(表可能跨页)。
    t->size_ = kWindow;
    const uint64_t candidates[2] = {
        args[0], (static_cast<uint64_t>(args[1]) << 32) | args[0]};
    void* virt = nullptr;
    for (const uint64_t cand : candidates) {
        const uint64_t page = cand & ~0xFFFull;
        const size_t mapLen = kWindow + static_cast<size_t>(cand & 0xFFFull);
        if (io.MapPhys(page, mapLen, virt) && virt != nullptr) {
            t->addr_ = cand;
            t->map_ = virt;
            break;
        }
        virt = nullptr;
    }
    if (t->map_ == nullptr) return nullptr;
    // (f) 首次 transfer:此刻起表内存新鲜(失败 = SMU 拒绝服务,放弃)。
    if (!t->Refresh()) return nullptr;
    return t;
}

/* Task 10:握手逐步诊断(--pmdump TryCreate 失败后的取证)。与 TryCreate
 * 同一序列逐步执行,记录每步中间量(响应码/回读/版本/地址),首败即停
 * (其后字段保持 0)。与 TryCreate 的两点刻意差异:step7 不做映射(诊断
 * 不占用/泄漏映射窗口;failedStep=7 保留枚举语义但本函数不会产生),
 * step8 只发一条 0x65 记录响应码(不走 Refresh 的 0x80 重试)。内部临时
 * 实例仅为复用私有 SmuMsg(map_ 恒空,析构无副作用),不产出对象状态。
 * Krackan 实机:TryCreate 报 handshake failed 而邮箱终态 msg=0x65/rep=0x1,
 * 本函数给出精确失败步与固件版本应答。 */
SmuHandshakeTrace SmuPmTable::Diagnose(DriverIo& io) {
    SmuHandshakeTrace tr;
    SmuPmTable tmp(io);                    // 无状态临时实例:仅借 SmuMsg
    // (a1) 写自检魔数:写不进 = SMN 窗/驱动拒绝。
    if (!io.WriteSmn(kSmnArgs, kArgSelftest)) { tr.failedStep = 1; return tr; }
    // (a2) 回读自检:记下实际值(不符 = 窗不可写/被覆盖)。
    uint32_t readback = 0;
    if (!io.ReadSmn(kSmnArgs, readback)) { tr.failedStep = 2; return tr; }
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
    for (int i = 0; i < 6; ++i) tr.addrArgs[i] = args[i];
    tr.addr = args[0];                        // 主候选(见 TryCreate (d) 实证注记)
    if (tr.addr == 0 && args[1] == 0) { tr.failedStep = 6; return tr; }
    // (e) 映射步:跳过(见上);(f) 首次 transfer:单发 0x65 只看响应码。
    uint32_t targs[kArgCount] = {};
    tr.transferRep = tmp.SmuMsg(kMsgTransfer, targs);
    if (tr.transferRep != kRepOk) tr.failedStep = 8;
    return tr;
}

} // namespace pd
