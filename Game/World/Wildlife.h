#pragma once

#include "Engine/Core/Math.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pred
{

// Wildlife (Phase 7): the world's own animals, Earth-like ones (you, 2026-11-23: birds and fish and stuff like that) -- flocks of
// birds wheeling over the land, gulls over the sea, a hawk circling high, small birds in tight flocks low down; schools of fish in the
// water round the eye. Drawn as real shapes, their wings beating and gliding, their tails swishing. Where they are comes from the
// site's seed and the campaign's clock, so every machine has them in the same places; only their flight -- birds from a shot heard,
// fish from somebody swimming at them -- is each machine's own. Most worlds have some; airless ones none, nor fish where there is no
// water, or it is ice, lava or acid.
class Wildlife
{
public:
    struct Setup
    {
        uint32_t seed = 0;
        std::string biome;
        bool air = true;   // birds only where there is air to fly in
        bool water = false; // fish only in liquid water
        glm::vec2 middle{0.0f};
        float size = 300.0f;
        std::function<float(float, float)> ground;
        // How deep a point is in the open water (0 or less: not in it), and its surface there.
        std::function<float(const glm::vec3&, float&)> depth;
    };

    // (Made into the scene as the site is, and taken out with it: its owner destroys what Entities and Meshes give, then Forget.)
    void Build(Scene& scene, MeshLibrary& meshes, const Setup& setup);
    void Forget();
    std::vector<Entity> Entities() const;
    std::vector<MeshHandle> Meshes() const;
    // Where everything is now, by the campaign's clock; `daylight` (0 night to 1 day) and `storm` (0 to 1) keep the birds in their
    // roosts; `shots` heard since the last update scatter the flocks near them; the eye sends fish off from it.
    void Update(Scene& scene, MeshLibrary& meshes, double clock, float dt, const glm::vec3& eye, float daylight, float storm,
                const std::vector<glm::vec3>& shots);
    int Birds() const;
    bool HasFish() const { return m_setup.water && m_fishMesh.IsValid(); }
    std::string Describe() const;
    // Where each flock is now (as last updated): for a test to look at them.
    const std::vector<glm::vec3>& FlockCentres() const { return m_centres; }
    bool BirdsOut() const { return m_out; }

private:
    enum class Kind : uint8_t
    {
        Gull,
        Crow,
        Finch,
        Hawk
    };
    struct Flock
    {
        Kind kind = Kind::Crow;
        glm::vec2 home{0.0f};
        float radius = 60.0f;
        float height = 30.0f;
        float speed = 9.0f;
        float phase = 0.0f;
        float spread = 8.0f;
        float wingspan = 1.0f;
        glm::vec3 dark{0.1f};
        glm::vec3 light{0.2f};
        std::vector<glm::vec3> offsets;
        std::vector<float> beats;
        float scare = 0.0f;
        glm::vec3 away{1.0f, 0.0f, 0.0f};
        // How far it has got away (0 to 1) and which way, eased: put there at once by a shot, the flock jumped across the sky in a frame
        // -- it seemed to vanish (you, 2026-11-24; FIX 750).
        float fled = 0.0f;
        glm::vec3 fledWay{1.0f, 0.0f, 0.0f};
    };

    Setup m_setup;
    std::vector<Flock> m_flocks;
    std::vector<glm::vec3> m_centres;
    bool m_out = false;
    int m_birdCount = 0;
    MeshHandle m_birdMesh;
    Entity m_birdEntity;
    std::vector<MeshVertex> m_birdVertices;
    MeshHandle m_fishMesh;
    Entity m_fishEntity;
    std::vector<MeshVertex> m_fishVertices;
};

} // namespace pred
