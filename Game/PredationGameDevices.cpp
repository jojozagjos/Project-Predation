// The things carried for finding the way: handheld devices with screens on them, looked down at in the hand -- and seen
// in anybody else's. The map shows the plan of what is round you (the site's buildings, a floor of them, or the ship's
// deck), never a creature or the objective; the objective tracker points at what is to be done next, a fan like a motion
// tracker's, and beeps faster the closer it is. Each holder's screen is its own texture, drawn here a few times a second.

#include "Game/PredationGame.h"

#include "Engine/Render/TextureLibrary.h"
#include "Game/World/ScreenCanvas.h"
#include "Game/Items/ItemAppearance.h"

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

void PredationGame::BuildDeviceFaces()
{
    // What a device's screen shows lying on a bench or in the bag: a map's plan and a tracker's fan, still -- drawn once,
    // for its icon and for when nobody is holding it.
    TextureLibrary& textures = m_app->GetTextures();
    {
        ScreenCanvas canvas(kScreenSize, kScreenSize);
        canvas.Clear(kGlass);
        const std::array<glm::vec4, 5> rooms{glm::vec4(40, 60, 120, 130), glm::vec4(120, 60, 200, 110), glm::vec4(120, 110, 170, 190),
                                             glm::vec4(170, 110, 220, 150), glm::vec4(40, 130, 120, 210)};
        for (const glm::vec4& room : rooms)
        {
            canvas.Fill(room.x, room.y, room.z, room.w, {26, 64, 40});
            canvas.Box(room.x, room.y, room.z, room.w, kGreen);
        }
        const glm::vec2 you{128.0f, 150.0f};
        canvas.Line(you.x, you.y - 9.0f, you.x - 5.0f, you.y + 5.0f, {235, 245, 235}, 2);
        canvas.Line(you.x, you.y - 9.0f, you.x + 5.0f, you.y + 5.0f, {235, 245, 235}, 2);
        canvas.Line(you.x - 5.0f, you.y + 5.0f, you.x + 5.0f, you.y + 5.0f, {235, 245, 235}, 2);
        canvas.Text(12, 10, "MAP", kGreen);
        canvas.Text(kScreenSize / 2 - 5, 24, "N", kGreen);
        canvas.Lines();
        canvas.PaintCase(kCase);
        const TextureHandle face = textures.CreateDynamic(kScreenSize, kScreenSize, "device_face_map");
        textures.Update(face, canvas.image);
        SetDeviceFace("map", face);
    }
    {
        ScreenCanvas canvas(kScreenSize, kScreenSize);
        canvas.Clear(kGlass);
        const float cx = kScreenSize * 0.5f;
        const float cy = kScreenSize - 26.0f;
        const float reach = kScreenSize - 60.0f;
        const float half = glm::radians(45.0f);
        canvas.Line(cx, cy, cx + std::sin(-half) * reach, cy - std::cos(-half) * reach, kGreen, 2);
        canvas.Line(cx, cy, cx + std::sin(half) * reach, cy - std::cos(half) * reach, kGreen, 2);
        canvas.Arc(cx, cy, reach, -half, half, kGreen);
        for (const float ring : {0.33f, 0.66f})
        {
            canvas.Arc(cx, cy, reach * ring, -half, half, kDim);
        }
        canvas.Dot(cx + 22.0f, cy - reach * 0.6f, 6.0f, {170, 255, 200});
        canvas.Text(12, 10, "OBJ", kGreen);
        canvas.Lines();
        canvas.PaintCase(kCase);
        const TextureHandle face = textures.CreateDynamic(kScreenSize, kScreenSize, "device_face_tracker");
        textures.Update(face, canvas.image);
        SetDeviceFace("tracker", face);
    }
    {
        ImageData face;
        DrawFinderScreen(face, glm::vec3(0.0f), 0.0f, 0.0f);
        const TextureHandle texture = textures.CreateDynamic(kScreenSize, kScreenSize, "device_face_finder");
        textures.Update(texture, face);
        SetDeviceFace("finder", texture);
    }
}

bool PredationGame::ObjectiveTargetFrom(const glm::vec3& here, glm::vec3& at) const
{
    // Aboard: the shuttle when it is waiting to go, the console where the next site is chosen otherwise.
    if (m_ship.Contains(here))
    {
        // (Or, with contracts on offer and none taken on, the ship's terminal where they are.)
        const bool offered = Contracts::Offered(m_campaign) > 0 &&
                             std::none_of(m_campaign.contracts.begin(), m_campaign.contracts.end(), [](const CampaignState::Contract& contract)
                                          { return contract.status == CampaignState::Contract::Status::Accepted; });
        at = m_shipReady                                                          ? m_ship.Shuttle().Home().position
             : m_campaignOpen && offered && !m_campaign.plan.set && m_terminalScreen.IsValid() ? m_ship.OpsTerminal().position
                                                                                  : m_ship.BriefingConsole().position;
        return true;
    }
    if (!m_facility.Built() || !m_facility.Contains(here) || !m_missionPlan.Valid())
    {
        return false;
    }
    // A plant's chain (Phase 6), not done: its filler (or, no fuel carried, the can), its pump, its valves, in turn -- then the console.
    if (m_missionPlan.chain && m_mission.stage == MissionState::Stage::Find && !MissionRules::PlantReady(m_mission, m_missionPlan) &&
        (m_missionPlan.mapGiven || m_mission.found))
    {
        if ((m_mission.plant & MissionState::kFueled) == 0u)
        {
            at = m_inventory.CountOf(m_fuelItem) > 0 ? m_missionPlan.filler : m_missionPlan.fuelCan;
            for (const WorldObjects::Pickup& pickup : m_world.Pickups())
            {
                if (m_inventory.CountOf(m_fuelItem) == 0 && pickup.alive && pickup.item == m_fuelItem)
                {
                    if (const Transform* where = m_scene.GetTransform(pickup.entity))
                    {
                        at = where->position;
                    }
                }
            }
        }
        else
        {
            at = m_mission.primed < MissionSpec::kPrimeStrokes ? m_missionPlan.pump : m_missionPlan.valves;
        }
        return true;
    }
    // The data in a server rack (Phase 6): a damaged drive's board wanted -- the other rack, unless one is carried; an archive's copy unit
    // wanted -- the shuttle's bench, unless it is carried; otherwise the rack. Copied, the unit still connected: the rack.
    if (m_missionPlan.AtRack() && (m_mission.stage == MissionState::Stage::Find || m_mission.stage == MissionState::Stage::Downloading ||
                                   (m_mission.stage == MissionState::Stage::Carry && (m_mission.rack & MissionState::kRackUnit) != 0u)))
    {
        at = m_missionPlan.terminal;
        if (m_mission.stage == MissionState::Stage::Find && m_missionPlan.data == MissionPlan::Data::Damaged && (m_mission.rack & MissionState::kRackBoard) == 0u &&
            (m_mission.rack & MissionState::kDonorTaken) == 0u && m_inventory.CountOf(m_boardItem) == 0)
        {
            at = m_missionPlan.donor;
        }
        if (m_mission.stage == MissionState::Stage::Find && m_missionPlan.data == MissionPlan::Data::Archive && (m_mission.rack & MissionState::kRackUnit) == 0u &&
            m_inventory.CountOf(m_copyItem) == 0)
        {
            at = m_missionProps.BenchSpot();
        }
        return m_missionPlan.mapGiven || m_mission.found || at != m_missionPlan.terminal;
    }
    // A survey (Phase 6): carrying a marker, the nearest point with none planted; not, the crate in the shuttle while points want
    // them; then the nearest marker not working (knocked over, or not set up); then any still reading. Then the shuttle.
    if (m_missionPlan.kind == MissionPlan::Kind::Survey && m_mission.stage == MissionState::Stage::Find)
    {
        const bool carrying = m_markerItem != kInvalidItem && m_inventory.CountOf(m_markerItem) > 0;
        const auto nearest = [&](auto wanted)
        {
            float best = 1.0e9f;
            for (int m = 0; m < m_mission.markers; ++m)
            {
                const float distance = glm::distance(here, m_missionPlan.markers[static_cast<size_t>(m)]);
                if (wanted(m_mission.survey[static_cast<size_t>(m)], m) && distance < best)
                {
                    best = distance;
                    at = m_missionPlan.markers[static_cast<size_t>(m)] + glm::vec3(0.0f, 1.0f, 0.0f);
                }
            }
            return best < 1.0e9f;
        };
        const auto empty = [](const MissionState::Marker& one, int) { return (one.setup & (MissionState::kPlanted | MissionState::kPacked)) == 0u; };
        if (carrying && nearest(empty))
        {
            return true;
        }
        if (!carrying && nearest(empty) && !m_missionProps.CrateSlots().empty())
        {
            at = m_missionProps.CrateSlots().front();
            return true;
        }
        if (nearest([](const MissionState::Marker& one, int) { return (one.setup & MissionState::kPlanted) != 0u && !MissionRules::MarkerWorking(one); }))
        {
            return true;
        }
        return nearest([&](const MissionState::Marker&, int m) { return ((m_mission.readings >> m) & 1u) == 0u; });
    }
    // A signal: its transmitter, the one marker; then the shuttle. (One still sending, tracked down with the direction finder: not known
    // to the tracker until somebody has found it. One gone silent: where it was last heard, from the briefing.)
    if (m_missionPlan.AtTransmitter() && (m_mission.stage == MissionState::Stage::Find || m_mission.stage == MissionState::Stage::Downloading))
    {
        if (m_missionPlan.kind == MissionPlan::Kind::Signal && !m_mission.found)
        {
            return false;
        }
        at = m_missionPlan.markers.front() + glm::vec3(0.0f, 1.0f, 0.0f);
        return true;
    }
    switch (m_mission.stage)
    {
    case MissionState::Stage::Find:
    {
        // Found dead (or a station's, known to be out from the briefing), the terminal waits on the breaker: its building's.
        glm::vec3 panel;
        float yaw = 0.0f;
        if (!m_mission.powered && (m_missionFoundNoPower || m_missionPlan.kind == MissionPlan::Kind::Restore) && MissionBreakerAt(panel, yaw))
        {
            at = panel;
            return true;
        }
        // Without a map, where the terminal is is not known until somebody has found it: nothing to point at until then.
        if (!m_missionPlan.mapGiven && !m_mission.found)
        {
            return false;
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
        // The one deck.
        for (const glm::vec4& room : ShipMap::DeckPlan(0))
        {
            quad(ShipSpec::kOrigin.x + room.x, ShipSpec::kOrigin.z + room.y, ShipSpec::kOrigin.x + room.z, ShipSpec::kOrigin.z + room.w, kFaint, true);
        }
        std::snprintf(label, sizeof(label), "THE SHIP");
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
                    case FacilityLayout::Cell::Duct:
                    case FacilityLayout::Cell::Outside:
                    case FacilityLayout::Cell::Void: continue;
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

float PredationGame::SignalStrength(const glm::vec3& here, float yaw, float& seems) const
{
    // A transmitter still sending (a signal's, its sender and power on), heard out to a few hundred metres -- faintly through walls and
    // floors -- the way it seems to come from swinging wide far off and settling as it gets near.
    seems = 0.0f;
    const bool sending = m_missionPlan.AtTransmitter() && !m_missionPlan.markers.empty() && m_mission.stage != MissionState::Stage::None &&
                         m_mission.stage != MissionState::Stage::Over && (m_mission.device & (MissionState::kSenderOff | MissionState::kPowerOff)) == 0u;
    if (!sending || !AtSite())
    {
        return 0.0f;
    }
    const glm::vec3 source = m_missionPlan.markers.front() + glm::vec3(0.0f, 0.5f, 0.0f);
    const glm::vec2 to{source.x - here.x, source.z - here.z};
    const float distance = glm::length(glm::vec3(source - here));
    float strength = std::clamp(1.0f - std::log(1.0f + distance) / std::log(1.0f + 420.0f), 0.0f, 1.0f);
    // (Under different roofs, or a floor or two apart: weaker for it.)
    if (std::abs(source.y - here.y) > 3.0f)
    {
        strength *= 0.7f;
    }
    const float t = m_deviceClock;
    const float wander = (std::sin(t * 1.7f) + std::sin(t * 2.9f + 1.0f) + 0.6f * std::sin(t * 4.3f + 2.0f)) / 2.6f;
    const float spread = glm::radians(glm::mix(75.0f, 3.0f, strength * strength));
    seems = std::remainder(std::atan2(to.x, -to.y) - yaw + wander * spread, glm::two_pi<float>());
    return strength;
}

void PredationGame::DrawFinderScreen(ImageData& out, const glm::vec3& here, float yaw, float flash) const
{
    // A dial: its ring, ahead marked at its top, its needle from the middle the way the signal seems; under it, its strength in bars.
    ScreenCanvas canvas(kScreenSize, kScreenSize);
    canvas.Clear(kGlass);
    const float cx = kScreenSize * 0.5f;
    const float cy = kScreenSize * 0.46f;
    const float radius = kScreenSize * 0.34f;
    canvas.Arc(cx, cy, radius, -glm::pi<float>(), glm::pi<float>(), kGreen);
    canvas.Arc(cx, cy, radius - 1.0f, -glm::pi<float>(), glm::pi<float>(), kGreen);
    for (int tick = 0; tick < 12; ++tick)
    {
        const float a = static_cast<float>(tick) * glm::two_pi<float>() / 12.0f;
        const float inner = tick == 0 ? radius - 18.0f : radius - 9.0f;
        canvas.Line(cx + std::sin(a) * inner, cy - std::cos(a) * inner, cx + std::sin(a) * radius, cy - std::cos(a) * radius, tick == 0 ? kGreen : kDim, tick == 0 ? 3 : 1);
    }
    canvas.Text(12, 10, "DF", kGreen);
    float seems = 0.0f;
    const float strength = SignalStrength(here, yaw, seems);
    if (strength > 0.0f)
    {
        const float reach = radius - 14.0f;
        const Rgb needle{static_cast<uint8_t>(200 + 50 * flash), static_cast<uint8_t>(150 + 60 * flash), 70};
        canvas.Line(cx, cy, cx + std::sin(seems) * reach, cy - std::cos(seems) * reach, needle, 4);
        canvas.Dot(cx, cy, 6.0f, needle);
    }
    else
    {
        const char* none = "NO SIGNAL";
        canvas.Text(static_cast<int>(cx) - canvas.TextWidth(none) / 2, static_cast<int>(cy) - 6, none, kAmber);
    }
    // Its strength: ten bars, lit as strong as it is.
    const int bars = static_cast<int>(std::round(strength * 10.0f));
    for (int b = 0; b < 10; ++b)
    {
        const float x = 30.0f + static_cast<float>(b) * 20.0f;
        const float h = 6.0f + static_cast<float>(b) * 2.2f;
        const float base = kScreenSize - 22.0f;
        if (b < bars)
        {
            canvas.Fill(x, base - h, x + 14.0f, base, b < 7 ? kGreen : kAmber);
        }
        else
        {
            canvas.Box(x, base - h, x + 14.0f, base, kFaint);
        }
    }
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
        const bool finder = holder.item->device == "finder";
        // The finder's: as quick as the signal is strong, a click of static otherwise.
        float seems = 0.0f;
        const float strength = finder ? SignalStrength(holder.at, holder.yaw, seems) : 0.0f;
        if (finder && strength > 0.0f && !CinematicHoldsPlayers() && !m_cine.Active())
        {
            screen.beepIn -= dt;
            if (screen.beepIn <= 0.0f)
            {
                screen.beepIn = glm::mix(1.6f, 0.12f, strength);
                screen.beepAt = m_deviceClock;
                const bool mine = holder.id == LocalPlayerId();
                PlayNamed("Items/tracker_beep", holder.at, mine ? 0.4f : 0.25f, 0.7f + 0.6f * strength, !mine);
            }
        }
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
            else if (finder)
            {
                DrawFinderScreen(picture, holder.at, holder.yaw, std::clamp(1.0f - (m_deviceClock - screen.beepAt) / 0.25f, 0.0f, 1.0f));
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
