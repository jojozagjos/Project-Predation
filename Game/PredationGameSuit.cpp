// The suit against the world (Suit): what each world throws at whoever is out on it, the suit's state for everybody, and the
// harm when something has beaten it -- given out by whoever decides what hurts, as burns are.
#include "Game/PredationGame.h"
#include "Game/PredationGameShared.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pred
{

SuitConditions PredationGame::SuitConditionsAt(const glm::vec3& at) const
{
    SuitConditions out;
    const SiteMap* ground = GroundHere(at);
    if (ground == nullptr)
    {
        // Aboard the ship, or anywhere else: under cover.
        out.sheltered = true;
        return out;
    }
    const SiteMap::Look& look = ground->GetLook();
    const bool atSite = ground == &m_facility;
    out.sheltered = atSite ? (m_facility.Plan().Indoors(at) || m_facility.Shuttle().Aboard(at)) : UnderOutpostRoof(at, 0.0f);
    // Its air: only breathable air can be breathed (thin is too thin, dense too heavy with what is not air); toxic air is worse
    // through a tear.
    out.breathable = look.air == "breathable";
    out.toxic = look.air == "toxic";
    // The cold: a frozen world's, harder the colder it is there and in a blizzard; the heat: a scorching world's, and beside
    // lava, whatever the world.
    const Weather::Kind falling = Weather::KindOf(look.weather);
    const float storm = std::clamp(look.weatherAmount, 0.0f, 1.0f);
    out.cold = glm::smoothstep(-15.0f, -90.0f, look.temperature) * 0.75f;
    if (falling == Weather::Kind::Snow && look.temperature < 5.0f)
    {
        out.cold += glm::smoothstep(0.5f, 1.0f, storm) * 0.5f;
    }
    out.heat = glm::smoothstep(45.0f, 120.0f, look.temperature) * 0.75f;
    if (atSite && look.liquid == SiteMap::Look::Liquid::Lava && m_facility.Terrain().HasWater())
    {
        // How near the nearest lava is, within a few metres every way.
        float nearest = 9.0f;
        for (int k = 0; k < 8; ++k)
        {
            const float a = 0.785398f * static_cast<float>(k);
            for (const float out2 : {1.5f, 3.5f, 6.0f, 9.0f})
            {
                if (m_facility.Terrain().WaterDepth(at.x + std::cos(a) * out2, at.z + std::sin(a) * out2) > 0.02f)
                {
                    nearest = std::min(nearest, out2);
                }
            }
        }
        out.heat += 0.9f * (1.0f - glm::smoothstep(1.0f, 9.0f, nearest));
    }
    out.cold = std::clamp(out.cold, 0.0f, 1.0f);
    out.heat = std::clamp(out.heat, 0.0f, 1.0f);
    // Radiation: on worlds with no air to stop it and on storm-beaten ones, in spells that come and go over the hours -- the
    // same for everybody, by the campaign's clock.
    const float exposedTo = look.airless ? 0.9f : look.biome == "stormworld" ? 0.6f : look.biome == "crystalline" ? 0.35f : 0.0f;
    if (exposedTo > 0.0f)
    {
        out.radiation = exposedTo * glm::smoothstep(0.35f, 0.85f, Weather::SpellAt(ground->Seed() ^ 0x5AD1A7u, m_campaign.clock));
    }
    return out;
}

void PredationGame::UpdateSuits(float dt)
{
    const bool deciding = IsAuthority();
    // Everybody alive, where they are -- or, on a client, only its own player (for the suit's readings; the host hurts).
    std::vector<std::pair<uint8_t, glm::vec3>> here;
    if (m_player.State().alive)
    {
        here.emplace_back(LocalPlayerId(), m_player.State().position);
    }
    else
    {
        Suit::Reset(m_suits[LocalPlayerId()]);
    }
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : m_host.Remotes())
        {
            if (remote.id == LocalPlayerId())
            {
                continue;
            }
            if (remote.alive)
            {
                here.emplace_back(remote.id, remote.position);
            }
            else
            {
                Suit::Reset(m_suits[remote.id]);
            }
        }
    }
    // A client tears its own when it is hurt (the host does so for everybody as it hurts them: ApplyPlayerDamage).
    if (!deciding)
    {
        const float health = m_player.State().health;
        if (health < m_suitHealthSeen - 0.5f)
        {
            Suit::Tear(m_suits[LocalPlayerId()], m_suitHealthSeen - health);
        }
        m_suitHealthSeen = health;
    }
    for (const auto& [id, at] : here)
    {
        SuitState& suit = m_suits[id];
        const SuitHarm harm = Suit::Step(suit, SuitConditionsAt(at), dt);
        if (!deciding)
        {
            continue;
        }
        // Given out a few at a time, not a sliver every frame.
        float& owed = m_suitHarm[id];
        owed += harm.perSecond * dt;
        if (owed >= 3.0f)
        {
            ApplyPlayerDamage(id, owed, kNoKiller, glm::vec3(0.0f, -1.0f, 0.0f), harm.cause);
            owed = 0.0f;
        }
        else if (harm.perSecond <= 0.0f)
        {
            owed = 0.0f;
        }
    }

    // And what the suit tells its wearer, now and then, when something is wrong.
    const SuitState& mine = m_suits[LocalPlayerId()];
    const char* say = nullptr;
    if (mine.breach > 0.05f && !SuitConditionsAt(m_player.State().position).breathable && !SuitConditionsAt(m_player.State().position).sheltered)
    {
        say = mine.air < 0.25f ? "Suit air low. Get under cover." : "Suit breached. Sealing.";
    }
    else if (mine.warmth < -0.6f)
    {
        say = "Suit losing heat. Get under cover.";
    }
    else if (mine.warmth > 0.6f)
    {
        say = "Suit overheating. Get away from the heat.";
    }
    else if (mine.dose > 0.45f)
    {
        say = "Radiation dose high. Get under cover.";
    }
    if (say != nullptr && m_player.State().alive && (m_time - m_suitToldAt > 8.0 || std::strcmp(say, m_suitTold) != 0))
    {
        m_suitToldAt = m_time;
        m_suitTold = say;
        ShowCampaignNotice(say);
    }
}

} // namespace pred
