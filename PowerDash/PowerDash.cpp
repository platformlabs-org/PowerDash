#include <iostream>
#include <vector>
#include <iomanip>
#include <windows.h>
#include "./src/cpucounters.h"
#include "./src/topology.h"

using namespace pcm;
using namespace std;

// 打印系统级指标
void printSystemMetrics(PCM* m, const SystemCounterState& s1, const SystemCounterState& s2,
    const CoreCounterState& c1, const CoreCounterState& c2) {
    cout << "\n--- System Metrics (1s interval) ---\n";
    cout << fixed << setprecision(6);

    if (m->isCoreCStateResidencySupported(0))
        cout << "UTIL          : " << getCoreCStateResidency(0, s1, s2) * 100.0 << " %\n";

    cout << "IPC           : " << getIPC(c1, c2) << "\n";

    if (m->isActiveRelativeFrequencyAvailable())
        cout << "CFREQ (GHz)   : " << getActiveRelativeFrequency(c1, c2) << "\n";

    if (m->memoryTrafficMetricsAvailable()) {
        cout << "READ (GB)     : " << getBytesReadFromMC(s1, s2) / 1e9 << "\n";
        cout << "WRITE (GB)    : " << getBytesWrittenToMC(s1, s2) / 1e9 << "\n";
    }

    if (m->localMemoryRequestRatioMetricAvailable())
        cout << "LOCAL (%)     : " << getLocalMemoryRequestRatio(s1, s2) * 100.0 << "\n";

    if (m->PMMTrafficMetricsAvailable()) {
        cout << "PMM RD (GB)   : " << getBytesReadFromPMM(s1, s2) / 1e9 << "\n";
        cout << "PMM WR (GB)   : " << getBytesWrittenToPMM(s1, s2) / 1e9 << "\n";
    }

    if (m->HBMmemoryTrafficMetricsAvailable()) {
        cout << "HBM RD (GB)   : " << getBytesReadFromEDC(s1, s2) / 1e9 << "\n";
        cout << "HBM WR (GB)   : " << getBytesWrittenToEDC(s1, s2) / 1e9 << "\n";
    }

    if (m->memoryIOTrafficMetricAvailable()) {
        cout << "IO (GB)       : " << getIORequestBytesFromMC(s1, s2) / 1e9 << "\n";
        cout << "IA (GB)       : " << getIARequestBytesFromMC(s1, s2) / 1e9 << "\n";
        cout << "GT (GB)       : " << getGTRequestBytesFromMC(s1, s2) / 1e9 << "\n";
    }

    if (m->packageEnergyMetricsAvailable())
        cout << "CPU Energy (W): " << getConsumedJoules(s1, s2) << "\n";

    if (m->dramEnergyMetricsAvailable())
        cout << "DIMM Energy(W): " << getDRAMConsumedJoules(s1, s2) << "\n";

    cout << "------------------------------------\n";
}

// 打印每个 socket 的 PP0 / PP1 能耗
void printSocketEnergy(PCM* m,
    const vector<SocketCounterState>& skt1,
    const vector<SocketCounterState>& skt2) {
    if (!m->ppEnergyMetricsAvailable()) return;

    const uint32 sockets = m->getNumSockets();
    for (uint32 i = 0; i < sockets; ++i) {
        cout << "Socket " << i << ":\n";
        cout << "  PP0 (IA) Power (w): " << getConsumedJoules(0, skt1[i], skt2[i]) << "\n";
        cout << "  PP1 (GT) Power (w): " << getConsumedJoules(1, skt1[i], skt2[i]) << "\n";
    }
    cout << "------------------------------------\n";
}

int main() {
    // 静默所有 std::cerr 输出
    static null_stream ns;
    std::cerr.rdbuf(&ns);

    PCM* m = PCM::getInstance();
    if (!m->memoryTrafficMetricsAvailable() &&
        !m->packageEnergyMetricsAvailable() &&
        !m->ppEnergyMetricsAvailable()) {
        cerr << "Required metrics not available.\n";
        return EXIT_FAILURE;
    }

    auto status = m->program(PCM::DEFAULT_EVENTS, nullptr);
    if (status != PCM::Success) {
        cerr << "PCM init failed: ";
        if (status == PCM::MSRAccessDenied)
            cerr << "MSR access denied.\n";
        else if (status == PCM::PMUBusy) {
            cerr << "PMU busy, attempting reset...\n";
            m->resetPMU();
            if (m->program(PCM::DEFAULT_EVENTS, nullptr) != PCM::Success) {
                cerr << "PCM reprogramming failed after reset.\n";
                return EXIT_FAILURE;
            }
            else {
                cout << "PCM reprogrammed after reset.\n";
            }
        }
        else {
            cerr << "Unknown error.\n";
            return EXIT_FAILURE;
        }
    }

    cout << "Press Ctrl+C to stop...\n";

    while (true) {
        vector<CoreCounterState> c1, c2;
        vector<SocketCounterState> s1, s2;
        SystemCounterState sys1, sys2;

        m->getAllCounterStates(sys1, s1, c1);
        Sleep(1000);
        m->getAllCounterStates(sys2, s2, c2);

        if (!c1.empty() && !c2.empty()) {
            printSystemMetrics(m, sys1, sys2, c1[0], c2[0]);
            printSocketEnergy(m, s1, s2);
        }
        else {
            cerr << "Failed to read core states.\n";
        }
    }

    m->cleanup();
    return 0;
}
