#include <windows.h>
#include <intrin.h>
#include "PowerDashUi.h"
#include "PowerDashProbe.h"
#include "PowerDashSampler.h"
#include "PowerDashIoctl.h"
#include <iostream>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <thread>
#include <string>
#include <vector>

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

/* CPU vendor via the CPUID.0 vendor string - decides the probe factory's
 * dispatch ("AuthenticAMD" -> Amd, anything else -> Intel) */
static pd::Vendor CpuVendor() {
    int info[4] = {};
    __cpuid(info, 0);
    char vendor[13] = {};
    memcpy(vendor + 0, &info[1], 4);
    memcpy(vendor + 4, &info[3], 4);
    memcpy(vendor + 8, &info[2], 4);
    return !strcmp(vendor, "AuthenticAMD") ? pd::Vendor::Amd
                                           : pd::Vendor::Intel;
}

struct PCICFG_Request {
    ULONG bus, dev, func, reg, bytes;
    ULONG64 write_value;
};

struct MMAP_Request {
    LARGE_INTEGER address;
    SIZE_T size;
};

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

    /* ---- -setpl: set & lock PL1/PL2 through the MCHBAR MMIO window ----
     * (this branch maps the PL window itself; the power-monitor path gets
     * its mapping from the IntelProbe, so no shared mapping lives here) */
    if (argc == 4 && std::string(argv[1]) == "-setpl") {
        uint64_t mchbar_val = read_pci_config(hDriver, 0, 0, 0, 0x48);
        if ((mchbar_val & 0x1) == 0) {
            std::cerr << "MCHBAR is not enabled" << std::endl;
            rc = 1; break;
        }

        mchbar_val &= ~0x1;
        uint64_t mmio_phys = mchbar_val + 0x59A0;
        uint64_t mmio_page_base = mmio_phys & ~0xFFF;

        MMAP_Request mmap_req = {};
        mmap_req.address.QuadPart = mmio_page_base;
        mmap_req.size = 0x1000;

        uint64_t user_virtual = 0;
        DWORD returned = 0;
        if (!DeviceIoControl(hDriver, IO_CTL_MMAP, &mmap_req, sizeof(mmap_req), &user_virtual, sizeof(user_virtual), &returned, nullptr)) {
            std::cerr << "Failed to map MMIO address. Error code: " << GetLastError() << std::endl;
            rc = 1; break;
        }

        uint32_t* mmio = reinterpret_cast<uint32_t*>(user_virtual + (mmio_phys & 0xFFF));

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

    /* ---- static CPU facts (CPUID only; RAPL units / TjMax / thermal spec
     * / PL window statics live inside the IntelProbe) + probe wiring ---- */
    const std::string cpuLine = CpuIdLine();
    std::string brand = cpuLine, codeTag;   /* "brand  [codename]" split */
    {
        size_t tb = cpuLine.find("  [");
        if (tb != std::string::npos) {
            brand = cpuLine.substr(0, tb);
            codeTag = cpuLine.substr(tb + 3, cpuLine.size() - tb - 4);
        }
    }
    double baseMHz = 0;   /* CPUID 0x16 EAX - pcm's preferred source */
    {
        int info[4] = {};
        __cpuid(info, 0);
        if ((unsigned)info[0] >= 0x16) {
            __cpuid(info, 0x16);
            baseMHz = (double)(info[0] & 0xFFFF);
        }
    }

    pd::WindowsDriverIo io(hDriver);

    pd::PlatformInfo info;
    info.vendor = CpuVendor();
    info.cpuName = cpuLine;
    info.logicalProcessors = std::thread::hardware_concurrency();
    info.baseGHz = baseMHz / 1000.0;
    if (info.baseGHz <= 0.0) {
        /* pcm fallback: CPUID 0x16 returns 0 on some parts (observed on
         * Arrow Lake-H); PLATFORM_INFO max non-turbo ratio x 100 MHz.
         * Read through the DriverIo abstraction so no raw MSR helper
         * returns to the entry layer. */
        uint64_t pi = 0;
        if (io.ReadMsr(0, 0xCE /* MSR_PLATFORM_INFO */, pi)) {
            const unsigned ratio = (unsigned)((pi >> 8) & 0xFF);
            if (ratio) info.baseGHz = ratio * 100.0 / 1000.0;
        }
    }

    auto probe = pd::CreateProbe(io, info);   /* vendor 分流工厂 */
    if (!probe) {
        std::cerr << "Unsupported platform." << std::endl;
        rc = 1; break;
    }
    const pd::PlatformCaps& caps = probe->caps();

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

    if (vtOn) std::cout << "\x1b[?25l" << std::flush;   /* hide cursor */

    pd::DashboardInfo dashboardInfo;
    dashboardInfo.version = PD_VER;
    dashboardInfo.cpuBrand = brand;
    dashboardInfo.codeName = codeTag;
    dashboardInfo.width = W;
    dashboardInfo.csvActive = csv.is_open();
    if (dashboardInfo.csvActive) {
        const size_t slash = monitorOptions.csvPath.find_last_of("\\/");
        dashboardInfo.csvName = slash == std::string::npos
                              ? monitorOptions.csvPath
                              : monitorOptions.csvPath.substr(slash + 1);
    }
    dashboardInfo.ansi = vtOn;

    /* per-frame DYTC mode query, same call the old loop body made */
    auto QueryDytcMode = []() -> std::string {
        uint32_t mraw = 0;
        if (EnergyDytc(DYTC_GET, mraw) && (mraw & 1))
            return DecodeMode(mraw);
        return "n/a";
    };

    pd::Sampler sampler(*probe, QueryDytcMode, &g_exitRequested);

    std::vector<double> hist;   /* rolling pkg power samples (sparkline) */
    int frame = 0;
    std::vector<std::string> previousFrame;   /* last frame, line by line */
    int cursorRowsBelowFrame = 0;             /* 1 after wiping a shrank frame */
    const bool ok = sampler.Run(monitorOptions.runSeconds,
        [&](const pd::Sample& sample) {

        if (csv.is_open()) {
            csv << pd::CsvRow(caps.vendor, sample) << '\n';
            csv.flush();
            if (!csv) {
                std::cerr << "CSV write failed: " << monitorOptions.csvPath
                          << std::endl;
                rc = 2;
                g_exitRequested = true;   /* stop the sampler after this frame */
                return;
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
            hist.push_back(sample.pkgW.value);
            if (hist.size() > 60)
                hist.erase(hist.begin(), hist.end() - 60);
            const std::string dashboard =
                pd::RenderDashboard(dashboardInfo, caps, sample, hist);

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
                      << " pkg " << std::setw(6) << sample.pkgW.value
                      << " ia " << std::setw(5) << sample.coresW.value
                      << " gt " << std::setw(5) << sample.gfxW.value
                      << " sys " << std::setw(5)
                      << (sample.platformW.valid ? sample.platformW.value : 0.0)
                      << " PL1 " << std::setw(5)
                      << sample.powerLimit.sustainedW.value
                      << " PL2 " << std::setw(5)
                      << (sample.powerLimit.burstW.valid
                              ? sample.powerLimit.burstW.value
                              : 0.0);
            if (sample.tempC.valid)
                std::cout << " T " << sample.tempC.value << "C";
            if (sample.freqGHz.valid)
                std::cout << " f " << sample.freqGHz.value << "GHz";
            std::cout << " C0 " << (int)(sample.c0Pct.value + 0.5) << "%"
                      << " C6+ " << (int)(sample.c6Pct.value + 0.5) << "%"
                      << " SMI+" << sample.smiDelta.value_or(0) << std::endl;
        }
        frame++;
    });

    if (!ok) {   /* 5 consecutive frames without any valid power domain */
        std::cerr << "Telemetry lost (5 consecutive failures)." << std::endl;
        rc = 2;
    }

    if (vtOn) std::cout << "\r\n\x1b[?25h" << std::flush;   /* restore cursor */

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
        "On AMD platforms the monitor currently supports a monitoring\n"
        "subset (power, temperature, frequency, utilization, mode).\n"
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
