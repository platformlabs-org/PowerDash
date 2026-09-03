// PowerDashPmTable.cpp —— SMU PSMU 邮箱 PMTable 客户端实现(v3 Task 6)。
// 协议逐条对齐 ryzenAdj nb_smu_ops.c/api.c(上游核对;Krackan Point 走
// PSMU 默认邮箱):
//   寄存器    msg 0x3B10a20 / response 0x3B10a80 / args 0x3B10a88(+4*i,
//             i = 0..5)
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
//             对象生存期持有,析构 UnmapPhys。At(byteOff) 读映射内
//             (addr & 0xFFF) + byteOff 处 4 字节 memcpy 位转换为 float
//             (避开 strict-aliasing UB);byteOff + 4 > 0x1000 或未刷新
//             -> NaN。
#include "PowerDashPmTable.h"
#include <windows.h>
#include <cmath>
#include <cstring>

namespace pd {
namespace {
constexpr uint32_t kMsg = 0x3B10a20;           // 消息寄存器(写消息号触发)
constexpr uint32_t kRep = 0x3B10a80;           // 响应寄存器(0 = 处理中)
constexpr uint32_t kArgs = 0x3B10a88;          // 参数区基址(args[i] @ +4*i)
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

/* 单条 SMU 消息:清 response -> 写 args[0..5] -> 写 msg -> 轮询
 * response 至非零(≤1e6 次,超时返回 0)-> 回读 args[0..5]。
 * 任一 IO 失败同样返回 0(调用方只认 0x1 = OK)。 */
uint32_t SmuPmTable::SmuMsg(uint32_t msg, uint32_t args[kArgCount]) {
    if (!io_.WriteSmn(kRep, 0)) return 0;
    for (uint32_t i = 0; i < kArgCount; ++i)
        if (!io_.WriteSmn(kArgs + 4u * i, args[i])) return 0;
    if (!io_.WriteSmn(kMsg, msg)) return 0;
    uint32_t rep = 0;
    for (uint32_t n = 0; n < kPollLimit; ++n) {
        if (!io_.ReadSmn(kRep, rep)) return 0;
        if (rep != 0) break;
    }
    if (rep == 0) return 0;                    // 超时:SMU 未应答
    for (uint32_t i = 0; i < kArgCount; ++i)
        if (!io_.ReadSmn(kArgs + 4u * i, args[i])) return 0;
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
    // (a) arg0 写 0x47 回读自检:SMN 窗不可写(地址错/驱动拒绝)即失败。
    if (!io.WriteSmn(kArgs, kArgSelftest)) return nullptr;
    uint32_t readback = 0;
    if (!io.ReadSmn(kArgs, readback) || readback != kArgSelftest)
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

} // namespace pd
