#include "Game/World/Weather.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

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
// And the box the puffs drift through: wider, from the ground up.
constexpr float kPuffBox = 56.0f;
constexpr float kPuffHigh = 14.0f;

// How each kind falls and looks. `pieces` at the most (fewer as there is less of it); `low` of them blown along the
// ground rather than falling; `puffs` big soft ones in a heavy fall; `fall` metres a second, from and to; `size` metres
// across, from and to; `alpha` how much it hides; `blow` how much of the wind it takes; `swirl` how far it wanders;
// `streak` drawn along the way it is going, `streakTime` how long a moment it is smeared over; `glow` the share that are
// embers; `puffAlpha` how thick a puff is at the heaviest.
struct KindLook
{
    int pieces;
    int low;
    int puffs;
    float fallFrom, fallTo;
    float sizeFrom, sizeTo;
    float alpha;
    float blow;
    float swirl;
    bool streak;
    float streakTime;
    float glow;
    glm::vec3 color;
    float puffAlpha;
    float reach = 1.0f; // of the box
};

const KindLook& LookOf(Weather::Kind kind)
{
    static const KindLook kSnow{760, 170, 46, 0.7f, 1.7f, 0.04f, 0.09f, 0.95f, 1.0f, 0.35f, false, 0.0f, 0.0f, {0.92f, 0.94f, 0.97f}, 0.16f, 0.7f};
    static const KindLook kRain{1500, 0, 24, 8.0f, 11.0f, 0.018f, 0.026f, 0.5f, 1.6f, 0.0f, true, 0.07f, 0.0f, {0.72f, 0.76f, 0.82f}, 0.08f, 0.55f};
    static const KindLook kDust{1100, 650, 80, -0.15f, 0.35f, 0.02f, 0.04f, 0.6f, 2.6f, 0.8f, true, 0.022f, 0.0f, {0.6f, 0.5f, 0.38f}, 0.3f, 0.55f};
    static const KindLook kAsh{760, 140, 50, 0.3f, 0.8f, 0.04f, 0.09f, 0.85f, 0.8f, 0.6f, false, 0.0f, 0.08f, {0.16f, 0.16f, 0.17f}, 0.2f, 0.7f};
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

} // namespace

Weather::Kind Weather::KindOf(const std::string& particles)
{
    return particles == "snow" ? Kind::Snow : particles == "rain" ? Kind::Rain : particles == "dust" ? Kind::Dust : particles == "ash" ? Kind::Ash : Kind::None;
}

float Weather::GustAt(float seconds)
{
    return 0.7f + 0.6f * std::sin(seconds * 0.31f) * std::sin(seconds * 0.77f + 1.3f) + 0.35f * std::max(std::sin(seconds * 0.13f), 0.0f);
}

void Weather::Make(Kind kind, const glm::vec3& eye)
{
    m_pieces.clear();
    m_kind = kind;
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
    const int glows = static_cast<int>(std::round(static_cast<float>(look.pieces) * look.glow));
    for (int i = 0; i < look.pieces; ++i)
    {
        Piece piece;
        piece.low = i < look.low;
        piece.glow = i >= look.pieces - glows;
        piece.at = eye + glm::vec3((random() - 0.5f) * box.x, above - random() * box.y, (random() - 0.5f) * box.z);
        piece.fall = piece.low ? 0.0f : look.fallFrom + random() * (look.fallTo - look.fallFrom);
        piece.sway = random() * 6.2831853f;
        piece.size = look.sizeFrom + random() * (look.sizeTo - look.sizeFrom);
        piece.alpha = look.alpha * (0.7f + 0.3f * random());
        if (piece.low)
        {
            // Along the ground: sand snaking across it in ribbons; a little snow skittering.
            piece.size = kind == Kind::Dust ? 0.06f + random() * 0.08f : piece.size * 0.7f;
            piece.alpha *= kind == Kind::Dust ? 0.55f : 1.0f;
        }
        if (piece.glow)
        {
            piece.size = 0.025f;
        }
        // Mostly just over it; now and then lifted higher (dust higher than snow).
        piece.height = 0.05f + random() * random() * (kind == Kind::Dust ? 4.0f : 0.7f);
        m_pieces.push_back(piece);
    }
    // The puffs: big and faint, from the ground up, all through the box round the eye.
    for (int i = 0; i < look.puffs; ++i)
    {
        Piece piece;
        piece.puff = true;
        piece.at = eye + glm::vec3((random() - 0.5f) * kPuffBox, 0.0f, (random() - 0.5f) * kPuffBox);
        piece.height = random() * random() * kPuffHigh;
        piece.size = 3.0f + random() * 5.0f;
        piece.alpha = 0.55f + 0.45f * random();
        piece.sway = random() * 6.2831853f;
        m_pieces.push_back(piece);
    }
}

void Weather::Update(const glm::vec3& eye, float ground, float dt, Kind kind, float intensity, const glm::vec3& tint, const glm::vec3& wind,
                     const Light& light, const std::function<bool(const glm::vec3&)>& indoors)
{
    m_sprites.clear();
    if (kind != m_kind)
    {
        Make(kind, eye);
    }
    if (kind == Kind::None || intensity <= 0.0f)
    {
        return;
    }
    const KindLook& look = LookOf(kind);
    const glm::vec3 box = kBigBox * look.reach;
    const float above = kBigAbove * look.reach;
    m_clock += dt;
    const float gust = GustAt(m_clock);
    // Dust is only there because the wind is lifting it: never less than a stiff breeze.
    glm::vec3 blown = wind;
    if (kind == Kind::Dust && glm::length(blown) < 1.2f)
    {
        blown = (glm::length(blown) > 1.0e-4f ? glm::normalize(blown) : glm::vec3(0.8f, 0.0f, 0.6f)) * 1.2f;
    }
    const glm::vec3 blowing = blown * gust * look.blow;
    const float amount = std::clamp(intensity, 0.0f, 1.0f);
    // As much of it as there is: a light fall is a third of a heavy one. The puffs only in a heavy one, thicker in the gusts.
    const int falling = static_cast<int>(std::count_if(m_pieces.begin(), m_pieces.end(), [](const Piece& p) { return !p.puff; }));
    const int shown = static_cast<int>(static_cast<float>(falling) * (0.35f + 0.65f * amount));
    const float puffs = look.puffAlpha * glm::smoothstep(0.4f, 1.0f, amount) * glm::mix(0.75f, 1.25f, std::clamp(gust - 0.4f, 0.0f, 1.2f) / 1.2f);
    // Its own colour: dust the ground it was lifted from, a little browner.
    const glm::vec3 own = kind == Kind::Dust ? tint * glm::vec3(0.9f, 0.82f, 0.7f) : look.color;
    const auto fogged = [&](const glm::vec3& colour, const glm::vec3& at)
    {
        const float distance = glm::length(at - eye);
        const float fog = std::clamp((distance - light.fogStart) / std::max(light.fogEnd - light.fogStart, 0.01f), 0.0f, 1.0f);
        return glm::mix(colour, light.fogColour, fog);
    };
    int index = 0;
    for (Piece& piece : m_pieces)
    {
        if (piece.puff)
        {
            // Drifting with the wind, rising and sinking a little, round again at the other side.
            piece.at += blowing * (kind == Kind::Dust || kind == Kind::Snow ? 2.2f : 0.8f) * dt;
            glm::vec2 local{piece.at.x - eye.x, piece.at.z - eye.z};
            local.x = std::fmod(local.x + kPuffBox * 1.5f, kPuffBox) - kPuffBox * 0.5f;
            local.y = std::fmod(local.y + kPuffBox * 1.5f, kPuffBox) - kPuffBox * 0.5f;
            piece.at = {eye.x + local.x, ground + piece.height + 0.6f * std::sin(m_clock * 0.21f + piece.sway), eye.z + local.y};
            if (puffs <= 0.002f || indoors(piece.at))
            {
                continue;
            }
            // Faded as it nears the eye, so the eye never goes through the face of one.
            const float distance = glm::length(piece.at - eye);
            const float near = glm::smoothstep(piece.size * 0.35f, piece.size * 0.9f, distance);
            Sprite sprite;
            sprite.at = piece.at;
            sprite.size = piece.size;
            // A puff is the colour of the air it thickens -- the fog's, a little lighter or darker -- with some of its own in it;
            // in a sandstorm or a blizzard drawn out along the wind, streamers of it blowing past.
            const float horizontal = glm::length(glm::vec2(blowing.x, blowing.z));
            if ((kind == Kind::Dust || kind == Kind::Snow) && horizontal > 0.5f)
            {
                sprite.size = piece.size * 0.55f;
                sprite.length = piece.size * 2.4f;
                sprite.along = glm::vec3(blowing.x, 0.0f, blowing.z) / horizontal;
            }
            sprite.colour = glm::mix(light.fogColour * (0.88f + 0.24f * std::sin(piece.sway * 3.0f)), own * light.light, 0.4f);
            sprite.alpha = piece.alpha * puffs * near;
            m_sprites.push_back(sprite);
            continue;
        }
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
        // Only as many as there is weather for, and not indoors.
        if (index++ >= shown || indoors(piece.at))
        {
            continue;
        }
        Sprite sprite;
        sprite.at = piece.at;
        sprite.size = piece.size;
        // Faded close to the eye: a flake or a grain a hand's breadth from it would fill the view as a blot.
        const float fromEye = glm::length(piece.at - eye);
        sprite.alpha = piece.alpha * glm::smoothstep(0.5f, 2.0f, fromEye);
        if (look.streak)
        {
            // Smeared along the way it is going over the moment the eye sees it: rain as streaks leaning with the wind, sand
            // as streaks blowing past.
            const glm::vec3 velocity = piece.low ? blowing * 3.2f : glm::vec3(blowing.x, -piece.fall, blowing.z);
            // (Never long against how far off it is: smeared across half the view, close streaks all seem to pour out of
            // the one point the wind comes from.)
            sprite.length = std::min(glm::length(velocity) * look.streakTime * (piece.low ? 1.8f : 1.0f), fromEye * 0.12f);
            sprite.along = glm::normalize(velocity);
        }
        if (piece.glow)
        {
            // An ember: its own light, flickering, unlit by anything.
            sprite.colour = glm::vec3(3.0f, 1.1f, 0.22f) * (0.6f + 0.4f * std::sin(m_clock * 9.0f + piece.sway * 5.0f));
            sprite.alpha = 1.0f;
        }
        else
        {
            sprite.colour = fogged(own * light.light, piece.at);
        }
        m_sprites.push_back(sprite);
    }
}

} // namespace pred
