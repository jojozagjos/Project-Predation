find_package(SDL3 CONFIG REQUIRED)
find_package(bgfx CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image_write.h" REQUIRED)

set(PRED_ENGINE_SOURCES
    Application.h
    Application.cpp

    Core/Log.h
    Core/Log.cpp
    Core/CVar.h
    Core/CVar.cpp
    Core/Config.h
    Core/Config.cpp
    Core/Time.h
    Core/Paths.h
    Core/Paths.cpp
    Core/SystemInfo.h
    Core/SystemInfo.cpp
    Core/Math.h

    Animation/Skeleton.h
    Animation/Skeleton.cpp
    Animation/IK.h
    Animation/IK.cpp

    Scene/Entity.h
    Scene/Scene.h
    Scene/Scene.cpp

    Physics/PhysicsLayers.h
    Physics/PhysicsWorld.h
    Physics/PhysicsWorld.cpp
    Physics/CharacterController.h
    Physics/CharacterController.cpp

    Platform/Window.h
    Platform/Window.cpp
    Platform/Input.h
    Platform/Input.cpp

    Render/Renderer.h
    Render/Renderer.cpp
    Render/ShaderLibrary.h
    Render/ShaderLibrary.cpp
    Render/DebugDraw.h
    Render/DebugDraw.cpp
    Render/Camera.h
    Render/Camera.cpp
    Render/Material.h
    Render/Mesh.h
    Render/Mesh.cpp
    Render/Primitives.h
    Render/Primitives.cpp
    Render/SceneRenderer.h
    Render/SceneRenderer.cpp
    Render/StbImpl.cpp

    Debug/ImGuiLayer.h
    Debug/ImGuiLayer.cpp
    Debug/DebugCategories.h
    Debug/DebugCategories.cpp
    Debug/Console.h
    Debug/Console.cpp
    Debug/FrameStats.h
    Debug/FrameStats.cpp
    Debug/DebugOverlay.h
    Debug/DebugOverlay.cpp

    Assets/FileWatcher.h
    Assets/FileWatcher.cpp
)

add_library(PredationEngine STATIC ${PRED_ENGINE_SOURCES})
add_library(Predation::Engine ALIAS PredationEngine)
pred_set_warnings(PredationEngine)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${PRED_ENGINE_SOURCES})

# Includes are written as "Engine/Core/Log.h" from the repository root.
target_include_directories(PredationEngine PUBLIC ${CMAKE_SOURCE_DIR})
target_include_directories(PredationEngine PRIVATE ${STB_INCLUDE_DIRS})

target_link_libraries(PredationEngine
    PUBLIC
        SDL3::SDL3
        bgfx::bgfx
        bgfx::bx
        Jolt::Jolt
        imgui::imgui
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        glm::glm
)

if(WIN32)
    target_link_libraries(PredationEngine PUBLIC psapi)
endif()

target_compile_definitions(PredationEngine
    PUBLIC
        GLM_ENABLE_EXPERIMENTAL
        BX_CONFIG_DEBUG=$<IF:$<CONFIG:Debug>,1,0>
        PRED_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        PRED_GENERATED_ASSETS_DIR="${PRED_GENERATED_ASSETS_DIR}"
        PRED_VERSION_STRING="${PROJECT_VERSION}"
)

# Compile all engine shaders for every supported backend profile.
pred_compile_shaders(
    TARGET PredationEngineShaders
    SHADER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Shaders
    OUTPUT_DIR ${PRED_GENERATED_ASSETS_DIR}
)
add_dependencies(PredationEngine PredationEngineShaders)
