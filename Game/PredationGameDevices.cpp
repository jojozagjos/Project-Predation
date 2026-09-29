// The things carried for finding the way: handheld devices with screens on them, looked down at in the hand -- and seen
// in anybody else's. The map shows the plan of what is round you (the site's buildings, a floor of them, or the ship's
// deck), never a creature or the objective; the objective tracker points at what is to be done next, a fan like a motion
// tracker's, and beeps faster the closer it is. Each holder's screen is its own texture, drawn here a few times a second.

#include "Game/PredationGame.h"

#include "Engine/Render/TextureLibrary.h"
#include "Game/World/ScreenCanvas.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pred
{

namespace
{

constexpr int kScreenSize = 256;
constexpr float kScreenEvery = 1.0f / 15.0f;
// How much of the plan round you the map shows, across: more on a site that came with map data.
constexpr float kMapAcrossMapped = 44.0f;
constexpr float kMapAcrossUnmapped = 22.0f;
constexpr float kMapAcrossShip = 40.0f;

constexpr Rgb kCase{16, 17, 18};
constexpr Rgb kGlass{4, 12, 7};
constexpr Rgb kGreen{110, 235, 150};
constexpr Rgb kDim{40, 95, 60};
constexpr Rgb kFaint{20, 48, 30};
constexpr Rgb kAmber{235, 160, 70};

} // namespace

const std::string& PredationGame::HeldDevice() const
{
    static const std::string none;
    const ItemDefinition* held = m_items.Get(m_heldItem);
    return held != nullptr && m_player.State().alive ? held->device : none;
}

bool PredationGame::ObjectiveTarget(glm::vec3& at) const
{
    return ObjectiveTargetFrom(m_player.State().position, at);
}

bool PredationGame::ObjectiveTargetFrom(const glm::vec3& here, glm::vec3& at) const
{
    // Aboard: the shuttle when it is waiting to go, the console where the next site is chosen otherwise.
    if (m_ship.Contains(here))
    {
        at = m_shipReady ? m_ship.Shuttle().Home().position : m_ship.BriefingConsole().position;
        return true;
    }
    if (!m_facility.Built() || !m_facility.Contains(here) || !m_missionPlan.Valid())
    {
        return false;
    }
    switch (m_mission.stage)
    {
    case MissionState::Stage::Find:
    {
        // Found dead, the terminal waits on its building's breaker.
        glm::vec3 panel;
        float yaw = 0.0f;
        if (!m_mission.powered && m_missionFoundNoPower &&
            BreakerPanel(m_facility.Plan().buildings[static_cast<size_t>(m_missionPlan.building)], panel, yaw))
        {
            at = panel;
            return true;
        }
        at = m_missionPlan.terminal;
        return true;
    }
    case MissionState::Stage::Downloading:
        at = m_missionPlan.terminal;
        return true;
    case MissionState::Stage::Carry:
        at = m_facility.Shuttle().Home().position;
        return true;
    case MissionState::Stage::None:
    case MissionState::Stage::Over:
        break;
    }
    return false;
}

void PredationGame::DrawMapScreen(ImageData& out, const glm::vec3& here, float yaw) const
{
    ScreenCanvas canvas(kScreenSize, kScreenSize);
    canvas.Clear(kGlass);
    const float middle = kScreenSize * 0.5f;
    const auto header = [&](const char* text) { canvas.Text(12, 10, text, kGreen); };

    // The plan round you, north up: what it shows, and how much of it.
    const bool aboard = m_ship.Contains(here);
    const bool atSite = m_facility.Built() && m_facility.Contains(here) && m_missionPlan.Valid();
    if (!aboard && !atSite)
    {
        header("MAP");
        const char* text = "NO MAP DATA";
        canvas.Text(static_cast<int>(middle) - canvas.TextWidth(text) / 2, static_cast<int>(middle) - 7, text, kDim);
        canvas.Lines();
        canvas.PaintCase(kCase);
        out = std::move(canvas.image);
        return;
    }
    const float across = aboard ? kMapAcrossShip : (m_missionPlan.mapGiven ? kMapAcrossMapped : kMapAcrossUnmapped);
    const float scale = (kScreenSize - 24.0f) / across;
    // Turned with whoever holds it, the way they face up the screen, and them a little below the middle, where more of
    // what is ahead fits.
    const glm::vec2 ahead{std::sin(yaw), -std::cos(yaw)};
    const glm::vec2 side{std::cos(yaw), std::sin(yaw)};
    const glm::vec2 centre{middle, middle + 24.0f};
    const auto at = [&](float x, float z)
    {
        const glm::vec2 offset{x - here.x, z - here.z};
        return centre + glm::vec2(glm::dot(offset, side), -glm::dot(offset, ahead)) * scale;
    };
    const auto quad = [&](float x0, float z0, float x1, float z1, Rgb fill, bool outline)
    {
        const glm::vec2 a = at(x0, z0);
        const glm::vec2 b = at(x1, z0);
        const glm::vec2 c = at(x1, z1);
        const glm::vec2 d = at(x0, z1);
        canvas.FillQuad(a, b, c, d, fill);
        if (outline)
        {
            canvas.Line(a.x, a.y, b.x, b.y, kDim);
            canvas.Line(b.x, b.y, c.x, c.y, kDim);
            canvas.Line(c.x, c.y, d.x, d.y, kDim);
            canvas.Line(d.x, d.y, a.x, a.y, kDim);
        }
    };
    char label[32];
    if (aboard)
    {
        // The deck you are on.
        const glm::vec3 local = here - ShipSpec::kOrigin;
        const int deck = local.y > 2.8f && (local.z < 6.15f) ? 1 : 0;
        for (const glm::vec4& room : ShipMap::DeckPlan(deck))
        {
            quad(ShipSpec::kOrigin.x + room.x, ShipSpec::kOrigin.z + room.y, ShipSpec::kOrigin.x + room.z, ShipSpec::kOrigin.z + room.w, kFaint, true);
        }
        std::snprintf(label, sizeof(label), "DECK %d", deck + 1);
        header(label);
    }
    else
    {
        // Each building near: the floor you are on in the one you are in, the ground floor of the rest.
        const SitePlan& site = m_facility.Plan();
        for (const FacilityLayout& building : site.buildings)
        {
            glm::vec2 lo;
            glm::vec2 hi;
            SitePlan::Footprint(building, 0.0f, lo, hi);
            int floor = 0;
            if (here.x > lo.x && here.x < hi.x && here.z > lo.y && here.z < hi.y)
            {
                floor = std::clamp(static_cast<int>(std::floor((here.y - building.origin.y + 0.5f) / FacilityLayout::kStorey)), 0, building.floors - 1);
            }
            for (int z = 0; z < building.depth; ++z)
            {
                for (int x = 0; x < building.width; ++x)
                {
                    Rgb colour{};
                    switch (building.At(floor, x, z))
                    {
                    case FacilityLayout::Cell::Room: colour = {26, 64, 40}; break;
                    case FacilityLayout::Cell::Corridor: colour = {18, 44, 28}; break;
                    case FacilityLayout::Cell::Stair: colour = {60, 70, 30}; break;
                    case FacilityLayout::Cell::Solid:
                    case FacilityLayout::Cell::Duct: continue;
                    }
                    const float cx = building.origin.x + static_cast<float>(x) * FacilityLayout::kCell;
                    const float cz = building.origin.z + static_cast<float>(z) * FacilityLayout::kCell;
                    const float cx1 = cx + FacilityLayout::kCell;
                    const float cz1 = cz + FacilityLayout::kCell;
                    quad(cx, cz, cx1, cz1, colour, false);
                    // A wall wherever the next cell along is not open.
                    const auto open = [&](int nx, int nz) { return nx >= 0 && nz >= 0 && nx < building.width && nz < building.depth && building.Open(floor, nx, nz); };
                    const auto wall = [&](float x0, float z0, float x1, float z1)
                    {
                        const glm::vec2 a = at(x0, z0);
                        const glm::vec2 b = at(x1, z1);
                        canvas.Line(a.x, a.y, b.x, b.y, kGreen);
                    };
                    if (!open(x - 1, z))
                    {
                        wall(cx, cz, cx, cz1);
                    }
                    if (!open(x + 1, z))
                    {
                        wall(cx1, cz, cx1, cz1);
                    }
                    if (!open(x, z - 1))
                    {
                        wall(cx, cz, cx1, cz);
                    }
                    if (!open(x, z + 1))
                    {
                        wall(cx, cz1, cx1, cz1);
                    }
                }
            }
        }
        // The way home: the pad, and the shuttle on it.
        if (m_facility.Shuttle().Built())
        {
            const glm::vec3 parked = m_facility.Shuttle().Home().position;
            const glm::vec2 p = at(parked.x, parked.z);
            canvas.Fill(p.x - 3.0f, p.y - 3.0f, p.x + 3.0f, p.y + 3.0f, kAmber);
        }
        const glm::vec3 pad = site.ShuttleBase();
        const glm::vec2 p = at(pad.x, pad.z);
        canvas.Arc(p.x, p.y, 7.0f * scale, 0.0f, glm::two_pi<float>(), kDim);
        std::snprintf(label, sizeof(label), "MAP %s", m_missionPlan.mapGiven ? "" : "LOCAL");
        header(label);
    }
    // Everybody else, and you, pointing the way you face.
    for (const RemotePlayerView& other : RemotePlayers())
    {
        if (other.alive && glm::distance(other.position, here) > 0.3f)
        {
            const glm::vec2 p = at(other.position.x, other.position.z);
            canvas.Dot(p.x, p.y, 3.0f, {200, 200, 120});
        }
    }
    if (m_player.State().alive && glm::distance(m_player.State().position, here) > 0.3f)
    {
        const glm::vec2 p = at(m_player.State().position.x, m_player.State().position.z);
        canvas.Dot(p.x, p.y, 3.0f, {200, 200, 120});
    }
    // You, always pointing up the screen: the map turns, not you.
    const glm::vec2 you = centre;
    const glm::vec2 up{0.0f, -1.0f};
    const glm::vec2 right{1.0f, 0.0f};
    const glm::vec2 tip = you + up * 9.0f;
    const glm::vec2 left = you - up * 5.0f - right * 5.0f;
    const glm::vec2 rightCorner = you - up * 5.0f + right * 5.0f;
    canvas.Line(tip.x, tip.y, left.x, left.y, {235, 245, 235}, 2);
    canvas.Line(tip.x, tip.y, rightCorner.x, rightCorner.y, {235, 245, 235}, 2);
    canvas.Line(left.x, left.y, rightCorner.x, rightCorner.y, {235, 245, 235}, 2);
    // North, round the edge where it is, and how far across.
    {
        const glm::vec2 north{-std::sin(yaw), -std::cos(yaw)};
        const glm::vec2 mark = glm::vec2(middle, middle) + north * (middle - 22.0f);
        canvas.Text(static_cast<int>(mark.x) - 5, static_cast<int>(mark.y) - 7, "N", kGreen);
    }
    std::snprintf(label, sizeof(label), "%dM", static_cast<int>(across));
    canvas.Text(kScreenSize - 12 - canvas.TextWidth(label), kScreenSize - 24, label, kDim);
    canvas.Lines();
    canvas.PaintCase(kCase);
    out = std::move(canvas.image);
}

void PredationGame::DrawTrackerScreen(ImageData& out, const glm::vec3& here, float yaw, float flash) const
{
    // A fan, opening forward from the bottom, rings across it, and the objective as a blip in it -- or, off to one side,
    // at its edge that side.
    ScreenCanvas canvas(kScreenSize, kScreenSize);
    canvas.Clear(kGlass);
    const float cx = kScreenSize * 0.5f;
    const float cy = kScreenSize - 26.0f;
    const float reach = kScreenSize - 60.0f;
    const float half = glm::radians(45.0f);
    canvas.Line(cx, cy, cx + std::sin(-half) * reach, cy - std::cos(-half) * reach, kGreen, 2);
    canvas.Line(cx, cy, cx + std::sin(half) * reach, cy - std::cos(half) * reach, kGreen, 2);
    canvas.Arc(cx, cy, reach, -half, half, kGreen);
    canvas.Arc(cx, cy, reach - 1.0f, -half, half, kGreen);
    for (const float ring : {0.33f, 0.66f})
    {
        canvas.Arc(cx, cy, reach * ring, -half, half, kDim);
    }
    canvas.Line(cx, cy, cx, cy - reach, kFaint);
    canvas.Text(12, 10, "OBJ", kGreen);
    glm::vec3 target;
    char label[32];
    if (ObjectiveTargetFrom(here, target))
    {
        const glm::vec2 to{target.x - here.x, target.z - here.z};
        const float distance = glm::length(to);
        const float relative = std::remainder(std::atan2(to.x, -to.y) - yaw, glm::two_pi<float>());
        const float fraction = std::clamp(std::log(1.0f + distance) / std::log(1.0f + 150.0f), 0.1f, 1.0f);
        if (std::abs(relative) <= half)
        {
            const float radius = 5.0f + 5.0f * flash;
            canvas.Dot(cx + std::sin(relative) * reach * fraction, cy - std::cos(relative) * reach * fraction, radius,
                       {static_cast<uint8_t>(150 + 90 * flash), 255, static_cast<uint8_t>(180 + 60 * flash)});
        }
        else
        {
            // Off the fan: an arrow on its edge, the side to turn.
            const float side = relative > 0.0f ? half : -half;
            const float x = cx + std::sin(side) * reach * 0.7f;
            const float y = cy - std::cos(side) * reach * 0.7f;
            const float dir = relative > 0.0f ? 1.0f : -1.0f;
            canvas.Line(x, y, x + dir * 14.0f, y, kAmber, 3);
            canvas.Line(x + dir * 14.0f, y, x + dir * 7.0f, y - 7.0f, kAmber, 3);
            canvas.Line(x + dir * 14.0f, y, x + dir * 7.0f, y + 7.0f, kAmber, 3);
        }
        std::snprintf(label, sizeof(label), "%dM", static_cast<int>(distance));
    }
    else
    {
        std::snprintf(label, sizeof(label), "NO SIGNAL");
    }
    canvas.Text(static_cast<int>(cx) - canvas.TextWidth(label) / 2, 10, label, kAmber);
    canvas.Lines();
    canvas.PaintCase(kCase);
    out = std::move(canvas.image);
}

void PredationGame::UpdateDevices(float dt)
{
    m_deviceClock += dt;
    if (m_screen != Screen::Playing)
    {
        return;
    }
    // Everybody holding one: you, and anybody else, each with a screen of their own.
    struct Holder
    {
        uint8_t id;
        PlayerBody* body;
        glm::vec3 at;
        float yaw;
        const ItemDefinition* item;
    };
    std::vector<Holder> holders;
    if (const ItemDefinition* held = m_items.Get(m_heldItem); held != nullptr && !held->device.empty() && m_player.State().alive)
    {
        holders.push_back({LocalPlayerId(), &m_body, m_player.State().position, m_lookYaw, held});
    }
    for (const std::unique_ptr<RemoteAvatar>& avatar : m_avatars)
    {
        if (avatar == nullptr || !avatar->built)
        {
            continue;
        }
        const ItemDefinition* held = m_items.Get(static_cast<ItemId>(avatar->heldItem));
        if (held == nullptr || held->device.empty())
        {
            continue;
        }
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            if (remote.id == avatar->id && remote.alive)
            {
                holders.push_back({remote.id, &avatar->body, remote.position, remote.yaw, held});
            }
        }
    }
    TextureLibrary& textures = m_app->GetTextures();
    for (const Holder& holder : holders)
    {
        DeviceScreen& screen = m_deviceScreens[holder.id];
        if (!screen.texture.IsValid() || screen.texture.index == 0)
        {
            screen.texture = textures.CreateDynamic(kScreenSize, kScreenSize, "device_screen_" + std::to_string(holder.id));
        }
        // Its beep: faster and higher the closer it is -- in your head for your own, from where they stand for others'.
        const bool tracker = holder.item->device == "tracker";
        glm::vec3 target;
        // Quiet while a cinematic has everybody: its sound is the cinematic's.
        if (tracker && !CinematicHoldsPlayers() && !m_cine.Active() && ObjectiveTargetFrom(holder.at, target))
        {
            const float distance = glm::length(glm::vec2(target.x - holder.at.x, target.z - holder.at.z));
            screen.beepIn -= dt;
            if (screen.beepIn <= 0.0f)
            {
                screen.beepIn = std::clamp(0.15f + distance / 55.0f, 0.15f, 2.0f);
                screen.beepAt = m_deviceClock;
                const bool mine = holder.id == LocalPlayerId();
                PlayNamed("Items/tracker_beep", holder.at, mine ? 0.45f : 0.3f, 1.0f + 0.4f * std::clamp(1.0f - distance / 40.0f, 0.0f, 1.0f), !mine);
            }
        }
        if (m_deviceClock - screen.drawnAt >= kScreenEvery || screen.device != holder.item->device)
        {
            screen.drawnAt = m_deviceClock;
            screen.device = holder.item->device;
            ImageData picture;
            if (tracker)
            {
                DrawTrackerScreen(picture, holder.at, holder.yaw, std::clamp(1.0f - (m_deviceClock - screen.beepAt) / 0.25f, 0.0f, 1.0f));
            }
            else
            {
                DrawMapScreen(picture, holder.at, holder.yaw);
            }
            textures.Update(screen.texture, picture);
        }
        // On the device in the hand: its picture, glowing.
        if (MeshRenderer* renderer = holder.body->HeldItemRenderer(m_scene))
        {
            renderer->material.baseColor = glm::vec3(1.0f);
            renderer->material.baseColorTexture = screen.texture;
            renderer->material.emissive = glm::vec3(2.4f);
            renderer->material.emissiveTextured = true;
        }
    }
}

} // namespace pred
