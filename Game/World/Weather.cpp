#include "Game/World/Weather.h"

#include "Engine/Render/Material.h"
#include "Engine/Render/Primitives.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

// The box the weather fills round the eye: wide, and taller above than below. (Smaller for what is fine and close --
// dust, ash -- so it is thick where it is seen.)
constexpr glm::vec3 kBigBox{34.0f, 16.0f, 34.0f};
constexpr float kBigAbove = 11.0f;

// How each kind falls and looks. `pieces` at the most (fewer as there is less of it); `low` of them blown along the
// ground rather than falling; `fall` metres a second, from and to; `size` metres, from and to; `shape` the piece's
// proportions (a flake flat, a drop a streak); `blow` how much of the wind it takes; `swirl` how far it wanders;
// `glow` the share that are embers.
struct KindLook
{
    int pieces;
    int low;
    float fallFrom, fallTo;
    float sizeFrom, sizeTo;
    glm::vec3 shape;
    float blow;
    float swirl;
    bool spins;
    bool streak; // drawn along the way it is going
    float glow;
    glm::vec3 color;
    glm::vec3 emissive;
    float reach = 1.0f; // of the box
};

const KindLook& LookOf(Weather::Kind kind)
{
    static const KindLook kSnow{720, 170, 0.7f, 1.7f, 0.026f, 0.064f, {1.0f, 0.35f, 1.0f}, 1.0f, 0.35f, true, false, 0.0f,
                                {0.92f, 0.94f, 0.97f}, {0.16f, 0.165f, 0.18f}};
    static const KindLook kRain{1300, 0, 8.0f, 11.0f, 0.4f, 0.65f, {0.014f, 1.0f, 0.014f}, 1.6f, 0.0f, false, true, 0.0f,
                                {0.6f, 0.66f, 0.72f}, {0.05f, 0.055f, 0.06f}};
    static const KindLook kDust{1000, 600, -0.15f, 0.35f, 0.02f, 0.05f, {1.0f, 0.8f, 1.0f}, 2.6f, 0.8f, true, false, 0.0f,
                                {0.6f, 0.5f, 0.38f}, {0.03f, 0.026f, 0.02f}, 0.55f};
    static const KindLook kAsh{800, 140, 0.3f, 0.8f, 0.03f, 0.07f, {1.0f, 0.3f, 1.0f}, 0.8f, 0.6f, true, false, 0.08f,
                               {0.16f, 0.16f, 0.17f}, {0.0f, 0.0f, 0.0f}, 0.7f};
    switch (kind)
    {
    case Weather::Kind::Rain:
        return kRain;
    case Weather::Kind::Dust:
        return kDust;
    case Weather::Kind::Ash:
        return kAsh;
    default:
        return kSnow;
    }
}

// One piece: a box of 24 corners (four a face, so each face has its own way out), made once and turned into place.
const MeshData& UnitBox()
{
    static const MeshData box = Primitives::Box({1.0f, 1.0f, 1.0f});
    return box;
}

MeshData Pieces(int count)
{
    MeshData mesh;
    const MeshData& box = UnitBox();
    for (int i = 0; i < count; ++i)
    {
        mesh.Append(box, glm::mat4(1.0f));
    }
    return mesh;
}

} // namespace

Weather::Kind Weather::KindOf(const std::string& particles)
{
    return particles == "snow" ? Kind::Snow : particles == "rain" ? Kind::Rain : particles == "dust" ? Kind::Dust : particles == "ash" ? Kind::Ash : Kind::None;
}

void Weather::Make(Scene& scene, MeshLibrary& meshes, Kind kind, const glm::vec3& eye, const glm::vec3& tint)
{
    // Whatever was falling before goes.
    for (Entity* entity : {&m_entity, &m_glowEntity})
    {
        if (entity->IsValid())
        {
            scene.Destroy(*entity);
            *entity = Entity{};
        }
    }
    for (MeshHandle* mesh : {&m_mesh, &m_glowMesh})
    {
        if (mesh->IsValid())
        {
            meshes.Release(*mesh);
            *mesh = MeshHandle{};
        }
    }
    m_pieces.clear();
    m_kind = kind;
    m_shown = false;
    if (kind == Kind::None)
    {
        return;
    }
    const auto random = [this]()
    {
        m_seed = m_seed * 1664525u + 1013904223u;
        return static_cast<float>(m_seed >> 8) / static_cast<float>(1u << 24);
    };
    const KindLook& look = LookOf(kind);
    const glm::vec3 box = kBigBox * look.reach;
    const float above = kBigAbove * look.reach;
    m_glowCount = static_cast<int>(std::round(static_cast<float>(look.pieces) * look.glow));
    for (int i = 0; i < look.pieces; ++i)
    {
        Piece piece;
        piece.low = i < look.low;
        piece.glow = i >= look.pieces - m_glowCount;
        piece.at = eye + glm::vec3((random() - 0.5f) * box.x, above - random() * box.y, (random() - 0.5f) * box.z);
        piece.fall = piece.low ? 0.0f : look.fallFrom + random() * (look.fallTo - look.fallFrom);
        piece.sway = random() * 6.2831853f;
        piece.size = look.sizeFrom + random() * (look.sizeTo - look.sizeFrom);
        if (piece.low)
        {
            piece.size *= 0.7f;
        }
        piece.spin = glm::normalize(glm::vec3(random() - 0.5f, random() - 0.5f, random() - 0.5f) + glm::vec3(0.0f, 0.001f, 0.0f));
        piece.spinRate = 1.5f + random() * 4.0f;
        // Mostly just over it; now and then lifted higher (dust higher than snow).
        piece.height = 0.05f + random() * random() * (kind == Kind::Dust ? 4.0f : 0.7f);
        m_pieces.push_back(piece);
    }
    const auto entityFor = [&](MeshHandle mesh, const Material& material, const char* name)
    {
        const Entity entity = scene.CreateMeshEntity(name, Transform{}, mesh, material);
        if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
        {
            renderer->castsShadow = false;
            renderer->blocksSky = false;
            renderer->visible = false;
        }
        return entity;
    };
    const int plain = look.pieces - m_glowCount;
    // Dust is the ground it was lifted from, a shade darker and browner, so it shows crossing it.
    Material material = Material::Diffuse(kind == Kind::Dust ? tint * glm::vec3(0.62f, 0.55f, 0.46f) : look.color, kind == Kind::Rain ? 0.3f : 1.0f);
    // A little light of its own, so it shows against the dark as it does, catching whatever light there is.
    material.emissive = look.emissive;
    m_tint = tint;
    m_mesh = meshes.CreateDynamic(Pieces(plain), "weather");
    m_entity = entityFor(m_mesh, material, "weather");
    m_vertices.assign(static_cast<size_t>(plain) * UnitBox().vertices.size(), MeshVertex{});
    if (m_glowCount > 0)
    {
        Material ember = Material::Diffuse({1.0f, 0.4f, 0.1f}, 1.0f);
        ember.emissive = {2.6f, 0.9f, 0.18f};
        m_glowMesh = meshes.CreateDynamic(Pieces(m_glowCount), "weather_glow");
        m_glowEntity = entityFor(m_glowMesh, ember, "weather_glow");
        m_glowVertices.assign(static_cast<size_t>(m_glowCount) * UnitBox().vertices.size(), MeshVertex{});
    }
}

void Weather::Show(Scene& scene, bool shown)
{
    if (shown == m_shown)
    {
        return;
    }
    m_shown = shown;
    for (const Entity entity : {m_entity, m_glowEntity})
    {
        if (MeshRenderer* renderer = scene.GetMeshRenderer(entity))
        {
            renderer->visible = shown;
        }
    }
}

void Weather::Update(Scene& scene, MeshLibrary& meshes, const glm::vec3& eye, float ground, float dt, Kind kind, float intensity, const glm::vec3& tint,
                     const glm::vec3& wind, const std::function<bool(const glm::vec3&)>& indoors)
{
    if (kind != m_kind || (kind == Kind::Dust && glm::length(tint - m_tint) > 0.02f))
    {
        Make(scene, meshes, kind, eye, tint);
    }
    if (kind == Kind::None || intensity <= 0.0f)
    {
        Show(scene, false);
        return;
    }
    Show(scene, true);
    const KindLook& look = LookOf(kind);
    const glm::vec3 box = kBigBox * look.reach;
    const float above = kBigAbove * look.reach;
    m_clock += dt;
    // Gusting: the wind rises and falls, now and then hard.
    const float gust = 0.7f + 0.6f * std::sin(m_clock * 0.31f) * std::sin(m_clock * 0.77f + 1.3f) + 0.35f * std::max(std::sin(m_clock * 0.13f), 0.0f);
    // Dust is only there because the wind is lifting it: never less than a stiff breeze.
    glm::vec3 blown = wind;
    if (kind == Kind::Dust && glm::length(blown) < 1.2f)
    {
        blown = (glm::length(blown) > 1.0e-4f ? glm::normalize(blown) : glm::vec3(0.8f, 0.0f, 0.6f)) * 1.2f;
    }
    const glm::vec3 blowing = blown * gust * look.blow;
    // As much of it as there is: a light fall is a third of a heavy one.
    const int shown = static_cast<int>(static_cast<float>(m_pieces.size()) * (0.35f + 0.65f * std::clamp(intensity, 0.0f, 1.0f)));
    const MeshData& unit = UnitBox();
    const size_t corners = unit.vertices.size();
    glm::vec3 lo{1e9f};
    glm::vec3 hi{-1e9f};
    int plainIndex = 0;
    int glowIndex = 0;
    for (int i = 0; i < static_cast<int>(m_pieces.size()); ++i)
    {
        Piece& piece = m_pieces[static_cast<size_t>(i)];
        glm::vec3 local = piece.at - eye;
        if (piece.low)
        {
            // Low over the ground, streaming with the wind, lifting and settling.
            piece.at += blowing * 3.2f * dt;
            piece.at.y = ground + piece.height + 0.12f * std::sin(m_clock * 2.3f + piece.sway);
            local = piece.at - eye;
        }
        else
        {
            const float swayX = std::sin(m_clock * 1.3f + piece.sway) * look.swirl;
            const float swayZ = std::cos(m_clock * 1.1f + piece.sway * 1.7f) * look.swirl;
            piece.at += glm::vec3(blowing.x + swayX, -piece.fall, blowing.z + swayZ) * dt;
            local = piece.at - eye;
            // Below, it starts again at the top (and what rises, at the bottom).
            if (local.y < above - box.y)
            {
                local.y += box.y;
            }
            else if (local.y > above)
            {
                local.y -= box.y;
            }
        }
        // Round again at the other side of the box when it leaves it.
        local.x = std::fmod(local.x + box.x * 1.5f, box.x) - box.x * 0.5f;
        local.z = std::fmod(local.z + box.z * 1.5f, box.z) - box.z * 0.5f;
        piece.at.x = eye.x + local.x;
        piece.at.z = eye.z + local.z;
        if (!piece.low)
        {
            piece.at.y = eye.y + local.y;
        }
        // Not indoors, and only as many as there is weather for: the rest drawn as nothing.
        const bool show = i < shown && !indoors(piece.at);
        glm::quat turn;
        if (look.streak)
        {
            // Along the way it is going: a streak of rain leans with the wind.
            const glm::vec3 way = glm::normalize(glm::vec3(blowing.x, -piece.fall, blowing.z));
            turn = glm::quat(glm::vec3(0.0f, -1.0f, 0.0f), way);
        }
        else
        {
            turn = glm::angleAxis(m_clock * piece.spinRate + piece.sway, piece.spin);
        }
        const glm::vec3 scale = show ? look.shape * piece.size : glm::vec3(0.0f);
        std::vector<MeshVertex>& out = piece.glow ? m_glowVertices : m_vertices;
        const size_t base = static_cast<size_t>(piece.glow ? glowIndex++ : plainIndex++) * corners;
        for (size_t c = 0; c < corners; ++c)
        {
            MeshVertex vertex = unit.vertices[c];
            vertex.position = piece.at + turn * (vertex.position * scale);
            vertex.normal = turn * vertex.normal;
            out[base + c] = vertex;
        }
        lo = glm::min(lo, piece.at);
        hi = glm::max(hi, piece.at);
    }
    const AABB bounds{lo - glm::vec3(1.0f), hi + glm::vec3(1.0f)};
    meshes.UpdateDynamic(m_mesh, m_vertices, bounds);
    if (m_glowMesh.IsValid())
    {
        meshes.UpdateDynamic(m_glowMesh, m_glowVertices, bounds);
    }
}

} // namespace pred
