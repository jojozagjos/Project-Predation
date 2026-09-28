#include "Engine/Debug/FrameStats.h"

#include <algorithm>
#include <numeric>
#include <string>

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
    // Last frame's timings, one row a name however many times it ran, and every name there has been, in the
    // order they first ran. A scope inside the fixed-step loop runs no times on some frames and three on
    // others, and listing each run as a row of its own made the overlay grow and shrink every frame and
    // show some rows twice.
    for (const Timing& timing : m_currentTimings)
    {
        if (std::find(m_names.begin(), m_names.end(), std::string(timing.name)) == m_names.end())
        {
            m_names.emplace_back(timing.name);
            m_smoothed.push_back(0.0);
        }
    }
    m_previousTimings.clear();
    for (size_t i = 0; i < m_names.size(); ++i)
    {
        double total = 0.0;
        for (const Timing& timing : m_currentTimings)
        {
            total += m_names[i] == timing.name ? timing.milliseconds : 0.0;
        }
        m_previousTimings.push_back({m_names[i].c_str(), total});
        m_smoothed[i] += (total - m_smoothed[i]) * 0.1;
    }
    m_currentTimings.clear();
    ++m_frameIndex;
}

void FrameStats::EndFrame()
{
    m_lastCpuMs = static_cast<float>(std::chrono::duration<double, std::milli>(Clock::now() - m_frameStart).count());
}

std::vector<FrameStats::Timing> FrameStats::SmoothedTimings() const
{
    std::vector<Timing> smoothed;
    for (size_t i = 0; i < m_names.size(); ++i)
    {
        smoothed.push_back({m_names[i].c_str(), m_smoothed[i]});
    }
    return smoothed;
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
