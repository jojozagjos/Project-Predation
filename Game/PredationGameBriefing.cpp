// Orders, and the briefing: a deployment comes in at a random time while everybody is aboard -- a chime, the briefing
// room's screens asking for it to be played -- and anybody plays it at the console. The screens then run the briefing,
// slide by slide, with its voice-over put together from recordings of the site's own words (Game/Mission/Briefing), and
// once it is over the console deploys. An order is an order: there is no turning it down. The host decides when one
// comes and when it is played; everybody's screens show the same.

#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/TextureLibrary.h"
#include "Game/World/ScreenCanvas.h"

#include <imgui.h>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace pred
{

CVar<float> cv_firstOrderSeconds{"game.first_order_seconds", 30.0f, "Seconds aboard at the start of a game before the first orders come in"};
CVar<float> cv_orderSecondsMin{"game.order_seconds_min", 60.0f, "Least seconds back aboard before the next orders come in"};
CVar<float> cv_orderSecondsMax{"game.order_seconds_max", 180.0f, "Most seconds back aboard before the next orders come in"};

namespace
{

// The screens: wide, as the wall they are on is, and drawn a few times a second.
constexpr int kWide = 704;
constexpr int kHigh = 243;
constexpr float kRedrawEvery = 1.0f / 12.0f;
// Where they are, in the ship's frame: either side of the door, on the briefing room's front wall.
constexpr float kScreenX = 4.65f;
constexpr float kScreenY = 3.6f + 1.65f;
constexpr float kScreenZ = -21.78f; // well clear of the glass it hangs over, which it fought seen from across the room
constexpr float kScreenW = 5.5f;
constexpr float kScreenH = 1.9f;
// Heard and subtitled within this of the screens.
constexpr float kBriefingHeard = 30.0f;

constexpr Rgb kBack{5, 9, 13};
constexpr Rgb kText{205, 226, 238};
constexpr Rgb kSoft{110, 150, 172};
constexpr Rgb kFaintLine{28, 44, 56};
constexpr Rgb kAccent{120, 195, 235};
constexpr Rgb kWarn{240, 170, 80};

float RandomBetween(float lo, float hi)
{
    const auto now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const float unit = static_cast<float>((now * 6364136223846793005ull + 1442695040888963407ull) >> 40) / static_cast<float>(1ull << 24);
    return lo + (hi - lo) * unit;
}

// Text typed out: as much of it as there has been time for at so many letters a second.
std::string Typed(const std::string& text, float since, float perSecond = 28.0f)
{
    const auto shown = static_cast<size_t>(std::max(since, 0.0f) * perSecond);
    return text.substr(0, std::min(shown, text.size()));
}

std::string Upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

// A flat panel facing +z, its picture the whole of a texture -- both ways round, so it cannot be wound the wrong way.
MeshData ScreenQuad(float width, float height)
{
    MeshData mesh;
    const float w = width * 0.5f;
    const float h = height * 0.5f;
    const glm::vec3 normal{0.0f, 0.0f, 1.0f};
    mesh.vertices.push_back(MeshVertex{{-w, h, 0.0f}, normal, {0.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{w, h, 0.0f}, normal, {1.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{w, -h, 0.0f}, normal, {1.0f, 1.0f}});
    mesh.vertices.push_back(MeshVertex{{-w, -h, 0.0f}, normal, {0.0f, 1.0f}});
    mesh.indices = {0, 3, 2, 0, 2, 1, 0, 1, 2, 0, 2, 3};
    return mesh;
}

void Frame(ScreenCanvas& canvas, const char* heading, const std::string& right, float progress)
{
    canvas.Fill(0.0f, 0.0f, static_cast<float>(kWide), 30.0f, {10, 18, 26});
    canvas.Text(14, 8, heading, kAccent);
    canvas.Text(kWide - 14 - canvas.TextWidth(right.c_str()), 8, right.c_str(), kSoft);
    canvas.Line(0.0f, 30.0f, static_cast<float>(kWide), 30.0f, kFaintLine);
    if (progress >= 0.0f)
    {
        canvas.Fill(14.0f, kHigh - 10.0f, kWide - 14.0f, kHigh - 8.0f, kFaintLine);
        canvas.Fill(14.0f, kHigh - 10.0f, 14.0f + (kWide - 28.0f) * std::clamp(progress, 0.0f, 1.0f), kHigh - 8.0f, kAccent);
    }
}

// A planet, banded and turning, lit from one side.
void Planet(ScreenCanvas& canvas, float cx, float cy, float radius, float turn, Rgb base)
{
    for (int y = static_cast<int>(cy - radius); y <= static_cast<int>(cy + radius); ++y)
    {
        for (int x = static_cast<int>(cx - radius); x <= static_cast<int>(cx + radius); ++x)
        {
            const float dx = (static_cast<float>(x) - cx) / radius;
            const float dy = (static_cast<float>(y) - cy) / radius;
            const float r2 = dx * dx + dy * dy;
            if (r2 > 1.0f)
            {
                continue;
            }
            const float dz = std::sqrt(1.0f - r2);
            const float lon = std::atan2(dx, dz) + turn;
            const float band = 0.75f + 0.25f * std::sin(dy * 9.0f + std::sin(lon * 3.0f) * 0.8f);
            const float light = std::clamp(0.15f + 0.95f * (dx * -0.55f + dy * -0.25f + dz * 0.8f), 0.05f, 1.0f);
            const float k = band * light;
            canvas.Put(x, y, {static_cast<uint8_t>(base.r * k), static_cast<uint8_t>(base.g * k), static_cast<uint8_t>(base.b * k)});
        }
    }
    canvas.Arc(cx, cy, radius + 3.0f, -2.2f, 0.9f, kAccent);
}

} // namespace

void PredationGame::LoadBriefing()
{
    std::string error;
    if (!m_briefingScript.LoadFromFile(Paths::AssetsRoot() / "Data" / "briefing.json", &error))
    {
        PRED_LOG_WARN(Gameplay, "Briefing script: {} (the built-in one is used)", error);
    }
}

void PredationGame::BuildBriefingScreens()
{
    MeshLibrary& meshes = m_app->GetMeshes();
    const MeshHandle quad = meshes.Upload(ScreenQuad(kScreenW, kScreenH), "briefing_screen");
    TextureLibrary& textures = m_app->GetTextures();
    for (int i = 0; i < 2; ++i)
    {
        m_briefingTextures[i] = textures.CreateDynamic(kWide, kHigh, "briefing_screen_" + std::to_string(i));
        Transform at;
        at.position = ShipSpec::kOrigin + glm::vec3(i == 0 ? -kScreenX : kScreenX, kScreenY, kScreenZ);
        Material material = Material::Diffuse(glm::vec3(1.0f), 0.3f);
        material.baseColorTexture = m_briefingTextures[i];
        material.emissive = glm::vec3(1.25f);
        material.emissiveTextured = true;
        m_briefingScreens[i] = m_scene.CreateMeshEntity("briefing_screen", at, quad, material);
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_briefingScreens[i]))
        {
            renderer->castsShadow = false;
        }
    }
    m_briefingDrawnAt = -10.0f;
}

void PredationGame::ScheduleOrders(bool firstOfGame)
{
    if (!IsAuthority())
    {
        return;
    }
    m_orderIn = firstOfGame ? std::max(cv_firstOrderSeconds.Get(), 1.0f)
                            : RandomBetween(std::max(cv_orderSecondsMin.Get(), 1.0f), std::max(cv_orderSecondsMax.Get(), cv_orderSecondsMin.Get() + 1.0f));
    PRED_LOG_INFO(Gameplay, "Orders in {:.0f} s", m_orderIn);
}

void PredationGame::ClearOrders()
{
    m_order = OrderState::None;
    m_orderSite = 0;
    m_orderIn = -1.0f;
    m_briefingAt = 0.0f;
    m_briefingCue = 0;
}

void PredationGame::PrepareBriefing(uint16_t site)
{
    if (site == 0 || site == m_briefingFor)
    {
        return;
    }
    m_briefingFor = site;
    m_briefingPlan = SitePlan::Generate(site);
    m_briefingFacts = BriefingFacts{};
    m_briefingFacts.title = m_siteNames.For(site);
    m_briefingFacts.conditions = ConditionsFor(site, m_briefingPlan.sky.fogEnd);
    m_briefingFacts.mapGiven = MissionPlan::Generate(m_briefingPlan, site).mapGiven;
    // Timed by the recordings there are: the first of each, so every machine lays it out the same.
    m_briefingTimeline = BuildBriefing(m_briefingScript, m_briefingFacts, [this](const std::string& clip)
                                       {
                                           const SoundVariants& sound = Sounds(clip);
                                           return sound.ids.empty() ? -1.0f : m_app->GetAudio().SecondsOf(sound.ids.front());
                                       });
}

void PredationGame::IssueOrder(uint16_t site)
{
    if (site == 0)
    {
        site = static_cast<uint16_t>(1 + static_cast<int>(RandomBetween(0.0f, 65534.0f)));
    }
    m_orderSite = site;
    m_order = OrderState::Incoming;
    m_orderIn = -1.0f;
    PrepareBriefing(site);
    PRED_LOG_INFO(Gameplay, "Orders in: site {}", site);
}

void PredationGame::StartBriefing()
{
    if (m_order != OrderState::Incoming && m_order != OrderState::Ready)
    {
        return;
    }
    PrepareBriefing(m_orderSite);
    m_order = OrderState::Briefing;
    m_briefingAt = 0.0f;
    m_briefingCue = 0;
    PRED_LOG_INFO(Gameplay, "Briefing for site {}: {:.0f} s", m_orderSite, m_briefingTimeline.length);
}

void PredationGame::UpdateOrders(float dt)
{
    m_orderClock += dt;
    const bool aboard = m_screen == Screen::Playing && m_map == MapChoice::Ship;
    // Coming in: the host's clock, only while there is nothing else going on aboard.
    if (IsAuthority() && aboard && m_order == OrderState::None && m_orderIn >= 0.0f && m_shipTravel <= 0.0f && !m_shipReady && !m_cine.Active())
    {
        m_orderIn -= dt;
        if (m_orderIn <= 0.0f)
        {
            IssueOrder(0);
        }
    }
    // The chime, wherever anybody is aboard, as soon as orders come in -- the same on every machine, from the state.
    if (m_order == OrderState::Incoming && m_orderHeard != m_orderSite)
    {
        m_orderHeard = m_orderSite;
        if (aboard)
        {
            PlayNamed("World/transmission", m_renderEye, 0.8f, 1.0f, false);
            Say("orders", 1.6f);
        }
    }
    const glm::vec3 screensAt = ShipSpec::kOrigin + glm::vec3(0.0f, kScreenY, kScreenZ);
    if (m_order == OrderState::Briefing)
    {
        PrepareBriefing(m_orderSite);
        m_briefingAt += dt;
        // The voice from the screens, each recording as its moment comes; the subtitle for whoever is near enough.
        while (m_briefingCue < m_briefingTimeline.cues.size() && m_briefingTimeline.cues[m_briefingCue].at <= m_briefingAt)
        {
            const std::string& clip = m_briefingTimeline.cues[m_briefingCue].clip;
            const SoundVariants& sound = Sounds(clip);
            // Late by more than a moment -- joined part way through -- it is not started out of time.
            if (!sound.ids.empty() && m_briefingAt - m_briefingTimeline.cues[m_briefingCue].at < 0.3f)
            {
                PlaySound(sound.ids.front(), screensAt, 1.0f, 1.0f, true);
            }
            ++m_briefingCue;
        }
        const bool near = glm::distance(m_renderEye, screensAt) < kBriefingHeard;
        if (const BriefingTimeline::Caption* caption = m_briefingTimeline.CaptionAt(m_briefingAt); caption != nullptr && near)
        {
            m_subtitle = caption->text;
            m_subtitleLeft = std::max(caption->to - m_briefingAt, 0.1f);
        }
        if (m_briefingAt >= m_briefingTimeline.length)
        {
            m_order = OrderState::Ready;
        }
    }
    // The console says what it will do.
    if (Interactable* console = m_interactions.Find(m_deployConsole))
    {
        const bool incoming = m_order == OrderState::Incoming;
        const bool ready = m_order == OrderState::Ready && m_shipTravel <= 0.0f && !m_shipReady;
        console->enabled = aboard && (incoming || ready);
        console->verb = incoming ? "Play" : "Deploy";
        console->name = incoming ? "the briefing" : "to the site";
    }
    if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_deployScreen))
    {
        const float pulse = 0.5f + 0.5f * std::sin(m_orderClock * 6.0f);
        renderer->material.emissive = m_order == OrderState::Incoming ? glm::vec3(0.9f, 0.55f, 0.15f) * (0.4f + 0.6f * pulse)
                                      : m_order == OrderState::Ready  ? glm::vec3(0.15f, 0.55f, 0.35f)
                                                                     : glm::vec3(0.12f, 0.25f, 0.4f);
    }
    DrawBriefingScreens();
}

void PredationGame::DrawBriefingScreens()
{
    // Only while there is somebody to see them, and a few times a second.
    if (m_screen != Screen::Playing || !m_briefingTextures[0].IsValid() || m_orderClock - m_briefingDrawnAt < kRedrawEvery ||
        glm::distance(m_renderEye, ShipSpec::kOrigin) > ShipSpec::kReach)
    {
        return;
    }
    m_briefingDrawnAt = m_orderClock;
    const bool flash = std::fmod(m_orderClock, 1.0f) < 0.6f;
    const auto seconds = [](float s)
    {
        char text[16];
        const int whole = std::max(static_cast<int>(std::ceil(s)), 0);
        std::snprintf(text, sizeof(text), "%d:%02d", whole / 60, whole % 60);
        return std::string(text);
    };
    char clock[24];
    // The same on every screen aboard: the host's tick, which every machine follows (sixty a second), not the time since this
    // machine started its game.
    const double shared = m_sessionMode == SessionMode::Host     ? static_cast<double>(m_host.CurrentTick()) / 60.0
                          : m_sessionMode == SessionMode::Client ? static_cast<double>(m_client.RenderTick()) / 60.0
                                                                 : static_cast<double>(m_orderClock);
    const int shipTime = static_cast<int>(shared);
    std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d", (shipTime / 3600) % 24, (shipTime / 60) % 60, shipTime % 60);
    const BriefingFacts& facts = m_briefingFacts;
    const std::string planet = Upper(facts.title.planet);
    const std::string site = Upper(facts.title.site);

    for (int screen = 0; screen < 2; ++screen)
    {
        ScreenCanvas canvas(kWide, kHigh);
        canvas.Clear(kBack);
        const bool left = screen == 0;
        const auto centred = [&](const std::string& text, int y, Rgb colour, int scale)
        {
            canvas.Text(kWide / 2 - canvas.TextWidth(text.c_str(), scale) / 2, y, text.c_str(), colour, scale);
        };
        if (m_shipTravel > 0.0f)
        {
            Frame(canvas, "UNDER WAY", clock, -1.0f);
            centred(left ? "UNDER WAY" : site, 88, kText, left ? 4 : 2);
            centred("ARRIVING IN " + seconds(m_shipTravel), 150, kSoft, 3);
        }
        else if (m_shipReady)
        {
            Frame(canvas, "IN ORBIT", clock, -1.0f);
            centred(left ? planet : site, 88, kText, left ? 4 : 2);
            centred("SHUTTLE READY IN THE HANGAR", 150, kAccent, 2);
        }
        else if (m_order == OrderState::Incoming)
        {
            Frame(canvas, "BRIEFING", clock, -1.0f);
            if (flash)
            {
                centred("INCOMING TRANSMISSION", 80, kWarn, 4);
            }
            centred("PLAY AT THE CONSOLE", 150, kSoft, 2);
        }
        else if (m_order == OrderState::Briefing || m_order == OrderState::Ready)
        {
            const float progress = m_order == OrderState::Ready ? 1.0f : m_briefingAt / std::max(m_briefingTimeline.length, 0.1f);
            Frame(canvas, "DEPLOYMENT BRIEFING", "T+" + seconds(m_briefingAt), progress);
            const BriefingTimeline::Part* part = m_order == OrderState::Briefing ? m_briefingTimeline.PartAt(m_briefingAt) : nullptr;
            const std::string show = part != nullptr ? part->show : "end";
            const float since = part != nullptr ? m_briefingAt - part->from : 10.0f;
            if (left)
            {
                // The briefing itself, slide by slide.
                if (show == "header")
                {
                    centred(Typed("DEPLOYMENT BRIEFING", since), 90, kText, 4);
                    centred(Typed(site, since - 0.6f), 150, kSoft, 2);
                }
                else if (show == "planet" || show == "site")
                {
                    Planet(canvas, 150.0f, 132.0f, 78.0f, m_orderClock * 0.25f, {170, 190, 205});
                    canvas.Text(280, 70, "DESTINATION", kSoft, 2);
                    canvas.Text(280, 96, Typed(planet, since).c_str(), kText, 3);
                    if (show == "site")
                    {
                        canvas.Text(280, 140, "SITE", kSoft, 2);
                        const size_t comma = site.find(", ");
                        canvas.Text(280, 164, Typed(site.substr(0, comma), since).c_str(), kText, 2);
                        if (comma != std::string::npos)
                        {
                            canvas.Text(280, 190, Typed(site.substr(comma + 2), since - 0.8f).c_str(), kSoft, 2);
                        }
                    }
                }
                else if (show == "conditions")
                {
                    canvas.Text(40, 50, "SURFACE CONDITIONS", kSoft, 2);
                    const auto row = [&](int y, const char* name, const std::string& value, float fill, float delay)
                    {
                        canvas.Text(40, y, name, kText, 2);
                        canvas.Text(300, y, Typed(value, since - delay).c_str(), kText, 2);
                        canvas.Fill(470.0f, y + 2.0f, 660.0f, y + 12.0f, kFaintLine);
                        canvas.Fill(470.0f, y + 2.0f, 470.0f + 190.0f * std::clamp(fill * std::min(since - delay, 1.0f), 0.0f, 1.0f), y + 12.0f, kAccent);
                    };
                    row(90, "TEMPERATURE", std::to_string(facts.conditions.temperature) + " C", -static_cast<float>(facts.conditions.temperature) / 45.0f, 0.3f);
                    row(130, "WIND", std::to_string(facts.conditions.wind) + " M/S", static_cast<float>(facts.conditions.wind) / 25.0f, 1.2f);
                    const float seen = facts.conditions.visibility == "POOR" ? 0.25f : facts.conditions.visibility == "LOW" ? 0.55f : 0.85f;
                    row(170, "VISIBILITY", facts.conditions.visibility, seen, 2.1f);
                }
                else if (show == "map")
                {
                    centred(facts.mapGiven ? "SITE MAP ON FILE" : "NO MAP DATA ON FILE", 100, facts.mapGiven ? kText : kWarn, 3);
                    centred(facts.mapGiven ? "SEE THE OTHER SCREEN" : "SEARCH THE BUILDINGS", 150, kSoft, 2);
                }
                else if (show == "objective")
                {
                    canvas.Text(40, 50, "OBJECTIVE", kSoft, 2);
                    canvas.Text(40, 84, Typed("1  LOCATE THE TERMINAL", since).c_str(), kText, 2);
                    canvas.Text(40, 118, Typed("2  DOWNLOAD THE DATA", since - 1.2f).c_str(), kText, 2);
                    canvas.Text(40, 152, Typed("3  BRING THE DRIVE BACK TO THE SHUTTLE", since - 2.4f).c_str(), kText, 2);
                }
                else
                {
                    centred(m_order == OrderState::Ready ? "BRIEFING COMPLETE" : "DEPLOY WHEN READY", 84, kText, 4);
                    centred(site, 140, kSoft, 2);
                    if (m_order == OrderState::Ready)
                    {
                        centred("DEPLOY AT THE CONSOLE", 180, kAccent, 2);
                    }
                }
            }
            else
            {
                // The site: its map when there is one, what it is called, and its weather.
                canvas.Text(20, 44, planet.c_str(), kSoft, 2);
                const size_t comma = site.find(", ");
                canvas.Text(20, 68, site.substr(0, comma).c_str(), kText, 2);
                char weather[64];
                std::snprintf(weather, sizeof(weather), "%d C  WIND %d M/S", facts.conditions.temperature, facts.conditions.wind);
                canvas.Text(20, 100, weather, kSoft, 2);
                canvas.Text(20, 124, ("VISIBILITY " + facts.conditions.visibility).c_str(), kSoft, 2);
                const float mapLeft = 430.0f;
                const float mapTop = 40.0f;
                const float mapSize = 190.0f;
                canvas.Box(mapLeft, mapTop, mapLeft + mapSize, mapTop + mapSize, kFaintLine);
                if (facts.mapGiven && m_briefingFor == m_orderSite)
                {
                    const SitePlan& plan = m_briefingPlan;
                    const float scale = mapSize / plan.size;
                    const auto at = [&](float x, float z) { return glm::vec2(mapLeft + (x - plan.origin.x) * scale, mapTop + (z - plan.origin.z) * scale); };
                    for (const FacilityLayout& building : plan.buildings)
                    {
                        glm::vec2 lo;
                        glm::vec2 hi;
                        SitePlan::Footprint(building, 0.0f, lo, hi);
                        const glm::vec2 a = at(lo.x, lo.y);
                        const glm::vec2 b = at(hi.x, hi.y);
                        canvas.Fill(a.x, a.y, b.x, b.y, {24, 52, 70});
                        canvas.Box(a.x, a.y, b.x, b.y, kAccent);
                    }
                    const glm::vec2 pad = at(plan.landing.x, plan.landing.z);
                    canvas.Arc(pad.x, pad.y, 5.0f, 0.0f, glm::two_pi<float>(), kWarn);
                    canvas.Text(static_cast<int>(pad.x) + 8, static_cast<int>(pad.y) - 7, "PAD", kWarn, 1);
                }
                else
                {
                    // Static, where the map would be.
                    uint32_t noise = static_cast<uint32_t>(m_orderClock * 60.0f) * 2654435761u;
                    for (int i = 0; i < 1400; ++i)
                    {
                        noise = noise * 1664525u + 1013904223u;
                        const float x = mapLeft + static_cast<float>((noise >> 8) % static_cast<uint32_t>(mapSize));
                        const float y = mapTop + static_cast<float>((noise >> 20) % static_cast<uint32_t>(mapSize));
                        const auto grey = static_cast<uint8_t>(40 + (noise >> 26));
                        canvas.Put(static_cast<int>(x), static_cast<int>(y), {grey, grey, grey});
                    }
                    canvas.Text(static_cast<int>(mapLeft) + 32, static_cast<int>(mapTop + mapSize * 0.5f) - 7, "NO MAP DATA", kWarn, 2);
                }
            }
        }
        else
        {
            Frame(canvas, "BRIEFING", clock, -1.0f);
            centred("STANDING BY", 100, kSoft, 3);
            centred("NO ORDERS", 150, kSoft, 2);
        }
        canvas.Lines();
        m_app->GetTextures().Update(m_briefingTextures[screen], canvas.image);
    }
}

} // namespace pred
