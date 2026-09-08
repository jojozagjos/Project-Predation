#pragma once

#include <cstdint>
#include <string>

namespace pred
{

struct ProcessMemoryInfo
{
    uint64_t workingSetBytes = 0;
    uint64_t privateBytes = 0;
    uint64_t peakWorkingSetBytes = 0;
};

class SystemInfo
{
public:
    static ProcessMemoryInfo QueryProcessMemory();
    static std::string CpuName();
    static uint64_t TotalPhysicalMemoryBytes();
    static unsigned HardwareThreads();
};

} // namespace pred
