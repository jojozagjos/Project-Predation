#pragma once

#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace pred
{

// What falls from a world's sky round whoever is looking -- snow, rain, dust or ash -- in a box that goes with the eye,
// each piece coming round again at the other side as it leaves, so it seems to fill the sky wherever anybody goes; blown
// by the wind, gusting, and not indoors.
//
// Drawn as one mesh whose vertices are written again every frame (and a second for the embers in ash): a thousand
// pieces are one thing to draw, not a thousand.
class Weather
{
public:
    enum class Kind : uint8_t
    {
        None,
        Snow,
        Rain,
        Dust,
        Ash
    };
    // From a weather's particles ("snow", "rain", "dust", "ash"; anything else none).
    static Kind KindOf(const std::string& particles);

    // `intensity` 0 to 1, how much of it there is; `tint` the colour of dust (the ground it was lifted from); `wind`
    // metres a second of drift, sideways; `ground` how high the ground is under the eye, for what blows along it.
    void Update(Scene& scene, MeshLibrary& meshes, const glm::vec3& eye, float ground, float dt, Kind kind, float intensity, const glm::vec3& tint,
                const glm::vec3& wind, const std::function<bool(const glm::vec3&)>& indoors);

private:
    struct Piece
    {
        glm::vec3 at{0.0f};
        float fall = 1.0f;
        float sway = 0.0f;
        float size = 0.03f;
        glm::vec3 spin{0.0f, 1.0f, 0.0f};
        float spinRate = 2.0f;
        float height = 0.2f; // blown along the ground: how far over it
        bool low = false;    // blown along the ground rather than falling
        bool glow = false;   // an ember
    };
    // The meshes made for a kind, and the pieces in them.
    void Make(Scene& scene, MeshLibrary& meshes, Kind kind, const glm::vec3& eye, const glm::vec3& tint);
    void Show(Scene& scene, bool shown);

    Kind m_kind = Kind::None;
    std::vector<Piece> m_pieces;
    Entity m_entity;
    Entity m_glowEntity;
    MeshHandle m_mesh;
    MeshHandle m_glowMesh;
    std::vector<MeshVertex> m_vertices;
    std::vector<MeshVertex> m_glowVertices;
    int m_glowCount = 0;
    glm::vec3 m_tint{1.0f};
    float m_clock = 0.0f;
    uint32_t m_seed = 0x5A0Fu;
    bool m_shown = false;
};

} // namespace pred
