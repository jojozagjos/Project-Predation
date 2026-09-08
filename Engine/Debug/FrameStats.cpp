#include "Engine/Debug/FrameStats.h"

#include <numeric>

namespace pred
{

FrameStats& FrameStats::Instance()
{
    static FrameStats stats;
    return stats;
}

void FrameStats::BeginFrame()
{
    const auto now = Clock::now();
    if (m_hasPrevious)
    {
        m_lastFrameMs = static_cast<float>(std::chrono::duration<double, std::milli>(now - m_previousFrameStart).count());
        m_history[static_cast<size_t>(m_historyIndex)] = m_lastFrameMs;
        m_historyIndex = (m_historyIndex + 1) % static_cast<int>(m_history.size());
    }
    m_previousFrameStart = now;
    m_frameStart = now;
    m_hasPrevious = true;
    m_previousTimings.swap(m_currentTimings);
    m_currentTimings.clear();
    ++m_frameIndex;
}

void FrameStats::EndFrame()
{
    m_lastCpuMs = static_cast<float>(std::chrono::duration<double, std::milli>(Clock::now() - m_frameStart).count());
}

void FrameStats::AddTiming(const char* name, double milliseconds)
{
    m_currentTimings.push_back({name, milliseconds});
}

float FrameStats::AverageFrameMs() const
{
    const size_t window = 60;
    double sum = 0.0;
    size_t count = 0;
    for (size_t i = 0; i < window && i < m_history.size(); ++i)
    {
        const int index = (m_historyIndex - 1 - static_cast<int>(i) + static_cast<int>(m_history.size()) * 2) %
                          static_cast<int>(m_history.size());
        const float value = m_history[static_cast<size_t>(index)];
        if (value > 0.0f)
        {
            sum += value;
            ++count;
        }
    }
    return count > 0 ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
}

float FrameStats::Fps() const
{
    const float average = AverageFrameMs();
    return average > 0.0f ? 1000.0f / average : 0.0f;
}

} // namespace pred
