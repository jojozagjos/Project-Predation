#include "Game/World/FacilityRuin.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pred
{

namespace FacilityRuin
{

namespace
{

using Kind = FacilityMap::Piece::Kind;
constexpr float kCell = FacilityLayout::kCell;
constexpr float kStorey = FacilityLayout::kStorey;
constexpr float kSlab = FacilityLayout::kSlab;
// Where the outside wall stands about the line of its cells: its inside face, its concrete, its cladding.
constexpr float kHalfWall = FacilitySpec::kHalfWall;
constexpr float kShell = FacilitySpec::kShellThickness;
constexpr float kCladding = FacilitySpec::kCladding;
constexpr float kReach = kShell + kCladding - kHalfWall;

uint32_t Mix(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return h;
}

float Hash01(uint32_t seed, uint32_t n)
{
    return static_cast<float>(Mix(seed, n) & 0xFFFFFFu) / static_cast<float>(0x1000000u);
}

// Smooth noise along a line, 0 to 1: a value at every whole x, eased between.
float Noise(uint32_t seed, float x)
{
    const float f = std::floor(x);
    const float t = x - f;
    const auto i = static_cast<uint32_t>(static_cast<int32_t>(f) + 0x40000000);
    return glm::mix(Hash01(seed, i), Hash01(seed, i + 1u), t * t * (3.0f - 2.0f * t));
}

// And over a plane.
float Noise(uint32_t seed, glm::vec2 p)
{
    const glm::vec2 f = glm::floor(p);
    const glm::vec2 t = p - f;
    const glm::vec2 s = t * t * (3.0f - 2.0f * t);
    const auto ix = static_cast<uint32_t>(static_cast<int32_t>(f.x) + 0x40000000);
    const auto iz = static_cast<uint32_t>(static_cast<int32_t>(f.y) + 0x40000000);
    const auto at = [&](uint32_t x, uint32_t z) { return Hash01(Mix(seed, x), z); };
    return glm::mix(glm::mix(at(ix, iz), at(ix + 1u, iz), s.x), glm::mix(at(ix, iz + 1u), at(ix + 1u, iz + 1u), s.x), s.y);
}

// A triangle facing `outward` (whichever way round it is given): its own corners, lit flat as broken concrete is, its texture laid
// on by whichever way it faces most, a metre to a unit.
void Tri(MeshData& mesh, glm::vec3 a, glm::vec3 b, glm::vec3 c, const glm::vec3& outward)
{
    glm::vec3 n = glm::cross(b - a, c - a);
    const float length = glm::length(n);
    if (length < 1.0e-7f)
    {
        return;
    }
    n /= length;
    if (glm::dot(n, outward) < 0.0f)
    {
        std::swap(b, c);
        n = -n;
    }
    const glm::vec3 m = glm::abs(n);
    const auto uv = [&](const glm::vec3& p) { return m.y >= m.x && m.y >= m.z ? glm::vec2(p.x, p.z) : m.x >= m.z ? glm::vec2(p.z, p.y) : glm::vec2(p.x, p.y); };
    const auto base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::vec3& p : {a, b, c})
    {
        MeshVertex vertex;
        vertex.position = p;
        vertex.normal = n;
        vertex.uv = uv(p);
        mesh.vertices.push_back(vertex);
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1u, base + 2u});
}

void Quad(MeshData& mesh, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, const glm::vec3& outward)
{
    Tri(mesh, a, b, c, outward);
    Tri(mesh, a, c, d, outward);
}

struct Builder
{
    const FacilityLayout& layout;
    FacilityMap::Blueprint& out;
    const FacilityLayout::Ruin& ruin;
    uint32_t seed = 0;
    float ground = 0.0f;
    MeshData concrete; // what broke: the tops of the stumps, the edges of the floors, the slabs' sides
    MeshData inside;   // the stumps' inside faces, in the walls' paint
    MeshData skin;     // their cladding
    MeshData floor;    // the floor, and the tops of floors that jut or fell
    MeshData under;    // the undersides of those
    MeshData heaps;    // the rubble heaped up
    MeshData chunks;   // and lying about
    MeshData shards;   // cladding torn off
    std::vector<glm::vec2> doors;
    uint32_t draws = 0;

    float Roll(float low, float high) { return low + (high - low) * Hash01(seed ^ 0x52D1u, draws++); }
    float X(float cells) const { return layout.origin.x + cells * kCell; }
    float Z(float cells) const { return layout.origin.z + cells * kCell; }
    bool Ruined(int x, int z) const { return ruin.Height(x, z) > 0; }
    bool Standing(int x, int z) const { return layout.Height(x, z) > 0; }
    bool Open(int x, int z) const { return !Ruined(x, z) && !Standing(x, z); }
    static glm::vec3 At(bool alongX, float along, float y, float across) { return alongX ? glm::vec3(along, y, across) : glm::vec3(across, y, along); }

    // How far a place is from what still stands, in cells (to four).
    float FromStanding(glm::vec2 cells) const
    {
        float nearest = 4.0f;
        for (int z = ruin.min.y - 4; z <= ruin.max.y + 4; ++z)
        {
            for (int x = ruin.min.x - 4; x <= ruin.max.x + 4; ++x)
            {
                if (Standing(x, z))
                {
                    const glm::vec2 lo{static_cast<float>(x), static_cast<float>(z)};
                    nearest = std::min(nearest, glm::distance(cells, glm::clamp(cells, lo, lo + glm::vec2(1.0f))));
                }
            }
        }
        return nearest;
    }
    float FromDoor(glm::vec2 at) const
    {
        float nearest = 1.0e9f;
        for (const glm::vec2& door : doors)
        {
            nearest = std::min(nearest, glm::distance(door, at));
        }
        return nearest;
    }

    // A steel bar out of what broke: from `foot` along `way`, so long.
    void Bar(const glm::vec3& foot, const glm::vec3& way, float length)
    {
        FacilityMap::Dressing bar;
        bar.form = FacilityMap::Dressing::Form::Cylinder;
        bar.kind = Kind::DarkSteel;
        bar.centre = foot + way * (length * 0.5f);
        bar.size = {0.018f, length, 0.018f};
        bar.rotation = glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(way));
        out.dressing.push_back(bar);
    }

    // A chunk of broken concrete, about `size` across, flattish: a box with every corner pushed about, turned any way and tipped a little.
    void Chunk(MeshData& mesh, const glm::vec3& at, const glm::vec3& size)
    {
        const glm::quat turn = glm::angleAxis(Roll(0.0f, glm::two_pi<float>()), glm::vec3(0.0f, 1.0f, 0.0f)) *
                               glm::angleAxis(Roll(-0.5f, 0.5f), glm::vec3(1.0f, 0.0f, 0.0f)) * glm::angleAxis(Roll(-0.4f, 0.4f), glm::vec3(0.0f, 0.0f, 1.0f));
        glm::vec3 corner[8];
        for (int i = 0; i < 8; ++i)
        {
            const glm::vec3 sign{(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f};
            const glm::vec3 pushed = sign * size * 0.5f * glm::vec3(Roll(0.55f, 1.2f), Roll(0.5f, 1.2f), Roll(0.55f, 1.2f));
            corner[i] = at + turn * pushed;
        }
        const int faces[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
        const glm::vec3 normals[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
        for (int f = 0; f < 6; ++f)
        {
            Quad(mesh, corner[faces[f][0]], corner[faces[f][1]], corner[faces[f][2]], corner[faces[f][3]], turn * normals[f]);
        }
    }

    // A straight run of edges of the ruin's cells, all of one kind: on the line of `line` (cells), facing `side` (0 -x, 1 +x, 2 -z,
    // 3 +z), cells `from` to `to` along it (not including `to`).
    struct Run
    {
        int side = 0;
        int line = 0;
        int from = 0;
        int to = 0;
    };
    glm::ivec2 Cell(const Run& run, int along) const { return run.side >= 2 ? glm::ivec2(along, run.line) : glm::ivec2(run.line, along); }
    glm::ivec2 Past(const Run& run, int along) const
    {
        static const glm::ivec2 steps[4] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        return Cell(run, along) + steps[run.side];
    }
    // The runs of edges out of the ruin past which there is nothing (free: the stumps of its outside walls) or what still stands.
    std::vector<Run> Runs(bool free) const
    {
        std::vector<Run> runs;
        for (int side = 0; side < 4; ++side)
        {
            const bool alongX = side >= 2;
            const int lines0 = alongX ? ruin.min.y : ruin.min.x;
            const int lines1 = alongX ? ruin.max.y : ruin.max.x;
            const int along0 = alongX ? ruin.min.x : ruin.min.y;
            const int along1 = alongX ? ruin.max.x : ruin.max.y;
            for (int line = lines0; line <= lines1; ++line)
            {
                Run run{side, line, -1, -1};
                for (int along = along0; along <= along1 + 1; ++along)
                {
                    bool is = false;
                    if (along <= along1)
                    {
                        const glm::ivec2 c = Cell(run, along);
                        const glm::ivec2 p = Past(run, along);
                        is = Ruined(c.x, c.y) && !Ruined(p.x, p.y) && (free ? !Standing(p.x, p.y) : Standing(p.x, p.y));
                    }
                    if (is && run.from < 0)
                    {
                        run.from = along;
                    }
                    if (!is && run.from >= 0)
                    {
                        run.to = along;
                        runs.push_back(run);
                        run.from = -1;
                    }
                }
            }
        }
        return runs;
    }

    // Where a run's wall line is (world), which way is out of the ruin across it, and its cells' extent along it (world).
    void Frame(const Run& run, float& line, glm::vec3& out, float& from, float& to) const
    {
        const bool alongX = run.side >= 2;
        const float sign = run.side == 0 || run.side == 2 ? -1.0f : 1.0f;
        line = alongX ? Z(static_cast<float>(run.line + (run.side == 3 ? 1 : 0))) : X(static_cast<float>(run.line + (run.side == 1 ? 1 : 0)));
        out = alongX ? glm::vec3(0.0f, 0.0f, sign) : glm::vec3(sign, 0.0f, 0.0f);
        from = alongX ? X(static_cast<float>(run.from)) : Z(static_cast<float>(run.from));
        to = alongX ? X(static_cast<float>(run.to)) : Z(static_cast<float>(run.to));
    }

    // How many storeys the ruin had over a place along a run.
    float Full(const Run& run, float along) const
    {
        const float origin = run.side >= 2 ? layout.origin.x : layout.origin.z;
        const int cell = std::clamp(static_cast<int>(std::floor((along - origin) / kCell)), run.from, run.to - 1);
        const glm::ivec2 c = Cell(run, cell);
        return static_cast<float>(ruin.Height(c.x, c.y) - layout.ground) * kStorey;
    }

    // Rubble heaped along a wall, from `start` (across, world) out the way `way` points, `wide` metres, along `from` to `to`, as high
    // as `high` says at a place along it; kept down near the doors. Chunks on it, and now and then a shard of cladding.
    template <typename High>
    void Heap(bool alongX, float start, float way, float from, float to, float wide, const High& high, uint32_t key)
    {
        constexpr int kAcross = 6;
        const int along = std::max(2, static_cast<int>(std::ceil((to - from) / 0.3f)));
        const auto height = [&](float s, float u)
        {
            const glm::vec3 p = At(alongX, s, 0.0f, start + way * u * wide);
            const float ends = glm::smoothstep(0.0f, 0.9f, std::min(s - from, to - s));
            const float h = high(s) * ends * glm::smoothstep(1.2f, 2.8f, FromDoor({p.x, p.z}));
            const float bump = (Noise(key, glm::vec2(p.x, p.z) * 1.7f) - 0.5f) * 0.35f * h;
            return ground - 0.04f + (h + bump) * std::pow(1.0f - u, 1.4f);
        };
        const auto point = [&](float s, float u) { return At(alongX, s, height(s, u), start + way * u * wide); };
        for (int i = 0; i < along; ++i)
        {
            const float s0 = from + (to - from) * static_cast<float>(i) / static_cast<float>(along);
            const float s1 = from + (to - from) * static_cast<float>(i + 1) / static_cast<float>(along);
            for (int k = 0; k < kAcross; ++k)
            {
                const float u0 = static_cast<float>(k) / static_cast<float>(kAcross);
                const float u1 = static_cast<float>(k + 1) / static_cast<float>(kAcross);
                Quad(heaps, point(s0, u0), point(s1, u0), point(s1, u1), point(s0, u1), {0.0f, 1.0f, 0.0f});
            }
        }
        const int count = static_cast<int>((to - from) * wide * Roll(1.6f, 3.0f));
        for (int c = 0; c < count; ++c)
        {
            const float s = Roll(from + 0.3f, to - 0.3f);
            const float u = Roll(0.05f, 0.85f);
            const float size = Roll(0.12f, 0.5f);
            const glm::vec3 p = point(s, u);
            if (Roll(0.0f, 1.0f) < 0.12f)
            {
                Chunk(shards, p + glm::vec3(0.0f, 0.03f, 0.0f), {Roll(0.4f, 1.0f), 0.035f, Roll(0.3f, 0.7f)});
                continue;
            }
            Chunk(chunks, p + glm::vec3(0.0f, size * 0.15f, 0.0f), {size, size * Roll(0.4f, 0.8f), size * Roll(0.6f, 1.1f)});
        }
    }

    // The stump of an outside wall along a free run: its concrete broken off at a height wandering along it -- higher nearer what
    // stands, notched here and there nearly to the floor -- its top ragged across its thickness, its cladding torn off lower than
    // the concrete behind it; steel out of its top; rubble heaped along it inside and out.
    void Stump(const Run& run)
    {
        const bool alongX = run.side >= 2;
        float line = 0.0f;
        float from = 0.0f;
        float to = 0.0f;
        glm::vec3 out{0.0f};
        Frame(run, line, out, from, to);
        const float sign = alongX ? out.z : out.x;
        const float inner = line - sign * kHalfWall;
        const float outer = inner + sign * kShell;
        const float clad = outer + sign * kCladding;
        // (At an outside corner of the ruin, the one along x carried over the corner, a few millimetres past the other's face; the one
        // along z stopping just inside it.)
        const glm::ivec2 before = Cell(run, run.from - 1);
        const glm::ivec2 after = Cell(run, run.to);
        if (Open(before.x, before.y))
        {
            from += alongX ? -(kReach + 0.005f) : kHalfWall - 0.01f;
        }
        if (Open(after.x, after.y))
        {
            to += alongX ? kReach + 0.005f : -(kHalfWall - 0.01f);
        }
        const uint32_t key = Mix(seed, static_cast<uint32_t>(run.side * 7919 + run.line * 131 + run.from));
        struct Notch
        {
            float at;
            float wide;
            float deep;
        };
        std::vector<Notch> notches;
        for (int n = static_cast<int>(Roll(0.0f, 2.99f)); n > 0; --n)
        {
            notches.push_back({Roll(from, to), Roll(0.4f, 1.3f), Roll(0.4f, 1.6f)});
        }
        const auto high = [&](float s)
        {
            const glm::vec3 p = At(alongX, s, 0.0f, line);
            const glm::vec2 cells = (glm::vec2(p.x, p.z) - glm::vec2(layout.origin.x, layout.origin.z)) / kCell;
            const float full = Full(run, s);
            const float near = FromStanding(cells);
            // (Lower away from what stands, but not on a straight slope: wandering, and broken off in lumps -- a length here gone, its
            // neighbour standing -- each lump's edge a few centimetres across, not a cut.)
            const float lump = s / 0.85f;
            const float edge = glm::smoothstep(0.0f, 0.12f, lump - std::floor(lump)) * glm::smoothstep(1.0f, 0.88f, lump - std::floor(lump));
            const float lumps = glm::mix(Hash01(key + 9u, static_cast<uint32_t>(static_cast<int32_t>(std::floor(lump)) + 0x40000000)), 0.5f, 1.0f - edge);
            float h = full * std::pow(std::clamp(1.0f - near / 3.0f, 0.08f, 0.96f), 0.75f) *
                          (0.2f + 0.5f * Noise(key, s / 1.6f) + 0.3f * Noise(key + 1u, s / 0.55f) + 0.45f * (lumps - 0.5f)) +
                      0.22f * (Noise(key + 2u, s / 0.2f) - 0.5f);
            for (const Notch& notch : notches)
            {
                h -= notch.deep * std::max(0.0f, 1.0f - std::abs(s - notch.at) / notch.wide);
            }
            return std::clamp(h, 0.1f, std::max(full, 0.1f));
        };
        const float bottom = ground - 0.29f;
        const int steps = std::max(2, static_cast<int>(std::ceil((to - from) / 0.22f)));
        const glm::vec3 up{0.0f, 1.0f, 0.0f};
        const glm::vec3 along = alongX ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
        const auto tops = [&](float s, float& hi, float& ho, float& k)
        {
            const float h = high(s);
            hi = ground + h + 0.08f * (Noise(key + 3u, s / 0.3f) - 0.5f);
            ho = ground + h + 0.08f * (Noise(key + 4u, s / 0.3f) - 0.5f);
            k = std::min(ground + std::max(0.05f, h - (0.04f + 0.5f * Noise(key + 5u, s / 0.45f))), ho - 0.02f);
        };
        for (int i = 0; i < steps; ++i)
        {
            const float a = from + (to - from) * static_cast<float>(i) / static_cast<float>(steps);
            const float b = from + (to - from) * static_cast<float>(i + 1) / static_cast<float>(steps);
            float hiA = 0.0f, hoA = 0.0f, kA = 0.0f, hiB = 0.0f, hoB = 0.0f, kB = 0.0f;
            tops(a, hiA, hoA, kA);
            tops(b, hiB, hoB, kB);
            // Its inside face, its broken top, the concrete over the torn cladding, the cladding and its torn edge.
            Quad(inside, At(alongX, a, bottom, inner), At(alongX, b, bottom, inner), At(alongX, b, hiB, inner), At(alongX, a, hiA, inner), -out);
            Quad(concrete, At(alongX, a, hiA, inner), At(alongX, b, hiB, inner), At(alongX, b, hoB, outer), At(alongX, a, hoA, outer), up);
            Quad(concrete, At(alongX, a, kA, outer), At(alongX, b, kB, outer), At(alongX, b, hoB, outer), At(alongX, a, hoA, outer), out);
            Quad(skin, At(alongX, a, ground - 0.27f, clad), At(alongX, b, ground - 0.27f, clad), At(alongX, b, kB, clad), At(alongX, a, kA, clad), out);
            Quad(skin, At(alongX, a, kA, outer), At(alongX, b, kB, outer), At(alongX, b, kB, clad), At(alongX, a, kA, clad), up);
        }
        // Its ends, where they stand free.
        for (const bool start : {true, false})
        {
            const float s = start ? from : to;
            float hi = 0.0f, ho = 0.0f, k = 0.0f;
            tops(s, hi, ho, k);
            const glm::vec3 way = start ? -along : along;
            Quad(concrete, At(alongX, s, bottom, inner), At(alongX, s, bottom, outer), At(alongX, s, ho, outer), At(alongX, s, hi, inner), way);
            Quad(skin, At(alongX, s, ground - 0.27f, outer), At(alongX, s, ground - 0.27f, clad), At(alongX, s, k, clad), At(alongX, s, k, outer), way);
        }
        // Steel out of its top, bent this way and that.
        for (float s = from + Roll(0.2f, 0.8f); s < to - 0.15f; s += Roll(0.5f, 1.2f))
        {
            if (Roll(0.0f, 1.0f) < 0.5f)
            {
                continue;
            }
            const float h = ground + high(s) - 0.05f;
            const glm::vec3 lean = glm::normalize(up + along * Roll(-0.8f, 0.8f) + out * Roll(-0.7f, 0.7f));
            Bar(At(alongX, s, h, (inner + outer) * 0.5f), lean, Roll(0.25f, 0.9f));
        }
        // The rubble along its foot, inside, and spilt out past it.
        const auto heaped = [&](float scale) { return [&, scale](float s) { return std::min(Full(run, s) * 0.18f + 0.25f, 1.15f) * scale * (0.3f + 0.9f * Noise(key + 6u, s / 1.3f)); }; };
        Heap(alongX, inner + sign * 0.05f, -sign, from, to, Roll(1.3f, 2.6f), heaped(1.0f), key + 7u);
        Heap(alongX, clad - sign * 0.05f, sign, from, to, Roll(0.8f, 1.8f), heaped(0.7f), key + 8u);
    }

    // What is left of the floors that were over the ruin, at the wall left standing along a run: the end of each, jutting from it
    // broken -- further here, hardly at all there -- sagging as it goes, its edge crumbled thinner; none over a door through it; steel
    // hanging out of its edge; rubble heaped at the wall's foot.
    void Stubs(const Run& run)
    {
        const bool alongX = run.side >= 2;
        float line = 0.0f;
        float from = 0.0f;
        float to = 0.0f;
        glm::vec3 out{0.0f};
        Frame(run, line, out, from, to);
        const float sign = alongX ? out.z : out.x;
        const float face = line - sign * kReach; // the standing wall's face (`out` points into it from the ruin)
        const float in = -sign;
        const uint32_t key = Mix(seed ^ 0x57B5u, static_cast<uint32_t>(run.side * 7919 + run.line * 131 + run.from));
        const float origin = alongX ? layout.origin.x : layout.origin.z;
        const auto cellOf = [&](float s) { return std::clamp(static_cast<int>(std::floor((s - origin) / kCell)), run.from, run.to - 1); };
        const auto storeysAt = [&](float s)
        {
            const int along = cellOf(s);
            const glm::ivec2 c = Cell(run, along);
            const glm::ivec2 p = Past(run, along);
            return std::min(ruin.Height(c.x, c.y), layout.Height(p.x, p.y)) - layout.ground;
        };
        const auto doorAt = [&](float s)
        {
            const glm::ivec2 p = Past(run, cellOf(s));
            return std::any_of(layout.exits.begin(), layout.exits.end(),
                               [&](const FacilityLayout::Exit& exit) { return exit.cell == p && exit.side == (run.side ^ 1); });
        };
        int tallest = 0;
        for (int along = run.from; along < run.to; ++along)
        {
            const glm::ivec2 c = Cell(run, along);
            const glm::ivec2 p = Past(run, along);
            tallest = std::max(tallest, std::min(ruin.Height(c.x, c.y), layout.Height(p.x, p.y)) - layout.ground);
        }
        const int steps = std::max(2, static_cast<int>(std::ceil((to - from) / 0.2f)));
        const glm::vec3 up{0.0f, 1.0f, 0.0f};
        const glm::vec3 along = alongX ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
        for (int f = 1; f <= tallest; ++f)
        {
            const float top = ground + static_cast<float>(f) * kStorey;
            const uint32_t floorKey = key + static_cast<uint32_t>(f) * 977u;
            // How far it juts at a place, how far its edge has sagged and how thin it has crumbled there.
            const auto jut = [&](float s)
            {
                if (f > storeysAt(s) || (f == 1 && doorAt(s)))
                {
                    return 0.0f;
                }
                const float far = 1.5f * (0.1f + 0.9f * Noise(floorKey, s / 0.9f));
                return Noise(floorKey + 1u, s / 0.5f) < 0.3f ? far * 0.08f : far;
            };
            const auto edge = [&](float s, glm::vec3& wallTop, glm::vec3& wallBottom, glm::vec3& tipTop, glm::vec3& tipBottom)
            {
                const float j = jut(s);
                const float sag = j * (0.05f + 0.35f * Noise(floorKey + 2u, s / 0.7f));
                const float thin = kSlab * (0.35f + 0.55f * Noise(floorKey + 3u, s / 0.35f));
                const float wall = face - in * 0.03f; // (a little into the wall: where it does not jut, all of it there)
                wallTop = At(alongX, s, top, wall);
                wallBottom = At(alongX, s, top - kSlab, wall);
                tipTop = At(alongX, s, top - sag, wall + in * j);
                tipBottom = At(alongX, s, top - sag - (j > 0.0f ? thin : kSlab), wall + in * j);
            };
            for (int i = 0; i < steps; ++i)
            {
                const float a = from + (to - from) * static_cast<float>(i) / static_cast<float>(steps);
                const float b = from + (to - from) * static_cast<float>(i + 1) / static_cast<float>(steps);
                glm::vec3 wtA, wbA, ttA, tbA, wtB, wbB, ttB, tbB;
                edge(a, wtA, wbA, ttA, tbA);
                edge(b, wtB, wbB, ttB, tbB);
                Quad(floor, wtA, wtB, ttB, ttA, up);
                Quad(under, wbA, wbB, tbB, tbA, -up);
                Quad(concrete, ttA, ttB, tbB, tbA, -out);
            }
            for (const bool start : {true, false})
            {
                glm::vec3 wt, wb, tt, tb;
                edge(start ? from : to, wt, wb, tt, tb);
                Quad(concrete, wt, tt, tb, wb, start ? -along : along);
            }
            for (float s = from + Roll(0.2f, 0.6f); s < to - 0.15f; s += Roll(0.35f, 0.9f))
            {
                glm::vec3 wt, wb, tt, tb;
                edge(s, wt, wb, tt, tb);
                if (jut(s) < 0.3f || Roll(0.0f, 1.0f) < 0.45f)
                {
                    continue;
                }
                const float droop = Roll(0.15f, 1.3f);
                Bar(glm::mix(tt, tb, Roll(0.3f, 0.7f)) - out * 0.02f, glm::normalize(-out * std::cos(droop) - up * std::sin(droop)), Roll(0.3f, 1.0f));
            }
        }
        // What came down from them, heaped at the wall's foot.
        const auto heaped = [&](float s)
        { return std::min(static_cast<float>(storeysAt(s)) * 0.3f + 0.2f, 1.1f) * (0.3f + 0.9f * Noise(key + 9u, s / 1.4f)); };
        Heap(alongX, face + in * 0.05f, in, from, to, Roll(1.4f, 2.8f), heaped, key + 10u);
    }

    // A slab come down from over the ruin, lying tipped across the floor: a broken plate, its edge ragged, a bar or two out of it.
    void Fallen(const glm::vec3& at)
    {
        constexpr int kSides = 7;
        const float thick = Roll(0.18f, 0.28f);
        const glm::vec2 radius{Roll(1.2f, 2.0f), Roll(0.9f, 1.5f)};
        const glm::quat turn = glm::angleAxis(Roll(0.0f, glm::two_pi<float>()), glm::vec3(0.0f, 1.0f, 0.0f)) *
                               glm::angleAxis(Roll(0.25f, 0.7f), glm::vec3(1.0f, 0.0f, 0.0f));
        std::vector<glm::vec3> rim;
        for (int i = 0; i < kSides; ++i)
        {
            const float angle = glm::two_pi<float>() * (static_cast<float>(i) + Roll(-0.3f, 0.3f)) / static_cast<float>(kSides);
            rim.push_back({std::cos(angle) * radius.x * Roll(0.55f, 1.15f), 0.0f, std::sin(angle) * radius.y * Roll(0.55f, 1.15f)});
        }
        const glm::vec3 half{0.0f, thick * 0.5f, 0.0f};
        float lowest = 1.0e9f;
        for (const glm::vec3& p : rim)
        {
            lowest = std::min(lowest, (turn * (p - half)).y);
        }
        const glm::vec3 centre{at.x, ground - 0.03f - lowest, at.z};
        const auto world = [&](const glm::vec3& p) { return centre + turn * p; };
        const glm::vec3 up = turn * glm::vec3(0.0f, 1.0f, 0.0f);
        MeshData top;
        MeshData bottom;
        for (int i = 0; i < kSides; ++i)
        {
            const glm::vec3& p = rim[static_cast<size_t>(i)];
            const glm::vec3& q = rim[static_cast<size_t>((i + 1) % kSides)];
            Tri(top, world(half), world(p + half), world(q + half), up);
            Tri(bottom, world(-half), world(p - half), world(q - half), -up);
            const glm::vec3 side = turn * glm::normalize(glm::vec3((p + q).x, 0.0f, (p + q).z));
            Quad(concrete, world(p - half), world(q - half), world(q + half), world(p + half), side);
        }
        out.shapes.push_back({Kind::Floor, std::move(top), true});
        out.shapes.push_back({Kind::Ceiling, std::move(bottom), true});
        for (int b = static_cast<int>(Roll(0.0f, 2.99f)); b > 0; --b)
        {
            const glm::vec3& p = rim[static_cast<size_t>(Roll(0.0f, static_cast<float>(kSides) - 0.01f))];
            const glm::vec3 way = turn * glm::normalize(glm::vec3(p.x, Roll(-0.3f, 0.4f), p.z));
            Bar(world(p), way, Roll(0.3f, 0.8f));
        }
    }

    void Draw()
    {
        for (const FacilityLayout::Exit& exit : layout.exits)
        {
            const glm::vec3 at = FacilityMap::ExitOutside(layout, exit, 0.5f);
            doors.emplace_back(at.x, at.z);
        }
        // The floor: whole (it was poured as one), at the ground floor's level.
        for (int z = ruin.min.y; z <= ruin.max.y; ++z)
        {
            for (int x = ruin.min.x; x <= ruin.max.x; ++x)
            {
                if (!Ruined(x, z))
                {
                    continue;
                }
                const float x0 = X(static_cast<float>(x));
                const float x1 = X(static_cast<float>(x + 1));
                const float z0 = Z(static_cast<float>(z));
                const float z1 = Z(static_cast<float>(z + 1));
                Quad(floor, {x0, ground, z0}, {x1, ground, z0}, {x1, ground, z1}, {x0, ground, z1}, {0.0f, 1.0f, 0.0f});
                // Grimed, under the open sky: dirt and the dust of what came down, in patches (FacilityMap's stains), each a
                // millimetre over the last so none lies in another's place.
                for (int g = static_cast<int>(Roll(1.0f, 3.99f)); g > 0; --g)
                {
                    FacilityMap::Dressing stain;
                    stain.form = FacilityMap::Dressing::Form::Patch;
                    stain.kind = Kind::Floor;
                    stain.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)) *
                                     glm::angleAxis(Roll(0.0f, glm::two_pi<float>()), glm::vec3(0.0f, 0.0f, 1.0f));
                    stain.centre = {Roll(x0 + 0.5f, x1 - 0.5f), ground + 0.002f + 0.001f * static_cast<float>(draws % 4u), Roll(z0 + 0.5f, z1 - 0.5f)};
                    const float wide = Roll(1.0f, 2.6f);
                    stain.size = {wide, wide * Roll(0.6f, 1.0f), 0.85f};
                    stain.tint = glm::vec3(Roll(0.38f, 0.55f), Roll(0.35f, 0.5f), Roll(0.3f, 0.44f));
                    stain.shape = Mix(seed, draws++);
                    out.dressing.push_back(stain);
                }
                // Chunks over it.
                for (int c = static_cast<int>(Roll(1.0f, 5.0f)); c > 0; --c)
                {
                    const float size = Roll(0.1f, 0.4f);
                    const glm::vec3 at{Roll(x0, x1), ground + size * 0.12f, Roll(z0, z1)};
                    if (FromDoor({at.x, at.z}) > 1.5f)
                    {
                        Chunk(chunks, at, {size, size * Roll(0.35f, 0.7f), size * Roll(0.6f, 1.1f)});
                    }
                }
            }
        }
        for (const Run& run : Runs(true))
        {
            Stump(run);
        }
        for (const Run& run : Runs(false))
        {
            Stubs(run);
        }
        // A slab or two come down, out of the way of the doors.
        const int cells = (ruin.max.x - ruin.min.x + 1) * (ruin.max.y - ruin.min.y + 1);
        for (int s = std::clamp(cells / 6, 1, 3); s > 0; --s)
        {
            const glm::vec3 at{X(Roll(static_cast<float>(ruin.min.x) + 0.6f, static_cast<float>(ruin.max.x) + 0.4f)), ground,
                               Z(Roll(static_cast<float>(ruin.min.y) + 0.6f, static_cast<float>(ruin.max.y) + 0.4f))};
            const glm::vec2 cell = (glm::vec2(at.x, at.z) - glm::vec2(layout.origin.x, layout.origin.z)) / kCell;
            if (Ruined(static_cast<int>(cell.x), static_cast<int>(cell.y)) && FromDoor({at.x, at.z}) > 3.5f && Roll(0.0f, 1.0f) < 0.7f)
            {
                Fallen(at);
            }
        }
        out.shapes.push_back({Kind::Floor, std::move(floor), true});
        out.shapes.push_back({Kind::Ceiling, std::move(under), true});
        out.shapes.push_back({Kind::Wall, std::move(inside), true});
        out.shapes.push_back({Kind::Cladding, std::move(skin), true});
        out.shapes.push_back({Kind::Rubble, std::move(concrete), true});
        out.shapes.push_back({Kind::RockFloor, std::move(heaps), true}); // (gravelly: what was crushed)
        out.shapes.push_back({Kind::Rubble, std::move(chunks), false});
        out.shapes.push_back({Kind::Cladding, std::move(shards), false});
    }
};

} // namespace

void Draw(const FacilityLayout& layout, FacilityMap::Blueprint& out)
{
    if (!layout.ruin.Has())
    {
        return;
    }
    Builder builder{layout, out, layout.ruin};
    builder.seed = Mix(layout.seed, 0x2F11u);
    builder.ground = layout.GroundLevel();
    builder.Draw();
}

} // namespace FacilityRuin

} // namespace pred
