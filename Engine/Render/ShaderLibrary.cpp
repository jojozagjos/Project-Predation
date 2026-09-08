#include "Engine/Render/ShaderLibrary.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"

#include <fstream>
#include <vector>

namespace pred
{

void ShaderLibrary::Init(std::string profileDir)
{
    m_profileDir = std::move(profileDir);
    PRED_LOG_INFO(Render, "Shader library profile: {}", m_profileDir);
}

void ShaderLibrary::Shutdown()
{
    for (auto& [name, program] : m_programs)
    {
        if (bgfx::isValid(program))
        {
            bgfx::destroy(program);
        }
    }
    m_programs.clear();
    for (auto& [name, shader] : m_shaders)
    {
        if (bgfx::isValid(shader))
        {
            bgfx::destroy(shader);
        }
    }
    m_shaders.clear();
}

bgfx::ShaderHandle ShaderLibrary::LoadShader(const std::string& name)
{
    if (const auto it = m_shaders.find(name); it != m_shaders.end())
    {
        return it->second;
    }

    const std::filesystem::path relative = std::filesystem::path("Shaders") / m_profileDir / (name + ".bin");
    const auto resolved = Paths::Resolve(relative);
    if (!resolved)
    {
        PRED_LOG_ERROR(Render, "Shader not found: {} (searched asset roots)", relative.string());
        return BGFX_INVALID_HANDLE;
    }

    std::ifstream stream(*resolved, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        PRED_LOG_ERROR(Render, "Shader file is empty or unreadable: {}", resolved->string());
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::Memory* memory = bgfx::copy(bytes.data(), static_cast<uint32_t>(bytes.size()));
    const bgfx::ShaderHandle handle = bgfx::createShader(memory);
    if (!bgfx::isValid(handle))
    {
        PRED_LOG_ERROR(Render, "bgfx rejected shader: {}", resolved->string());
        return BGFX_INVALID_HANDLE;
    }
    bgfx::setName(handle, name.c_str());
    m_shaders[name] = handle;
    PRED_LOG_DEBUG(Render, "Loaded shader {} from {}", name, resolved->string());
    return handle;
}

bgfx::ProgramHandle ShaderLibrary::LoadProgram(const std::string& vertexName, const std::string& fragmentName)
{
    const std::string key = vertexName + "|" + fragmentName;
    if (const auto it = m_programs.find(key); it != m_programs.end())
    {
        return it->second;
    }

    const bgfx::ShaderHandle vs = LoadShader(vertexName);
    const bgfx::ShaderHandle fs = LoadShader(fragmentName);
    if (!bgfx::isValid(vs) || !bgfx::isValid(fs))
    {
        PRED_LOG_ERROR(Render, "Cannot create program {} + {}", vertexName, fragmentName);
        return BGFX_INVALID_HANDLE;
    }

    const bgfx::ProgramHandle program = bgfx::createProgram(vs, fs, false);
    if (!bgfx::isValid(program))
    {
        PRED_LOG_ERROR(Render, "bgfx::createProgram failed for {} + {}", vertexName, fragmentName);
        return BGFX_INVALID_HANDLE;
    }
    m_programs[key] = program;
    return program;
}

} // namespace pred
