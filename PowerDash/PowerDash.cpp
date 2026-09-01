#include <windows.h>
#include <intrin.h>
#include "PowerDashUi.h"
#include <iostream>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <thread>
#include <string>
#include <vector>

#define POWERDASH_DEV_TYPE 55000
#define IO_CTL_MSR_READ        CTL_CODE(POWERDASH_DEV_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_PCICFG_READ     CTL_CODE(POWERDASH_DEV_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_MMAP            CTL_CODE(POWERDASH_DEV_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_MUNMAP          CTL_CODE(POWERDASH_DEV_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_FNQ_INJECT      CTL_CODE(POWERDASH_DEV_TYPE, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Lenovo EnergyDrv (AcpiVpc.sys) - raw VPC0.DYTC ACPI passthrough.
 * Read-only commands per Lenovo ITSDriver (E:\Module\Lenovo.DYTC):
 *   GET=2, FUNC_CAP=3, FUNC_CAP_EXT=10
 *   GET word = valid(bit0) | func(bits8-11) | mode(bits12-15)
 *   FUNC_CAP bitmap = word>>16: bit5 APM, bit6 AQM, bit7 iEPM, bit8 iBSM,
 *                     bit11 EPM/BSM (MMC func-11 family)
 * Modes are never written through this channel; the Fn+Q notification
 * injected by the kernel driver makes the Lenovo stack do the switch. */
constexpr uint32_t IOCTL_ENERGYDRV_DYTC = 0x8310213C;
constexpr uint32_t DYTC_GET        = 0x2;
constexpr uint32_t DYTC_FUNC_CAP   = 0x3;
constexpr uint32_t DYTC_FUNC_CAP_EXT = 0xA;

constexpr char PD_VER[] = "1.0";

constexpr auto MSR_PKG_ENERGY_STATUS = 0x611;
constexpr auto MSR_PP0_ENERGY_STATUS = 0x639;
constexpr auto MSR_PP1_ENERGY_STATUS = 0x641;
constexpr auto MSR_RAPL_POWER_UNIT = 0x606;
constexpr auto MSR_PKG_POWER_INFO = 0x614;
constexpr auto MSR_SYS_ENERGY_STATUS = 0x64D;

/* extra monitor registers (addresses cross-checked against intel pcm
 * 2026-08: src/types.h) */
constexpr auto MSR_TEMPERATURE_TARGET = 0x1A2;    /* TjMax in bits 23:16   */
constexpr auto MSR_PACKAGE_THERM_STATUS = 0x1B1;  /* headroom in bits 22:16 */
constexpr auto MSR_IA32_APERF = 0xE8;
constexpr auto MSR_IA32_MPERF = 0xE7;
constexpr auto MSR_PKG_C2_RESIDENCY = 0x60D;
constexpr auto MSR_PKG_C6_RESIDENCY = 0x3F9;
constexpr auto MSR_SMI_COUNT = 0x34;
constexpr auto MSR_PLATFORM_INFO = 0xCE;          /* max non-turbo ratio   */

// Ctrl+C exit flag
volatile bool g_exitRequested = false;
BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT) {
        g_exitRequested = true;
        return TRUE;
    }
    return FALSE;
}

/* ---- entry prelude: clean screen, window to the front ---- */

/* every entry: clear the console and pull its window to the front so
 * leftover console content never interferes with the output */
static void ConsolePrelude() {
    HWND con = GetConsoleWindow();
    if (con) {
        SetForegroundWindow(con);   /* best-effort; usually already ours */
        /* topmost round-trip reliably raises the z-order without pinning
         * the window above everything forever */
        SetWindowPos(con, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(con, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD cm = 0;
    if (GetConsoleMode(hOut, &cm) &&
        SetConsoleMode(hOut, cm | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        std::cout << "\x1b[2J\x1b[H" << std::flush;   /* real console only */
        SetConsoleOutputCP(CP_UTF8);   /* for the block-drawing glyphs */
    }
}

/* ---- CPU identification (CPUID; no driver needed) ---- */

/* brand string + Intel family-6 codename, e.g.
 * "Intel(R) Core(TM) Ultra 7 258V  [Lunar Lake]" */
static std::string CpuIdLine() {
    int info[4] = {};
    __cpuid(info, 0);
    char vendor[13] = {};
    memcpy(vendor + 0, &info[1], 4);
    memcpy(vendor + 4, &info[3], 4);
    memcpy(vendor + 8, &info[2], 4);

    std::string bs;
    __cpuid(info, 0x80000000);               /* max extended leaf in EAX */
    if ((unsigned)info[0] >= 0x80000004u) {  /* brand string supported */
        char brand[49] = {};
        for (int i = 0; i < 3; ++i) {
            __cpuid(info, 0x80000002 + i);
            memcpy(brand + i * 16, info, 16);
        }
        bs = brand;
        size_t b = bs.find_first_not_of(" \t");
        size_t e = bs.find_last_not_of(" \t");
        bs = (b == std::string::npos) ? std::string(vendor)
                                      : bs.substr(b, e - b + 1);
    } else {
        bs = vendor;
    }

    __cpuid(info, 1);
    unsigned family = ((info[0] >> 8) & 0xF) + ((info[0] >> 20) & 0xFF);
    unsigned model  = ((info[0] >> 4) & 0xF) | (((info[0] >> 16) & 0xF) << 4);

    const char* codename = nullptr;
    if (!strcmp(vendor, "GenuineIntel") && family == 6) {
        /* model numbers cross-checked against intel pcm 2026-08
         * (src/cpucounters.h SupportedCPUModels) */
        switch (model) {
        case 0x4E: case 0x5E:   codename = "Skylake";                    break;
        case 0x8E:            codename = "Kaby~Comet Lake (mobile)";     break;
        case 0x9E:            codename = "Kaby~Comet Lake";              break;
        case 0xA5: case 0xA6: codename = "Comet Lake";                   break;
        case 0x66:            codename = "Cannon Lake";                  break;
        case 0xA7:            codename = "Rocket Lake";                  break;
        case 0x7D: case 0x7E: case 0x9D:
                              codename = "Ice Lake";                     break;
        case 0x8C: case 0x8D: codename = "Tiger Lake";                   break;
        case 0x97: case 0x9A: codename = "Alder Lake";                   break;
        case 0xBE:            codename = "Raptor Lake-N";                break;
        case 0xB7: case 0xBA: case 0xBF:
                              codename = "Raptor Lake";                  break;
        case 0xAA: case 0xAC: codename = "Meteor Lake";                  break;
        case 0xC5:            codename = "Arrow Lake-H";                 break;
        case 0xC6:            codename = "Arrow Lake";                   break;
        case 0xBD:            codename = "Lunar Lake";                   break;
        case 0xCC:            codename = "Panther Lake";                 break;
        case 0xDD:            codename = "Clearwater Forest";            break;
        case 0x5C:            codename = "Apollo Lake";                  break;
        case 0x7A:            codename = "Gemini Lake";                  break;
        case 0x96:            codename = "Elkhart Lake";                 break;
        case 0x9C:            codename = "Jasper Lake";                  break;
        case 0x55:            codename = "Skylake-SP / Cascade Lake-SP"; break;
        case 0x6A: case 0x6C: codename = "Ice Lake-SP";                  break;
        case 0x8F:            codename = "Sapphire Rapids";              break;
        case 0xCF:            codename = "Emerald Rapids";               break;
        case 0xAD: case 0xAE: codename = "Granite Rapids";               break;
        case 0xAF:            codename = "Sierra Forest";                break;
        case 0xB6:            codename = "Grand Ridge";                  break;
        case 0x4F:            codename = "Broadwell-EP/EX";              break;
        default: break;
        }
    }

    char tail[48];
    if (codename)
        sprintf_s(tail, "  [%s]", codename);
    else
        sprintf_s(tail, "  [Family %u Model 0x%X]", family, model);
    return bs + tail;
}

struct MSR_Request {
    int core_id;
    uint64_t msr_address;
    uint64_t write_value;
};

struct PCICFG_Request {
    ULONG bus, dev, func, reg, bytes;
    ULONG64 write_value;
};

struct MMAP_Request {
    LARGE_INTEGER address;
    SIZE_T size;
};

bool read_msr(HANDLE driver, int core_id, uint64_t address, uint64_t& out_value) {
    MSR_Request request{ core_id, address, 0 };
    DWORD bytesReturned = 0;
    return DeviceIoControl(
        driver,
        IO_CTL_MSR_READ,
        &request,
        sizeof(request),
        &out_value,
        sizeof(out_value),
        &bytesReturned,
        nullptr
    );
}

uint64_t read_pci_config(HANDLE hDriver, ULONG bus, ULONG dev, ULONG func, ULONG reg) {
    PCICFG_Request req = { bus, dev, func, reg, 4, 0 };
    ULONG64 value = 0;
    DWORD returned = 0;
    DeviceIoControl(hDriver, IO_CTL_PCICFG_READ, &req, sizeof(req), &value, sizeof(value), &returned, nullptr);
    return value;
}

/* ============================ mode subsystem ============================ */
/* Software Fn+Q for Lenovo IdeaPad (AcpiVpc.sys 15.11.30.11 family):
 * the kernel driver synthesizes the exact notification a real Fn+Q press
 * leaves behind; FnHotkeyUtility pops the OSD and the Lenovo Dispatcher
 * performs the actual mode change. No DYTC writes from user mode. */

static HANDLE OpenDeviceRaw(const char* name) {
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE,
                       0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* -------------------- embedded driver session --------------------
 * PowerDash.exe ships as a single file with PowerDash.sys embedded as a
 * resource (IDR_SYS_DRIVER). When the \\.\POWERDASH device is not present
 * the driver is transparently extracted to %TEMP%, installed as service
 * "PowerDashSYS" (name must differ from the device name \Driver\POWERDASH)
 * and started; if *we* installed it, it is removed again on exit. */

#define PD_SVC_NAME      "PowerDashSYS"
#define PD_SVC_FILE_W    L"PowerDashDrv.sys"
#define IDR_SYS_DRIVER   101

static bool g_weLoadedDriver = false;
static wchar_t g_driverPath[MAX_PATH] = L"";

static bool ExtractEmbeddedDriverW() {
    HRSRC rs = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_SYS_DRIVER), RT_RCDATA);
    if (!rs) return false;
    HGLOBAL rg = LoadResource(NULL, rs);
    if (!rg) return false;
    void* p = LockResource(rg);
    DWORD sz = SizeofResource(NULL, rs);
    if (!p || !sz) return false;

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(g_driverPath, MAX_PATH, L"%s%s", tmp, PD_SVC_FILE_W);

    HANDLE f = CreateFileW(g_driverPath, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(f, p, sz, &wr, NULL) && wr == sz;
    CloseHandle(f);
    return ok;
}

static char g_driverPathA[MAX_PATH] = "";

static bool ExtractEmbeddedDriver() {
    if (!ExtractEmbeddedDriverW()) return false;
    WideCharToMultiByte(CP_ACP, 0, g_driverPath, -1,
                        g_driverPathA, MAX_PATH, NULL, NULL);
    return true;
}

static void RemoveOursDriver() {
    if (!g_weLoadedDriver) return;   /* only remove what we installed */
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceA(scm, PD_SVC_NAME, SERVICE_ALL_ACCESS);
    if (svc) {
        SERVICE_STATUS st;
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 30; i++) {
            if (!QueryServiceStatus(svc, &st) || st.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(100);
        }
        DeleteService(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    DeleteFileW(g_driverPath);
    g_weLoadedDriver = false;
}

/* returns an open \\.\POWERDASH handle, or INVALID_HANDLE_VALUE (error printed) */
static HANDLE EnsureDriverLoaded() {
    /* already running?  adopt it if the service image is our own extracted
     * temp file (orphan left behind by a hard-killed previous instance) */
    HANDLE h = OpenDeviceRaw("\\\\.\\POWERDASH");
    if (h != INVALID_HANDLE_VALUE) {
        SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
        if (scm) {
            SC_HANDLE svc = OpenServiceA(scm, PD_SVC_NAME, SERVICE_QUERY_CONFIG);
            if (svc) {
                QUERY_SERVICE_CONFIGA cfg;
                BYTE buf[1024];
                DWORD need = 0;
                if (QueryServiceConfigA(svc, (QUERY_SERVICE_CONFIGA*)&buf, sizeof(buf), &need)) {
                    cfg = *(QUERY_SERVICE_CONFIGA*)&buf;
                    if (cfg.lpBinaryPathName &&
                        strstr(cfg.lpBinaryPathName, "PowerDashDrv.sys")) {
                        g_weLoadedDriver = true;   /* orphan adopted */
                        MultiByteToWideChar(CP_ACP, 0, cfg.lpBinaryPathName, -1,
                                            g_driverPath, MAX_PATH);
                    }
                }
                CloseServiceHandle(svc);
            }
            CloseServiceHandle(scm);
        }
        return h;
    }

    if (!ExtractEmbeddedDriver()) {
        std::cerr << "failed to extract embedded PowerDash.sys resource" << std::endl;
        return INVALID_HANDLE_VALUE;
    }

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        std::cerr << "administrator privileges required (err " << GetLastError() << ")" << std::endl;
        return INVALID_HANDLE_VALUE;
    }
    /* clean any leftover, then create fresh */
    SC_HANDLE svc = OpenServiceA(scm, PD_SVC_NAME, SERVICE_ALL_ACCESS);
    if (svc) {
        SERVICE_STATUS st;
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        DeleteService(svc);
        CloseServiceHandle(svc);
        svc = NULL;
        Sleep(300);
    }
    svc = CreateServiceA(scm, PD_SVC_NAME, "PowerDash kernel helper",
                         SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
                         SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
                         g_driverPathA, NULL, NULL, NULL, NULL, NULL);
    if (!svc) {
        std::cerr << "CreateService failed (err " << GetLastError() << ")" << std::endl;
        CloseServiceHandle(scm);
        return INVALID_HANDLE_VALUE;
    }
    if (!StartServiceA(svc, 0, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_SERVICE_MARKED_FOR_DELETE) { Sleep(1000); StartServiceA(svc, 0, NULL); e = GetLastError(); }
        if (e) {
            if (e == 577)
                std::cerr << "driver signature rejected (577). Enable test signing first:\n"
                          << "  bcdedit /set testsigning on   (reboot once)" << std::endl;
            else if (e == 4551 || e == 5)
                std::cerr << "driver blocked by application control policy (" << e
                          << "). Disable Smart App Control." << std::endl;
            else
                std::cerr << "StartService failed (err " << e << ")" << std::endl;
            DeleteService(svc);
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return INVALID_HANDLE_VALUE;
        }
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    /* wait for the device to appear */
    for (int i = 0; i < 30; i++) {
        h = OpenDeviceRaw("\\\\.\\POWERDASH");
        if (h != INVALID_HANDLE_VALUE) {
            g_weLoadedDriver = true;
            return h;
        }
        Sleep(100);
    }
    std::cerr << "driver service started but device did not appear" << std::endl;
    RemoveOursDriver();
    return INVALID_HANDLE_VALUE;
}


static bool EnergyDytc(uint32_t cmd, uint32_t& out) {
    HANDLE h = OpenDeviceRaw("\\\\.\\EnergyDrv");
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD in = cmd, returned = 0;
    bool ok = DeviceIoControl(h, IOCTL_ENERGYDRV_DYTC, &in, sizeof(in),
                              &out, sizeof(out), &returned, nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

/* check whether the Lenovo Dispatcher is running (it reacts to the
 * injected Fn+Q notification and programs the DYTC mode) */
static bool DispatcherRunning() {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceA(scm, "LenovoProcessManagement",
                                       SERVICE_QUERY_STATUS) : NULL;
    bool running = false;
    if (svc) {
        SERVICE_STATUS st;
        running = QueryServiceStatus(svc, &st) &&
                  st.dwCurrentState == SERVICE_RUNNING;
        CloseServiceHandle(svc);
    }
    if (scm) CloseServiceHandle(scm);
    return running;
}

/* decode per Lenovo ITSDriver::GetCurrentMode, with the mode hierarchy:
 * fixed modes are EPM (func11 m2) and BSM (func11 m3); everything else
 * (STD/APM/AQM/IEPM/IBSM) is a sub-state the Dispatcher's AI engine
 * picks automatically under the "intelligent" umbrella while it keeps
 * the OS power overlay at Balanced */
static const char* DecodeMode(uint32_t raw) {
    if (!(raw & 1)) return "Unknown";
    uint32_t func = (raw >> 8) & 0xF, mode = (raw >> 12) & 0xF;
    switch (func) {
    case 11:
        if (mode == 2) return "Performance (EPM)";
        if (mode == 3) return "Quiet (BSM)";
        return "Intelligent (STD)";
    case 0:  return "Intelligent (STD)";
    case 5:  return "Intelligent (APM)";
    case 6:  return "Intelligent (AQM)";
    case 7:  return "Intelligent (IEPM)";
    case 8:  return "Intelligent (IBSM)";
    default: return "Unknown";
    }
}

/* per Lenovo ITSDriver::GetFunCapability - which modes this machine's
 * ITS firmware actually implements; shown by 'mode status' */
struct DytcCaps {
    bool mmc;    /* func-11 family: EPM + BSM                */
    bool apm;    /* func 5  auto performance (smart)         */
    bool aqm;    /* func 6  auto quiet                       */
    bool iepm;   /* func 7  intelligent extreme performance  */
    bool ibsm;   /* func 8  intelligent battery saving       */
    bool geek;   /* FUNC_CAP_EXT bit17                       */
    uint32_t bitmap;
};

static bool QueryCaps(DytcCaps& c) {
    memset(&c, 0, sizeof(c));
    uint32_t fc = 0;
    if (!EnergyDytc(DYTC_FUNC_CAP, fc) || !(fc & 1))
        return false;
    c.bitmap = fc >> 16;
    c.mmc   = (c.bitmap >> 11) & 1;
    c.apm   = (c.bitmap >> 5)  & 1;
    c.aqm   = (c.bitmap >> 6)  & 1;
    c.iepm  = (c.bitmap >> 7)  & 1;
    c.ibsm  = (c.bitmap >> 8)  & 1;
    uint32_t ext = 0;
    if (EnergyDytc(DYTC_FUNC_CAP_EXT, ext) && (ext & 1))
        c.geek = (ext >> 17) & 1;
    return true;
}

/* inject one Fn+Q notification through the kernel driver; returns the
 * number of events signaled, or -1 on failure. This is the ONLY write
 * path for modes: FnHotkeyUtility pops the OSD and the Lenovo
 * Dispatcher performs the actual DYTC mode change. */
static int InjectFnQ() {
    HANDLE hDriver = EnsureDriverLoaded();
    if (hDriver == INVALID_HANDLE_VALUE)
        return -1;
    uint64_t in = 1, out = 0;
    DWORD returned = 0;
    BOOL ok = DeviceIoControl(hDriver, IO_CTL_FNQ_INJECT, &in, sizeof(in),
                              &out, sizeof(out), &returned, nullptr);
    CloseHandle(hDriver);
    RemoveOursDriver();
    if (!ok || (out & 0x80000000ULL))
        return -1;
    return (int)out;
}

static int CmdMode(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout <<
            "usage: PowerDash mode <next|status>\n"
            "       next       inject the Fn+Q notification\n"
            "       status|s   show current mode and capabilities\n";
        return 1;
    }

    std::string m = argv[2];

    uint32_t raw = 0;
    if (m == "status" || m == "s") {
        if (!EnergyDytc(DYTC_GET, raw)) {
            std::cerr << "cannot open \\\\.\\EnergyDrv (err " << GetLastError()
                      << ") - is AcpiVpc.sys (Lenovo EnergyDrv) running?" << std::endl;
            return 3;
        }
        std::cout << "CPU : " << CpuIdLine() << std::endl;
        std::cout << "Current mode: " << DecodeMode(raw)
                  << " (func=" << ((raw >> 8) & 0xF)
                  << " mode=" << ((raw >> 12) & 0xF)
                  << " raw=0x" << std::hex << raw << std::dec << ")" << std::endl;
        DytcCaps c;
        if (QueryCaps(c))
            std::cout << "Capabilities: mmc=" << c.mmc << " apm=" << c.apm
                      << " aqm=" << c.aqm << " iepm=" << c.iepm
                      << " ibsm=" << c.ibsm << " geek=" << c.geek
                      << " (bitmap=0x" << std::hex << c.bitmap << std::dec << ")" << std::endl;
        std::cout << "Dispatcher: " << (DispatcherRunning() ? "running" : "absent") << std::endl;
        return 0;
    }

    if (m == "next") {
        /* Software Fn+Q is notification-only. The Lenovo stack owns the
         * actual mode transition and OSD; do not query or echo mode state. */
        int sig = InjectFnQ();
        if (sig < 0) {
            std::cerr << "inject failed (PowerDash.sys unavailable or rejected)" << std::endl;
            return 4;
        }
        std::cout << "Notification applied (" << sig << " consumers)" << std::endl;
        return 0;
    }

    std::cerr << "unknown mode: " << m << std::endl;
    return 1;
}

/* ============================ power monitor ============================ */

static std::string LocalIsoTimestamp() {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char text[24] = {};
    sprintf_s(text, "%04u-%02u-%02uT%02u:%02u:%02u",
              now.wYear, now.wMonth, now.wDay,
              now.wHour, now.wMinute, now.wSecond);
    return text;
}

static int RunMonitor(int argc, char* argv[],
                      const pd::MonitorOptions& monitorOptions = {}) {
    std::ofstream csv;
    if (!monitorOptions.csvPath.empty()) {
        csv.open(monitorOptions.csvPath, std::ios::out | std::ios::trunc);
        if (!csv) {
            std::cerr << "cannot create CSV file: " << monitorOptions.csvPath
                      << std::endl;
            return 2;
        }
        csv << pd::CsvHeader() << '\n';
        csv.flush();
    }

    HANDLE hDriver = EnsureDriverLoaded();   /* transparent install if needed */

    if (hDriver == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open driver." << std::endl;
        return 1;
    }

    int rc = 0;   /* monitor body result */
    do {

    uint64_t power_unit_raw = 0;
    if (!read_msr(hDriver, 0, MSR_RAPL_POWER_UNIT, power_unit_raw)) {
        std::cerr << "Failed to read MSR_RAPL_POWER_UNIT." << std::endl;
        rc = 1; break;
    }

    uint32_t power_unit_bits = (power_unit_raw >> 0) & 0x0F;
    uint32_t energy_unit_bits = (power_unit_raw >> 8) & 0x1F;
    uint32_t time_unit_bits = (power_unit_raw >> 16) & 0x0F;

    double power_unit = 1.0 / pow(2.0, power_unit_bits);
    double energy_unit = 1.0 / pow(2.0, energy_unit_bits);
    double time_unit = 1.0 / pow(2.0, time_unit_bits);
    (void)power_unit; (void)time_unit;

    uint64_t mchbar_val = read_pci_config(hDriver, 0, 0, 0, 0x48);
    if ((mchbar_val & 0x1) == 0) {
        std::cerr << "MCHBAR is not enabled" << std::endl;
        rc = 1; break;
    }

    mchbar_val &= ~0x1;
    uint64_t mmio_phys = mchbar_val + 0x59A0;
    uint64_t mmio_page_base = mmio_phys & ~0xFFF;
    size_t mmio_size = 0x1000;

    MMAP_Request mmap_req = {};
    mmap_req.address.QuadPart = mmio_page_base;
    mmap_req.size = mmio_size;

    uint64_t user_virtual = 0;
    DWORD returned = 0;
    if (!DeviceIoControl(hDriver, IO_CTL_MMAP, &mmap_req, sizeof(mmap_req), &user_virtual, sizeof(user_virtual), &returned, nullptr)) {
        std::cerr << "Failed to map MMIO address. Error code: " << GetLastError() << std::endl;
        rc = 1; break;
    }

    uint32_t* mmio = reinterpret_cast<uint32_t*>(user_virtual + (mmio_phys & 0xFFF));

    // Set PL command-line function
    if (argc == 4 && std::string(argv[1]) == "-setpl") {
        double setPL1 = std::stod(argv[2]);
        double setPL2 = std::stod(argv[3]);

        uint32_t pl1_bits = static_cast<uint32_t>(setPL1 / 0.125) & 0x7FFF;
        uint32_t pl2_bits = static_cast<uint32_t>(setPL2 / 0.125) & 0x7FFF;

        uint32_t pl1_value = pl1_bits | (1 << 15);
        uint32_t pl2_value = pl2_bits | (1 << 15) | (1U << 31);  // lock bit

        std::cout << "Setting MMIO PL1 = " << setPL1 << " W, PL2 = " << setPL2 << " W and locking..." << std::endl;
        mmio[0] = pl1_value;
        mmio[1] = pl2_value;
        std::cout << "Done! Exiting." << std::endl;

        MMAP_Request unmap = {};
        unmap.address.QuadPart = user_virtual;
        DeviceIoControl(hDriver, IO_CTL_MUNMAP, &unmap, sizeof(unmap), nullptr, 0, &returned, nullptr);
        rc = 0; break;
    }

    /* package power envelope - also the fallback bar scale when the
     * MMIO PL registers read zero */
    double thermal_spec_power = 0;
    uint64_t pkg_power_info = 0;
    if (read_msr(hDriver, 0, MSR_PKG_POWER_INFO, pkg_power_info)) {
        double min_power = ((pkg_power_info >> 16) & 0x7FFF) * power_unit;
        double max_power = ((pkg_power_info >> 32) & 0x7FFF) * power_unit;
        thermal_spec_power = ((pkg_power_info >> 0) & 0x7FFF) * power_unit;
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "Package envelope: min " << min_power
                  << " / max " << max_power
                  << " / thermal spec " << thermal_spec_power << " W" << std::endl;
    }

    /* ---- static facts: TjMax, base frequency, PL time windows ---- */
    int tjMaxC = 0;
    {
        uint64_t t = 0;
        if (read_msr(hDriver, 0, MSR_TEMPERATURE_TARGET, t))
            tjMaxC = (int)((t >> 16) & 0xFF);
    }
    double baseMHz = 0;   /* CPUID 0x16 EAX - pcm's preferred source */
    {
        int info[4] = {};
        __cpuid(info, 0);
        if ((unsigned)info[0] >= 0x16) {
            __cpuid(info, 0x16);
            baseMHz = (double)(info[0] & 0xFFFF);
        }
        if (baseMHz == 0) {   /* pcm fallback: PLATFORM_INFO ratio */
            uint64_t pi = 0;
            if (read_msr(hDriver, 0, MSR_PLATFORM_INFO, pi)) {
                unsigned ratio = (unsigned)((pi >> 8) & 0xFF);
                if (ratio) baseMHz = ratio * 100.0;
            }
        }
    }
    unsigned nLP = std::thread::hardware_concurrency();

    // Initialize MSR values (all deltas sampled on core 0)
    uint64_t prev_pkg = 0, prev_pp0 = 0, prev_pp1 = 0, prev_sys = 0;
    read_msr(hDriver, 0, MSR_PKG_ENERGY_STATUS, prev_pkg);
    read_msr(hDriver, 0, MSR_PP0_ENERGY_STATUS, prev_pp0);
    read_msr(hDriver, 0, MSR_PP1_ENERGY_STATUS, prev_pp1);
    read_msr(hDriver, 0, MSR_SYS_ENERGY_STATUS, prev_sys);
    uint64_t prev_aperf = 0, prev_mperf = 0;
    uint64_t prev_c2 = 0, prev_c6 = 0, prev_smi = 0;
    read_msr(hDriver, 0, MSR_IA32_APERF, prev_aperf);
    read_msr(hDriver, 0, MSR_IA32_MPERF, prev_mperf);
    read_msr(hDriver, 0, MSR_PKG_C2_RESIDENCY, prev_c2);
    read_msr(hDriver, 0, MSR_PKG_C6_RESIDENCY, prev_c6);
    read_msr(hDriver, 0, MSR_SMI_COUNT, prev_smi);
    uint64_t prev_tsc = __rdtsc();

    /* true CPU utilization (Task-Manager style) comes from GetSystemTimes
     * deltas; package C0+C1 residency (100 - PKG_C2_RESIDENCY) is NOT it:
     * a single busy thread keeps the package out of C2 and would read
     * ~100% "utilization" on an otherwise idle machine */
    FILETIME prevIdle = {}, prevKernel = {}, prevUser = {};
    GetSystemTimes(&prevIdle, &prevKernel, &prevUser);

    std::cout << std::fixed << std::setprecision(2);

    /* ---- display setup: a grouped dashboard, redrawn in place ---- */
    bool vtOn = false;
    if (getenv("PDASH_FORCE_VT")) {
        vtOn = true;   /* test hook: emit VT frames even when piped */
    } else {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD cm = 0;
        if (GetConsoleMode(hOut, &cm) &&
            SetConsoleMode(hOut, cm | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            vtOn = true;
    }
    /* Prefer 96 columns; stack diagnostics below 92 and never drop fields. */
    auto consoleColumns = [] {
        CONSOLE_SCREEN_BUFFER_INFO sbi = {};
        return GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &sbi)
            ? sbi.srWindow.Right - sbi.srWindow.Left + 1 : 0;
    };
    auto pickDashboardWidth = [&consoleColumns] {
        int w = 96;
        const int visibleColumns = consoleColumns();
        if (visibleColumns > 0 && visibleColumns - 2 < w)
            w = visibleColumns - 2;   /* leave a wrap-safe margin */
        return w < 72 ? 72 : w;
    };
    int W = pickDashboardWidth();
    int lastConsoleColumns = consoleColumns();
    auto fmtTau = [&](uint64_t rawTau) {   /* RAPL time units -> seconds */
        if (rawTau == 0) return std::string("n/a");
        double s = rawTau * time_unit;
        std::ostringstream o;
        o << std::fixed << std::setprecision(2);
        if      (s >= 1)     o << s       << " s";
        else if (s >= 0.001) o << s * 1e3 << " ms";
        else                 o << s * 1e6 << " us";
        return o.str();
    };

    if (vtOn) std::cout << "\x1b[?25l" << std::flush;   /* hide cursor */

    const std::string cpuLine = CpuIdLine();
    std::string brand = cpuLine, codeTag;   /* "brand  [codename]" split */
    {
        size_t tb = cpuLine.find("  [");
        if (tb != std::string::npos) {
            brand = cpuLine.substr(0, tb);
            codeTag = cpuLine.substr(tb + 3, cpuLine.size() - tb - 4);
        }
    }

    pd::DashboardInfo dashboardInfo;
    dashboardInfo.version = PD_VER;
    dashboardInfo.cpuBrand = brand;
    dashboardInfo.codeName = codeTag;
    dashboardInfo.logicalProcessors = nLP;
    dashboardInfo.baseGHz = baseMHz / 1000.0;
    dashboardInfo.tjMaxC = tjMaxC;
    dashboardInfo.width = W;
    dashboardInfo.fallbackScaleW = thermal_spec_power;
    dashboardInfo.csvActive = csv.is_open();
    if (dashboardInfo.csvActive) {
        const size_t slash = monitorOptions.csvPath.find_last_of("\\/");
        dashboardInfo.csvName = slash == std::string::npos
                              ? monitorOptions.csvPath
                              : monitorOptions.csvPath.substr(slash + 1);
    }
    dashboardInfo.ansi = vtOn;
    const ULONGLONG monitorStarted = GetTickCount64();

    // Main loop
    std::vector<double> hist;   /* rolling pkg power samples (sparkline) */
    int frame = 0;
    std::vector<std::string> previousFrame;   /* last frame, line by line */
    int cursorRowsBelowFrame = 0;             /* 1 after wiping a shrank frame */
    while (!g_exitRequested &&
           (monitorOptions.runSeconds < 0 ||
            frame < (int)std::ceil(monitorOptions.runSeconds))) {
        for (int i = 0; i < 10 && !g_exitRequested; ++i)
            Sleep(100);   /* 1 s sample window, interruptible */

        uint64_t curr_pkg = 0, curr_pp0 = 0, curr_pp1 = 0, curr_sys = 0;
        read_msr(hDriver, 0, MSR_PKG_ENERGY_STATUS, curr_pkg);
        read_msr(hDriver, 0, MSR_PP0_ENERGY_STATUS, curr_pp0);
        read_msr(hDriver, 0, MSR_PP1_ENERGY_STATUS, curr_pp1);
        read_msr(hDriver, 0, MSR_SYS_ENERGY_STATUS, curr_sys);

        if (curr_pkg < prev_pkg) curr_pkg += (1ULL << 32);
        if (curr_pp0 < prev_pp0) curr_pp0 += (1ULL << 32);
        if (curr_pp1 < prev_pp1) curr_pp1 += (1ULL << 32);
        if (curr_sys < prev_sys) curr_sys += (1ULL << 32);

        double pkg_power = (curr_pkg - prev_pkg) * energy_unit;
        double pp0_power = (curr_pp0 - prev_pp0) * energy_unit;
        double pp1_power = (curr_pp1 - prev_pp1) * energy_unit;
        double sys_power = (curr_sys - prev_sys) * energy_unit;

        prev_pkg = curr_pkg;
        prev_pp0 = curr_pp0;
        prev_pp1 = curr_pp1;
        prev_sys = curr_sys;

        uint32_t pl1_raw = mmio[0];
        uint32_t pl2_raw = mmio[1];
        double pl1_watt = (pl1_raw & 0x7FFF) * 0.125;
        double pl2_watt = (pl2_raw & 0x7FFF) * 0.125;
        bool plLocked = ((pl1_raw >> 31) & 1) != 0;

        /* temperature (package therm status: headroom below TjMax) */
        int tempC = -1;
        {
            uint64_t th = 0;
            if (tjMaxC > 0 &&
                read_msr(hDriver, 0, MSR_PACKAGE_THERM_STATUS, th) &&
                (th & (1ULL << 31)))
                tempC = tjMaxC - (int)((th >> 16) & 0x7F);
        }

        /* avg frequency via APERF/MPERF (core 0) */
        uint64_t aperf = 0, mperf = 0, c2 = 0, c6 = 0, smi = 0;
        read_msr(hDriver, 0, MSR_IA32_APERF, aperf);
        read_msr(hDriver, 0, MSR_IA32_MPERF, mperf);
        read_msr(hDriver, 0, MSR_PKG_C2_RESIDENCY, c2);
        read_msr(hDriver, 0, MSR_PKG_C6_RESIDENCY, c6);
        read_msr(hDriver, 0, MSR_SMI_COUNT, smi);
        uint64_t tsc = __rdtsc();

        double freqGHz = 0;
        if (baseMHz > 0 && mperf > prev_mperf)
            freqGHz = baseMHz * (double)(aperf - prev_aperf)
                      / (double)(mperf - prev_mperf) / 1000.0;

        double dtsc = (double)(tsc - prev_tsc);
        double c2pct = dtsc > 0 ? 100.0 * (double)(c2 - prev_c2) / dtsc : 0;
        double c6pct = dtsc > 0 ? 100.0 * (double)(c6 - prev_c6) / dtsc : 0;
        if (c6pct > c2pct) c6pct = c2pct;      /* C6 counts within C2+ */
        double c0pct = 100.0 - c2pct; if (c0pct < 0) c0pct = 0;
        double c2mid = c2pct - c6pct;
        uint64_t smiDelta = smi - prev_smi;

        double utilPct = 0.0;
        {
            FILETIME idle, kernel, user;
            if (GetSystemTimes(&idle, &kernel, &user)) {
                const auto toU64 = [](const FILETIME& ft) -> unsigned long long {
                    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32)
                           | ft.dwLowDateTime;
                };
                const double dIdle = static_cast<double>(
                    toU64(idle) - toU64(prevIdle));
                const double dTotal = static_cast<double>(
                    (toU64(kernel) - toU64(prevKernel)) +
                    (toU64(user) - toU64(prevUser)));
                if (dTotal > 0.0) {
                    utilPct = 100.0 * (1.0 - dIdle / dTotal);
                    if (utilPct < 0.0) utilPct = 0.0;
                    if (utilPct > 100.0) utilPct = 100.0;
                }
                prevIdle = idle;
                prevKernel = kernel;
                prevUser = user;
            }
        }

        prev_aperf = aperf; prev_mperf = mperf;
        prev_c2 = c2;       prev_c6 = c6;
        prev_smi = smi;     prev_tsc = tsc;

        /* PL time windows: same layout as the MSR, but this machine
         * programs PLs through MCHBAR MMIO - read tau from there */
        std::string tau1 = fmtTau((pl1_raw >> 17) & 0x7F);
        std::string tau2 = fmtTau((pl2_raw >> 17) & 0x7F);

        const char* modeName = "n/a";
        {
            uint32_t mraw = 0;
            if (EnergyDytc(DYTC_GET, mraw) && (mraw & 1))
                modeName = DecodeMode(mraw);
        }

        pd::PowerSample sample;
        sample.timestamp = LocalIsoTimestamp();
        sample.elapsedSeconds = (GetTickCount64() - monitorStarted) / 1000.0;
        sample.pkgPower = pkg_power;
        sample.iaPower = pp0_power;
        sample.gtPower = pp1_power;
        sample.sysPower = sys_power;
        sample.pl1Watt = pl1_watt;
        sample.pl2Watt = pl2_watt;
        sample.pl1Window = tau1;
        sample.pl2Window = tau2;
        sample.plLocked = plLocked;
        sample.tempC = tempC;
        sample.freqGHz = freqGHz;
        sample.c0Pct = c0pct;
        sample.c2Pct = c2mid;
        sample.c6Pct = c6pct;
        sample.utilPct = utilPct;
        sample.smiDelta = smiDelta;
        sample.mode = modeName;

        if (csv.is_open()) {
            csv << pd::CsvRow(sample) << '\n';
            csv.flush();
            if (!csv) {
                std::cerr << "CSV write failed: " << monitorOptions.csvPath
                          << std::endl;
                rc = 2;
                break;
            }
        }

        if (vtOn) {
            /* a console resize rewraps every row and breaks the in-place
             * frame addressing - repaint from scratch at the new width */
            const int columns = consoleColumns();
            const bool fullRepaint =
                columns > 0 && columns != lastConsoleColumns;
            if (fullRepaint) {
                lastConsoleColumns = columns;
                W = pickDashboardWidth();
                dashboardInfo.width = W;
            }
            hist.push_back(pkg_power);
            if (hist.size() > 60)
                hist.erase(hist.begin(), hist.end() - 60);
            const std::string dashboard =
                pd::RenderDashboard(dashboardInfo, sample, hist);

            /* flicker-free refresh: rewind to the frame top and overwrite
             * in place. Unchanged lines are only skipped over (cursor-down,
             * no cell is touched, nothing is ever blanked), changed lines
             * are rewritten at the same width so they cover the old bytes.
             * The frame is emitted as one write; no trailing newline, which
             * would scroll when the frame ends at the window bottom. */
            std::vector<std::string> frameLines;
            {
                std::istringstream dashStream(dashboard);
                std::string line;
                while (std::getline(dashStream, line))
                    frameLines.push_back(line);
            }
            std::ostringstream out;
            if (frame == 0 || fullRepaint) {
                out << "\x1b[2J\x1b[H" << dashboard;
                cursorRowsBelowFrame = 0;
            } else {
                const int rewind = static_cast<int>(previousFrame.size()) - 1 +
                                   cursorRowsBelowFrame;
                out << "\x1b[" << rewind << "A\r";
                for (std::size_t i = 0; i < frameLines.size(); ++i) {
                    const bool same = i < previousFrame.size() &&
                                      frameLines[i] == previousFrame[i];
                    if (same) {
                        if (i + 1 < frameLines.size()) out << "\x1b[B";
                    } else {
                        out << frameLines[i];
                        if (i + 1 < frameLines.size()) out << "\r\n";
                    }
                }
                if (frameLines.size() < previousFrame.size()) {
                    out << "\x1b[B\r\x1b[J";   /* frame shrank: wipe leftovers */
                    cursorRowsBelowFrame = 1;
                } else {
                    cursorRowsBelowFrame = 0;
                }
            }
            std::cout << out.str() << std::flush;
            previousFrame = std::move(frameLines);
        } else {
            /* no VT console (piped/remote): one compact line per frame */
            std::cout << "f " << std::setw(4) << frame
                      << " pkg " << std::setw(6) << sample.pkgPower
                      << " ia " << std::setw(5) << sample.iaPower
                      << " gt " << std::setw(5) << sample.gtPower
                      << " sys " << std::setw(5) << sample.sysPower
                      << " PL1 " << std::setw(5) << sample.pl1Watt
                      << " PL2 " << std::setw(5) << sample.pl2Watt;
            if (sample.tempC >= 0) std::cout << " T " << sample.tempC << "C";
            if (sample.freqGHz > 0) std::cout << " f " << sample.freqGHz << "GHz";
            std::cout << " C0 " << (int)(sample.c0Pct + 0.5) << "%"
                      << " C6+ " << (int)(sample.c6Pct + 0.5) << "%"
                      << " SMI+" << sample.smiDelta << std::endl;
        }
        frame++;
    }

    if (vtOn) std::cout << "\r\n\x1b[?25h" << std::flush;   /* restore cursor */

    // Cleanup
    MMAP_Request unmap = {};
    unmap.address.QuadPart = user_virtual;
    DeviceIoControl(hDriver, IO_CTL_MUNMAP, &unmap, sizeof(unmap), nullptr, 0, &returned, nullptr);

    } while (0);

    CloseHandle(hDriver);
    RemoveOursDriver();   /* transparent uninstall if we loaded it */
    return rc;
}

/* ================================ main ================================= */

static void Usage() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    const bool ansi = GetConsoleMode(hOut, &mode) &&
                      (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    std::cout << "\n" << pd::RenderLogo(80, ansi) << "\n\n";
    std::cout <<
        "PowerDash - Lenovo power toolbox\n"
        "\n";
    std::cout << "CPU : " << CpuIdLine() << "\n\n";
    std::cout <<
        "  PowerDash power [seconds] [--csv file]\n"
        "                                      live dashboard; optionally save CSV\n"
        "  PowerDash -setpl <PL1> <PL2>       set & lock power limits (W)\n"
        "  PowerDash mode next               inject Fn+Q notification (OSD pops)\n"
        "  PowerDash mode status             show current mode and capabilities\n"
        "  PowerDash -h                      show this help\n"
        "\n"
        "Running with no arguments (e.g. double-click) shows this help and\n"
        "leaves an open cmd prompt in this exe's folder for further commands.\n"
        "Panel: responsive 96/72-column dashboard with a PL1/PL2 gauge,\n"
        "balanced diagnostics, color status and 60-second statistics.\n"
        "\n"
        "Kernel driver (PowerDashSYS) is loaded on demand for power/-setpl/\n"
        "mode next; 'mode status' only needs Lenovo AcpiVpc.sys\n"
        "(EnergyDrv) present.\n";
}

/* double-click handoff: a console app's window dies with the process, so
 * "exit and leave a prompt waiting" is impossible directly. Instead we
 * hand the console over to a fresh `cmd.exe /K` whose CWD is this exe's
 * folder, then exit - the window stays, owned by cmd, and the user can
 * keep typing PowerDash commands right there. */
static void HandoffToCmdIfDoubledClicked() {
    DWORD procs[2] = {};
    if (GetConsoleProcessList(procs, 2) != 1)
        return;   /* launched from an existing shell: nothing to hand off */

    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(NULL, path, MAX_PATH)) {
        char* slash = strrchr(path, '\\');
        if (slash) *slash = '\0';
        SetCurrentDirectoryA(path);   /* the spawned cmd inherits this CWD */
    }

    char cl[] = "cmd.exe /K echo [PowerDash] %CD%   try: PowerDash power   "
                "PowerDash mode status   PowerDash mode next";
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    SetHandleInformation(si.hStdInput,  HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdError,  HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(NULL, cl, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return;   /* exiting now is safe: cmd keeps the console alive */
    }

    /* cmd could not be spawned - fall back to the old pause */
    std::cout << "\nPress Enter to exit . . . " << std::flush;
    std::string line;
    std::getline(std::cin, line);
}

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    ConsolePrelude();   /* clean screen + window to the front, every run */

    if (argc < 2) { Usage(); HandoffToCmdIfDoubledClicked(); return 0; }

    std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "/?") { Usage(); return 0; }
    if (cmd == "mode")   return CmdMode(argc, argv);
    if (cmd == "power") {
        std::vector<std::string> args;
        for (int i = 2; i < argc; ++i) args.emplace_back(argv[i]);
        pd::MonitorOptions options;
        std::string error;
        if (!pd::ParsePowerArguments(args, options, error)) {
            std::cerr << "power: " << error << '\n'
                      << "usage: PowerDash power [seconds] [--csv file]"
                      << std::endl;
            return 1;
        }
        return RunMonitor(argc, argv, options);
    }
    if (cmd == "-setpl") {
        if (argc != 4) {
            std::cerr << "usage: PowerDash -setpl <PL1> <PL2>   (watts)"
                      << std::endl;
            return 1;
        }
        return RunMonitor(argc, argv);
    }

    std::cerr << "unknown command: " << cmd << std::endl << std::endl;
    Usage();
    return 1;
}
