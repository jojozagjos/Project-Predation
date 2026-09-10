#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred
{

// CPU-side image, before upload. Always four channels, because that is the one layout every backend
// takes without argument and a texture is uploaded once.
struct ImageData
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // rgba8, row major, top row first

    bool IsValid() const
    {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    }
};

// Decodes PNG, JPEG or anything else stb reads, from memory or from a file. Returns false and leaves
// `out` untouched when it cannot.
bool DecodeImage(const uint8_t* bytes, size_t count, ImageData& out);
bool LoadImageFile(const std::string& file, ImageData& out);
// Writes an image out as a PNG. Used to put the textures inside a downloaded model somewhere the
// game can find them without carrying megabytes of base64 in a JSON file.
bool WritePng(const std::string& file, const ImageData& image);

struct TextureHandle
{
    static constexpr uint16_t kInvalid = 0xffff;
    uint16_t index = kInvalid;

    bool IsValid() const { return index != kInvalid; }
    bool operator==(const TextureHandle& other) const = default;
};

// Owns every uploaded texture for the lifetime of the renderer.
//
// Keyed by name, because the same texture is named by several parts of a model and uploading it
// once per part would be one copy of a two megabyte image per barrel and receiver. The first entry
// is always a single white pixel: a material with no texture samples that instead, so the shader
// needs no branch and nothing has to decide whether a draw is textured.
class TextureLibrary
{
public:
    void Init();
    void Shutdown();

    TextureHandle White() const { return TextureHandle{0}; }
    // Uploads, or returns what is already under that name.
    TextureHandle Upload(const ImageData& image, const std::string& name);
    // Reads a file and uploads it. Returns the white texture when it cannot be read, so a missing
    // file draws a flat surface rather than nothing at all.
    TextureHandle LoadFromFile(const std::string& file, const std::string& name);
    TextureHandle Find(const std::string& name) const;

    bgfx::TextureHandle Get(TextureHandle handle) const;
    size_t Count() const { return m_textures.size(); }

private:
    struct Entry
    {
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        std::string name;
    };

    std::vector<Entry> m_textures;
    std::unordered_map<std::string, uint16_t> m_byName;
};

} // namespace pred
