#include "Engine/Render/TextureLibrary.h"

#include "Engine/Core/Log.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <cstring>

namespace pred
{

bool DecodeImage(const uint8_t* bytes, size_t count, ImageData& out)
{
    if (bytes == nullptr || count == 0)
    {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    // Four channels always. Every backend takes rgba8 without argument, and a texture is decoded
    // once, so the memory spent on an alpha channel nobody uses is not worth a second code path.
    stbi_uc* decoded = stbi_load_from_memory(bytes, static_cast<int>(count), &width, &height, &channels, 4);
    if (decoded == nullptr)
    {
        PRED_LOG_WARN(Asset, "Could not decode an image: {}", stbi_failure_reason());
        return false;
    }

    ImageData image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::memcpy(image.pixels.data(), decoded, image.pixels.size());
    stbi_image_free(decoded);

    out = std::move(image);
    return true;
}

bool LoadImageFile(const std::string& file, ImageData& out)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(file.c_str(), &width, &height, &channels, 4);
    if (decoded == nullptr)
    {
        return false;
    }

    ImageData image;
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::memcpy(image.pixels.data(), decoded, image.pixels.size());
    stbi_image_free(decoded);

    out = std::move(image);
    return true;
}

bool WritePng(const std::string& file, const ImageData& image)
{
    if (!image.IsValid())
    {
        return false;
    }
    return stbi_write_png(file.c_str(), image.width, image.height, 4, image.pixels.data(),
                          image.width * 4) != 0;
}

void TextureLibrary::Init()
{
    if (!m_textures.empty())
    {
        return;
    }

    // One white pixel, always index zero. A material with no texture of its own samples this, so
    // the shader multiplies by one and needs no branch and no second variant.
    ImageData white;
    white.width = 1;
    white.height = 1;
    white.pixels = {255, 255, 255, 255};
    Upload(white, "white");
}

void TextureLibrary::Shutdown()
{
    for (Entry& entry : m_textures)
    {
        if (bgfx::isValid(entry.texture))
        {
            bgfx::destroy(entry.texture);
        }
    }
    m_textures.clear();
    m_byName.clear();
}

TextureHandle TextureLibrary::Upload(const ImageData& image, const std::string& name)
{
    if (const auto found = m_byName.find(name); found != m_byName.end())
    {
        return TextureHandle{found->second};
    }
    if (!image.IsValid())
    {
        PRED_LOG_ERROR(Render, "Texture '{}' has no pixels ({} by {})", name, image.width, image.height);
        return White();
    }
    if (m_textures.size() >= TextureHandle::kInvalid)
    {
        PRED_LOG_ERROR(Render, "Texture library is full, cannot upload '{}'", name);
        return White();
    }

    const bgfx::Memory* memory = bgfx::copy(image.pixels.data(), static_cast<uint32_t>(image.pixels.size()));
    const bgfx::TextureHandle texture =
        bgfx::createTexture2D(static_cast<uint16_t>(image.width), static_cast<uint16_t>(image.height),
                              false, 1, bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_NONE, memory);
    if (!bgfx::isValid(texture))
    {
        PRED_LOG_ERROR(Render, "Texture '{}' failed to upload", name);
        return White();
    }
    bgfx::setName(texture, name.c_str());

    const auto index = static_cast<uint16_t>(m_textures.size());
    m_textures.push_back({texture, name});
    m_byName.emplace(name, index);
    return TextureHandle{index};
}

TextureHandle TextureLibrary::LoadFromFile(const std::string& file, const std::string& name)
{
    if (const auto found = m_byName.find(name); found != m_byName.end())
    {
        return TextureHandle{found->second};
    }
    ImageData image;
    if (!LoadImageFile(file, image))
    {
        // Flat rather than absent. A missing texture that draws nothing looks like a broken model;
        // one that draws in its material's colour looks like a model without a texture yet.
        PRED_LOG_WARN(Render, "Texture '{}' could not be read from {}", name, file);
        return White();
    }
    PRED_LOG_INFO(Render, "Texture '{}' loaded, {} by {}", name, image.width, image.height);
    return Upload(image, name);
}

TextureHandle TextureLibrary::Find(const std::string& name) const
{
    const auto found = m_byName.find(name);
    return found == m_byName.end() ? TextureHandle{} : TextureHandle{found->second};
}

bgfx::TextureHandle TextureLibrary::Get(TextureHandle handle) const
{
    if (!handle.IsValid() || handle.index >= m_textures.size())
    {
        return m_textures.empty() ? bgfx::TextureHandle(BGFX_INVALID_HANDLE) : m_textures.front().texture;
    }
    return m_textures[handle.index].texture;
}

} // namespace pred
