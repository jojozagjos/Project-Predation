#include "Engine/Render/TextureLibrary.h"

#include "Engine/Core/Log.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <semaphore>
#include <thread>
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

TextureLibrary::PreparedSurface TextureLibrary::PrepareSurface(const std::string& file, const std::string& roughnessFile, bool packRoughness)
{
    // Everything but the upload, so it can be done on any thread: read, the roughness packed in, its average, its mips.
    PreparedSurface prepared;
    ImageData image;
    if (!LoadImageFile(file, image) || !image.IsValid())
    {
        return prepared;
    }
    // Its roughness in the alpha, read where the colour is (the nearest of its own pixels, were it a different size), or
    // all rough (white, as the shader multiplies it in) without one.
    if (packRoughness)
    {
        ImageData rough;
        const bool hasRough = !roughnessFile.empty() && LoadImageFile(roughnessFile, rough) && rough.IsValid();
        for (int y = 0; y < image.height; ++y)
        {
            for (int x = 0; x < image.width; ++x)
            {
                uint8_t value = 255;
                if (hasRough)
                {
                    const int rx = x * rough.width / image.width;
                    const int ry = y * rough.height / image.height;
                    value = rough.pixels[static_cast<size_t>((ry * rough.width + rx) * 4)];
                }
                image.pixels[static_cast<size_t>((y * image.width + x) * 4 + 3)] = value;
            }
        }
    }
    // Its average colour, in linear light: a photograph has a colour of its own, and a surface tinted to a world's colour
    // wants only its detail (see Surfaces::Apply).
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
        prepared.mean = glm::vec3(static_cast<float>(sum[0] / count), static_cast<float>(sum[1] / count), static_cast<float>(sum[2] / count));
    }
    // Every smaller copy, each the average of four of the one before, laid one after another as bgfx wants them.
    prepared.width = image.width;
    prepared.height = image.height;
    prepared.all = image.pixels;
    int width = image.width;
    int height = image.height;
    std::vector<uint8_t> level = std::move(image.pixels);
    prepared.mips = 1;
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
        prepared.all.insert(prepared.all.end(), next.begin(), next.end());
        level = std::move(next);
        width = nextWidth;
        height = nextHeight;
        ++prepared.mips;
    }
    prepared.ok = true;
    return prepared;
}

void TextureLibrary::PrefetchSurface(const std::string& file, const std::string& roughnessFile, bool packRoughness, const std::string& name)
{
    if (m_byName.contains(name) || m_pending.contains(name))
    {
        return;
    }
    // As many at once as there are threads to spare, the rest waiting their turn: each holds a large image while it works.
    static std::counting_semaphore<64> running(static_cast<std::ptrdiff_t>(std::clamp(std::thread::hardware_concurrency(), 2u, 64u) - 1u));
    m_pending.emplace(name, std::async(std::launch::async,
                                       [file, roughnessFile, packRoughness]
                                       {
                                           running.acquire();
                                           PreparedSurface prepared = PrepareSurface(file, roughnessFile, packRoughness);
                                           running.release();
                                           return prepared;
                                       }));
}

TextureHandle TextureLibrary::LoadSurface(const std::string& file, const std::string& roughnessFile, bool packRoughness, const std::string& name)
{
    if (const auto found = m_byName.find(name); found != m_byName.end())
    {
        return TextureHandle{found->second};
    }
    // Made already on another thread (PrefetchSurface), or now.
    PreparedSurface prepared;
    if (auto pending = m_pending.find(name); pending != m_pending.end())
    {
        prepared = pending->second.get();
        m_pending.erase(pending);
    }
    else
    {
        prepared = PrepareSurface(file, roughnessFile, packRoughness);
    }
    if (!prepared.ok)
    {
        PRED_LOG_WARN(Render, "Surface map '{}' could not be read from {}", name, file);
        return White();
    }
    if (m_textures.size() >= TextureHandle::kInvalid)
    {
        return White();
    }
    const bgfx::Memory* memory = bgfx::copy(prepared.all.data(), static_cast<uint32_t>(prepared.all.size()));
    const bgfx::TextureHandle texture =
        bgfx::createTexture2D(static_cast<uint16_t>(prepared.width), static_cast<uint16_t>(prepared.height), true, 1, bgfx::TextureFormat::RGBA8,
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
    m_means[index] = prepared.mean;
    PRED_LOG_INFO(Render, "Surface map '{}': {} by {}, {} levels", name, prepared.width, prepared.height, static_cast<int>(prepared.mips));
    return TextureHandle{index};
}

TextureHandle TextureLibrary::LoadSurfaceMap(const std::string& file, const std::string& name)
{
    return LoadSurface(file, std::string(), false, name);
}

TextureHandle TextureLibrary::LoadSurfaceColour(const std::string& file, const std::string& roughnessFile, const std::string& name)
{
    return LoadSurface(file, roughnessFile, true, name);
}

glm::vec3 TextureLibrary::MeanColour(TextureHandle handle) const
{
    const auto found = m_means.find(handle.index);
    return found != m_means.end() ? found->second : glm::vec3(1.0f);
}

void TextureLibrary::Shutdown()
{
    // (Waiting for any still being made: their threads go with them.)
    m_pending.clear();
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

TextureHandle TextureLibrary::UploadCutout(const ImageData& image, const std::string& name)
{
    if (const auto found = m_byName.find(name); found != m_byName.end())
    {
        return TextureHandle{found->second};
    }
    if (!image.IsValid() || m_textures.size() >= TextureHandle::kInvalid)
    {
        return White();
    }
    const auto covered = [](const std::vector<uint8_t>& pixels)
    {
        size_t over = 0;
        for (size_t i = 3; i < pixels.size(); i += 4)
        {
            over += pixels[i] >= 128 ? 1 : 0;
        }
        return static_cast<double>(over) / static_cast<double>(std::max<size_t>(pixels.size() / 4, 1));
    };
    const double want = covered(image.pixels);
    std::vector<uint8_t> all = image.pixels;
    std::vector<uint8_t> level = image.pixels;
    int w = image.width;
    int h = image.height;
    while (w > 1 || h > 1)
    {
        const int nw = std::max(w / 2, 1);
        const int nh = std::max(h / 2, 1);
        std::vector<uint8_t> next(static_cast<size_t>(nw) * static_cast<size_t>(nh) * 4);
        for (int y = 0; y < nh; ++y)
        {
            for (int x = 0; x < nw; ++x)
            {
                for (int c = 0; c < 4; ++c)
                {
                    int sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                    {
                        for (int dx = 0; dx < 2; ++dx)
                        {
                            const int sx = std::min(x * 2 + dx, w - 1);
                            const int sy = std::min(y * 2 + dy, h - 1);
                            sum += level[(static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)) * 4 + static_cast<size_t>(c)];
                        }
                    }
                    next[(static_cast<size_t>(y) * static_cast<size_t>(nw) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>(sum / 4);
                }
            }
        }
        // The alpha scaled until as much of the level is over half as was at full size (a few halvings of the search).
        float low = 0.5f;
        float high = 4.0f;
        std::vector<uint8_t> scaled = next;
        for (int step = 0; step < 10; ++step)
        {
            const float scale = (low + high) * 0.5f;
            for (size_t i = 3; i < next.size(); i += 4)
            {
                scaled[i] = static_cast<uint8_t>(std::min(255.0f, static_cast<float>(next[i]) * scale));
            }
            (covered(scaled) < want ? low : high) = scale;
        }
        level = scaled;
        all.insert(all.end(), level.begin(), level.end());
        w = nw;
        h = nh;
    }
    const bgfx::Memory* memory = bgfx::copy(all.data(), static_cast<uint32_t>(all.size()));
    const bgfx::TextureHandle texture = bgfx::createTexture2D(static_cast<uint16_t>(image.width), static_cast<uint16_t>(image.height), true, 1,
                                                              bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_NONE, memory);
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
