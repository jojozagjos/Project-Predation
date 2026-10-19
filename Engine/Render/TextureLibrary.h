#pragma once

#include <bgfx/bgfx.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <future>
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
    // A normal map that bends nothing: what a material with no normal map of its own samples.
    TextureHandle FlatNormal() const { return TextureHandle{1}; }
    // A surface map -- a colour or normal map that repeats across a level -- read and uploaded with its smaller copies
    // (mips) and sharpened at grazing angles, so it holds together into the distance. White when the file cannot be read.
    TextureHandle LoadSurfaceMap(const std::string& file, const std::string& name);
    // A set's colour map, as LoadSurfaceMap, with its roughness map (`roughnessFile`; empty or unreadable: all rough) in its
    // alpha: one texture to bind for both, so the mesh shader has samplers to spare for a second set.
    TextureHandle LoadSurfaceColour(const std::string& file, const std::string& roughnessFile, const std::string& name);
    // Starts making one ready on another thread, so that when it is loaded (under the same name) only the upload is left:
    // reading and working out a large one takes a fifth of a second, and a place uses dozens. Nothing if it is loaded already.
    void PrefetchSurface(const std::string& file, const std::string& roughnessFile, bool packRoughness, const std::string& name);
    // A surface map's average colour, in linear light (as the shader reads it): white for anything else.
    glm::vec3 MeanColour(TextureHandle handle) const;
    // Uploads, or returns what is already under that name.
    TextureHandle Upload(const ImageData& image, const std::string& name);
    // As Upload, with a mip chain, made keeping as much of each level over half alpha as there is at full size (for a
    // texture whose alpha cuts out what it is on: Material::cutout). Its sides must be powers of two.
    TextureHandle UploadCutout(const ImageData& image, const std::string& name);
    // Reads a file and uploads it. Returns the white texture when it cannot be read, so a missing
    // file draws a flat surface rather than nothing at all.
    TextureHandle LoadFromFile(const std::string& file, const std::string& name);
    TextureHandle Find(const std::string& name) const;
    // One that is drawn into again and again -- a screen on a device -- and what is drawn into it now.
    TextureHandle CreateDynamic(int width, int height, const std::string& name);
    void Update(TextureHandle handle, const ImageData& image);

    bgfx::TextureHandle Get(TextureHandle handle) const;
    size_t Count() const { return m_textures.size(); }

private:
    // A surface map made ready to upload: read, its roughness packed in (a colour map), its average and its smaller copies
    // worked out. All of it but the upload, so on any thread.
    struct PreparedSurface
    {
        bool ok = false;
        int width = 0;
        int height = 0;
        uint8_t mips = 0;
        std::vector<uint8_t> all;
        glm::vec3 mean{1.0f};
    };
    static PreparedSurface PrepareSurface(const std::string& file, const std::string& roughnessFile, bool packRoughness);
    // What LoadSurfaceMap and LoadSurfaceColour share: one made already (PrefetchSurface) collected, or made now; uploaded.
    TextureHandle LoadSurface(const std::string& file, const std::string& roughnessFile, bool packRoughness, const std::string& name);
    // Those being made on other threads, by name.
    std::unordered_map<std::string, std::future<PreparedSurface>> m_pending;

    struct Entry
    {
        bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
        std::string name;
    };

    std::vector<Entry> m_textures;
    std::unordered_map<std::string, uint16_t> m_byName;
    std::unordered_map<uint16_t, glm::vec3> m_means;
};

} // namespace pred
