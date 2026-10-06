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
    // And one flat normal, always index one.
    ImageData flat;
    flat.width = 1;
    flat.height = 1;
    flat.pixels = {128, 128, 255, 255};
    Upload(flat, "flat_normal");
}

TextureHandle TextureLibrary::LoadSurfaceMap(const std::string& file, const std::string& name)
{
    if (const auto found = m_byName.find(name); found != m_byName.end())
    {
        return TextureHandle{found->second};
    }
    ImageData image;
    if (!LoadImageFile(file, image) || !image.IsValid())
    {
        PRED_LOG_WARN(Render, "Surface map '{}' could not be read from {}", name, file);
        return White();
    }
    if (m_textures.size() >= TextureHandle::kInvalid)
    {
        return White();
    }
    // Its average colour, in linear light: a photograph has a colour of its own, and a surface tinted to a world's colour
    // wants only its detail (see Surfaces::Apply).
    glm::vec3 mean{0.0f};
    {
        float linear[256];
        for (int v = 0; v < 256; ++v)
        {
            linear[v] = std::pow(static_cast<float>(v) / 255.0f, 2.2f);
        }
        double sum[3] = {0.0, 0.0, 0.0};
        const size_t pixels = image.pixels.size() / 4;
        for (size_t i = 0; i < pixels; ++i)
        {
            for (int c = 0; c < 3; ++c)
            {
                sum[c] += linear[image.pixels[i * 4 + static_cast<size_t>(c)]];
            }
        }
        const double count = static_cast<double>(std::max<size_t>(pixels, 1));
        mean = glm::vec3(static_cast<float>(sum[0] / count), static_cast<float>(sum[1] / count), static_cast<float>(sum[2] / count));
    }
    // Every smaller copy, each the average of four of the one before, laid one after another as bgfx wants them.
    std::vector<uint8_t> all = image.pixels;
    int width = image.width;
    int height = image.height;
    std::vector<uint8_t> level = image.pixels;
    uint8_t mips = 1;
    while (width > 1 || height > 1)
    {
        const int nextWidth = std::max(width / 2, 1);
        const int nextHeight = std::max(height / 2, 1);
        std::vector<uint8_t> next(static_cast<size_t>(nextWidth * nextHeight * 4));
        for (int y = 0; y < nextHeight; ++y)
        {
            for (int x = 0; x < nextWidth; ++x)
            {
                for (int c = 0; c < 4; ++c)
                {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                    {
                        for (int dx = 0; dx < 2; ++dx)
                        {
                            const int sx = std::min(x * 2 + dx, width - 1);
                            const int sy = std::min(y * 2 + dy, height - 1);
                            sum += level[static_cast<size_t>((sy * width + sx) * 4 + c)];
                        }
                    }
                    next[static_cast<size_t>((y * nextWidth + x) * 4 + c)] = static_cast<uint8_t>((sum + 2) / 4);
                }
            }
        }
        all.insert(all.end(), next.begin(), next.end());
        level = std::move(next);
        width = nextWidth;
        height = nextHeight;
        ++mips;
    }
    const bgfx::Memory* memory = bgfx::copy(all.data(), static_cast<uint32_t>(all.size()));
    const bgfx::TextureHandle texture =
        bgfx::createTexture2D(static_cast<uint16_t>(image.width), static_cast<uint16_t>(image.height), true, 1, bgfx::TextureFormat::RGBA8,
                              BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC, memory);
    if (!bgfx::isValid(texture))
    {
        PRED_LOG_ERROR(Render, "Surface map '{}' failed to upload", name);
        return White();
    }
    bgfx::setName(texture, name.c_str());
    const auto index = static_cast<uint16_t>(m_textures.size());
    m_textures.push_back({texture, name});
    m_byName.emplace(name, index);
    m_means[index] = mean;
    PRED_LOG_INFO(Render, "Surface map '{}': {} by {}, {} levels", name, image.width, image.height, static_cast<int>(mips));
    return TextureHandle{index};
}

glm::vec3 TextureLibrary::MeanColour(TextureHandle handle) const
{
    const auto found = m_means.find(handle.index);
    return found != m_means.end() ? found->second : glm::vec3(1.0f);
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

TextureHandle TextureLibrary::CreateDynamic(int width, int height, const std::string& name)
{
    if (const auto found = m_byName.find(name); found != m_byName.end())
    {
        return TextureHandle{found->second};
    }
    if (m_textures.size() >= TextureHandle::kInvalid || width <= 0 || height <= 0)
    {
        return White();
    }
    // No pixels given: it can be updated afterwards, as a screen is.
    const bgfx::TextureHandle texture =
        bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_NONE);
    if (!bgfx::isValid(texture))
    {
        return White();
    }
    bgfx::setName(texture, name.c_str());
    const auto index = static_cast<uint16_t>(m_textures.size());
    m_textures.push_back({texture, name});
    m_byName.emplace(name, index);
    return TextureHandle{index};
}

void TextureLibrary::Update(TextureHandle handle, const ImageData& image)
{
    if (!handle.IsValid() || handle.index == 0 || handle.index >= m_textures.size() || !image.IsValid())
    {
        return;
    }
    bgfx::updateTexture2D(m_textures[handle.index].texture, 0, 0, 0, 0, static_cast<uint16_t>(image.width), static_cast<uint16_t>(image.height),
                          bgfx::copy(image.pixels.data(), static_cast<uint32_t>(image.pixels.size())));
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
