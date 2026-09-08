#include "Engine/Core/SystemInfo.h"

#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace pred
{

ProcessMemoryInfo SystemInfo::QueryProcessMemory()
{
    ProcessMemoryInfo info;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)))
    {
        info.workingSetBytes = counters.WorkingSetSize;
        info.privateBytes = counters.PrivateUsage;
        info.peakWorkingSetBytes = counters.PeakWorkingSetSize;
    }
#endif
    return info;
}

std::string SystemInfo::CpuName()
{
#ifdef _WIN32
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) ==
        ERROR_SUCCESS)
    {
        char buffer[256] = {};
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        const LONG status = RegQueryValueExA(key, "ProcessorNameString", nullptr, &type,
                                             reinterpret_cast<LPBYTE>(buffer), &size);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS && type == REG_SZ)
        {
            return std::string(buffer);
        }
    }
#endif
    return "unknown";
}

uint64_t SystemInfo::TotalPhysicalMemoryBytes()
{
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
    {
        return status.ullTotalPhys;
    }
#endif
    return 0;
}

unsigned SystemInfo::HardwareThreads()
{
    return std::thread::hardware_concurrency();
}

} // namespace pred
