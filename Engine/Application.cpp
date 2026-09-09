#include "Engine/Application.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Config.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Core/SystemInfo.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Debug/FrameStats.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

namespace pred
{
namespace
{

CVar<int> cv_fixedHz{"app.fixed_hz", 60, "Fixed simulation tick rate in Hz"};
CVar<int> cv_maxFrames{"app.max_frames", 0, "Exit after this many frames (0 = run until closed)"};
CVar<int> cv_width{"r.width", 1600, "Window width", CVarFlags::Archive};
CVar<int> cv_height{"r.height", 900, "Window height", CVarFlags::Archive};
CVar<bool> cv_fullscreen{"r.fullscreen", false, "Fullscreen window (applied at startup)", CVarFlags::Archive};
CVar<bool> cv_vsync{"r.vsync", true, "Vertical sync", CVarFlags::Archive};
CVar<int> cv_msaa{"r.msaa", 0, "MSAA samples: 0, 2, 4, 8, 16 (applied at startup)", CVarFlags::Archive};
CVar<std::string> cv_backend{"r.backend", "auto", "Render backend: auto, dx11, dx12, vulkan, opengl",
                             CVarFlags::Archive};
CVar<bool> cv_renderDebug{"r.debug", false, "Enable graphics API validation (slow, applied at startup)"};
CVar<bool> cv_bgfxStats{"r.bgfx_stats", false, "Show bgfx's built-in statistics text"};
CVar<bool> cv_overlay{"debug.overlay", false, "Show the F3 debug overlay"};
CVar<bool> cv_physicsEnabled{"physics.enabled", true, "Step the physics simulation"};
CVar<int> cv_physicsSteps{"physics.collision_steps", 1, "Jolt collision steps per simulation tick"};

std::string TimestampForFile()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
    return buffer;
}

} // namespace

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

const char* CommandLine::Usage()
{
    return "Project Predation\n"
           "  --assets <dir>        Use this Assets directory\n"
           "  --frames <n>          Exit after n frames (useful for automated checks)\n"
           "  --screenshot <file>   Save a PNG screenshot before exiting (with --frames, use n >= 4)\n"
           "  --backend <name>      auto | dx11 | dx12 | vulkan | opengl\n"
           "  --log-level <level>   trace | debug | info | warn | error | critical | off\n"
           "  --set <cvar>=<value>  Override a cvar (repeatable)\n"
           "  +<cvar> <value>       Same as --set\n"
           "  --exec \"<command>\"    Run a console command at startup (repeatable)\n"
           "  --help                Show this text\n";
}

CommandLine CommandLine::Parse(int argc, char** argv)
{
    CommandLine result;
    auto nextValue = [&](int& i) -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h" || arg == "/?")
        {
            result.showHelp = true;
        }
        else if (arg == "--assets")
        {
            if (const char* value = nextValue(i))
            {
                result.assetsDir = value;
            }
        }
        else if (arg == "--frames")
        {
            if (const char* value = nextValue(i))
            {
                result.maxFrames = std::atoi(value);
            }
        }
        else if (arg == "--screenshot")
        {
            if (const char* value = nextValue(i))
            {
                result.screenshotPath = value;
            }
        }
        else if (arg == "--backend")
        {
            if (const char* value = nextValue(i))
            {
                result.backend = value;
            }
        }
        else if (arg == "--log-level")
        {
            if (const char* value = nextValue(i))
            {
                result.logLevel = value;
            }
        }
        else if (arg == "--set")
        {
            if (const char* value = nextValue(i))
            {
                const std::string_view text = value;
                const size_t eq = text.find('=');
                if (eq != std::string_view::npos)
                {
                    result.overrides.emplace_back(std::string(text.substr(0, eq)), std::string(text.substr(eq + 1)));
                }
                else
                {
                    std::fprintf(stderr, "--set expects <cvar>=<value>, got '%s'\n", value);
                }
            }
        }
        else if (arg == "--exec")
        {
            if (const char* value = nextValue(i))
            {
                result.execCommands.emplace_back(value);
            }
        }
        else if (!arg.empty() && arg[0] == '+')
        {
            if (const char* value = nextValue(i))
            {
                result.overrides.emplace_back(std::string(arg.substr(1)), std::string(value));
            }
        }
        else
        {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------

Application::Application() = default;

Application::~Application()
{
    ShutdownSubsystems();
}

int Application::Run(Game& game, int argc, char** argv)
{
    m_commandLine = CommandLine::Parse(argc, argv);
    if (m_commandLine.showHelp)
    {
        std::printf("%s", CommandLine::Usage());
        return 0;
    }

    Paths::Init(argc > 0 ? argv[0] : nullptr, m_commandLine.assetsDir);

    Log::InitOptions logOptions;
    logOptions.logFile = Paths::UserDataDir() / "Logs" / "predation.log";
    Log::Init(logOptions);
    if (m_commandLine.logLevel)
    {
        Log::SetGlobalLevel(spdlog::level::from_str(*m_commandLine.logLevel));
    }

    PRED_LOG_INFO(Engine, "Project Predation {} starting", PRED_VERSION_STRING);
    PRED_LOG_INFO(Engine, "Paths: {}", Paths::Describe());
    PRED_LOG_INFO(Engine, "CPU: {} ({} threads), RAM {:.1f} GB", SystemInfo::CpuName(), SystemInfo::HardwareThreads(),
                  static_cast<double>(SystemInfo::TotalPhysicalMemoryBytes()) / (1024.0 * 1024.0 * 1024.0));

    m_userSettingsFile = Paths::UserDataDir() / "settings.json";
    Config::LoadFile(Paths::AssetsRoot() / "Config" / "defaults.json");
    Config::LoadFile(m_userSettingsFile);
    ApplyCommandLineOverrides();
    if (m_commandLine.maxFrames)
    {
        cv_maxFrames.Set(*m_commandLine.maxFrames);
    }

    if (!InitSubsystems(m_commandLine))
    {
        PRED_LOG_CRITICAL(Engine, "Subsystem initialization failed");
        ShutdownSubsystems();
        Log::Shutdown();
        return 1;
    }

    if (!game.OnInit(*this))
    {
        PRED_LOG_CRITICAL(Engine, "Game initialization failed");
        ShutdownSubsystems();
        Log::Shutdown();
        return 1;
    }

    for (const std::string& command : m_commandLine.execCommands)
    {
        PRED_LOG_INFO(Engine, "--exec: {}", command);
        m_console.Execute(command);
    }

    PRED_LOG_INFO(Engine, "Entering main loop");
    FrameStats& stats = FrameStats::Instance();
    bool screenshotRequested = false;

    while (!m_quitRequested)
    {
        stats.BeginFrame();
        const double dt = m_clock.Tick();

        {
            PRED_PROFILE_SCOPE("Events");
            PumpEvents(game);
        }
        if (m_quitRequested)
        {
            break;
        }

        {
            PRED_PROFILE_SCOPE("FixedUpdate");
            m_fixedStep.SetStep(1.0 / static_cast<double>(std::max(1, cv_fixedHz.Get())));
            m_fixedStepsLastFrame = m_fixedStep.Accumulate(dt);
            for (int i = 0; i < m_fixedStepsLastFrame; ++i)
            {
                // Gameplay decides first, then physics resolves what it asked for. Both run at the
                // fixed rate so the simulation stays reproducible and network-predictable.
                game.OnFixedUpdate(m_fixedStep.Step());
                if (cv_physicsEnabled.Get())
                {
                    m_physics.Step(static_cast<float>(m_fixedStep.Step()));
                }
            }
        }

        {
            PRED_PROFILE_SCOPE("Update");
            game.OnUpdate(dt, m_fixedStep.Alpha());
        }

        const int maxFrames = cv_maxFrames.Get();
        if (maxFrames > 0 && !m_commandLine.screenshotPath.empty() && !screenshotRequested &&
            m_frameIndex + 2 >= static_cast<uint64_t>(maxFrames))
        {
            TakeScreenshot(m_commandLine.screenshotPath);
            screenshotRequested = true;
        }

        size_t debugLines = 0;
        {
            PRED_PROFILE_SCOPE("Render");
            m_renderer.BeginFrame();
            game.OnRender();
            debugLines = m_debugDraw.PendingLineCount();
            m_debugDraw.Flush(Renderer::kViewDebug);
        }

        {
            PRED_PROFILE_SCOPE("ImGui");
            m_imgui.BeginFrame();
            if (cv_overlay.Get())
            {
                DebugOverlay::Info info;
                info.renderer = &m_renderer;
                info.stats = &stats;
                info.fixedStepsLastFrame = m_fixedStepsLastFrame;
                info.fixedStepHz = 1.0 / m_fixedStep.Step();
                info.droppedFixedTime = m_fixedStep.DroppedTimeLastFrame();
                info.entityCount = m_entityCount;
                info.debugLineCount = debugLines;
                info.meshesDrawn = m_sceneRenderer.LastStats().meshesSubmitted;
                info.trianglesDrawn = m_sceneRenderer.LastStats().trianglesSubmitted;
                info.physicsBodies = m_physics.GetStats().bodyCount;
                info.physicsActiveBodies = m_physics.GetStats().activeBodyCount;
                info.physicsStepMs = m_physics.GetStats().lastStepMs;
                info.physicsEnabled = cv_physicsEnabled.Get();
                m_overlay.Draw(info);
            }
            int windowWidth = 0;
            int windowHeight = 0;
            m_window.GetSize(windowWidth, windowHeight);
            m_console.Draw(windowWidth, windowHeight);
            game.OnImGui();
            m_imgui.EndFrame();
        }

        {
            PRED_PROFILE_SCOPE("Present");
            m_renderer.EndFrame();
        }

        m_fileWatcher.Poll(m_clock.ElapsedSeconds());
        stats.EndFrame();
        ++m_frameIndex;

        if (maxFrames > 0 && m_frameIndex >= static_cast<uint64_t>(maxFrames))
        {
            PRED_LOG_INFO(Engine, "Reached app.max_frames ({}), exiting", maxFrames);
            m_quitRequested = true;
        }
    }

    PRED_LOG_INFO(Engine, "Main loop finished after {} frames", m_frameIndex);
    game.OnShutdown();
    ShutdownSubsystems();
    PRED_LOG_INFO(Engine, "Shutdown complete");
    Log::Shutdown();
    return 0;
}

bool Application::IsUiCapturingMouse() const
{
    return m_imgui.WantCaptureMouse();
}

bool Application::IsUiCapturingKeyboard() const
{
    return m_imgui.WantCaptureKeyboard();
}

bool Application::IsOverlayVisible() const
{
    return cv_overlay.Get();
}

void Application::SetOverlayVisible(bool visible)
{
    cv_overlay.Set(visible);
}

void Application::ReloadConfig()
{
    Config::LoadFile(Paths::AssetsRoot() / "Config" / "defaults.json");
    Config::LoadFile(m_userSettingsFile);
    ApplyCommandLineOverrides();
    m_console.Print("Config reloaded", Console::kColorMuted);
}

void Application::TakeScreenshot(const std::filesystem::path& pngPath)
{
    m_renderer.RequestScreenshot(pngPath);
    PRED_LOG_INFO(Engine, "Screenshot requested: {}", pngPath.string());
}

void Application::ApplyCommandLineOverrides()
{
    for (const auto& [name, value] : m_commandLine.overrides)
    {
        Config::ApplyOverride(name, value, "command line");
    }
}

bool Application::InitSubsystems(const CommandLine& commandLine)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        PRED_LOG_CRITICAL(Platform, "SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    m_sdlInitialized = true;
    PRED_LOG_INFO(Platform, "SDL {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);

    WindowDesc windowDesc;
    windowDesc.title = "Project Predation";
    windowDesc.width = std::max(320, cv_width.Get());
    windowDesc.height = std::max(240, cv_height.Get());
    windowDesc.fullscreen = cv_fullscreen.Get();
    if (!m_window.Create(windowDesc))
    {
        return false;
    }

    int pixelWidth = 0;
    int pixelHeight = 0;
    m_window.GetSizeInPixels(pixelWidth, pixelHeight);

    RendererDesc rendererDesc;
    rendererDesc.nativeWindowHandle = m_window.NativeHandle();
    rendererDesc.width = pixelWidth;
    rendererDesc.height = pixelHeight;
    rendererDesc.vsync = cv_vsync.Get();
    rendererDesc.msaa = cv_msaa.Get();
    rendererDesc.backend = commandLine.backend.value_or(cv_backend.Get());
    rendererDesc.debug = cv_renderDebug.Get();
    if (!m_renderer.Init(rendererDesc))
    {
        return false;
    }
    m_renderer.SetBgfxStatsOverlay(cv_bgfxStats.Get());
    cv_vsync.OnChange([this](CVarBase&) { m_renderer.SetVSync(cv_vsync.Get()); });
    cv_bgfxStats.OnChange([this](CVarBase&) { m_renderer.SetBgfxStatsOverlay(cv_bgfxStats.Get()); });

    m_shaders.Init(m_renderer.ShaderProfileDir());
    if (!m_debugDraw.Init(m_shaders))
    {
        return false;
    }
    if (!m_sceneRenderer.Init(m_shaders))
    {
        return false;
    }

    PhysicsWorld::Settings physicsSettings;
    physicsSettings.collisionSteps = std::max(1, cv_physicsSteps.Get());
    if (!m_physics.Init(physicsSettings))
    {
        return false;
    }
    if (!m_imgui.Init(m_window, m_renderer, m_shaders))
    {
        return false;
    }

    m_console.Init();
    m_console.AttachLogSink();
    RegisterCoreCommands();

    const std::filesystem::path bindingsFile = Paths::AssetsRoot() / "Config" / "input.json";
    m_input.LoadBindings(bindingsFile);
    m_fileWatcher.Watch(bindingsFile, [this](const std::filesystem::path& path) { m_input.LoadBindings(path); });
    m_fileWatcher.Watch(Paths::AssetsRoot() / "Config" / "defaults.json",
                        [this](const std::filesystem::path&) { ReloadConfig(); });

    m_subsystemsInitialized = true;
    return true;
}

void Application::ShutdownSubsystems()
{
    if (m_subsystemsInitialized)
    {
        Config::SaveArchive(m_userSettingsFile);
    }
    cv_vsync.ClearOnChange();
    cv_bgfxStats.ClearOnChange();
    m_fileWatcher.Clear();
    m_console.Shutdown();
    m_imgui.Shutdown();
    m_debugDraw.Shutdown();
    m_physics.Shutdown();
    m_sceneRenderer.Shutdown();
    // GPU buffers must go before the shader library and the device itself.
    m_meshes.Shutdown();
    m_shaders.Shutdown();
    m_renderer.Shutdown();
    m_window.Destroy();
    if (m_sdlInitialized)
    {
        SDL_Quit();
        m_sdlInitialized = false;
    }
    m_subsystemsInitialized = false;
}

void Application::PumpEvents(Game& game)
{
    m_input.BeginFrame();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            m_quitRequested = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            m_renderer.Resize(event.window.data1, event.window.data2);
            break;
        default:
            break;
        }

        const bool consumed = HandleHotkey(event);
        if (!consumed)
        {
            m_imgui.ProcessEvent(event);
        }
        m_input.HandleEvent(event);
        game.OnEvent(event);
    }

    // While the mouse is captured the OS cursor does not move, so ImGui's idea of where the pointer
    // is freezes wherever it last was. If that happened to be over a panel, it reports wanting the
    // mouse for ever, and the game stops receiving any mouse movement at all: opening the debug
    // overlay would silently take mouse look away until the pointer was released and moved. A
    // captured mouse belongs to the game, full stop.
    const bool captured = m_window.IsRelativeMouse();
    m_input.SetBlocked(m_console.IsOpen() || m_imgui.WantCaptureKeyboard(),
                       !captured && m_imgui.WantCaptureMouse());
}

bool Application::HandleHotkey(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_KEY_DOWN)
    {
        if (event.key.repeat)
        {
            return false;
        }
        const SDL_Scancode key = event.key.scancode;
        if (m_input.ActionHasKey("console", key))
        {
            m_console.Toggle();
            m_swallowNextGraveText = true;
            return true;
        }
        m_swallowNextGraveText = false;
        if (m_input.ActionHasKey("debug_overlay", key))
        {
            cv_overlay.Set(!cv_overlay.Get());
            return true;
        }
        if (m_input.ActionHasKey("screenshot", key))
        {
            TakeScreenshot(Paths::UserDataDir() / "Screenshots" / (TimestampForFile() + ".png"));
            return true;
        }
    }
    else if (event.type == SDL_EVENT_KEY_UP)
    {
        const SDL_Scancode key = event.key.scancode;
        if (m_input.ActionHasKey("console", key) || m_input.ActionHasKey("debug_overlay", key) ||
            m_input.ActionHasKey("screenshot", key))
        {
            return true;
        }
    }
    else if (event.type == SDL_EVENT_TEXT_INPUT && m_swallowNextGraveText)
    {
        m_swallowNextGraveText = false;
        if (event.text.text != nullptr && std::strcmp(event.text.text, "`") == 0)
        {
            return true;
        }
    }
    return false;
}

void Application::RegisterCoreCommands()
{
    m_console.RegisterCommand("quit", "Exit the application",
                              [this](const std::vector<std::string>&) { RequestQuit(); });
    m_console.RegisterCommand("exit", "Exit the application",
                              [this](const std::vector<std::string>&) { RequestQuit(); });

    m_console.RegisterCommand("reload_config", "Reload defaults.json and user settings",
                              [this](const std::vector<std::string>&) { ReloadConfig(); });

    m_console.RegisterCommand(
        "screenshot", "Save a PNG screenshot to the user Screenshots folder",
        [this](const std::vector<std::string>& args)
        {
            const std::string name = args.size() > 1 ? args[1] : TimestampForFile();
            TakeScreenshot(Paths::UserDataDir() / "Screenshots" / (name + ".png"));
        },
        "screenshot [name]");

    m_console.RegisterCommand(
        "debug", "Toggle a debug category: debug <category> [on|off]",
        [this](const std::vector<std::string>& args)
        {
            if (args.size() < 2)
            {
                std::string list = "Categories:";
                for (size_t i = 0; i < static_cast<size_t>(DebugCategory::Count); ++i)
                {
                    const auto category = static_cast<DebugCategory>(i);
                    list += ' ';
                    list += DebugCategories::Name(category);
                    list += DebugCategories::IsEnabled(category) ? "(on)" : "(off)";
                }
                m_console.Print(list);
                return;
            }
            DebugCategory category;
            if (!DebugCategories::Parse(args[1], category))
            {
                m_console.PrintError("Unknown debug category: " + args[1]);
                return;
            }
            bool enabled = !DebugCategories::IsEnabled(category);
            if (args.size() > 2)
            {
                enabled = (args[2] == "on" || args[2] == "1" || args[2] == "true");
            }
            DebugCategories::SetEnabled(category, enabled);
            m_console.Print(std::string(DebugCategories::Name(category)) + (enabled ? " on" : " off"));
        },
        "debug <category> [on|off]");

    m_console.RegisterCommand("paths", "Print executable, asset and user directories",
                              [this](const std::vector<std::string>&) { m_console.Print(Paths::Describe()); });

    m_console.RegisterCommand("sysinfo", "Print CPU, memory and renderer information",
                              [this](const std::vector<std::string>&)
                              {
                                  const ProcessMemoryInfo memory = SystemInfo::QueryProcessMemory();
                                  char buffer[512];
                                  std::snprintf(buffer, sizeof(buffer),
                                                "CPU %s (%u threads)  RAM %.1f GB  working set %.1f MB  renderer %s",
                                                SystemInfo::CpuName().c_str(), SystemInfo::HardwareThreads(),
                                                static_cast<double>(SystemInfo::TotalPhysicalMemoryBytes()) /
                                                    (1024.0 * 1024.0 * 1024.0),
                                                static_cast<double>(memory.workingSetBytes) / (1024.0 * 1024.0),
                                                m_renderer.BackendName());
                                  m_console.Print(buffer);
                              });
}

} // namespace pred
