#pragma once

#include <bgfx/bgfx.h>

#include <string>
#include <unordered_map>

namespace pred
{

// Loads compiled bgfx shaders from "Shaders/<profile>/<name>.bin" via the asset
// search roots and caches programs by name. Hot reload arrives with the material system.
class ShaderLibrary
{
public:
    void Init(std::string profileDir);
    void Shutdown();

    bgfx::ShaderHandle LoadShader(const std::string& name);
    bgfx::ProgramHandle LoadProgram(const std::string& vertexName, const std::string& fragmentName);

    const std::string& ProfileDir() const { return m_profileDir; }

private:
    std::string m_profileDir;
    std::unordered_map<std::string, bgfx::ShaderHandle> m_shaders;
    std::unordered_map<std::string, bgfx::ProgramHandle> m_programs;
};

} // namespace pred
