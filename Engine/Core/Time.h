#pragma once

#include <algorithm>
#include <chrono>

namespace pred
{

// Wall-clock frame timer. Tick() returns the seconds elapsed since the previous
// Tick(), clamped so a debugger pause or hitch cannot produce a giant step.
class FrameClock
{
public:
    FrameClock() : m_start(Clock::now()), m_last(m_start) {}

    double Tick(double maxDelta = 0.25)
    {
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - m_last).count();
        m_last = now;
        m_lastDelta = std::min(dt, maxDelta);
        return m_lastDelta;
    }

    double ElapsedSeconds() const { return std::chrono::duration<double>(Clock::now() - m_start).count(); }
    double LastDelta() const { return m_lastDelta; }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_start;
    Clock::time_point m_last;
    double m_lastDelta = 0.0;
};

// Accumulates variable frame time into fixed simulation steps.
class FixedStepAccumulator
{
public:
    explicit FixedStepAccumulator(double stepSeconds = 1.0 / 60.0, int maxStepsPerFrame = 5)
        : m_step(stepSeconds), m_maxSteps(maxStepsPerFrame)
    {
    }

    void SetStep(double stepSeconds) { m_step = stepSeconds; }
    double Step() const { return m_step; }

    // Adds frame time and returns how many fixed steps to run. If more than
    // maxStepsPerFrame would be needed the remainder is dropped (spiral of death guard).
    int Accumulate(double frameDt)
    {
        m_accumulator += frameDt;
        int steps = 0;
        while (m_accumulator >= m_step && steps < m_maxSteps)
        {
            m_accumulator -= m_step;
            ++steps;
        }
        m_droppedTime = false;
        if (m_accumulator >= m_step)
        {
            m_accumulator = 0.0;
            m_droppedTime = true;
        }
        return steps;
    }

    // Interpolation factor between the previous and current fixed state, 0..1.
    double Alpha() const { return m_step > 0.0 ? m_accumulator / m_step : 0.0; }
    bool DroppedTimeLastFrame() const { return m_droppedTime; }

private:
    double m_step;
    int m_maxSteps;
    double m_accumulator = 0.0;
    bool m_droppedTime = false;
};

} // namespace pred
