#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace pred
{

// Lightweight per-frame timing used by the F3 overlay. Tracy replaces the
// detailed view later; this stays as the always-available in-game summary.
class FrameStats
{
public:
    struct Timing
    {
        const char* name;
        double milliseconds;
    };

    static FrameStats& Instance();

    void BeginFrame();
    void EndFrame();
    void AddTiming(const char* name, double milliseconds);

    float FrameMs() const { return m_lastFrameMs; }
    float AverageFrameMs() const;
    float Fps() const;
    float CpuFrameMs() const { return m_lastCpuMs; }
    uint64_t FrameIndex() const { return m_frameIndex; }

    const std::vector<float>& History() const { return m_history; }
    int HistoryOffset() const { return m_historyIndex; }
    const std::vector<Timing>& Timings() const { return m_previousTimings; }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_frameStart{};
    Clock::time_point m_previousFrameStart{};
    bool m_hasPrevious = false;
    float m_lastFrameMs = 0.0f;
    float m_lastCpuMs = 0.0f;
    uint64_t m_frameIndex = 0;
    std::vector<float> m_history = std::vector<float>(240, 0.0f);
    int m_historyIndex = 0;
    std::vector<Timing> m_currentTimings;
    std::vector<Timing> m_previousTimings;
};

class ScopedTimer
{
public:
    explicit ScopedTimer(const char* name) : m_name(name), m_start(std::chrono::steady_clock::now()) {}
    ~ScopedTimer()
    {
        const auto end = std::chrono::steady_clock::now();
        FrameStats::Instance().AddTiming(m_name, std::chrono::duration<double, std::milli>(end - m_start).count());
    }

private:
    const char* m_name;
    std::chrono::steady_clock::time_point m_start;
};

} // namespace pred

#define PRED_CONCAT_INNER(a, b) a##b
#define PRED_CONCAT(a, b) PRED_CONCAT_INNER(a, b)
#define PRED_PROFILE_SCOPE(name) ::pred::ScopedTimer PRED_CONCAT(predScopedTimer_, __LINE__)(name)
