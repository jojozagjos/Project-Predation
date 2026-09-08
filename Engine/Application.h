#pragma once

#include "Engine/Assets/FileWatcher.h"
#include "Engine/Core/Time.h"
#include "Engine/Debug/Console.h"
#include "Engine/Debug/DebugOverlay.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/Window.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Renderer.h"
#include "Engine/Render/SceneRenderer.h"
#include "Engine/Render/ShaderLibrary.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

union SDL_Event;

namespace pred
{

class Application;

// The game plugs into the engine loop through this interface.
class Game
{
public:
    virtual ~Game() = default;

    virtual bool OnInit(Application& app) = 0;
    virtual void OnShutdown() {}

    virtual void OnEvent(const SDL_Event& event) {}
    virtual void OnFixedUpdate(double fixedDt) {}
    virtual void OnUpdate(double dt, double alpha) {}
    virtual void OnRender() {}
    virtual void OnImGui() {}
};

struct CommandLine
{
    std::filesystem::path assetsDir;
    std::optional<int> maxFrames;
    std::filesystem::path screenshotPath;
    std::optional<std::string> backend;
    std::optional<std::string> logLevel;
    std::vector<std::pair<std::string, std::string>> overrides;
    // Console commands run once, immediately after the game initializes. Makes headless
    // verification scriptable: position the camera, spawn things, then screenshot.
    std::vector<std::string> execCommands;
    bool showHelp = false;
    bool noWindowFocus = false;

    static CommandLine Parse(int argc, char** argv);
    static const char* Usage();
};

class Application
{
public:
    Application();
    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int Run(Game& game, int argc, char** argv);
    void RequestQuit() { m_quitRequested = true; }

    Window& GetWindow() { return m_window; }
    Input& GetInput() { return m_input; }
    Renderer& GetRenderer() { return m_renderer; }
    ShaderLibrary& GetShaders() { return m_shaders; }
    DebugDraw& GetDebugDraw() { return m_debugDraw; }
    Console& GetConsole() { return m_console; }
    FileWatcher& GetFileWatcher() { return m_fileWatcher; }
    MeshLibrary& GetMeshes() { return m_meshes; }
    SceneRenderer& GetSceneRenderer() { return m_sceneRenderer; }
    PhysicsWorld& GetPhysics() { return m_physics; }

    // Reported by the game each frame so the F3 overlay can show world statistics.
    void SetEntityCount(size_t count) { m_entityCount = count; }

    double FixedStepSeconds() const { return m_fixedStep.Step(); }
    double TimeSeconds() const { return m_clock.ElapsedSeconds(); }
    uint64_t FrameIndex() const { return m_frameIndex; }

    bool IsConsoleOpen() const { return m_console.IsOpen(); }
    bool IsOverlayVisible() const;
    void SetOverlayVisible(bool visible);

    void ReloadConfig();
    void TakeScreenshot(const std::filesystem::path& pngPath);

private:
    bool InitSubsystems(const CommandLine& commandLine);
    void ShutdownSubsystems();
    void PumpEvents(Game& game);
    bool HandleHotkey(const SDL_Event& event);
    void RegisterCoreCommands();
    void ApplyCommandLineOverrides();

    Window m_window;
    Input m_input;
    Renderer m_renderer;
    ShaderLibrary m_shaders;
    MeshLibrary m_meshes;
    SceneRenderer m_sceneRenderer;
    PhysicsWorld m_physics;
    DebugDraw m_debugDraw;
    ImGuiLayer m_imgui;
    Console m_console;
    DebugOverlay m_overlay;
    FileWatcher m_fileWatcher;

    FrameClock m_clock;
    FixedStepAccumulator m_fixedStep;
    CommandLine m_commandLine;
    std::filesystem::path m_userSettingsFile;

    uint64_t m_frameIndex = 0;
    size_t m_entityCount = 0;
    int m_fixedStepsLastFrame = 0;
    bool m_quitRequested = false;
    bool m_subsystemsInitialized = false;
    bool m_sdlInitialized = false;
    bool m_swallowNextGraveText = false;
};

} // namespace pred
