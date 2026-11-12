// Rain on the site shuttle's windows (PredationGame::UpdateWindscreenRain): drops landing on the glass as it comes down under the
// cloud and while it stands on the pad, beaded, running, blown across it by the air going past, drying off when the rain stops.

#include "Game/PredationGame.h"
#include "Game/PredationGameShared.h"

#include "Engine/Render/Primitives.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

namespace
{

// The two sheets of it: the windscreen and its corners either side, and the flight deck's side windows. Each its own picture of
// drops, laid over its panes so many metres to a repeat of it: the same drops over every pane of it, never the same drop twice on
// one pane.
constexpr int kFront = 0;
constexpr int kSides = 1;
constexpr int kSizes[2] = {1024, 768};     // its picture, square, in pixels
constexpr float kRepeat[2] = {1.4f, 1.2f}; // metres of glass to a repeat of it
constexpr size_t kMostDrops[2] = {2600, 1500};
// A bead this big (in pixels, about seven millimetres across) is heavy enough to run, and a run this small sticks again.
constexpr float kRuns = 5.0f;
constexpr float kStops = 3.4f;

int Wrap(int v, int size)
{
    v %= size;
    return v < 0 ? v + size : v;
}

float WrapF(float v, float size)
{
    v = std::fmod(v, size);
    return v < 0.0f ? v + size : v;
}

uint32_t Mix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// A spot on the glass that holds a drop back (a speck of dirt, a dry patch), 0 to 1 by where it is.
float Spot(int x, int y)
{
    return static_cast<float>(Mix32(static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u) & 0xFFFFu) / 65535.0f;
}

} // namespace

bool PredationGame::BuildRainGlass(VehicleProp& shuttle)
{
    // The windows' panes as the shuttle's model has them (fx_windscreen), each laid out flat as its own picture: up the picture is up
    // the glass, and across it -- on the windscreen to the right, on a side window towards the back, the way the air goes past it.
    const ModelAsset* model = shuttle.Model();
    const ModelPart* windows = nullptr;
    if (model != nullptr)
    {
        for (const ModelPart& part : model->parts)
        {
            windows = part.name == "fx_windscreen" ? &part : windows;
        }
    }
    if (windows == nullptr || windows->mesh.vertices.empty())
    {
        return false;
    }
    glm::vec3 middle{0.0f};
    for (const MeshVertex& vertex : windows->mesh.vertices)
    {
        middle += vertex.position;
    }
    middle /= static_cast<float>(windows->mesh.vertices.size());
    MeshData sheets[2];
    const std::vector<uint32_t>& indices = windows->mesh.indices;
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const MeshVertex* corner[3] = {&windows->mesh.vertices[indices[i]], &windows->mesh.vertices[indices[i + 1]], &windows->mesh.vertices[indices[i + 2]]};
        glm::vec3 normal = glm::cross(corner[1]->position - corner[0]->position, corner[2]->position - corner[0]->position);
        if (glm::length(normal) < 1.0e-8f)
        {
            continue;
        }
        normal = glm::normalize(normal);
        const glm::vec3 centre = (corner[0]->position + corner[1]->position + corner[2]->position) / 3.0f;
        if (glm::dot(normal, centre - middle) < 0.0f)
        {
            normal = -normal;
        }
        const int which = std::abs(normal.x) > 0.6f ? kSides : kFront;
        // Up the glass, and across it.
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f) - normal * normal.y;
        up = glm::length(up) > 1.0e-4f ? glm::normalize(up) : glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 across = which == kSides ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        across -= normal * glm::dot(across, normal) + up * glm::dot(across, up);
        across = glm::length(across) > 1.0e-4f ? glm::normalize(across) : glm::cross(up, normal);
        MeshData& sheet = sheets[which];
        const auto base = static_cast<uint32_t>(sheet.vertices.size());
        for (const MeshVertex* source : corner)
        {
            MeshVertex vertex = *source;
            // A few millimetres out from the glass, on its outside.
            vertex.position += normal * 0.006f;
            vertex.normal = normal;
            vertex.uv = {glm::dot(source->position, across) / kRepeat[which], glm::dot(source->position, up) / kRepeat[which]};
            sheet.vertices.push_back(vertex);
        }
        sheet.indices.insert(sheet.indices.end(), {base, base + 1, base + 2});
    }
    for (int which = 0; which < 2; ++which)
    {
        RainGlass& glass = m_rainGlass[static_cast<size_t>(which)];
        if (sheets[which].vertices.empty())
        {
            continue;
        }
        if (!glass.texture.IsValid())
        {
            glass.texture = m_app->GetTextures().CreateDynamic(kSizes[which], kSizes[which], which == kFront ? "rain_windscreen" : "rain_side_windows");
        }
        // Lit only by its own glow (set as it is drawn), the light through the water; there only where the picture says a drop is.
        Material drops = Material::Diffuse(glm::vec3(0.0f), 1.0f);
        drops.baseColorTexture = glass.texture;
        drops.emissiveTextured = true;
        drops.cutout = true;
        drops.still = true; // (not a plant: the wind does not bend it)
        drops.weathering = 0.0f;
        glass.film = m_scene.CreateMeshEntity(which == kFront ? "rain_windscreen" : "rain_side_windows", Transform{},
                                              m_app->GetMeshes().Upload(sheets[which], which == kFront ? "rain_windscreen" : "rain_side_windows"), drops);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(glass.film))
        {
            renderer->castsShadow = false;
            renderer->blocksSky = false;
            renderer->visible = false;
        }
        // The air going past drives drops up the windscreen, and back along the side windows.
        glass.blow = which == kFront ? glm::vec2(0.0f, 1.0f) : glm::vec2(1.0f, 0.0f);
        glass.share = which == kFront ? 1.0f : 0.55f;
        glass.wet.assign(static_cast<size_t>(kSizes[which] * kSizes[which]), 0);
        glass.drops.clear();
    }
    return true;
}

void PredationGame::UpdateWindscreenRain(float dt)
{
    // Rain on the site shuttle's windows, as it is on real glass. Each drop lands where it lands and stays put, a bead held by the
    // glass; one landing on another runs into it, the two one bigger bead. A bead grown heavy enough runs down -- in fits and starts,
    // held a moment here and there, wandering a little, taking up the beads in its way and leaving a wet trail that dries and breaks
    // up behind it -- and sticks again once it has left enough of itself behind. In flight the air going past drives the bigger
    // drops across the glass in short streaks (up the windscreen, back along the side windows), shedding water as they go, the small
    // ones clinging on. When the rain stops they slowly dry off. Laid on the panes themselves, moved with the glass every frame.
    VehicleProp& shuttle = m_facility.Shuttle();
    const Entity panes = m_map == MapChoice::Facility && shuttle.Built() ? shuttle.Part("fx_windscreen") : Entity{};
    const Transform* glassAt = panes.IsValid() ? m_scene.GetTransform(panes) : nullptr;
    float rain = 0.0f;
    if (glassAt != nullptr)
    {
        const SiteMap::Look& look = m_facility.GetLook();
        const Environment& sky = m_scene.GetEnvironment();
        const float under = 1.0f - glm::smoothstep(sky.deckBase, sky.deckBase + 300.0f, glassAt->position.y - m_facility.Plan().origin.y);
        // (Even a light rain beads a window over: the drops landing go by the square root of how hard it is coming down.)
        rain = Weather::KindOf(look.weather) == Weather::Kind::Rain ? std::sqrt(std::clamp(look.weatherAmount, 0.0f, 1.0f)) * under : 0.0f;
    }
    const bool any = !m_rainGlass[0].drops.empty() || !m_rainGlass[1].drops.empty();
    if (glassAt == nullptr || (rain < 0.01f && !any))
    {
        for (RainGlass& glass : m_rainGlass)
        {
            if (MeshRenderer* renderer = glass.film.IsValid() ? m_scene.GetMeshRenderer(glass.film) : nullptr)
            {
                renderer->visible = false;
            }
            glass.drops.clear();
        }
        return;
    }
    if (!m_rainGlass[kFront].film.IsValid() || m_scene.GetTransform(m_rainGlass[kFront].film) == nullptr)
    {
        if (!BuildRainGlass(shuttle))
        {
            return;
        }
    }
    // Where the glass is, exactly: the sheets carried with it.
    const Environment& sky = m_scene.GetEnvironment();
    for (RainGlass& glass : m_rainGlass)
    {
        if (Transform* placed = glass.film.IsValid() ? m_scene.GetTransform(glass.film) : nullptr)
        {
            *placed = *glassAt;
        }
        if (MeshRenderer* renderer = glass.film.IsValid() ? m_scene.GetMeshRenderer(glass.film) : nullptr)
        {
            // As bright as what is seen through the water: the sky and the haze, bent.
            renderer->material.emissive = sky.fogColor * 4.2f + sky.ambientSky * 0.8f;
        }
    }
    // How hard the air goes past: by how fast the shuttle is going.
    const float speed = dt > 0.0f && m_rainGlassAt != glm::vec3(0.0f) ? glm::length(glassAt->position - m_rainGlassAt) / dt : 0.0f;
    m_rainGlassAt = glassAt->position;
    m_rainAirflow = glm::mix(m_rainAirflow, glm::smoothstep(4.0f, 45.0f, speed), std::min(dt * 3.0f, 1.0f));
    const float air = m_rainAirflow;
    m_rainClock += dt;
    const auto next = [&]
    {
        m_rainSeed = m_rainSeed * 1664525u + 1013904223u;
        return static_cast<float>(m_rainSeed >> 8) / 16777216.0f;
    };
    const auto merge = [](float a, float b) { return std::cbrt(a * a * a + b * b * b); };
    for (int which = 0; which < 2; ++which)
    {
        RainGlass& glass = m_rainGlass[static_cast<size_t>(which)];
        if (!glass.film.IsValid())
        {
            continue;
        }
        const int size = kSizes[which];
        const float sizeF = static_cast<float>(size);
        // Landing: fewer stay on it at speed (most are swept off as they hit). A new one where there is one already joins it; with the
        // glass as full as it gets, it takes the place of an old bead, so the rain is always seen landing.
        glass.spawn += rain * glm::mix(320.0f, 120.0f, air) * glass.share * dt;
        while (glass.spawn >= 1.0f)
        {
            glass.spawn -= 1.0f;
            const glm::vec2 at{next() * sizeF, next() * sizeF};
            const float r = 1.3f + next() * next() * 2.8f;
            const float wander = next() - 0.5f;
            bool joined = false;
            for (RainDrop& drop : glass.drops)
            {
                if (glm::distance(drop.at, at) < drop.r + r)
                {
                    drop.r = merge(drop.r, r);
                    joined = true;
                    break;
                }
            }
            if (joined)
            {
                continue;
            }
            if (glass.drops.size() < kMostDrops[which])
            {
                glass.drops.push_back({at, r, 0.0f, wander});
            }
            else
            {
                RainDrop& old = glass.drops[static_cast<size_t>(next() * static_cast<float>(glass.drops.size())) % glass.drops.size()];
                if (old.speed <= 0.0f)
                {
                    old = {at, r, 0.0f, wander};
                }
            }
        }
        for (size_t i = 0; i < glass.drops.size(); ++i)
        {
            RainDrop& drop = glass.drops[i];
            if (drop.r <= 0.0f)
            {
                continue;
            }
            const float push = air * air * drop.r * 0.62f;
            if (push > 0.7f)
            {
                // Blown across the glass, the bigger and the faster the air the faster, shedding water as it goes.
                drop.speed = 0.0f;
                drop.at += glass.blow * ((push - 0.7f) * 170.0f * dt);
                drop.at += glm::vec2(glass.blow.y, -glass.blow.x) * (drop.wander * 5.0f * air * dt);
                drop.r -= 0.55f * air * dt;
                drop.at = {WrapF(drop.at.x, sizeF), WrapF(drop.at.y, sizeF)};
                continue;
            }
            if (drop.speed <= 0.0f && drop.r < kRuns)
            {
                continue;
            }
            // Running down: at a pace by how heavy it is, held back where the glass holds it.
            const float pace = (14.0f + 9.0f * std::max(drop.r - kStops, 0.0f)) *
                               (Spot(static_cast<int>(drop.at.x / 5.0f), static_cast<int>(drop.at.y / 5.0f)) < 0.22f ? 0.12f : 1.0f);
            drop.speed = std::max(glm::mix(drop.speed, pace, std::min(dt * 4.0f, 1.0f)), 0.01f);
            const float moved = drop.speed * dt;
            drop.at.y -= moved;
            drop.at.x += drop.wander * 0.3f * moved;
            drop.r -= 0.003f * moved;
            drop.at = {WrapF(drop.at.x, sizeF), WrapF(drop.at.y, sizeF)};
            // Its trail: a wet band a little narrower than it, behind (above) it.
            const int width = std::max(static_cast<int>(drop.r * 0.45f), 0);
            const int row = static_cast<int>(drop.at.y + drop.r * 0.6f);
            for (int x = static_cast<int>(drop.at.x) - width; x <= static_cast<int>(drop.at.x) + width; ++x)
            {
                for (int y = row; y <= row + 1; ++y)
                {
                    glass.wet[static_cast<size_t>(Wrap(y, size) * size + Wrap(x, size))] = 255;
                }
            }
            // The beads in its way, taken up into it.
            for (size_t j = 0; j < glass.drops.size(); ++j)
            {
                RainDrop& other = glass.drops[j];
                if (j != i && other.r > 0.0f && other.speed <= 0.0f && glm::distance(other.at, drop.at) < drop.r + other.r * 0.8f)
                {
                    drop.r = merge(drop.r, other.r);
                    other.r = 0.0f;
                }
            }
            if (drop.r < kStops)
            {
                drop.speed = 0.0f;
            }
        }
        // Drying off once it stops raining.
        if (rain < 0.01f)
        {
            for (RainDrop& drop : glass.drops)
            {
                drop.r -= 0.07f * dt;
            }
        }
        glass.drops.erase(std::remove_if(glass.drops.begin(), glass.drops.end(), [](const RainDrop& d) { return d.r < 0.7f; }), glass.drops.end());
    }
    // Drawn twenty times a second.
    if (m_rainClock - m_rainDrawn < 1.0f / 20.0f)
    {
        return;
    }
    const int dried = std::max(static_cast<int>((m_rainClock - m_rainDrawn) * 32.0f), 1);
    m_rainDrawn = m_rainClock;
    for (int which = 0; which < 2; ++which)
    {
        RainGlass& glass = m_rainGlass[static_cast<size_t>(which)];
        if (!glass.film.IsValid())
        {
            continue;
        }
        const int size = kSizes[which];
        ImageData image;
        image.width = size;
        image.height = size;
        image.pixels.assign(static_cast<size_t>(size * size * 4), 0);
        // (Empty, but the colour of water round the edge, so the edges filtered into it do not draw a dark line round each drop.)
        for (size_t i = 0; i < image.pixels.size(); i += 4)
        {
            image.pixels[i] = image.pixels[i + 1] = image.pixels[i + 2] = 140;
        }
        // The wet trails, breaking up into a line of tiny beads as they dry.
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                uint8_t& wet = glass.wet[static_cast<size_t>(y * size + x)];
                if (wet == 0)
                {
                    continue;
                }
                wet = static_cast<uint8_t>(std::max(static_cast<int>(wet) - dried, 0));
                if (static_cast<float>(wet) > 30.0f + 200.0f * Spot(x / 2, y / 3))
                {
                    uint8_t* p = &image.pixels[static_cast<size_t>((y * size + x) * 4)];
                    p[0] = p[1] = p[2] = 150;
                    p[3] = 200; // (under a bead drawn over it)
                }
            }
        }
        // One drop, a little lens: the world through it upside down -- the dark ground across its top, the bright sky across its
        // bottom --, a dark rim where it bends the light most (one big enough to have one to see), and the sky's glint near its top.
        // Drawn on whole pixels, so one standing still stays exactly as it is; round the edges of the picture onto the other side.
        const auto bead = [&](float cx, float cy, float r, bool trail)
        {
            for (int y = static_cast<int>(std::floor(cy - r)) - 1; y <= static_cast<int>(cy + r) + 1; ++y)
            {
                for (int x = static_cast<int>(std::floor(cx - r)) - 1; x <= static_cast<int>(cx + r) + 1; ++x)
                {
                    const float dx = (static_cast<float>(x) + 0.5f - cx) / r;
                    const float dy = (static_cast<float>(y) + 0.5f - cy) / r;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    // (Its edge soft over about a pixel, so the half-way line the glass is cut out along is round, not a square of pixels.)
                    const float cover = std::clamp((1.0f - d) * r + 0.5f, 0.0f, 1.0f);
                    if (cover <= 0.0f)
                    {
                        continue;
                    }
                    const float gx = dx + 0.35f;
                    const float gy = dy - 0.4f;
                    const bool glint = !trail && r > 1.3f && gx * gx + gy * gy < 0.08f;
                    const bool rim = !trail && r >= 2.0f && d > 1.0f - std::min(0.7f / r, 0.35f);
                    const float shade = glint ? 1.0f : trail ? 0.5f : rim ? 0.32f : glm::mix(0.66f, 0.42f, (dy + 1.0f) * 0.5f);
                    uint8_t* p = &image.pixels[static_cast<size_t>((Wrap(y, size) * size + Wrap(x, size)) * 4)];
                    const auto alpha = static_cast<uint8_t>(cover * 255.0f);
                    if (alpha <= p[3] || (trail && p[3] >= 128))
                    {
                        continue;
                    }
                    if (cover >= 0.5f || p[3] < 128)
                    {
                        p[0] = p[1] = p[2] = static_cast<uint8_t>(shade * 255.0f);
                    }
                    p[3] = alpha;
                }
            }
        };
        for (const RainDrop& drop : glass.drops)
        {
            const float x = std::floor(drop.at.x) + 0.5f;
            const float y = std::floor(drop.at.y) + 0.5f;
            // Blown across the glass: a short thin streak of water behind it.
            const float push = air * air * drop.r * 0.62f;
            if (push > 0.8f)
            {
                const float length = 5.0f * drop.r * std::min(push - 0.7f, 1.0f);
                for (float k = 0.4f; k < length; k += 0.4f)
                {
                    bead(x - glass.blow.x * k, y - glass.blow.y * k, std::max(drop.r * 0.3f * (1.0f - k / (length + 1.0f)), 0.7f), true);
                }
            }
            // Running down, a little longer than round: drawn out up the way it has come.
            if (drop.speed > 0.0f)
            {
                bead(x, y + drop.r * 0.35f, drop.r * 0.85f, false);
            }
            bead(x, y, drop.r, false);
        }
        m_app->GetTextures().Update(glass.texture, image);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(glass.film))
        {
            renderer->visible = true;
        }
    }
}

} // namespace pred
