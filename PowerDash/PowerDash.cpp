#include <windows.h>
#include <iostream>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <thread>
#include <string>

#define POWERDASH_DEV_TYPE 55000
#define IO_CTL_MSR_READ        CTL_CODE(POWERDASH_DEV_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_PCICFG_READ     CTL_CODE(POWERDASH_DEV_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_MMAP            CTL_CODE(POWERDASH_DEV_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IO_CTL_MUNMAP          CTL_CODE(POWERDASH_DEV_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

constexpr auto MSR_PKG_ENERGY_STATUS = 0x611;
constexpr auto MSR_PP0_ENERGY_STATUS = 0x639;
constexpr auto MSR_PP1_ENERGY_STATUS = 0x641;
constexpr auto MSR_RAPL_POWER_UNIT = 0x606;
constexpr auto MSR_PKG_POWER_INFO = 0x614;
constexpr auto MSR_SYS_ENERGY_STATUS = 0x64D;

// Ctrl+C exit flag
volatile bool g_exitRequested = false;
BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT) {
        g_exitRequested = true;
        return TRUE;
    }
    return FALSE;
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

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    HANDLE hDriver = CreateFileA(
        "\\\\.\\POWERDASH",
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hDriver == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open driver. Error code: " << GetLastError() << std::endl;
        return 1;
    }

    uint64_t power_unit_raw = 0;
    if (!read_msr(hDriver, 0, MSR_RAPL_POWER_UNIT, power_unit_raw)) {
        std::cerr << "Failed to read MSR_RAPL_POWER_UNIT." << std::endl;
        CloseHandle(hDriver);
        return 1;
    }

    uint32_t power_unit_bits = (power_unit_raw >> 0) & 0x0F;
    uint32_t energy_unit_bits = (power_unit_raw >> 8) & 0x1F;
    uint32_t time_unit_bits = (power_unit_raw >> 16) & 0x0F;

    double power_unit = 1.0 / pow(2.0, power_unit_bits);
    double energy_unit = 1.0 / pow(2.0, energy_unit_bits);
    double time_unit = 1.0 / pow(2.0, time_unit_bits);

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Units: Power = " << power_unit << " W, Energy = "
        << energy_unit << " J, Time = " << time_unit << " s\n" << std::endl;

    uint64_t mchbar_val = read_pci_config(hDriver, 0, 0, 0, 0x48);
    if ((mchbar_val & 0x1) == 0) {
        std::cerr << "MCHBAR is not enabled" << std::endl;
        CloseHandle(hDriver);
        return 1;
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
        CloseHandle(hDriver);
        return 1;
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
        CloseHandle(hDriver);
        return 0;
    }

    // Power Info (MSR)
    uint64_t pkg_power_info = 0;
    if (read_msr(hDriver, 0, MSR_PKG_POWER_INFO, pkg_power_info)) {
        double thermal_spec_power = ((pkg_power_info >> 0) & 0x7FFF) * power_unit;
        double min_power = ((pkg_power_info >> 16) & 0x7FFF) * power_unit;
        double max_power = ((pkg_power_info >> 32) & 0x7FFF) * power_unit;

        std::cout << "Package Power Info:\n"
            << "  Thermal Spec Power: " << thermal_spec_power << " W\n"
            << "  Minimum Power     : " << min_power << " W\n"
            << "  Maximum Power     : " << max_power << " W\n" << std::endl;
    }

    // Initialize MSR values
    uint64_t prev_pkg = 0, prev_pp0 = 0, prev_pp1 = 0, prev_sys = 0;
    read_msr(hDriver, 0, MSR_PKG_ENERGY_STATUS, prev_pkg);
    read_msr(hDriver, 0, MSR_PP0_ENERGY_STATUS, prev_pp0);
    read_msr(hDriver, 0, MSR_PP1_ENERGY_STATUS, prev_pp1);
    read_msr(hDriver, 0, MSR_SYS_ENERGY_STATUS, prev_sys);

    std::cout << std::fixed << std::setprecision(2);

    // Main loop
    while (!g_exitRequested) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

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

        std::cout << "Package Power = " << pkg_power << " W"
            << " | IA Power = " << pp0_power << " W"
            << " | GT Power = " << pp1_power << " W"
            << " | System Power = " << sys_power << " W"
            << " | PL1 = " << pl1_watt << " W"
            << " | PL2 = " << pl2_watt << " W"
            << std::endl;
    }

    // Cleanup
    MMAP_Request unmap = {};
    unmap.address.QuadPart = user_virtual;
    DeviceIoControl(hDriver, IO_CTL_MUNMAP, &unmap, sizeof(unmap), nullptr, 0, &returned, nullptr);
    CloseHandle(hDriver);
    return 0;
}
