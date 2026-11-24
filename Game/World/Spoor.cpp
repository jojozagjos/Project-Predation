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

const glm::vec3 kWhite{1.0f};

struct Builder
{
    const SiteTerrain& terrain;
    const SitePlan& plan;
    const std::function<glm::vec3(float, float)>& tint;
    const std::function<bool(float, float)>& covered;
    MeshData mesh;  // the stains (a decal: multiplied into what is under it)
    MeshData solid; // things lying there
    SeededRandom random;

    float Ground(float x, float z) const { return terrain.GroundHeight(x, z); }
    bool Open(float x, float z) const { return terrain.WaterDepth(x, z) <= 0.0f && !plan.OnBuilding({x, z}, 0.5f) && !(covered && covered(x, z)); }
    bool Built(float x, float z) const { return plan.OnBuilding({x, z}, 0.8f); }

    // What the ground there takes a print as (you, 2026-11-23: footprints that make sense for what they are on): bare rock and steep
    // ground none; grass a little, flattened; earth, sand and gravel plainly; snow deep; the mud by water deepest. 0: none.
    float Takes(float x, float z) const
    {
        if (SiteTerrain::Rockiness(terrain.GroundNormal(x, z)) > 0.45f)
        {
            return 0.0f;
        }
        const glm::vec3 ground = tint ? tint(x, z) : glm::vec3(0.35f);
        const bool grassy = ground.g > ground.r * 1.12f && ground.g > ground.b * 1.12f;
        bool wet = false;
        for (const glm::vec2 d : {glm::vec2(6.0f, 0.0f), glm::vec2(-6.0f, 0.0f), glm::vec2(0.0f, 6.0f), glm::vec2(0.0f, -6.0f)})
        {
            wet = wet || terrain.WaterDepth(x + d.x, z + d.y) > 0.05f;
        }
        return wet ? 1.0f : grassy ? 0.35f : 0.85f;
    }
    // Whether the ground there is grown over: a print in it the grass pressed flat, darker than the green drawn over the ground round
    // it (the ground's own colour there is lighter than the grass drawn: a print darkened from it came out pale -- you, 2026-11-23).
    bool Grassy(float x, float z) const
    {
        const glm::vec3 ground = tint ? tint(x, z) : glm::vec3(0.35f);
        return ground.g > ground.r * 1.12f && ground.g > ground.b * 1.12f;
    }

    void Put(const glm::vec3& position, const glm::vec3& normal, const glm::vec3& colour)
    {
        MeshVertex vertex;
        vertex.position = position;
        vertex.normal = normal;
        vertex.color = Pack(colour);
        mesh.vertices.push_back(vertex);
    }

    // A stain laid on the ground (each point at the ground's own height there, a little over it), its long way `along`: `mark` what it
    // multiplies the ground by, all over its middle, fading to nothing (white) at its edge; its outline `ragged` -- uneven by that much
    // of its size, never a drawn oval.
    void Oval(const glm::vec3& at, const glm::vec2& along, float length, float width, const glm::vec3& mark, float lift = 0.02f, float ragged = 0.12f)
    {
        const glm::vec2 a = glm::length(along) > 1e-4f ? glm::normalize(along) : glm::vec2(0.0f, 1.0f);
        const glm::vec2 side{-a.y, a.x};
        const auto first = static_cast<uint32_t>(mesh.vertices.size());
        const auto put = [&](const glm::vec2& p, const glm::vec3& colour) { Put({p.x, Ground(p.x, p.y) + lift, p.y}, {0.0f, 1.0f, 0.0f}, colour); };
        put({at.x, at.z}, mark);
        constexpr int kAround = 14;
        float out[kAround];
        for (int k = 0; k < kAround; ++k)
        {
            out[k] = 1.0f + random.Range(-ragged, ragged);
        }
        // An inner ring as dark as the middle, and an outer one fading out: soft at its edge, as a stain or a print in soft ground is.
        for (const float ring : {0.62f, 1.0f})
        {
            for (int k = 0; k < kAround; ++k)
            {
                const float u = glm::two_pi<float>() * static_cast<float>(k) / kAround;
                const float r = ring * (ring < 1.0f ? 1.0f : out[k]);
                put(glm::vec2(at.x, at.z) + a * (std::cos(u) * length * 0.5f * r) + side * (std::sin(u) * width * 0.5f * r), ring < 1.0f ? mark : kWhite);
            }
        }
        for (uint32_t k = 0; k < kAround; ++k)
        {
            const uint32_t i0 = first + 1 + k;
            const uint32_t i1 = first + 1 + (k + 1) % kAround;
            const uint32_t o0 = i0 + kAround;
            const uint32_t o1 = i1 + kAround;
            mesh.indices.insert(mesh.indices.end(), {first, i1, i0, i0, o1, o0, i0, i1, o1});
        }
    }

    // A smear along the ground from a to b, `width` across: `mark` down its middle, fading out to either side.
    void Strip(const glm::vec3& a, const glm::vec3& b, float width, const glm::vec3& mark, float lift = 0.02f)
    {
        glm::vec2 way{b.x - a.x, b.z - a.z};
        if (glm::length(way) < 1e-3f)
        {
            return;
        }
        way = glm::normalize(way);
        const glm::vec2 side = glm::vec2(-way.y, way.x) * (width * 0.5f);
        const float across[4] = {-1.0f, -0.35f, 0.35f, 1.0f};
        for (int band = 0; band < 3; ++band)
        {
            const auto first = static_cast<uint32_t>(mesh.vertices.size());
            const float o0 = across[band];
            const float o1 = across[band + 1];
            const glm::vec3 c0 = band == 0 ? kWhite : mark;
            const glm::vec3 c1 = band == 2 ? kWhite : mark;
            for (const auto& [p, c] : {std::pair{glm::vec2(a.x, a.z) + side * o0, c0}, std::pair{glm::vec2(a.x, a.z) + side * o1, c1},
                                       std::pair{glm::vec2(b.x, b.z) + side * o1, c1}, std::pair{glm::vec2(b.x, b.z) + side * o0, c0}})
            {
                Put({p.x, Ground(p.x, p.y) + lift, p.y}, {0.0f, 1.0f, 0.0f}, c);
            }
            mesh.indices.insert(mesh.indices.end(), {first, first + 2, first + 1, first, first + 3, first + 2});
        }
    }

    // A score on a wall from a to b (both on its face, a little out from it), `width` across along `across`: `mark` down its middle,
    // fading out to either side.
    void Score(const glm::vec3& a, const glm::vec3& b, const glm::vec3& across, const glm::vec3& normal, float width, const glm::vec3& mark)
    {
        const float offsets[4] = {-1.0f, -0.25f, 0.25f, 1.0f};
        for (int band = 0; band < 3; ++band)
        {
            const auto first = static_cast<uint32_t>(mesh.vertices.size());
            const glm::vec3 s0 = across * (offsets[band] * width * 0.5f);
            const glm::vec3 s1 = across * (offsets[band + 1] * width * 0.5f);
            const glm::vec3 c0 = band == 0 ? kWhite : mark;
            const glm::vec3 c1 = band == 2 ? kWhite : mark;
            Put(a + s0, normal, c0);
            Put(a + s1, normal, c1);
            Put(b + s1, normal, c1);
            Put(b + s0, normal, c0);
            // (Both ways round: whichever way the wall faces, it is seen from in front of it.)
            mesh.indices.insert(mesh.indices.end(), {first, first + 2, first + 1, first, first + 3, first + 2, first, first + 1, first + 2, first, first + 2, first + 3});
        }
    }

    // A thing lying there (a shed skin), painted one colour.
    void Piece(const MeshData& shape, const glm::mat4& place, const glm::vec3& colour)
    {
        const size_t first = solid.vertices.size();
        solid.Append(shape, place);
        for (size_t k = first; k < solid.vertices.size(); ++k)
        {
            solid.vertices[k].color = Pack(colour);
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

Spoor::Marks Spoor::Make(uint32_t seed, const SitePlan& plan, const SiteTerrain& terrain, const SiteFauna& fauna,
                         const std::function<glm::vec3(float, float)>& groundTint, const std::function<bool(float, float)>& covered)
{
    Builder b{terrain, plan, groundTint, covered, {}, {}, SeededRandom((static_cast<uint64_t>(seed) << 7) ^ 0x5B0012ull)};
    const auto done = [&] { return Marks{std::move(b.mesh), std::move(b.solid)}; };
    const bool strider = fauna.giant && !fauna.giantSwims;
    if (!fauna.Any() || (fauna.giant && fauna.giantSwims))
    {
        return done(); // (nothing lives here; or it lives in the sea, and leaves nothing on the land)
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
        // Pressed in: what the ground there is multiplied by -- darker the fresher (wet, its edges crisp), in snow a shadowed blue-grey,
        // in grass pressed flat a darker green.
        const glm::vec3 ground = groundTint ? groundTint(x, z) : glm::vec3(0.35f);
        const float pale = glm::dot(ground, glm::vec3(0.3f, 0.5f, 0.2f));
        const glm::vec3 pressed = pale > 0.6f ? glm::vec3(0.7f, 0.77f, 0.9f) : b.Grassy(x, z) ? glm::vec3(0.6f, 0.68f, 0.55f) : glm::vec3(0.55f, 0.52f, 0.5f);
        return glm::mix(kWhite, pressed, how * fresh);
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
        bool walled = false;
        for (size_t s = 0; s + 1 < trail.size() && !walled; ++s)
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
                // (Come to a building's wall: the trail ends there -- never on through it and out the other side.)
                const glm::vec2 onWay = glm::vec2(from.x, from.z) + way * along;
                if (b.Built(onWay.x, onWay.y))
                {
                    walled = true;
                    break;
                }
                if (!b.Open(at.x, at.y))
                {
                    continue;
                }
                const float takes = b.Takes(at.x, at.y);
                if (takes <= 0.0f || (takes < 0.5f && b.random.Unit() < 0.5f))
                {
                    continue;
                }
                const glm::vec3 spot{at.x, 0.0f, at.y};
                if (strider)
                {
                    // A pit where a foot came down, the ground pushed up round it in a lip.
                    const float r = b.random.Range(1.3f, 2.2f);
                    b.Oval(spot, way, r * 2.6f, r * 2.6f, darker(at.x, at.y, 0.3f * takes), 0.03f, 0.18f);
                    b.Oval(spot, way, r * 1.8f, r * 1.8f, darker(at.x, at.y, 0.7f * takes), 0.045f, 0.15f);
                    continue;
                }
                const glm::vec3 colour = darker(at.x, at.y, 0.9f * takes);
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
                        b.Oval(glm::vec3(claw.x, 0.0f, claw.y), way, 0.04f * scale, 0.014f * scale, colour * 0.85f);
                    }
                    break;
                }
            }
        }
    }

    if (strider)
    {
        return done();
    }

    // Where it fed (you, 2026-11-23: no bodies -- blood): off its way a little, a pool of it soaked into the ground and spattered round,
    // fresh and red where it feeds now, dried brown where it fed a while ago; dragged there from further off, the ground smeared along the
    // way; and from there back towards its lair, drops of it where it went.
    if (lives && !ends.empty())
    {
        const glm::vec3 toward = ends.front() - lair;
        const float off = fauna.doing == SiteFauna::Doing::Feeding ? b.random.Range(6.0f, 10.0f) : b.random.Range(14.0f, 26.0f);
        const glm::vec3 side = glm::normalize(glm::vec3(-toward.z, 0.0f, toward.x) + glm::vec3(1e-4f, 0.0f, 0.0f));
        glm::vec3 kill = lair + glm::normalize(toward + glm::vec3(1e-4f, 0.0f, 0.0f)) * off + side * b.random.Range(-4.0f, 4.0f);
        if (b.Open(kill.x, kill.z))
        {
            PRED_LOG_DEBUG(Gameplay, "Spoor: blood at {:.1f} {:.1f}", kill.x, kill.z);
            kill.y = b.Ground(kill.x, kill.z);
            const bool feeding = fauna.doing == SiteFauna::Doing::Feeding;
            // What blood soaked into the ground multiplies it by: dark red fresh, a dried brown-black older -- over grass, earth or snow
            // alike, its grain showing through (a decal). Layered, the patches darken each other where they overlap, as a pool does.
            const glm::vec3 blood = feeding ? glm::vec3(0.66f, 0.17f, 0.14f) : glm::vec3(0.55f, 0.38f, 0.3f);
            const auto stained = [&](float, float, float how) { return glm::mix(kWhite, blood, how); };
            // The pool: soaked patches over each other, darkest where most of them lie, ragged as it ran into the ground.
            for (int k = 0; k < 6; ++k)
            {
                // (What it ate was its own size or more: the pool no smaller than a person's worth would leave.)
                const float pool = std::max(scale, 0.9f);
                const glm::vec3 at = kill + glm::vec3(b.random.Range(-0.5f, 0.5f), 0.0f, b.random.Range(-0.5f, 0.5f)) * pool;
                const glm::vec2 way{b.random.Range(-1.0f, 1.0f), b.random.Range(-1.0f, 1.0f)};
                b.Oval(at, way, b.random.Range(0.7f, 1.6f) * pool, b.random.Range(0.5f, 1.1f) * pool, stained(at.x, at.z, k == 0 ? 0.9f : 0.4f),
                       0.022f + 0.002f * static_cast<float>(k), 0.3f);
            }
            // Spattered round it: drops thrown out, each a little tail away from the middle -- most close in, fewer further.
            for (int k = 0; k < 30; ++k)
            {
                const float angle = b.random.Range(0.0f, glm::two_pi<float>());
                const float out = (0.9f + 2.6f * b.random.Unit() * b.random.Unit()) * scale;
                const glm::vec3 at = kill + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * out;
                if (b.Open(at.x, at.z))
                {
                    const float r = b.random.Range(0.015f, 0.06f) * std::max(scale, 0.6f);
                    b.Oval(at, {std::cos(angle), std::sin(angle)}, r * b.random.Range(1.2f, 3.0f), r, stained(at.x, at.z, 0.8f), 0.026f, 0.2f);
                }
            }
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
                    if (!b.Open(a.x, a.z) || !b.Open(c.x, c.z) || b.Built(a.x, a.z) || b.Built(c.x, c.z))
                    {
                        continue;
                    }
                    // (Thinning out the further from where it lay: smeared heavily near it, faint where the dragging began.)
                    const float near = 1.0f - static_cast<float>(s * static_cast<size_t>(pieces) + static_cast<size_t>(p)) /
                                                  static_cast<float>(std::max<size_t>((drag.size() - 1) * static_cast<size_t>(pieces), 1));
                    b.Strip(a, c, 0.6f * scale, stained(a.x, a.z, (feeding ? 0.4f : 0.2f) * (0.4f + 0.6f * (1.0f - near))), 0.015f);
                    const glm::vec3 edge = glm::normalize(glm::vec3(-(c - a).z, 0.0f, (c - a).x) + glm::vec3(1e-4f, 0.0f, 0.0f)) * (0.26f * scale);
                    b.Strip(a + edge, c + edge, 0.1f * scale, darker(a.x, a.z, 0.6f), 0.025f);
                    b.Strip(a - edge, c - edge, 0.1f * scale, darker(a.x, a.z, 0.6f), 0.025f);
                }
            }
            // Drops of it from there back towards its lair, thinning out.
            const std::vector<glm::vec3> home = Trail(b.random, kill, lair);
            float walked = 0.0f;
            for (size_t s = 0; s + 1 < home.size() && walked < 40.0f; ++s)
            {
                const float length = glm::distance(home[s], home[s + 1]);
                for (float along = 0.0f; along < length && walked < 40.0f; along += b.random.Range(0.8f, 2.2f))
                {
                    walked += 1.5f;
                    const glm::vec3 at = glm::mix(home[s], home[s + 1], along / std::max(length, 0.01f)) +
                                         glm::vec3(b.random.Range(-0.3f, 0.3f), 0.0f, b.random.Range(-0.3f, 0.3f));
                    if (b.Built(at.x, at.z))
                    {
                        walked = 1.0e9f;
                        break;
                    }
                    if (b.Open(at.x, at.z) && b.random.Unit() > walked / 50.0f)
                    {
                        const float r = b.random.Range(0.02f, 0.05f) * std::max(scale, 0.6f);
                        b.Oval(at, {b.random.Range(-1.0f, 1.0f), b.random.Range(-1.0f, 1.0f)}, r * 1.4f, r, stained(at.x, at.z, 0.75f), 0.026f, 0.25f);
                    }
                }
            }
        }
    }

    // Claw marks scored in the wall either side of the doors of a building it lives in, in fours: dark grooves laid on the wall (a decal),
    // each tapering out at its ends.
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
                const glm::quat lean = facing * glm::angleAxis(slant, glm::vec3(0.0f, 0.0f, 1.0f));
                const glm::vec3 up = lean * glm::vec3(0.0f, 1.0f, 0.0f);
                const glm::vec3 across = lean * glm::vec3(1.0f, 0.0f, 0.0f);
                for (int c = 0; c < 4; ++c)
                {
                    const glm::vec3 at = face + along * (s * (1.05f + 0.07f * static_cast<float>(c) * std::max(scale, 0.7f))) + glm::vec3(0.0f, high, 0.0f) + out * 0.012f;
                    const float half = b.random.Range(0.2f, 0.35f) * std::max(scale, 0.7f);
                    // (Deepest in its middle: two halves, each fading to nothing at its far end.)
                    const glm::vec3 centre = at + up * b.random.Range(-0.04f, 0.04f);
                    b.Score(centre - up * half, centre, across, out, 0.016f, glm::vec3(0.3f, 0.29f, 0.28f));
                    b.Score(centre, centre + up * half * 0.8f, across, out, 0.012f, glm::vec3(0.38f, 0.37f, 0.36f));
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
    return done();
}

} // namespace pred
