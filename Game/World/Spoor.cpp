#include "Game/World/Spoor.h"

#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"
#include "Game/Creature/CreatureTraits.h"
#include "Game/World/FacilityMap.h"
#include "Game/World/SiteFauna.h"
#include "Game/World/SitePlan.h"
#include "Game/World/SiteTerrain.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

uint32_t Pack(const glm::vec3& c)
{
    const auto channel = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return channel(c.r) | (channel(c.g) << 8) | (channel(c.b) << 16) | (255u << 24);
}

struct Builder
{
    const SiteTerrain& terrain;
    const SitePlan& plan;
    const std::function<glm::vec3(float, float)>& tint;
    MeshData mesh;
    SeededRandom random;

    float Ground(float x, float z) const { return terrain.GroundHeight(x, z); }
    bool Open(float x, float z) const { return terrain.WaterDepth(x, z) <= 0.0f && !plan.OnBuilding({x, z}, 0.5f); }

    // A flat oval laid on the ground (each point on it at the ground's own height there, a little over it), its long way `along`.
    void Oval(const glm::vec3& at, const glm::vec2& along, float length, float width, const glm::vec3& colour, float lift = 0.02f)
    {
        const glm::vec2 a = glm::length(along) > 1e-4f ? glm::normalize(along) : glm::vec2(0.0f, 1.0f);
        const glm::vec2 side{-a.y, a.x};
        const auto first = static_cast<uint32_t>(mesh.vertices.size());
        const auto put = [&](const glm::vec2& p)
        {
            MeshVertex vertex;
            vertex.position = {p.x, Ground(p.x, p.y) + lift, p.y};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            vertex.color = Pack(colour);
            mesh.vertices.push_back(vertex);
        };
        put({at.x, at.z});
        constexpr int kAround = 10;
        for (int k = 0; k < kAround; ++k)
        {
            const float u = glm::two_pi<float>() * static_cast<float>(k) / kAround;
            put(glm::vec2(at.x, at.z) + a * (std::cos(u) * length * 0.5f) + side * (std::sin(u) * width * 0.5f));
        }
        for (uint32_t k = 0; k < kAround; ++k)
        {
            mesh.indices.insert(mesh.indices.end(), {first, first + 1 + (k + 1) % kAround, first + 1 + k});
        }
    }

    // A strip along the ground from a to b, `width` across.
    void Strip(const glm::vec3& a, const glm::vec3& b, float width, const glm::vec3& colour, float lift = 0.02f)
    {
        glm::vec2 way{b.x - a.x, b.z - a.z};
        if (glm::length(way) < 1e-3f)
        {
            return;
        }
        way = glm::normalize(way);
        const glm::vec2 side = glm::vec2(-way.y, way.x) * (width * 0.5f);
        const auto first = static_cast<uint32_t>(mesh.vertices.size());
        for (const glm::vec2& p : {glm::vec2(a.x, a.z) - side, glm::vec2(a.x, a.z) + side, glm::vec2(b.x, b.z) + side, glm::vec2(b.x, b.z) - side})
        {
            MeshVertex vertex;
            vertex.position = {p.x, Ground(p.x, p.y) + lift, p.y};
            vertex.normal = {0.0f, 1.0f, 0.0f};
            vertex.color = Pack(colour);
            mesh.vertices.push_back(vertex);
        }
        mesh.indices.insert(mesh.indices.end(), {first, first + 2, first + 1, first, first + 3, first + 2});
    }

    // A piece placed as it is (a bone, a gouge), painted one colour.
    void Piece(const MeshData& shape, const glm::mat4& place, const glm::vec3& colour)
    {
        const size_t first = mesh.vertices.size();
        mesh.Append(shape, place);
        for (size_t k = first; k < mesh.vertices.size(); ++k)
        {
            mesh.vertices[k].color = Pack(colour);
        }
    }
};

// A way it goes from one place to another: not straight, wandering a little either side.
std::vector<glm::vec3> Trail(SeededRandom& random, const glm::vec3& from, const glm::vec3& to)
{
    std::vector<glm::vec3> points{from};
    const glm::vec3 way = to - from;
    const glm::vec3 side = glm::normalize(glm::vec3(-way.z, 0.0f, way.x) + glm::vec3(1e-4f, 0.0f, 0.0f));
    const float length = glm::length(way);
    const int bends = 2 + static_cast<int>(random.Unit() * 3.0f);
    for (int k = 1; k <= bends; ++k)
    {
        const float u = static_cast<float>(k) / static_cast<float>(bends + 1);
        points.push_back(from + way * u + side * (random.Range(-0.12f, 0.12f) * length));
    }
    points.push_back(to);
    return points;
}

} // namespace

MeshData Spoor::Make(uint32_t seed, const SitePlan& plan, const SiteTerrain& terrain, const SiteFauna& fauna,
                     const std::function<glm::vec3(float, float)>& groundTint)
{
    Builder b{terrain, plan, groundTint, {}, SeededRandom((static_cast<uint64_t>(seed) << 7) ^ 0x5B0012ull)};
    const bool strider = fauna.giant && !fauna.giantSwims;
    if (!fauna.Any() || (fauna.giant && fauna.giantSwims))
    {
        return b.mesh; // (nothing lives here; or it lives in the sea, and leaves nothing on the land)
    }
    // How big what made them is, and how fresh they are: about now where it lives and goes about, a while ago where it sleeps it off,
    // and older still where it only came through.
    const float scale = strider ? 1.0f
                        : fauna.size == CreatureSize::Tiny  ? 0.4f
                        : fauna.size == CreatureSize::Small ? 0.6f
                        : fauna.size == CreatureSize::Big   ? 1.7f
                                                             : 1.0f;
    const bool lives = fauna.presence == SiteFauna::Presence::Lives || strider;
    const float fresh = !lives ? 0.4f : fauna.doing == SiteFauna::Doing::Sleeping ? 0.7f : 1.0f;
    const glm::vec2 middle = glm::vec2(plan.origin.x, plan.origin.z) + glm::vec2(plan.size * 0.5f);
    const auto darker = [&](float x, float z, float how)
    {
        // Pressed in and wet with it: darker than the ground round it, the fresher the more; in snow a shadowed blue-grey.
        const glm::vec3 ground = groundTint ? groundTint(x, z) : glm::vec3(0.35f);
        const float pale = glm::dot(ground, glm::vec3(0.3f, 0.5f, 0.2f));
        const glm::vec3 pressed = pale > 0.6f ? ground * glm::vec3(0.72f, 0.78f, 0.88f) : ground * 0.5f;
        return glm::mix(ground, pressed, how * fresh);
    };

    // Where it starts from: its lair's way out -- a building's door, a cave's mouth, the spot out on the land -- or, passing through, one
    // edge of the site.
    glm::vec3 lair{middle.x, 0.0f, middle.y};
    std::vector<glm::vec3> ends;
    if (strider)
    {
        const float angle = b.random.Range(0.0f, glm::two_pi<float>());
        const glm::vec2 far = middle + glm::vec2(std::cos(angle), std::sin(angle)) * (plan.size * 0.5f + 120.0f);
        lair = {far.x, 0.0f, far.y};
        ends.emplace_back(middle.x + b.random.Range(-0.3f, 0.3f) * plan.size, 0.0f, middle.y + b.random.Range(-0.3f, 0.3f) * plan.size);
    }
    else if (!lives || fauna.lair == SiteFauna::Lair::None)
    {
        const float angle = b.random.Range(0.0f, glm::two_pi<float>());
        const glm::vec2 way{std::cos(angle), std::sin(angle)};
        lair = glm::vec3(middle.x - way.x * plan.size * 0.55f, 0.0f, middle.y - way.y * plan.size * 0.55f);
        ends.emplace_back(middle.x + way.x * plan.size * 0.55f, 0.0f, middle.y + way.y * plan.size * 0.55f);
    }
    else
    {
        if (fauna.lair == SiteFauna::Lair::Building && fauna.lairBuilding >= 0 && fauna.lairBuilding < static_cast<int>(plan.buildings.size()) &&
            !plan.buildings[static_cast<size_t>(fauna.lairBuilding)].exits.empty())
        {
            const FacilityLayout& building = plan.buildings[static_cast<size_t>(fauna.lairBuilding)];
            lair = FacilityMap::ExitOutside(building, building.exits.front(), 1.5f);
        }
        else if (fauna.lair == SiteFauna::Lair::Cave && fauna.lairCave >= 0 && fauna.lairCave < static_cast<int>(plan.caves.size()))
        {
            const SitePlan::Cave& cave = plan.caves[static_cast<size_t>(fauna.lairCave)];
            lair = cave.mouth + cave.out * 3.0f;
        }
        else
        {
            lair = fauna.lairAt;
        }
        // Down to the water, where there is any near: the nearest shore round it.
        float nearest = 1.0e9f;
        glm::vec3 shore{0.0f};
        for (int k = 0; k < 24; ++k)
        {
            const float angle = glm::two_pi<float>() * static_cast<float>(k) / 24.0f;
            for (float out = 15.0f; out < 160.0f; out += 8.0f)
            {
                const float x = lair.x + std::cos(angle) * out;
                const float z = lair.z + std::sin(angle) * out;
                if (terrain.WaterDepth(x, z) > 0.2f)
                {
                    if (out < nearest)
                    {
                        nearest = out;
                        shore = {lair.x + std::cos(angle) * (out - 3.0f), 0.0f, lair.z + std::sin(angle) * (out - 3.0f)};
                    }
                    break;
                }
            }
        }
        if (nearest < 1.0e8f)
        {
            ends.push_back(shore);
        }
        // And out among the buildings: to another's door, or out across the open.
        for (const FacilityLayout& building : plan.buildings)
        {
            if (!building.exits.empty() && ends.size() < 3 && b.random.Unit() < 0.5f)
            {
                const glm::vec3 door = FacilityMap::ExitOutside(building, building.exits.front(), 3.0f);
                if (glm::distance(door, lair) > 15.0f)
                {
                    ends.push_back(door);
                }
            }
        }
        if (ends.size() < 2)
        {
            const float angle = b.random.Range(0.0f, glm::two_pi<float>());
            ends.emplace_back(lair.x + std::cos(angle) * 70.0f, 0.0f, lair.z + std::sin(angle) * 70.0f);
        }
    }

    for (const glm::vec3& end : ends)
    {
        PRED_LOG_DEBUG(Gameplay, "Spoor: tracks from {:.0f} {:.0f} to {:.0f} {:.0f}", lair.x, lair.z, end.x, end.z);
    }
    // The tracks along each way.
    for (const glm::vec3& end : ends)
    {
        const std::vector<glm::vec3> trail = Trail(b.random, lair, end);
        bool left = false;
        for (size_t s = 0; s + 1 < trail.size(); ++s)
        {
            const glm::vec3 from = trail[s];
            const glm::vec3 to = trail[s + 1];
            const glm::vec2 way = glm::normalize(glm::vec2(to.x - from.x, to.z - from.z) + glm::vec2(1e-4f));
            const glm::vec2 side{-way.y, way.x};
            const float length = glm::distance(glm::vec2(from.x, from.z), glm::vec2(to.x, to.z));
            const float stride = strider ? b.random.Range(12.0f, 16.0f) : 0.85f * scale;
            for (float along = 0.0f; along < length; along += stride * b.random.Range(0.85f, 1.15f))
            {
                left = !left;
                // (Older, fewer of them left to see.)
                if (b.random.Unit() > 0.45f + 0.55f * fresh)
                {
                    continue;
                }
                glm::vec2 at = glm::vec2(from.x, from.z) + way * along + side * ((left ? 1.0f : -1.0f) * (strider ? b.random.Range(4.0f, 9.0f) : 0.22f * scale));
                if (!b.Open(at.x, at.y))
                {
                    continue;
                }
                const glm::vec3 spot{at.x, 0.0f, at.y};
                if (strider)
                {
                    // A pit where a foot came down, the ground pushed up round it in a lip.
                    const float r = b.random.Range(1.3f, 2.2f);
                    b.Oval(spot, way, r * 2.6f, r * 2.6f, darker(at.x, at.y, 0.25f) * 1.12f, 0.03f);
                    b.Oval(spot, way, r * 2.0f, r * 2.0f, darker(at.x, at.y, 0.85f), 0.045f);
                    continue;
                }
                const glm::vec3 colour = darker(at.x, at.y, 0.9f);
                switch (fauna.plan)
                {
                case BodyPlan::Biped:
                    // Three long toes forward, one back, a bird's.
                    for (const float turn : {-0.45f, 0.0f, 0.45f})
                    {
                        const glm::vec2 toe = glm::vec2(glm::cos(turn) * way.x - glm::sin(turn) * way.y, glm::sin(turn) * way.x + glm::cos(turn) * way.y);
                        b.Oval(glm::vec3(at.x + toe.x * 0.11f * scale, 0.0f, at.y + toe.y * 0.11f * scale), toe, 0.2f * scale, 0.045f * scale, colour);
                    }
                    b.Oval(glm::vec3(at.x - way.x * 0.08f * scale, 0.0f, at.y - way.y * 0.08f * scale), way, 0.1f * scale, 0.04f * scale, colour);
                    break;
                case BodyPlan::Crawler:
                    // A hand: a long palm and four long fingers, spread.
                    b.Oval(spot, way, 0.2f * scale, 0.13f * scale, colour);
                    for (int f = 0; f < 4; ++f)
                    {
                        const float turn = -0.35f + 0.233f * static_cast<float>(f);
                        const glm::vec2 finger = glm::vec2(glm::cos(turn) * way.x - glm::sin(turn) * way.y, glm::sin(turn) * way.x + glm::cos(turn) * way.y);
                        b.Oval(glm::vec3(at.x + finger.x * 0.2f * scale, 0.0f, at.y + finger.y * 0.2f * scale), finger, 0.16f * scale, 0.03f * scale, colour);
                    }
                    break;
                default:
                    // A pad and four toes in front of it, a claw mark ahead of each.
                    b.Oval(spot, way, 0.13f * scale, 0.11f * scale, colour);
                    for (int t = 0; t < 4; ++t)
                    {
                        const float across = (-1.5f + static_cast<float>(t)) * 0.045f * scale;
                        const float ahead = (t == 0 || t == 3 ? 0.1f : 0.13f) * scale;
                        const glm::vec2 toe = at + way * ahead + side * across;
                        b.Oval(glm::vec3(toe.x, 0.0f, toe.y), way, 0.05f * scale, 0.04f * scale, colour);
                        const glm::vec2 claw = toe + way * (0.05f * scale);
                        b.Oval(glm::vec3(claw.x, 0.0f, claw.y), way, 0.03f * scale, 0.008f * scale, colour * 0.7f);
                    }
                    break;
                }
            }
        }
    }

    if (strider)
    {
        return b.mesh;
    }

    // Where it ate: off its way a little, a stain on the ground and what it left -- ribs, a run of backbone, long bones, a skull -- fresh
    // and red where it feeds now, bleached where it fed a while ago; and dragged there from further off, the ground smeared along the way.
    if (lives && !ends.empty())
    {
        const glm::vec3 toward = ends.front() - lair;
        const float off = fauna.doing == SiteFauna::Doing::Feeding ? b.random.Range(6.0f, 10.0f) : b.random.Range(14.0f, 26.0f);
        const glm::vec3 side = glm::normalize(glm::vec3(-toward.z, 0.0f, toward.x) + glm::vec3(1e-4f, 0.0f, 0.0f));
        glm::vec3 kill = lair + glm::normalize(toward + glm::vec3(1e-4f, 0.0f, 0.0f)) * off + side * b.random.Range(-4.0f, 4.0f);
        if (b.Open(kill.x, kill.z))
        {
            PRED_LOG_DEBUG(Gameplay, "Spoor: remains at {:.1f} {:.1f}", kill.x, kill.z);
            kill.y = b.Ground(kill.x, kill.z);
            const bool feeding = fauna.doing == SiteFauna::Doing::Feeding;
            const glm::vec3 blood = feeding ? glm::vec3(0.18f, 0.03f, 0.025f) : glm::vec3(0.16f, 0.11f, 0.08f);
            b.Oval(kill, {1.0f, 0.3f}, 2.6f * scale, 1.9f * scale, glm::mix(darker(kill.x, kill.z, 0.5f), blood, feeding ? 0.85f : 0.4f), 0.025f);
            const glm::vec3 bone = feeding ? glm::vec3(0.72f, 0.55f, 0.5f) : glm::vec3(0.78f, 0.74f, 0.64f);
            const float yaw = b.random.Range(0.0f, glm::two_pi<float>());
            const glm::quat turn = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 along = turn * glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 across = turn * glm::vec3(1.0f, 0.0f, 0.0f);
            // The backbone, a row of knuckles.
            for (int k = 0; k < 7; ++k)
            {
                const glm::vec3 at = kill + along * ((static_cast<float>(k) - 3.0f) * 0.12f * scale) + glm::vec3(0.0f, 0.04f * scale, 0.0f);
                b.Piece(Primitives::Box(glm::vec3(0.07f, 0.06f, 0.08f) * scale), glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(turn), bone);
            }
            // Its ribs, curved up off it, some broken.
            for (int k = 0; k < 6; ++k)
            {
                for (const float s : {-1.0f, 1.0f})
                {
                    if (b.random.Unit() < 0.25f)
                    {
                        continue;
                    }
                    const glm::vec3 root = kill + along * ((static_cast<float>(k) - 2.5f) * 0.1f * scale) + glm::vec3(0.0f, 0.05f * scale, 0.0f);
                    for (int seg = 0; seg < 3; ++seg)
                    {
                        const float a0 = 0.35f * static_cast<float>(seg);
                        const glm::vec3 p = root + (across * s * std::cos(a0) * 0.12f + glm::vec3(0.0f, std::sin(a0) * 0.12f, 0.0f)) * (scale * (1.0f + seg));
                        const glm::quat lean = turn * glm::angleAxis(s * (0.4f + 0.45f * static_cast<float>(seg)), glm::vec3(0.0f, 0.0f, 1.0f));
                        b.Piece(Primitives::Box(glm::vec3(0.13f, 0.02f, 0.025f) * scale), glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(lean), bone);
                    }
                }
            }
            // Two long bones thrown aside, and the skull.
            for (int k = 0; k < 2; ++k)
            {
                const glm::vec3 at = kill + across * b.random.Range(-1.0f, 1.0f) * scale + along * b.random.Range(-1.0f, 1.0f) * scale +
                                     glm::vec3(0.0f, 0.03f * scale, 0.0f);
                const glm::quat lie = glm::angleAxis(b.random.Range(0.0f, glm::two_pi<float>()), glm::vec3(0.0f, 1.0f, 0.0f));
                b.Piece(Primitives::Box(glm::vec3(0.05f, 0.05f, 0.5f) * scale), glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(lie), bone);
            }
            b.Piece(Primitives::Sphere(0.11f * scale, 10, 7), glm::translate(glm::mat4(1.0f), kill + along * (0.6f * scale) + glm::vec3(0.0f, 0.07f * scale, 0.0f)) *
                                                              glm::scale(glm::mat4(1.0f), glm::vec3(0.8f, 0.7f, 1.3f)),
                    bone);
            // Dragged there: a smear back along the ground the way it was brought, its edges two furrows.
            const glm::vec3 from = kill + glm::normalize(glm::vec3(b.random.Range(-1.0f, 1.0f), 0.0f, b.random.Range(-1.0f, 1.0f)) + glm::vec3(1e-3f)) * (12.0f + 8.0f * scale);
            const std::vector<glm::vec3> drag = Trail(b.random, from, kill);
            for (size_t s = 0; s + 1 < drag.size(); ++s)
            {
                const int pieces = static_cast<int>(glm::distance(drag[s], drag[s + 1]) / 1.5f) + 1;
                for (int p = 0; p < pieces; ++p)
                {
                    const glm::vec3 a = glm::mix(drag[s], drag[s + 1], static_cast<float>(p) / static_cast<float>(pieces));
                    const glm::vec3 c = glm::mix(drag[s], drag[s + 1], static_cast<float>(p + 1) / static_cast<float>(pieces));
                    if (!b.Open(a.x, a.z) || !b.Open(c.x, c.z))
                    {
                        continue;
                    }
                    b.Strip(a, c, 0.7f * scale, glm::mix(darker(a.x, a.z, 0.5f), blood, feeding ? 0.3f : 0.1f), 0.015f);
                    const glm::vec3 edge = glm::normalize(glm::vec3(-(c - a).z, 0.0f, (c - a).x) + glm::vec3(1e-4f, 0.0f, 0.0f)) * (0.3f * scale);
                    b.Strip(a + edge, c + edge, 0.06f * scale, darker(a.x, a.z, 1.0f), 0.025f);
                    b.Strip(a - edge, c - edge, 0.06f * scale, darker(a.x, a.z, 1.0f), 0.025f);
                }
            }
        }
    }

    // Claw marks gouged in the wall either side of the doors of a building it lives in: through the paint to the bare metal, in fours.
    if (lives && fauna.lair == SiteFauna::Lair::Building && fauna.lairBuilding >= 0 && fauna.lairBuilding < static_cast<int>(plan.buildings.size()))
    {
        const FacilityLayout& building = plan.buildings[static_cast<size_t>(fauna.lairBuilding)];
        for (const FacilityLayout::Exit& exit : building.exits)
        {
            const glm::vec3 face = FacilityMap::ExitOutside(building, exit, 0.0f);
            glm::vec3 out = FacilityMap::ExitOutside(building, exit, 1.0f) - face;
            out.y = 0.0f;
            out = glm::length(out) > 1e-3f ? glm::normalize(out) : glm::vec3(0.0f, 0.0f, 1.0f);
            const glm::vec3 along = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), out);
            const glm::quat facing = glm::quat_cast(glm::mat3(along, glm::vec3(0.0f, 1.0f, 0.0f), out));
            for (const float s : {-1.0f, 1.0f})
            {
                if (b.random.Unit() < 0.3f)
                {
                    continue;
                }
                const float slant = b.random.Range(-0.5f, 0.5f);
                const float high = b.random.Range(0.6f, 1.5f) * std::max(scale, 0.7f);
                for (int c = 0; c < 4; ++c)
                {
                    const glm::vec3 at = face + along * (s * (1.05f + 0.07f * static_cast<float>(c) * std::max(scale, 0.7f))) + glm::vec3(0.0f, high, 0.0f) + out * 0.03f;
                    const glm::quat lean = facing * glm::angleAxis(slant, glm::vec3(0.0f, 0.0f, 1.0f));
                    b.Piece(Primitives::Box(glm::vec3(0.018f, b.random.Range(0.4f, 0.7f) * std::max(scale, 0.7f), 0.01f)),
                            glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(lean), glm::vec3(0.62f, 0.63f, 0.64f));
                }
            }
        }
    }

    // A skin it shed, by its lair: pale, papery, crumpled flat, as long as it is.
    if (lives && fauna.lair != SiteFauna::Lair::None && b.random.Unit() < 0.8f)
    {
        glm::vec3 at = fauna.lair == SiteFauna::Lair::Building ? fauna.lairAt : lair + glm::vec3(b.random.Range(-3.0f, 3.0f), 0.0f, b.random.Range(-3.0f, 3.0f));
        at.y = fauna.lair == SiteFauna::Lair::Building ? fauna.lairAt.y : b.Ground(at.x, at.z);
        const float yaw = b.random.Range(0.0f, glm::two_pi<float>());
        const glm::quat turn = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 husk{0.66f, 0.6f, 0.5f};
        constexpr int kLengths = 9;
        for (int k = 0; k < kLengths; ++k)
        {
            const float u = static_cast<float>(k) / static_cast<float>(kLengths - 1);
            const float wide = (0.18f + 0.2f * std::sin(u * glm::pi<float>())) * scale;
            const glm::vec3 p = at + turn * glm::vec3(b.random.Range(-0.05f, 0.05f) * scale, 0.03f * scale, (u - 0.5f) * 1.6f * scale);
            const glm::quat crumple = turn * glm::angleAxis(b.random.Range(-0.35f, 0.35f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                                      glm::angleAxis(b.random.Range(-0.25f, 0.25f), glm::vec3(0.0f, 1.0f, 0.0f));
            b.Piece(Primitives::Sphere(1.0f, 10, 6), glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(crumple) * glm::scale(glm::mat4(1.0f), glm::vec3(wide, 0.035f * scale, 0.14f * scale)),
                    husk * b.random.Range(0.9f, 1.05f));
        }
    }
    return b.mesh;
}

} // namespace pred
