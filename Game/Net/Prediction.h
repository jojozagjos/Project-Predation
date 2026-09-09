#pragma once

#include "Game/Player/PlayerTypes.h"

#include <cstdint>
#include <vector>

namespace pred
{

// One tick the client ran before the host had seen it, kept so it can be run again.
struct PredictedTick
{
    uint32_t sequence = 0;
    PlayerInput input;
    PlayerState state; // the result of running `input`, for comparing against what the host says
};

// The client's record of what it guessed.
//
// A client cannot wait for the host to answer before it moves: at a hundred milliseconds of round
// trip that is a sixth of a second of dead controls, and the game would feel broken. So it runs
// the same movement code immediately and remembers what it did. When the host's answer arrives it
// says which input it last ran, and the client compares its own remembered result for that tick.
// If they agree, the guess was right and there is nothing to do. If they disagree, the client
// takes the host's state as the truth and replays every input the host had not yet processed.
//
// That is the whole of client-side prediction. This class holds only the bookkeeping, so it can be
// tested without a physics world or a socket.
class PredictionBuffer
{
public:
    // Two seconds at 60 Hz. Longer than any playable round trip, so a correction never arrives for
    // a tick that has already been forgotten.
    static constexpr size_t kCapacity = 128;

    void Clear();

    void Record(uint32_t sequence, const PlayerInput& input, const PlayerState& state);

    // What the client thought it would be at that tick. Null if the tick has been dropped or was
    // never recorded, which is how a stale or invented sequence number gets rejected.
    const PredictedTick* Find(uint32_t sequence) const;

    // Overwrites the remembered result of a tick after it has been replayed.
    void UpdateState(uint32_t sequence, const PlayerState& state);

    // Everything strictly after `sequence`, oldest first: the inputs the host has not run yet, and
    // so the ones the client has to replay on top of the state the host sent.
    std::vector<PredictedTick> After(uint32_t sequence) const;

    // Forgets everything up to and including `sequence`. Those ticks are settled.
    void DropTo(uint32_t sequence);

    size_t Size() const { return m_ticks.size(); }
    bool Empty() const { return m_ticks.empty(); }
    uint32_t OldestSequence() const;
    uint32_t NewestSequence() const;

private:
    // Kept in ascending sequence order, oldest at the front.
    std::vector<PredictedTick> m_ticks;
};

// How far the host and the client are allowed to differ before it is worth correcting, and how the
// remaining error is hidden.
struct ReconciliationSettings
{
    // Below this the guess counts as right. Set from what a player can see rather than from what a
    // float can measure: correcting a millimetre every snapshot would replay the whole buffer
    // thirty times a second for nothing.
    float positionTolerance = 0.03f;
    // Past this the client has diverged so far that smoothing would look worse than a snap: it has
    // been through a wall, or a creature has grabbed it.
    float snapDistance = 2.0f;
    // How quickly the leftover visual error is blended away, per second.
    float errorDecayRate = 12.0f;
};

struct ReconciliationResult
{
    bool corrected = false;
    bool snapped = false;
    float errorDistance = 0.0f;
    uint32_t replayedTicks = 0;
};

} // namespace pred
