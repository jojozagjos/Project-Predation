#include "Game/Net/Prediction.h"

#include <algorithm>

namespace pred
{

void PredictionBuffer::Clear()
{
    m_ticks.clear();
}

void PredictionBuffer::Record(uint32_t sequence, const PlayerInput& input, const PlayerState& state)
{
    // Re-recording a tick replaces it. That happens when a replay reruns a tick the client had
    // already guessed at, and the newer answer is the better one.
    const auto found = std::find_if(m_ticks.begin(), m_ticks.end(),
                                    [&](const PredictedTick& tick) { return tick.sequence == sequence; });
    if (found != m_ticks.end())
    {
        found->input = input;
        found->state = state;
        return;
    }

    PredictedTick tick;
    tick.sequence = sequence;
    tick.input = input;
    tick.state = state;
    m_ticks.push_back(std::move(tick));

    // Almost always already in order, so this is a no-op after one comparison.
    if (m_ticks.size() > 1 && m_ticks[m_ticks.size() - 2].sequence > sequence)
    {
        std::sort(m_ticks.begin(), m_ticks.end(),
                  [](const PredictedTick& a, const PredictedTick& b) { return a.sequence < b.sequence; });
    }

    while (m_ticks.size() > kCapacity)
    {
        m_ticks.erase(m_ticks.begin());
    }
}

const PredictedTick* PredictionBuffer::Find(uint32_t sequence) const
{
    const auto found = std::find_if(m_ticks.begin(), m_ticks.end(),
                                    [&](const PredictedTick& tick) { return tick.sequence == sequence; });
    return found == m_ticks.end() ? nullptr : &*found;
}

void PredictionBuffer::UpdateState(uint32_t sequence, const PlayerState& state)
{
    const auto found = std::find_if(m_ticks.begin(), m_ticks.end(),
                                    [&](const PredictedTick& tick) { return tick.sequence == sequence; });
    if (found != m_ticks.end())
    {
        found->state = state;
    }
}

std::vector<PredictedTick> PredictionBuffer::After(uint32_t sequence) const
{
    std::vector<PredictedTick> result;
    for (const PredictedTick& tick : m_ticks)
    {
        if (tick.sequence > sequence)
        {
            result.push_back(tick);
        }
    }
    return result;
}

void PredictionBuffer::DropTo(uint32_t sequence)
{
    const auto end = std::find_if(m_ticks.begin(), m_ticks.end(),
                                  [&](const PredictedTick& tick) { return tick.sequence > sequence; });
    m_ticks.erase(m_ticks.begin(), end);
}

uint32_t PredictionBuffer::OldestSequence() const
{
    return m_ticks.empty() ? 0u : m_ticks.front().sequence;
}

uint32_t PredictionBuffer::NewestSequence() const
{
    return m_ticks.empty() ? 0u : m_ticks.back().sequence;
}

} // namespace pred
