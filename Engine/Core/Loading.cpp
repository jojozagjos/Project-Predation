#include "Engine/Core/Loading.h"

#include <thread>

namespace pred
{

namespace
{
void (*g_pulse)(void*) = nullptr;
void* g_context = nullptr;
std::thread::id g_thread;
} // namespace

void LoadingPulse()
{
    if (g_pulse != nullptr && std::this_thread::get_id() == g_thread)
    {
        g_pulse(g_context);
    }
}

void SetLoadingPulse(void (*pulse)(void* context), void* context)
{
    g_pulse = pulse;
    g_context = context;
    g_thread = std::this_thread::get_id();
}

} // namespace pred
