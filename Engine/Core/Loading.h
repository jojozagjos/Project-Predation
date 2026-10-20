#pragma once

namespace pred
{

// While the game is loading (Application::ShowLoading), long work calls this now and then so the loading screen keeps
// moving -- its dots walking, its line filling -- rather than standing still until the next step and jumping: another
// frame of it is drawn when one is due. Only from the thread that set it (the main one); otherwise, and when nothing is
// loading, it does nothing.
void LoadingPulse();
// Set by whatever draws the loading screen, for as long as it is up (null: taken down).
void SetLoadingPulse(void (*pulse)(void* context), void* context);

} // namespace pred
