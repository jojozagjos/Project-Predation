#pragma once

#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace pred
{

// What kind of lamp it is, which decides its colour, how far it reaches and which way it shines.
enum class LightKind : uint8_t
{
    Ceiling,   // a strip light under a ceiling: cold, wide, down
    Wall,      // a caged lamp on a wall: warm, low, out and down
    Emergency, // a red lamp on its own battery: on whatever the power is doing
    Flood,     // a floodlight on a pole or a roof: a long, bright cone
};

// How it is behaving, which is most of what makes a building feel looked after or abandoned.
enum class LightMood : uint8_t
{
    Steady,
    Flicker, // on, with a dip now and then
    Failing, // mostly off, stuttering back on for a moment and dying again
    Dead,    // off; the fitting is there and nothing more
    Pulse,   // rising and falling, as an emergency beacon does
};

const char* LightKindName(LightKind kind);
const char* LightMoodName(LightMood mood);

// The lights that belong to a place.
//
// Each is a punctual light for the renderer and a fitting in the scene -- the lamp itself -- that
// glows while it is lit and goes dark when it is not. They are grouped into circuits, so a part of a
// building can lose its power and have its lights go out together, and restoring it brings them back.
// Emergency lights are on their own batteries and stay lit through anything: in a building with the
// power out they are what there is.
//
// How a light flickers or fails is worked out from its seed and the time, so every machine shows the
// same kind of flicker without anything being sent; only whether a circuit has power would need to be.
class WorldObjects;

class LevelLights
{
public:
    struct Light
    {
        LightKind kind = LightKind::Ceiling;
        LightMood mood = LightMood::Steady;
        glm::vec3 position{0.0f};
        glm::vec3 direction{0.0f, -1.0f, 0.0f};
        glm::vec3 color{1.0f};
        float intensity = 8.0f;
        float range = 9.0f;
        float innerAngle = 180.0f;
        float outerAngle = 180.0f;
        float sourceRadius = 0.4f;
        int circuit = 0;
        uint32_t seed = 0;
        Entity fitting;
        glm::vec3 glow{0.0f}; // the fitting's own light when fully lit
        float level = 1.0f;   // how lit it is this moment, 0 to 1
        // The room it lights and nothing outside it: see PunctualLight.
        bool bounded = false;
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
        // A lamp's light through a doorway (see AddSpill): the middle of the doorway, the door hung in it
        // (-1 an open archway, -2 not looked for yet), and how open that door is, which is how much of the
        // light gets through.
        bool spill = false;
        glm::vec3 doorway{0.0f};
        int door = -2;
        float gate = 1.0f;
    };

    // A light and its fitting. `direction` is where the fitting faces: down for a ceiling light, out
    // from the wall for a wall lamp, along the beam for a flood.
    // `range`, when given, is how far it reaches instead of how far its kind does: a building of several
    // floors needs its lamps to stop short of the floor below, since nothing here casts a lamp's shadow.
    int Add(Scene& scene, MeshLibrary& meshes, LightKind kind, LightMood mood, const glm::vec3& at,
            const glm::vec3& direction, int circuit = 0, uint32_t seed = 0, float range = 0.0f);
    void Clear(Scene& scene);
    // Every light from `first` on, taken away: the ones a map added after everything else, when that
    // map is rebuilt.
    void RemoveFrom(Scene& scene, size_t first);
    // Keeps a light to the room it is in: nothing outside the box is lit by it.
    void Bound(int index, const glm::vec3& min, const glm::vec3& max);
    // How much bigger than asked every box is made (see Bound). None for a level whose boxes end in the
    // middle of its walls, as the facility's do: there the margin only made neighbouring boxes overlap,
    // and where one lamp's box met another's -- or its own again, round a corner -- a strip of floor and
    // wall a hand wide was lit twice over, a bright line down the middle of a room.
    void SetBoundsMargin(float margin) { m_boundsMargin = margin; }
    // The light of `source` coming through a doorway: no fitting of its own, dimmer, lighting only the
    // box on the far side, and on, flickering and failing exactly as its lamp does. What a lamp kept to its
    // room gives the room next door.
    int AddSpill(int source, const glm::vec3& at, const glm::vec3& direction, const glm::vec3& min, const glm::vec3& max,
                 float share = 0.45f);
    size_t Count() const { return m_lights.size(); }

    // Each doorway's light let through as far as its door is open: none through a shut one. Called every
    // frame; which door hangs in which doorway is found again whenever the number of doors changes.
    void UpdateDoorways(const WorldObjects& world);
    // Another light exactly like the source, with no fitting, lighting a different box.
    int AddCopy(int source, const glm::vec3& min, const glm::vec3& max);
    void SetPowered(int circuit, bool powered);
    bool Powered(int circuit) const;

    // Flicker, failure and power, applied: each light's level, and its fitting's glow.
    void Update(Scene& scene, float time);
    // Every light lit at all, as the renderer wants them.
    void Gather(std::vector<PunctualLight>& out) const;

    const std::vector<Light>& Lights() const { return m_lights; }

    // How lit a light of this mood and seed is at a moment, 0 to 1. Exposed for the tests.
    static float MoodLevel(LightMood mood, uint32_t seed, float time);

private:
    std::vector<Light> m_lights;
    std::vector<int> m_unpowered;
    size_t m_doorsSeen = static_cast<size_t>(-1);
    float m_boundsMargin = 0.05f;
    MeshHandle m_fittingMeshes[4] = {};
};

} // namespace pred
