#pragma once

#include <cstddef>

namespace pred
{

class Renderer;
class FrameStats;

// The F3 overlay: frame timing, CPU/GPU time, memory, renderer stats, and the
// debug category toggles. Systems added later contribute their own rows.
class DebugOverlay
{
public:
    struct Info
    {
        const Renderer* renderer = nullptr;
        const FrameStats* stats = nullptr;
        int fixedStepsLastFrame = 0;
        double fixedStepHz = 60.0;
        bool droppedFixedTime = false;
        size_t entityCount = 0;
        size_t debugLineCount = 0;
        size_t meshesDrawn = 0;
        size_t trianglesDrawn = 0;
        size_t physicsBodies = 0;
        size_t physicsActiveBodies = 0;
        double physicsStepMs = 0.0;
        bool physicsEnabled = true;
    };

    void Draw(const Info& info);
};

} // namespace pred
