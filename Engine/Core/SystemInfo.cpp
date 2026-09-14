#include "Engine/Core/SystemInfo.h"

#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
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
#else
    // Linux keeps the same numbers in a text file, in kilobytes. VmRSS is what is resident now and
    // VmHWM the most it has ever been, which are the two the overlay shows; VmData is the closest
    // thing to what Windows calls private bytes.
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
    {
        const auto kilobytes = [&](const char* key, uint64_t& out)
        {
            const size_t length = std::strlen(key);
            if (line.compare(0, length, key) != 0)
            {
                return;
            }
            out = static_cast<uint64_t>(std::strtoull(line.c_str() + length, nullptr, 10)) * 1024ull;
        };
        kilobytes("VmRSS:", info.workingSetBytes);
        kilobytes("VmHWM:", info.peakWorkingSetBytes);
        kilobytes("VmData:", info.privateBytes);
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
#else
    std::ifstream info("/proc/cpuinfo");
    std::string line;
    while (std::getline(info, line))
    {
        // "model name" on x86, "Hardware" or "Model" on the ARM boards this might one day run on.
        for (const char* key : {"model name", "Hardware", "Model"})
        {
            if (line.compare(0, std::strlen(key), key) != 0)
            {
                continue;
            }
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
            {
                continue;
            }
            const size_t start = line.find_first_not_of(" \t", colon + 1);
            if (start != std::string::npos)
            {
                return line.substr(start);
            }
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
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0)
    {
        return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
    }
#endif
    return 0;
}

unsigned SystemInfo::HardwareThreads()
{
    return std::thread::hardware_concurrency();
}

} // namespace pred
