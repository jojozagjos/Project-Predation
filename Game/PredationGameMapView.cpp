// The navigation map: one picture at three scales -- the galaxy, a system, a body -- gone between by zooming, and the
// panels round it. Opened at the ship's navigation console. What it asks for (a course, a place to go down) goes
// through the campaign actions in PredationGameMap.cpp; the picture is drawn by Engine/Render/PlanetRenderer into its
// own target, with the stars, names and markers over it.
//
// Right-drag slides the map along under the mouse (the galaxy goes on as far as anybody cares to scroll), left-drag
// or middle-drag turns it, left-click picks something out, double-click opens it, the wheel zooms -- in past the
// nearest goes into whatever is under the middle, out past the furthest goes back up a scale.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/Renderer.h"

#include <imgui.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace pred
{

namespace
{

constexpr float kTau = 6.28318530718f;
constexpr float kDegrees = 0.0174532925f;

// How near and far the camera can be: light years over the galaxy, a system's map units, a body's radii.
constexpr float kGalaxyNearest = 6.0f;
constexpr float kGalaxyFurthest = 150.0f;
constexpr float kSystemNearest = 0.6f;
constexpr float kBodyNearest = 1.8f;
constexpr float kBodyFurthest = 5.0f;
// A press that moves less than this many pixels is a click, not a drag.
constexpr float kClickSlop = 4.0f;

constexpr ImU32 kAmber = IM_COL32(236, 156, 64, 255);
constexpr ImU32 kText = IM_COL32(220, 226, 230, 255);
constexpr ImU32 kDim = IM_COL32(130, 138, 146, 255);
constexpr ImU32 kShipColour = IM_COL32(255, 236, 200, 240);
constexpr ImU32 kNight = IM_COL32(130, 160, 230, 255);
constexpr ImU32 kGo = IM_COL32(120, 220, 140, 255);
constexpr ImU32 kHub = IM_COL32(150, 220, 255, 255);
constexpr ImVec4 kAmberText{0.93f, 0.61f, 0.25f, 1.0f};
constexpr ImVec4 kDimText{0.55f, 0.58f, 0.62f, 1.0f};
constexpr ImVec4 kGoText{0.47f, 0.86f, 0.55f, 1.0f};

constexpr ImGuiWindowFlags kPanel = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

uint32_t Abgr(int r, int g, int b, int a)
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}

int Byte(float value)
{
    return static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

uint32_t AbgrOf(const glm::vec3& colour, float alpha)
{
    return Abgr(Byte(colour.r), Byte(colour.g), Byte(colour.b), Byte(alpha));
}

ImU32 Colour(const glm::vec3& colour, float alpha)
{
    return IM_COL32(Byte(colour.r), Byte(colour.g), Byte(colour.b), Byte(alpha));
}

ImU32 Faded(ImU32 colour, float alpha)
{
    const float a = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFFu) * std::clamp(alpha, 0.0f, 1.0f);
    return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

std::string About(float seconds)
{
    if (seconds < 50.0f)
    {
        return "under a minute";
    }
    const int minutes = static_cast<int>(std::round(seconds / 60.0f));
    return "about " + std::to_string(std::max(minutes, 1)) + " min";
}

std::string Number(float value, int places)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.*f", places, static_cast<double>(value));
    return text;
}

bool Contains(const std::string& text, const char* search)
{
    std::string a = text;
    std::string b = search;
    for (char& c : a)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    for (char& c : b)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return a.find(b) != std::string::npos;
}

// How the panels look: dark glass with a thin amber edge, as the ship's screens are.
struct MapStyle
{
    MapStyle()
    {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.05f, 0.86f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.93f, 0.61f, 0.25f, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.93f, 0.61f, 0.25f, 0.24f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.93f, 0.61f, 0.25f, 0.13f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.93f, 0.61f, 0.25f, 0.32f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.93f, 0.61f, 0.25f, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.93f, 0.61f, 0.25f, 0.38f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.93f, 0.61f, 0.25f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.93f, 0.61f, 0.25f, 0.25f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 5.0f));
    }
    ~MapStyle()
    {
        ImGui::PopStyleVar(5);
        ImGui::PopStyleColor(11);
    }
    MapStyle(const MapStyle&) = delete;
    MapStyle& operator=(const MapStyle&) = delete;
};

// A panel's section: small amber capitals with a rule under them.
void Section(const char* title)
{
    ImGui::Spacing();
    ImGui::TextColored(kAmberText, "%s", title);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine({at.x, at.y - 1.0f}, {at.x + ImGui::GetContentRegionAvail().x, at.y - 1.0f}, IM_COL32(236, 156, 64, 70));
    ImGui::Spacing();
}

// Words in a colour, wrapped at the panel's edge rather than running off it.
void Wrapped(const ImVec4& colour, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextWrappedV(format, args);
    ImGui::PopStyleColor();
    va_end(args);
}

// A panel's title: larger, with a line of what it is under it.
void Title(const std::string& title, const std::string& under)
{
    ImGui::SetWindowFontScale(1.3f);
    ImGui::TextWrapped("%s", title.c_str());
    ImGui::SetWindowFontScale(1.0f);
    if (!under.empty())
    {
        Wrapped(kDimText, "%s", under.c_str());
    }
}

// A name and its value on one line, the values lined up.
void Row(const char* what, const std::string& value, bool known = true)
{
    ImGui::TextColored(kDimText, "%s", what);
    ImGui::SameLine(116.0f);
    if (known)
    {
        ImGui::TextWrapped("%s", value.c_str());
    }
    else
    {
        ImGui::TextColored(kDimText, "Unknown");
    }
}

// A tag to the right of a list's line: "HERE", "COURSE".
void Tag(const char* text, ImU32 colour)
{
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SameLine(std::max(ImGui::GetWindowContentRegionMax().x - width - 4.0f, 120.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

ImVec2 TextSize(const char* text, float size)
{
    return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

// Words on the picture, shadowed so they read over anything.
void Label(ImDrawList* draw, const ImVec2& at, ImU32 colour, const char* text, float size)
{
    ImFont* font = ImGui::GetFont();
    draw->AddText(font, size, {at.x + 1.0f, at.y + 1.0f}, Faded(IM_COL32(0, 0, 0, 210), static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xFFu) / 255.0f), text);
    draw->AddText(font, size, at, colour, text);
}

void Diamond(ImDrawList* draw, const ImVec2& at, float r, ImU32 colour, bool filled)
{
    const ImVec2 points[4] = {{at.x, at.y - r}, {at.x + r, at.y}, {at.x, at.y + r}, {at.x - r, at.y}};
    if (filled)
    {
        draw->AddConvexPolyFilled(points, 4, colour);
    }
    else
    {
        draw->AddPolyline(points, 4, colour, ImDrawFlags_Closed, 1.5f);
    }
}

// Corner brackets round something picked out, as a sight has.
void Brackets(ImDrawList* draw, const ImVec2& at, float r, ImU32 colour)
{
    const float arm = r * 0.45f;
    for (int i = 0; i < 4; ++i)
    {
        const float sx = (i & 1) != 0 ? 1.0f : -1.0f;
        const float sy = (i & 2) != 0 ? 1.0f : -1.0f;
        const ImVec2 corner{at.x + sx * r, at.y + sy * r};
        draw->AddLine(corner, {corner.x - sx * arm, corner.y}, colour, 1.6f);
        draw->AddLine(corner, {corner.x, corner.y - sy * arm}, colour, 1.6f);
    }
}

// The ship as the map marks it: a pointed shape, outlined, aimed along `way` (screen, normalised).
void ShipMark(ImDrawList* draw, const ImVec2& at, const glm::vec2& way, float size, ImU32 colour)
{
    const glm::vec2 side{-way.y, way.x};
    const ImVec2 tip{at.x + way.x * size * 1.4f, at.y + way.y * size * 1.4f};
    const ImVec2 left{at.x - way.x * size + side.x * size, at.y - way.y * size + side.y * size};
    const ImVec2 back{at.x - way.x * size * 0.4f, at.y - way.y * size * 0.4f};
    const ImVec2 right{at.x - way.x * size - side.x * size, at.y - way.y * size - side.y * size};
    const ImVec2 points[4] = {tip, left, back, right};
    draw->AddConvexPolyFilled(points, 3, colour);
    const ImVec2 rest[3] = {tip, back, right};
    draw->AddConvexPolyFilled(rest, 3, colour);
    draw->AddPolyline(points, 4, IM_COL32(0, 0, 0, 220), ImDrawFlags_Closed, 1.0f);
}

// A label under a point, centred on it: a line, and a quieter one under it.
void LabelUnder(ImDrawList* draw, const ImVec2& at, float gap, ImU32 colour, const std::string& first, ImU32 quiet, const std::string& second, float size,
                float smaller)
{
    const ImVec2 one = TextSize(first.c_str(), size);
    Label(draw, {at.x - one.x * 0.5f, at.y + gap}, colour, first.c_str(), size);
    if (!second.empty())
    {
        const ImVec2 two = TextSize(second.c_str(), smaller);
        Label(draw, {at.x - two.x * 0.5f, at.y + gap + one.y}, quiet, second.c_str(), smaller);
    }
}

// Where on its globe a place is: latitude and longitude in degrees, on the unit sphere, the pole up.
glm::vec3 Globe(const glm::vec2& latLon)
{
    const float lat = latLon.x * kDegrees;
    const float lon = latLon.y * kDegrees;
    return {std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)};
}

std::string LatLonText(const glm::vec2& latLon)
{
    return Number(std::abs(latLon.x), 1) + (latLon.x >= 0.0f ? " N   " : " S   ") + Number(std::abs(latLon.y), 1) + (latLon.y >= 0.0f ? " E" : " W");
}

} // namespace

// --- Opening and closing ----------------------------------------------------------------------------------------------

void PredationGame::OpenSystemMap()
{
    if (CurrentSystem() == nullptr)
    {
        return;
    }
    m_mapOpen = true;
    m_inventoryOpen = false;
    CloseLoadout();
    m_wantMouseCaptured = false;
    UpdateMouseCapture();
    m_mapDragButton = -1;
    if (!m_mapFramed)
    {
        m_mapFramed = true;
        if (m_campaign.travel.interstellar)
        {
            ApplyMapGalaxy(Travel::GalaxyPosition(m_campaign, m_universe), 60.0f);
        }
        else
        {
            // The whole system, with where the ship is (or is going) picked out.
            ApplyMapSystem(m_campaign.system, -1);
            m_mapSelected = m_campaign.travel.underway ? m_campaign.travel.target : m_campaign.body;
        }
    }
    PlayNamed("UI/confirm", m_renderEye, 0.5f, 1.0f, false);
}

void PredationGame::CloseSystemMap()
{
    if (!m_mapOpen)
    {
        return;
    }
    m_mapOpen = false;
    m_mapHovered = -1;
    m_mapDragButton = -1;
    m_wantMouseCaptured = !m_paused;
    UpdateMouseCapture();
    PlayNamed("UI/back", m_renderEye, 0.5f, 1.0f, false);
}

void PredationGame::DestroySystemMapTarget()
{
    if (bgfx::isValid(m_mapBuffer))
    {
        bgfx::destroy(m_mapBuffer);
    }
    m_mapBuffer = BGFX_INVALID_HANDLE;
    m_mapTexture = BGFX_INVALID_HANDLE;
    m_mapWidth = 0;
    m_mapHeight = 0;
}

std::string PredationGame::MapRecordsText(uint64_t system, int body)
{
    const uint8_t known = m_campaign.Known(system, body);
    if (body < 0)
    {
        if (system == m_universe.Home().Packed())
        {
            return "Surveyed: all of it on file";
        }
        return (known & CampaignState::kKnownRecords) != 0 ? "On file: its bodies, not what is on them" : "Nothing on file";
    }
    if ((known & CampaignState::kKnownRecords) != 0 && (known & CampaignState::kKnownDeep) != 0)
    {
        return "Surveyed: all of it on file";
    }
    if ((known & CampaignState::kKnownRecords) != 0)
    {
        return "On file: what it is, not what is on it";
    }
    return "Nothing on file";
}

// --- The map everybody shares -----------------------------------------------------------------------------------------
//
// One map for the whole crew: what it shows (its scale, its system, what is picked out on it) and the camera it is seen
// through. Whoever moves it -- drags, zooms, picks something, opens a body -- sends where it is now, a few times a second at
// most; the host passes that on to everybody; everybody else's eases to it. Applied moves are kept as the last known, so they
// are not sent straight back.

MapViewMessage PredationGame::MapSnapshot() const
{
    MapViewMessage view;
    view.level = m_mapLevel == MapLevel::Galaxy ? 0 : m_mapLevel == MapLevel::System ? 1 : 2;
    view.system = m_mapSystem;
    view.selected = static_cast<int8_t>(std::clamp(m_mapSelected, -1, 127));
    view.region = static_cast<int8_t>(std::clamp(m_mapRegion, -1, 127));
    view.hasPickedSystem = m_mapHasPickedSystem;
    view.pickedSystem = m_mapPickedSystem;
    const SystemMapView::Wanted wanted = m_mapView.GetWanted();
    view.focus = wanted.focus;
    view.distance = wanted.distance;
    view.yaw = wanted.yaw;
    view.pitch = wanted.pitch;
    return view;
}

namespace
{

bool MapMoved(const MapViewMessage& a, const MapViewMessage& b)
{
    if (a.level != b.level || a.system != b.system || a.selected != b.selected || a.region != b.region || a.hasPickedSystem != b.hasPickedSystem ||
        a.pickedSystem != b.pickedSystem)
    {
        return true;
    }
    const float scale = std::max(std::min(a.distance, b.distance), 1.0e-3f);
    return glm::length(a.focus - b.focus) > scale * 1.0e-3f || std::abs(a.distance - b.distance) > scale * 1.0e-3f ||
           std::abs(std::remainder(a.yaw - b.yaw, kTau)) > 1.0e-3f || std::abs(a.pitch - b.pitch) > 1.0e-3f;
}

} // namespace

void PredationGame::ApplyMapShared(const MapViewMessage& view)
{
    // To the scale and the place it is at, at once (under a fade), then eased to where it is looked at from.
    const MapLevel level = view.level == 0 ? MapLevel::Galaxy : view.level == 1 ? MapLevel::System : MapLevel::Body;
    m_mapTransition.active = false;
    const bool rescaled = level != m_mapLevel || view.system != m_mapSystem || (level == MapLevel::Body && view.selected != m_mapSelected);
    if (level == MapLevel::Galaxy && m_mapLevel != MapLevel::Galaxy)
    {
        ApplyMapGalaxy(view.focus, view.distance);
    }
    else if (level == MapLevel::System && (m_mapLevel != MapLevel::System || view.system != m_mapSystem))
    {
        ApplyMapSystem(view.system, view.selected);
    }
    else if (level == MapLevel::Body && rescaled)
    {
        m_mapSystem = view.system;
        ApplyMapBody(view.selected);
    }
    if (rescaled && m_mapOpen)
    {
        m_mapFadeIn = 1.0f;
    }
    m_mapSystem = view.system;
    m_mapSelected = view.selected;
    m_mapRegion = view.region;
    m_mapHasPickedSystem = view.hasPickedSystem;
    m_mapPickedSystem = view.pickedSystem;
    m_mapView.SetWanted({view.focus, view.distance, view.yaw, view.pitch});
    m_mapFramed = true;
    m_mapMovedBy = view.driver;
    m_mapMovedFor = 2.0f;
    m_mapShared = MapSnapshot();
    m_mapSharedSet = true;
}

void PredationGame::UpdateSharedMap(float dt)
{
    m_mapMovedFor = std::max(m_mapMovedFor - dt, 0.0f);
    if (!m_campaignOpen || m_sessionMode == SessionMode::Offline)
    {
        return;
    }
    // Moved by somebody else: applied, the newest of each player's only -- and on the host, passed on to everybody.
    const std::vector<MapViewMessage> moves = m_sessionMode == SessionMode::Host ? m_host.TakeMapViews() : m_client.TakeMapViews();
    for (const MapViewMessage& move : moves)
    {
        if (move.driver == LocalPlayerId() || move.driver >= kMaxPlayers)
        {
            continue;
        }
        int& seen = m_mapSerialSeen[move.driver];
        if (seen >= 0 && static_cast<int16_t>(move.serial - static_cast<uint16_t>(seen)) <= 0)
        {
            continue;
        }
        seen = move.serial;
        ApplyMapShared(move);
        if (m_sessionMode == SessionMode::Host)
        {
            m_host.SendMapView(move);
        }
    }
    // Moved here: sent, no more than ten times a second, the last of a run of moves always.
    if (m_mapOpen && !m_mapTransition.active)
    {
        const MapViewMessage now = MapSnapshot();
        if (!m_mapSharedSet || MapMoved(now, m_mapShared))
        {
            m_mapShared = now;
            m_mapSharedSet = true;
            m_mapSendPending = true;
        }
    }
    m_mapSendIn -= dt;
    if (m_mapSendPending && m_mapSendIn <= 0.0f)
    {
        m_mapSendIn = 0.1f;
        m_mapSendPending = false;
        MapViewMessage move = m_mapShared;
        move.driver = LocalPlayerId();
        move.serial = ++m_mapSerial;
        if (m_sessionMode == SessionMode::Host)
        {
            m_host.SendMapView(move);
        }
        else
        {
            m_client.SendMapView(move);
        }
    }
}

// --- The three scales ---------------------------------------------------------------------------------------------

glm::vec3 PredationGame::ShipOverGlobe(const Body& body)
{
    if (ShipLanded() && m_campaign.travel.region >= 0 && m_campaign.travel.region < static_cast<int>(body.regions.size()))
    {
        return AreaOnGlobe(body.regions[static_cast<size_t>(m_campaign.travel.region)]) * 1.01f;
    }
    // In orbit: once round every so often, on a tilted ring a third of a radius up.
    const float angle = static_cast<float>(std::fmod(m_campaign.clock / 90.0, 1.0)) * kTau;
    const glm::vec3 flat{std::cos(angle), 0.0f, std::sin(angle)};
    return glm::vec3(glm::rotate(glm::mat4(1.0f), 0.45f, glm::vec3(1.0f, 0.0f, 0.3f)) * glm::vec4(flat, 0.0f)) * 1.35f;
}

void PredationGame::ShowMapGalaxy(const glm::vec3& focus, float distance)
{
    QueueMap(MapLevel::Galaxy, 0, -1, focus, distance, m_mapView.FocusPoint(), false);
}

void PredationGame::ShowMapSystem(uint64_t system, int body)
{
    // From the galaxy, into the star; from a body, back out of it.
    const bool fromGalaxy = m_mapLevel == MapLevel::Galaxy;
    const glm::vec3 dive = fromGalaxy ? m_universe.Glance(SystemId::Unpack(system)).position : m_mapView.FocusPoint();
    QueueMap(MapLevel::System, system, body, glm::vec3(0.0f), 0.0f, dive, fromGalaxy);
}

void PredationGame::ShowMapBody(int body)
{
    glm::vec3 dive = m_mapView.FocusPoint();
    if (const StarSystem* shown = m_universe.System(m_mapSystem); shown != nullptr && shown->Find(body) != nullptr)
    {
        dive = SystemMapView::Layout(*shown, m_campaign.clock)[static_cast<size_t>(body)].at;
    }
    QueueMap(MapLevel::Body, m_mapSystem, body, glm::vec3(0.0f), 0.0f, dive, true);
}

void PredationGame::QueueMap(MapLevel level, uint64_t system, int body, const glm::vec3& focus, float distance, const glm::vec3& dive,
                             bool inward)
{
    // Not open yet, or nothing on the screen to move: at once.
    if (!m_mapOpen || !bgfx::isValid(m_mapTexture))
    {
        if (level == MapLevel::Galaxy)
        {
            ApplyMapGalaxy(focus, distance);
        }
        else if (level == MapLevel::System)
        {
            ApplyMapSystem(system, body);
        }
        else
        {
            ApplyMapBody(body);
        }
        return;
    }
    m_mapTransition.active = true;
    m_mapTransition.time = 0.0f;
    m_mapTransition.to = level;
    m_mapTransition.system = system;
    m_mapTransition.body = body;
    m_mapTransition.focus = focus;
    m_mapTransition.distance = distance;
    m_mapTransition.dive = dive;
    m_mapTransition.inward = inward;
    m_mapDragButton = -1;
    // Free to go as close or as far as the dive takes it; the next scale sets its own limits.
    m_mapView.SetLimits(0.0005f, 1.0e7f);
    PlayNamed("UI/click", m_renderEye, 0.3f, inward ? 0.85f : 0.7f, false);
}

void PredationGame::ApplyMapGalaxy(const glm::vec3& focus, float distance)
{
    const bool fromSystem = m_mapLevel != MapLevel::Galaxy;
    m_mapLevel = MapLevel::Galaxy;
    m_mapHovered = -1;
    m_mapHoverRegion = -1;
    m_mapHasHoverSystem = false;
    m_mapView.SetLimits(kGalaxyNearest, kGalaxyFurthest);
    // Out of a system: from right over it, eased back, so it reads as having pulled out of it.
    m_mapView.Jump(focus, fromSystem ? kGalaxyNearest : distance);
    m_mapView.Focus(focus, distance);
    if (fromSystem)
    {
        m_mapView.FaceFrom(glm::vec3(0.35f, 1.1f, 0.6f));
    }
}

void PredationGame::ApplyMapSystem(uint64_t system, int body)
{
    const StarSystem* shown = m_universe.System(system);
    if (shown == nullptr)
    {
        return;
    }
    const MapLevel from = m_mapLevel;
    m_mapLevel = MapLevel::System;
    if (system != m_mapSystem)
    {
        m_mapSelected = -1;
    }
    m_mapSystem = system;
    m_mapPickedSystem = system;
    m_mapHasPickedSystem = true;
    m_mapHoverRegion = -1;
    const std::vector<SystemMapView::Drawn> drawn = SystemMapView::Layout(*shown, m_campaign.clock);
    float outermost = 10.0f;
    for (const SystemMapView::Drawn& at : drawn)
    {
        outermost = std::max(outermost, glm::length(at.at));
    }
    m_mapView.SetLimits(kSystemNearest, outermost * 3.0f);
    if (const Body* picked = shown->Find(body))
    {
        m_mapSelected = body;
        const SystemMapView::Drawn& at = drawn[picked->index];
        // Back out of a body: from close to it; otherwise from a little further than it is framed.
        m_mapView.Jump(at.at, from == MapLevel::Body ? at.radius * 4.0f : at.radius * 22.0f);
        m_mapView.Focus(at.at, at.radius * 12.0f);
    }
    else
    {
        // Into a system from the galaxy: falling in towards it.
        m_mapView.Jump(glm::vec3(0.0f), from == MapLevel::Galaxy ? outermost * 3.0f : outermost * 2.2f);
        m_mapView.Focus(glm::vec3(0.0f), outermost * 2.0f);
    }
}

void PredationGame::ApplyMapBody(int body)
{
    const StarSystem* shown = m_universe.System(m_mapSystem);
    const Body* picked = shown != nullptr ? shown->Find(body) : nullptr;
    if (picked == nullptr)
    {
        return;
    }
    m_mapLevel = MapLevel::Body;
    m_mapSelected = body;
    m_mapHoverRegion = -1;
    // The place the ship is going down to, if it is this body's; or the first known, so there is something to read.
    m_mapRegion = -1;
    const bool ours = m_mapSystem == m_campaign.system &&
                      (m_campaign.travel.underway ? m_campaign.travel.target == body : m_campaign.body == body);
    if (ours && m_campaign.travel.region >= 0 && MapAreaKnown(m_mapSystem, *picked, m_campaign.travel.region))
    {
        m_mapRegion = m_campaign.travel.region;
    }
    for (int i = 0; i < static_cast<int>(picked->regions.size()) && m_mapRegion < 0; ++i)
    {
        m_mapRegion = MapAreaKnown(m_mapSystem, *picked, i) ? i : -1;
    }
    m_mapView.SetLimits(kBodyNearest, kBodyFurthest);
    m_mapView.Jump(glm::vec3(0.0f), kBodyFurthest);
    m_mapView.Focus(glm::vec3(0.0f), 3.0f);
    // Looking at the place picked out, or at the day side.
    const glm::vec3 sun = SunOverBody(*shown, *picked);
    const glm::vec3 face = m_mapRegion >= 0 ? Globe(picked->regions[static_cast<size_t>(m_mapRegion)].latLon)
                                            : glm::vec3(glm::rotate(glm::mat4(1.0f), 0.75f, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::vec4(sun, 0.0f));
    m_mapView.FaceFrom(face + glm::vec3(0.0f, 0.3f, 0.0f));
}

glm::vec3 PredationGame::AreaOnGlobe(const LandingRegion& region)
{
    return Globe(region.latLon);
}

glm::vec3 PredationGame::SunOverBody(const StarSystem& system, const Body& body)
{
    // The star's direction from the body, in the body's own frame (StarSystem::SunOver). So the globe stays still on the
    // map, and the day goes round it as it does.
    return glm::normalize(system.SunOver(body.index, m_campaign.clock));
}

bool PredationGame::AreaInDaylight(const StarSystem& system, const Body& body, const LandingRegion& region)
{
    return glm::dot(AreaOnGlobe(region), SunOverBody(system, body)) > 0.0f;
}

bool PredationGame::MapAreaKnown(uint64_t system, const Body& body, int region) const
{
    return region >= 0 && region < static_cast<int>(body.regions.size()) &&
           m_campaign.RegionFound(system, body.index, region, body.regions[static_cast<size_t>(region)].charted);
}

bool PredationGame::SystemCharted(uint64_t system)
{
    // Where the ship has been, and where it is: the charts reach so far round each.
    if (m_mapChartKnown != m_campaign.known.size() || m_mapChartCentres.empty())
    {
        m_mapChartKnown = m_campaign.known.size();
        m_mapChartCentres.clear();
        for (const auto& [key, bits] : m_campaign.known)
        {
            const size_t colon = key.find(':');
            if (colon != std::string::npos && key.compare(colon, std::string::npos, ":-1") == 0 && (bits & CampaignState::kKnownVisited) != 0)
            {
                m_mapChartCentres.push_back(m_universe.SystemPosition(SystemId::Unpack(std::stoull(key.substr(0, colon)))));
            }
        }
        m_mapChartCentres.push_back(m_universe.SystemPosition(SystemId::Unpack(m_campaign.system)));
    }
    const glm::vec3 at = m_universe.Glance(SystemId::Unpack(system)).position;
    const float reach = Travel::ChartRange(SensorTier());
    for (const glm::vec3& centre : m_mapChartCentres)
    {
        if (glm::length(at - centre) <= reach)
        {
            return true;
        }
    }
    return system == m_campaign.system || (m_campaign.travel.interstellar && system == m_campaign.travel.toSystem);
}

void PredationGame::AskSystemCourse(uint64_t system)
{
    AskCampaign(CampaignAction::PlotSystem, static_cast<int>(static_cast<uint32_t>(system & 0xFFFFFFFFu)),
                static_cast<int>(static_cast<uint32_t>(system >> 32)));
    PlayNamed("UI/confirm", m_renderEye, 0.6f, 1.0f, false);
}

std::string PredationGame::ShipStatus()
{
    const StarSystem* system = CurrentSystem();
    if (system == nullptr)
    {
        return "";
    }
    if (m_campaign.travel.interstellar)
    {
        const SystemGlance& to = m_universe.Glance(SystemId::Unpack(m_campaign.travel.toSystem));
        const float done = Travel::CrossingDone(m_campaign);
        const float left = (1.0f - done) * m_campaign.travel.duration;
        return "Crossing to " + to.name + "   " + std::to_string(static_cast<int>(done * 100.0f)) + "%, " + About(left) + " left";
    }
    const Body* target = system->Find(m_campaign.travel.target);
    const Body* at = system->Find(m_campaign.body);
    if (m_campaign.travel.underway && target != nullptr)
    {
        const float left = glm::length(system->Position(target->index, m_campaign.clock) - glm::vec3(m_campaign.travel.position));
        return "Under way to " + target->name + ", " + About(Travel::Seconds(left, DriveTier()));
    }
    if (m_campaign.travel.underway)
    {
        return "Coming to a stop";
    }
    if (at != nullptr && ShipLanded())
    {
        const int region = m_campaign.travel.region;
        return "Landed on " + at->name +
               (region >= 0 && region < static_cast<int>(at->regions.size()) ? ", " + at->regions[static_cast<size_t>(region)].designation : std::string());
    }
    if (at != nullptr)
    {
        const int region = m_campaign.travel.region;
        if (region >= 0 && region < static_cast<int>(at->regions.size()))
        {
            return "In orbit of " + at->name + "   Going down to " + at->regions[static_cast<size_t>(region)].designation;
        }
        return "In orbit of " + at->name;
    }
    return "Holding position in " + system->name;
}

// --- The picture ----------------------------------------------------------------------------------------------------

void PredationGame::RenderSystemMap()
{
    if (!m_mapOpen || !m_campaignOpen)
    {
        return;
    }
    // The camera moved here, before the picture is taken: what is drawn over it is drawn from the same camera after.
    m_mapView.Update(ImGui::GetIO().DeltaTime);
    if (!m_planets.IsValid())
    {
        return;
    }
    const Renderer& renderer = m_app->GetRenderer();
    const auto width = static_cast<uint16_t>(std::max(renderer.Width(), 16));
    const auto height = static_cast<uint16_t>(std::max(renderer.Height(), 16));
    if (width != m_mapWidth || height != m_mapHeight)
    {
        DestroySystemMapTarget();
    }
    if (!bgfx::isValid(m_mapBuffer))
    {
        const uint64_t flags = BGFX_TEXTURE_RT_MSAA_X4 | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::TextureHandle attachments[2] = {
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8, flags),
            bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT_MSAA_X4 | BGFX_TEXTURE_RT_WRITE_ONLY)};
        if (!bgfx::isValid(attachments[0]) || !bgfx::isValid(attachments[1]))
        {
            PRED_LOG_ERROR(Render, "System map: could not create its render target");
            return;
        }
        m_mapBuffer = bgfx::createFrameBuffer(2, attachments, true);
        m_mapTexture = attachments[0];
        m_mapWidth = width;
        m_mapHeight = height;
    }

    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const bool homogeneous = renderer.HomogeneousDepth();
    const glm::mat4 view = m_mapView.View();
    const glm::mat4 projection = m_mapView.Projection(aspect, homogeneous);
    const bgfx::ViewId sky = Renderer::kViewMapFirst;
    const bgfx::ViewId bodies = static_cast<bgfx::ViewId>(sky + 1);
    const bgfx::ViewId glow = static_cast<bgfx::ViewId>(sky + 2);
    const bgfx::ViewId lines = static_cast<bgfx::ViewId>(sky + 3);
    for (bgfx::ViewId id = sky; id <= lines; ++id)
    {
        bgfx::setViewFrameBuffer(id, m_mapBuffer);
        bgfx::setViewRect(id, 0, 0, width, height);
        bgfx::setViewTransform(id, glm::value_ptr(view), glm::value_ptr(projection));
        bgfx::setViewClear(id, id == sky ? (BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH) : BGFX_CLEAR_NONE, 0x000000ff, 1.0f, 0);
        bgfx::touch(id);
    }
    m_planets.SetCamera(m_mapView.Eye(), 1.0f);
    m_planets.SetOutput(false, BGFX_STATE_DEPTH_TEST_LESS);
    const StarSystem* shown = m_mapLevel == MapLevel::Galaxy ? nullptr : m_universe.System(m_mapSystem);
    if (m_mapLevel == MapLevel::Galaxy || shown == nullptr)
    {
        RenderMapGalaxy(sky, lines, view, projection);
    }
    else if (m_mapLevel == MapLevel::System)
    {
        RenderMapSystem(*shown, sky, bodies, glow, lines, view, projection);
    }
    else
    {
        RenderMapBody(*shown, sky, bodies, glow, view, projection);
    }
    m_planets.FlushLines(lines);
}

void PredationGame::RenderMapGalaxy(bgfx::ViewId sky, bgfx::ViewId lines, const glm::mat4& view, const glm::mat4& projection)
{
    (void)lines;
    // The far stars behind, faint: the galaxy's own are drawn over them.
    Environment space;
    space.stars = 0.55f;
    space.planetRadius = 0.0f;
    space.sunDirection = glm::vec3(0.0f, -1.0f, 0.0f);
    space.sunColor = glm::vec3(1.0f);
    space.fogColor = glm::vec3(0.0f);
    space.skySun = 0.0f;
    m_app->GetSkyRenderer().Draw(sky, space, view, projection);

    const glm::vec3 focus = m_mapView.FocusPoint();
    const float reach = std::clamp(m_mapView.Distance() * 1.3f, 30.0f, 180.0f);

    // The disc's plane, ruled in the galaxy's own cells close in and in fives of them further out, fading away from
    // the middle of the view: what makes sliding about feel like going somewhere.
    const float spacing = m_mapView.Distance() < 70.0f ? Universe::kCellSize : Universe::kCellSize * 5.0f;
    const int count = static_cast<int>(std::ceil(reach / spacing));
    const float baseX = std::round(focus.x / spacing) * spacing;
    const float baseZ = std::round(focus.z / spacing) * spacing;
    const auto fade = [&](const glm::vec3& at)
    {
        const float t = 1.0f - glm::length(glm::vec2(at.x - focus.x, at.z - focus.z)) / reach;
        return t <= 0.0f ? 0.0f : t * t;
    };
    for (int i = -count; i <= count; ++i)
    {
        for (int j = -count; j < count; ++j)
        {
            const float line = static_cast<float>(i) * spacing;
            const float a0 = static_cast<float>(j) * spacing;
            const float a1 = a0 + spacing;
            const glm::vec3 p0{baseX + line, 0.0f, baseZ + a0};
            const glm::vec3 p1{baseX + line, 0.0f, baseZ + a1};
            const glm::vec3 q0{baseX + a0, 0.0f, baseZ + line};
            const glm::vec3 q1{baseX + a1, 0.0f, baseZ + line};
            const float fp = fade((p0 + p1) * 0.5f);
            const float fq = fade((q0 + q1) * 0.5f);
            if (fp > 0.01f)
            {
                m_planets.Line(p0, p1, AbgrOf({0.45f, 0.55f, 0.7f}, 0.16f * fp));
            }
            if (fq > 0.01f)
            {
                m_planets.Line(q0, q1, AbgrOf({0.45f, 0.55f, 0.7f}, 0.16f * fq));
            }
        }
    }

    // Each star's line down to the plane, so how high or low it is reads.
    for (const SystemId& id : m_mapNearSystems)
    {
        const SystemGlance& glance = m_universe.Glance(id);
        const float f = fade(glance.position);
        if (f > 0.3f && std::abs(glance.position.y) > 0.2f)
        {
            m_planets.Line(glance.position, {glance.position.x, 0.0f, glance.position.z}, AbgrOf(glance.starColor, 0.14f * f * f));
        }
    }

    // Rings: how far one crossing can go from the ship, bright; how far the charts reach round each place been to, faint.
    const glm::vec3 ship = Travel::GalaxyPosition(m_campaign, m_universe);
    const auto ring = [&](const glm::vec3& centre, float radius, uint32_t colour, bool dashed)
    {
        constexpr int kSegments = 144;
        for (int i = 0; i < kSegments; i += dashed ? 2 : 1)
        {
            const float a0 = kTau * static_cast<float>(i) / kSegments;
            const float a1 = kTau * static_cast<float>(i + 1) / kSegments;
            m_planets.Line({centre.x + std::cos(a0) * radius, 0.0f, centre.z + std::sin(a0) * radius},
                           {centre.x + std::cos(a1) * radius, 0.0f, centre.z + std::sin(a1) * radius}, colour);
        }
    };
    if (const float reach = Travel::CrossingRange(DriveTier()); reach > 0.0f)
    {
        ring(ship, reach, Abgr(236, 156, 64, 110), false);
    }
    for (const glm::vec3& centre : m_mapChartCentres)
    {
        ring(centre, Travel::ChartRange(SensorTier()), Abgr(120, 170, 220, 45), true);
    }

    // The crossing under way: done solid, still to go dashed. And a line to whatever is picked out.
    const auto dashed = [&](const glm::vec3& a, const glm::vec3& b, uint32_t colour)
    {
        const int dashes = std::clamp(static_cast<int>(glm::length(b - a) / 1.2f), 4, 200) & ~1;
        for (int i = 0; i < dashes; i += 2)
        {
            m_planets.Line(glm::mix(a, b, static_cast<float>(i) / static_cast<float>(dashes)),
                           glm::mix(a, b, static_cast<float>(i + 1) / static_cast<float>(dashes)), colour);
        }
    };
    if (m_campaign.travel.interstellar)
    {
        m_planets.Line(m_campaign.travel.fromGalaxy, ship, Abgr(236, 156, 64, 120));
        dashed(ship, m_campaign.travel.toGalaxy, Abgr(236, 156, 64, 230));
    }
    if (m_mapHasPickedSystem && m_mapPickedSystem != m_campaign.system &&
        !(m_campaign.travel.interstellar && m_mapPickedSystem == m_campaign.travel.toSystem))
    {
        dashed(ship, m_universe.Glance(SystemId::Unpack(m_mapPickedSystem)).position, Abgr(220, 226, 230, 90));
    }
}

void PredationGame::RenderMapSystem(const StarSystem& system, bgfx::ViewId sky, bgfx::ViewId bodies, bgfx::ViewId glow, bgfx::ViewId lines,
                                    const glm::mat4& view, const glm::mat4& projection)
{
    (void)lines;
    const glm::vec3 eye = m_mapView.Eye();
    Environment space;
    space.stars = 1.0f;
    space.planetRadius = 0.0f;
    space.sunDirection = glm::length(eye) > 1.0e-3f ? glm::normalize(eye) : glm::vec3(0.0f, -1.0f, 0.0f);
    space.sunColor = system.starColor;
    space.fogColor = glm::vec3(0.0f);
    space.skySun = 0.0f;
    m_app->GetSkyRenderer().Draw(sky, space, view, projection);

    const float time = static_cast<float>(std::fmod(m_campaign.clock, 100000.0));
    m_planets.Star(bodies, glow, glm::vec3(0.0f), SystemMapView::kStarRadius * std::clamp(std::sqrt(system.starRadius), 0.7f, 1.6f),
                   system.starColor, m_mapView.Right(), m_mapView.Up(), time);

    // Distance rings, faint, at so many astronomical units: for the scale the drawing does not keep.
    float outermost = 1.0f;
    for (const Body& body : system.bodies)
    {
        outermost = body.kind == BodyKind::Planet ? std::max(outermost, body.orbit) : outermost;
    }
    for (const float au : {1.0f, 5.0f, 20.0f, 50.0f})
    {
        if (au > outermost * 1.4f)
        {
            break;
        }
        const float radius = glm::length(SystemMapView::Place({au, 0.0f, 0.0f}));
        constexpr int kSegments = 160;
        for (int i = 0; i < kSegments; i += 2)
        {
            const float a0 = kTau * static_cast<float>(i) / kSegments;
            const float a1 = kTau * static_cast<float>(i + 1) / kSegments;
            m_planets.Line({std::cos(a0) * radius, 0.0f, std::sin(a0) * radius}, {std::cos(a1) * radius, 0.0f, std::sin(a1) * radius},
                           Abgr(150, 160, 180, 26));
        }
    }

    const std::vector<SystemMapView::Drawn> drawn = SystemMapView::Layout(system, m_campaign.clock);
    const bool ours = system.id.Packed() == m_campaign.system && !m_campaign.travel.interstellar;
    for (const Body& body : system.bodies)
    {
        const SystemMapView::Drawn& at = drawn[body.index];
        // Turned on its axis as the day goes round, the axis tipped by its tilt.
        glm::mat4 model = glm::translate(glm::mat4(1.0f), at.at);
        model = glm::rotate(model, body.tilt, glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::mat4 ringModel = glm::scale(model, glm::vec3(at.radius));
        float spin = 0.0f;
        system.SunOver(body.index, m_campaign.clock, &spin);
        model = glm::rotate(model, spin, glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(at.radius));
        const float highlight = body.index == m_mapSelected ? 1.0f : body.index == m_mapHovered ? 0.55f : 0.0f;
        const glm::vec3 towardsStar = glm::length(at.at) > 1.0e-4f ? -glm::normalize(at.at) : glm::vec3(0.0f, 1.0f, 0.0f);
        const PlanetLook look = LookOf(body);
        m_planets.Body(bodies, model, look, towardsStar, system.starColor, highlight, time * 0.02f);
        m_planets.Rings(glow, ringModel, look, towardsStar, system.starColor);

        // Its orbit: faint, brighter for the one picked out.
        const bool picked = body.index == m_mapSelected;
        const std::vector<glm::vec3> loop = SystemMapView::Orbit(system, body.index, drawn, m_campaign.clock, body.kind == BodyKind::Planet ? 180 : 48);
        const uint32_t colour = picked ? Abgr(236, 156, 64, 170) : body.kind == BodyKind::Planet ? Abgr(120, 140, 170, 70) : Abgr(120, 140, 170, 35);
        for (size_t i = 1; i < loop.size(); ++i)
        {
            m_planets.Line(loop[i - 1], loop[i], colour);
        }
    }

    // The ship's course, if it is in this system: the way it will actually go, round the star rather than through it.
    if (ours && m_campaign.travel.underway && m_campaign.travel.target >= 0)
    {
        const std::vector<glm::vec3> path = Travel::Preview(m_campaign, system, DriveTier(), 48);
        for (size_t i = 1; i < path.size(); i += 2)
        {
            m_planets.Line(SystemMapView::Place(path[i - 1]), SystemMapView::Place(path[i]), Abgr(236, 156, 64, 220));
        }
        if (!path.empty())
        {
            m_planets.Line(SystemMapView::Place(path.back()), drawn[static_cast<size_t>(m_campaign.travel.target)].at, Abgr(236, 156, 64, 220));
        }
    }
}

void PredationGame::RenderMapBody(const StarSystem& system, bgfx::ViewId sky, bgfx::ViewId bodies, bgfx::ViewId glow, const glm::mat4& view,
                                  const glm::mat4& projection)
{
    const Body* body = system.Find(m_mapSelected);
    if (body == nullptr)
    {
        return;
    }
    const glm::vec3 sun = SunOverBody(system, *body);
    Environment space;
    space.stars = 1.0f;
    space.planetRadius = 0.0f;
    space.sunDirection = -sun;
    space.sunColor = system.starColor;
    space.fogColor = glm::vec3(0.0f);
    space.skySun = 1.0f;
    m_app->GetSkyRenderer().Draw(sky, space, view, projection);

    // The globe in its own frame, still; the sun goes round it.
    const PlanetLook look = LookOf(*body);
    const float time = static_cast<float>(std::fmod(m_campaign.clock, 100000.0));
    m_planets.Body(bodies, glm::mat4(1.0f), look, sun, system.starColor, 0.0f, time * 0.02f);
    m_planets.Rings(glow, glm::mat4(1.0f), look, sun, system.starColor);

    // Lines of latitude and longitude, faint; the line between day and night, warmer.
    constexpr int kSegments = 120;
    constexpr float kLift = 1.006f;
    for (int lat = -60; lat <= 60; lat += 30)
    {
        for (int i = 0; i < kSegments; ++i)
        {
            const glm::vec3 a = Globe({static_cast<float>(lat), 360.0f * static_cast<float>(i) / kSegments}) * kLift;
            const glm::vec3 b = Globe({static_cast<float>(lat), 360.0f * static_cast<float>(i + 1) / kSegments}) * kLift;
            m_planets.Line(a, b, Abgr(200, 215, 235, lat == 0 ? 60 : 36));
        }
    }
    for (int lon = 0; lon < 360; lon += 30)
    {
        for (int i = 0; i < kSegments / 2; ++i)
        {
            const glm::vec3 a = Globe({-90.0f + 180.0f * static_cast<float>(i) / (kSegments / 2), static_cast<float>(lon)}) * kLift;
            const glm::vec3 b = Globe({-90.0f + 180.0f * static_cast<float>(i + 1) / (kSegments / 2), static_cast<float>(lon)}) * kLift;
            m_planets.Line(a, b, Abgr(200, 215, 235, 32));
        }
    }
    const glm::vec3 side = glm::normalize(std::abs(sun.y) < 0.9f ? glm::cross(sun, glm::vec3(0.0f, 1.0f, 0.0f)) : glm::cross(sun, glm::vec3(1.0f, 0.0f, 0.0f)));
    const glm::vec3 other = glm::cross(sun, side);
    for (int i = 0; i < kSegments; ++i)
    {
        const float a0 = kTau * static_cast<float>(i) / kSegments;
        const float a1 = kTau * static_cast<float>(i + 1) / kSegments;
        m_planets.Line((side * std::cos(a0) + other * std::sin(a0)) * 1.009f, (side * std::cos(a1) + other * std::sin(a1)) * 1.009f, Abgr(236, 156, 64, 70));
    }
    // The ship's orbit, if it is going round this body.
    const bool orbitingHere = system.id.Packed() == m_campaign.system && !m_campaign.travel.underway && !m_campaign.travel.interstellar &&
                              m_campaign.body == body->index && !ShipLanded();
    if (orbitingHere)
    {
        const glm::mat4 tilt = glm::rotate(glm::mat4(1.0f), 0.45f, glm::vec3(1.0f, 0.0f, 0.3f));
        for (int i = 0; i < kSegments; i += 2)
        {
            const float a0 = kTau * static_cast<float>(i) / kSegments;
            const float a1 = kTau * static_cast<float>(i + 1) / kSegments;
            m_planets.Line(glm::vec3(tilt * glm::vec4(std::cos(a0), 0.0f, std::sin(a0), 0.0f)) * 1.35f,
                           glm::vec3(tilt * glm::vec4(std::cos(a1), 0.0f, std::sin(a1), 0.0f)) * 1.35f, Abgr(255, 236, 200, 90));
        }
    }
}

// --- What is over the picture, and what the mouse and keys do -------------------------------------------------------

void PredationGame::DrawSystemMap()
{
    if (!m_mapOpen || CurrentSystem() == nullptr)
    {
        return;
    }
    const StarSystem* shown = m_mapLevel == MapLevel::Galaxy ? nullptr : m_universe.System(m_mapSystem);
    if (m_mapLevel != MapLevel::Galaxy && shown == nullptr)
    {
        ShowMapGalaxy(Travel::GalaxyPosition(m_campaign, m_universe), 60.0f);
        return;
    }
    if (m_mapLevel == MapLevel::Body && shown->Find(m_mapSelected) == nullptr)
    {
        ShowMapSystem(m_mapSystem, -1);
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->Pos;
    const ImVec2 size = viewport->Size;
    const float aspect = size.x / std::max(size.y, 1.0f);
    const bool homogeneous = m_app->GetRenderer().HomogeneousDepth();
    ImGuiIO& io = ImGui::GetIO();
    // The camera as the picture was taken with it this frame: everything drawn over the picture goes by this, and what
    // the mouse and keys do changes the camera for the next.
    const SystemMapView shot = m_mapView;
    // Part way to another scale: diving in (or pulling out), then there, then easing in from the fade.
    if (m_mapTransition.active)
    {
        m_mapTransition.time += io.DeltaTime;
        const float factor = m_mapTransition.inward ? std::exp(-io.DeltaTime * 9.0f) : std::exp(io.DeltaTime * 4.0f);
        m_mapView.Focus(m_mapTransition.dive, m_mapView.Distance() * factor);
        if (m_mapTransition.time >= 0.28f)
        {
            m_mapTransition.active = false;
            m_mapFadeIn = 1.0f;
            if (m_mapTransition.to == MapLevel::Galaxy)
            {
                ApplyMapGalaxy(m_mapTransition.focus, m_mapTransition.distance);
            }
            else if (m_mapTransition.to == MapLevel::System)
            {
                ApplyMapSystem(m_mapTransition.system, m_mapTransition.body);
            }
            else
            {
                ApplyMapBody(m_mapTransition.body);
            }
            // Drawn on at once, under the fade, so no frame goes by without the map.
            shown = m_mapLevel == MapLevel::Galaxy ? nullptr : m_universe.System(m_mapSystem);
            if (m_mapLevel != MapLevel::Galaxy && shown == nullptr)
            {
                return;
            }
        }
    }
    m_mapFadeIn = std::max(m_mapFadeIn - io.DeltaTime / 0.4f, 0.0f);
    const bool interactive = !m_mapTransition.active;
    const std::vector<SystemMapView::Drawn> drawn = shown != nullptr ? SystemMapView::Layout(*shown, m_campaign.clock) : std::vector<SystemMapView::Drawn>{};
    const bool ours = shown != nullptr && m_mapSystem == m_campaign.system && !m_campaign.travel.interstellar;

    // The galaxy's systems near what the camera looks at: worked out again only when it has gone some way.
    if (m_mapLevel == MapLevel::Galaxy)
    {
        const float reach = std::clamp(m_mapView.Distance() * 1.3f, 30.0f, 180.0f);
        if (glm::length(m_mapView.FocusPoint() - m_mapNearFrom) > m_mapNearRadius * 0.15f || reach > m_mapNearRadius * 0.95f || reach < m_mapNearRadius * 0.45f)
        {
            m_mapNearRadius = reach * 1.2f;
            m_mapNearFrom = m_mapView.FocusPoint();
            m_mapNearSystems.clear();
            for (const SystemId& id : m_universe.Near(m_mapNearFrom, m_mapNearRadius))
            {
                if (SystemCharted(id.Packed()))
                {
                    m_mapNearSystems.push_back(id);
                }
            }
        }
    }

    MapStyle style;
    // The picture, and the whole of it a surface to drag, zoom and point at.
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(size);
    constexpr ImGuiWindowFlags kBack = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##systemmap", nullptr, kBack);
    ImGui::PopStyleVar(2);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (bgfx::isValid(m_mapTexture))
    {
        draw->AddImage(static_cast<ImTextureID>(ImGuiLayer::TextureId(m_mapTexture)), origin, {origin.x + size.x, origin.y + size.y});
    }
    else
    {
        draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(0, 0, 0, 255));
    }
    // A soft darkening at the edges, so the panels sit on it.
    draw->AddRectFilledMultiColor(origin, {origin.x + 360.0f, origin.y + size.y}, IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 120));
    draw->AddRectFilledMultiColor({origin.x + size.x - 380.0f, origin.y}, {origin.x + size.x, origin.y + size.y}, IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 0));
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##mapsurface", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered() && interactive;
    const glm::vec2 mouse{(io.MousePos.x - origin.x) / size.x, (io.MousePos.y - origin.y) / size.y};
    const auto toScreen = [&](const glm::vec3& at, ImVec2& out)
    {
        glm::vec2 point;
        if (!shot.OnScreen(at, aspect, homogeneous, point) || point.x < -0.1f || point.x > 1.1f || point.y < -0.1f || point.y > 1.1f)
        {
            return false;
        }
        out = {origin.x + point.x * size.x, origin.y + point.y * size.y};
        return true;
    };
    // Where the mouse points on the flat of the map: for zooming towards it.
    const auto onPlane = [&](glm::vec3& out)
    {
        const glm::mat4 inverse = glm::inverse(shot.Projection(aspect, homogeneous) * shot.View());
        const glm::vec2 ndc{mouse.x * 2.0f - 1.0f, 1.0f - mouse.y * 2.0f};
        const glm::vec4 nearPoint = inverse * glm::vec4(ndc, homogeneous ? -1.0f : 0.0f, 1.0f);
        const glm::vec4 farPoint = inverse * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec3 a = glm::vec3(nearPoint) / nearPoint.w;
        const glm::vec3 b = glm::vec3(farPoint) / farPoint.w;
        const glm::vec3 way = b - a;
        if (std::abs(way.y) < 1.0e-6f)
        {
            return false;
        }
        const float t = -a.y / way.y;
        if (t <= 0.0f)
        {
            return false;
        }
        out = a + way * t;
        return true;
    };

    // --- What is under the mouse ---
    m_mapHovered = -1;
    m_mapHasHoverSystem = false;
    m_mapHoverRegion = -1;
    const Body* globe = m_mapLevel == MapLevel::Body ? shown->Find(m_mapSelected) : nullptr;
    // Through a press too (it is a click until it has moved): the release picks what was under the pointer.
    if (hovered && (m_mapDragButton < 0 || m_mapDragged <= kClickSlop))
    {
        if (m_mapLevel == MapLevel::Galaxy)
        {
            float best = 16.0f;
            for (const SystemId& id : m_mapNearSystems)
            {
                ImVec2 point;
                if (!toScreen(m_universe.Glance(id).position, point))
                {
                    continue;
                }
                const float apart = std::hypot(point.x - io.MousePos.x, point.y - io.MousePos.y);
                if (apart < best)
                {
                    best = apart;
                    m_mapHoverSystem = id.Packed();
                    m_mapHasHoverSystem = true;
                }
            }
        }
        else if (m_mapLevel == MapLevel::System)
        {
            m_mapHovered = shot.Pick(mouse, drawn, aspect, homogeneous);
        }
        else if (globe != nullptr)
        {
            const glm::vec3 eye = glm::normalize(shot.Eye());
            float best = 16.0f;
            for (int i = 0; i < static_cast<int>(globe->regions.size()); ++i)
            {
                const glm::vec3 at = AreaOnGlobe(globe->regions[static_cast<size_t>(i)]);
                ImVec2 point;
                if (!MapAreaKnown(m_mapSystem, *globe, i) || glm::dot(at, eye) < 0.12f || !toScreen(at, point))
                {
                    continue;
                }
                const float apart = std::hypot(point.x - io.MousePos.x, point.y - io.MousePos.y);
                if (apart < best)
                {
                    best = apart;
                    m_mapHoverRegion = i;
                }
            }
        }
    }

    // --- The mouse: drag, click, double-click, wheel ---
    bool clicked = false;
    if (m_mapDragButton < 0 && hovered)
    {
        for (const int button : {ImGuiMouseButton_Left, ImGuiMouseButton_Right, ImGuiMouseButton_Middle})
        {
            if (ImGui::IsMouseClicked(button))
            {
                m_mapDragButton = button;
                m_mapDragged = 0.0f;
                break;
            }
        }
    }
    if (m_mapDragButton >= 0)
    {
        if (ImGui::IsMouseDown(m_mapDragButton))
        {
            m_mapDragged += std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y);
            if (m_mapDragged > kClickSlop)
            {
                // Right-drag slides the map along under the mouse; the others turn it. A body has nothing to slide.
                const bool slide = m_mapDragButton == ImGuiMouseButton_Right && !io.KeyShift && !io.KeyAlt && m_mapLevel != MapLevel::Body;
                if (slide)
                {
                    const float pixel = m_mapView.PixelSize(size.y);
                    const float stretch = 1.0f / std::max(std::sin(std::abs(m_mapView.Pitch())), 0.3f);
                    m_mapView.Slide(-io.MouseDelta.x * pixel, io.MouseDelta.y * pixel * stretch);
                }
                else
                {
                    // Left-drag turns gently; the middle button a little quicker.
                    const float rate = m_mapDragButton == ImGuiMouseButton_Left ? 0.0022f : 0.0038f;
                    m_mapView.Turn(-io.MouseDelta.x * rate, io.MouseDelta.y * rate);
                }
            }
        }
        else
        {
            clicked = m_mapDragButton == ImGuiMouseButton_Left && m_mapDragged <= kClickSlop && hovered;
            m_mapDragButton = -1;
        }
    }
    const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (clicked)
    {
        // Picking something out; a click on nothing leaves what was picked as it was.
        if (m_mapLevel == MapLevel::Galaxy && m_mapHasHoverSystem)
        {
            m_mapHasPickedSystem = true;
            m_mapPickedSystem = m_mapHoverSystem;
        }
        else if (m_mapLevel == MapLevel::System && m_mapHovered >= 0)
        {
            m_mapSelected = m_mapHovered;
        }
        else if (m_mapHoverRegion >= 0)
        {
            m_mapRegion = m_mapHoverRegion;
        }
        if (m_mapHasHoverSystem || m_mapHovered >= 0 || m_mapHoverRegion >= 0)
        {
            PlayNamed("UI/click", m_renderEye, 0.5f, 1.0f, false);
        }
    }
    if (doubleClicked)
    {
        if (m_mapLevel == MapLevel::Galaxy && m_mapHasHoverSystem)
        {
            ShowMapSystem(m_mapHoverSystem, -1);
        }
        else if (m_mapLevel == MapLevel::System && m_mapHovered >= 0)
        {
            ShowMapBody(m_mapHovered);
        }
        else if (m_mapLevel == MapLevel::Body && m_mapHoverRegion >= 0)
        {
            m_mapView.FaceFrom(AreaOnGlobe(globe->regions[static_cast<size_t>(m_mapHoverRegion)]) + glm::vec3(0.0f, 0.2f, 0.0f));
        }
        // The level may have changed under us: begin again next frame.
        ImGui::End();
        return;
    }
    if (hovered && io.MouseWheel != 0.0f)
    {
        const float factor = std::pow(0.85f, io.MouseWheel);
        const float before = m_mapView.WantedDistance();
        m_mapView.Zoom(factor);
        const float after = m_mapView.WantedDistance();
        // Going in, towards what the mouse is over, as maps do -- as far as the zoom actually went, and never by more than the
        // view is across (a point near the horizon is a long way off). Going out, straight out: at the limit, nothing moves.
        glm::vec3 towards;
        if (after < before && m_mapLevel != MapLevel::Body && onPlane(towards))
        {
            glm::vec3 offset = towards - m_mapView.FocusPoint();
            const float most = before * 1.2f;
            if (glm::length(offset) > most)
            {
                offset *= most / glm::length(offset);
            }
            m_mapView.Shift(offset * (1.0f - after / before) * 0.9f);
        }
    }

    // --- Keys, while nothing is being typed ---
    if (!io.WantTextInput && interactive)
    {
        const float dt = io.DeltaTime;
        const float speed = m_mapView.Distance() * 0.9f * dt;
        if (m_mapLevel != MapLevel::Body)
        {
            const float right = (ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ? 1.0f : 0.0f) -
                                (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow) ? 1.0f : 0.0f);
            const float away = (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow) ? 1.0f : 0.0f) -
                               (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow) ? 1.0f : 0.0f);
            m_mapView.Slide(right * speed, away * speed);
        }
        else
        {
            const float turn = (ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow) ? 1.0f : 0.0f) -
                               (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow) ? 1.0f : 0.0f);
            const float tip = (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow) ? 1.0f : 0.0f) -
                              (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow) ? 1.0f : 0.0f);
            if (turn != 0.0f || tip != 0.0f)
            {
                m_mapView.Turn(turn * 1.4f * dt, tip * 1.2f * dt);
            }
        }
        const float spin = (ImGui::IsKeyDown(ImGuiKey_E) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(ImGuiKey_Q) ? 1.0f : 0.0f);
        if (spin != 0.0f)
        {
            m_mapView.Turn(spin * 1.4f * dt, 0.0f);
        }
        if (ImGui::IsKeyDown(ImGuiKey_Equal) || ImGui::IsKeyDown(ImGuiKey_KeypadAdd) || ImGui::IsKeyDown(ImGuiKey_PageUp))
        {
            m_mapView.Zoom(std::pow(0.85f, dt * 8.0f));
        }
        if (ImGui::IsKeyDown(ImGuiKey_Minus) || ImGui::IsKeyDown(ImGuiKey_KeypadSubtract) || ImGui::IsKeyDown(ImGuiKey_PageDown))
        {
            m_mapView.Zoom(std::pow(0.85f, -dt * 8.0f));
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false))
        {
            // Focus on what is picked out.
            if (m_mapLevel == MapLevel::Galaxy && m_mapHasPickedSystem)
            {
                m_mapView.Focus(m_universe.Glance(SystemId::Unpack(m_mapPickedSystem)).position, 20.0f);
            }
            else if (m_mapLevel == MapLevel::System && m_mapSelected >= 0)
            {
                m_mapView.Focus(drawn[static_cast<size_t>(m_mapSelected)].at, drawn[static_cast<size_t>(m_mapSelected)].radius * 12.0f);
            }
            else if (m_mapLevel == MapLevel::Body && globe != nullptr && m_mapRegion >= 0)
            {
                m_mapView.FaceFrom(AreaOnGlobe(globe->regions[static_cast<size_t>(m_mapRegion)]) + glm::vec3(0.0f, 0.2f, 0.0f));
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_H, false))
        {
            // Back to the ship.
            if (m_campaign.travel.interstellar || m_mapLevel == MapLevel::Galaxy)
            {
                if (m_mapLevel != MapLevel::Galaxy)
                {
                    ShowMapGalaxy(Travel::GalaxyPosition(m_campaign, m_universe), 40.0f);
                }
                m_mapView.Focus(Travel::GalaxyPosition(m_campaign, m_universe), 40.0f);
            }
            else
            {
                ShowMapSystem(m_campaign.system, m_campaign.travel.underway ? -1 : m_campaign.body);
            }
            ImGui::End();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && m_mapLevel != MapLevel::Galaxy)
        {
            if (m_mapLevel == MapLevel::Body)
            {
                ShowMapSystem(m_mapSystem, m_mapSelected);
            }
            else
            {
                ShowMapGalaxy(shown->galaxy, 30.0f);
            }
            ImGui::End();
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        {
            if (m_mapLevel == MapLevel::Galaxy && m_mapHasPickedSystem)
            {
                ShowMapSystem(m_mapPickedSystem, -1);
                ImGui::End();
                return;
            }
            if (m_mapLevel == MapLevel::System && m_mapSelected >= 0)
            {
                ShowMapBody(m_mapSelected);
                ImGui::End();
                return;
            }
        }
    }

    // --- Zoomed past the nearest or furthest: on to the next scale ---
    if (const int past = m_mapView.TakePastLimit(); past != 0 && interactive)
    {
        bool changed = false;
        if (m_mapLevel == MapLevel::Galaxy && past < 0)
        {
            // Into the system under the mouse, or picked out, or nearest the middle.
            uint64_t into = 0;
            bool found = false;
            if (m_mapHasHoverSystem)
            {
                into = m_mapHoverSystem;
                found = true;
            }
            else
            {
                float best = 14.0f;
                for (const SystemId& id : m_mapNearSystems)
                {
                    const float apart = glm::length(m_universe.Glance(id).position - m_mapView.FocusPoint());
                    if (apart < best)
                    {
                        best = apart;
                        into = id.Packed();
                        found = true;
                    }
                }
            }
            if (found)
            {
                ShowMapSystem(into, -1);
                changed = true;
            }
        }
        else if (m_mapLevel == MapLevel::System && past > 0)
        {
            ShowMapGalaxy(shown->galaxy, 30.0f);
            changed = true;
        }
        else if (m_mapLevel == MapLevel::System && past < 0)
        {
            // Into the body under the mouse or picked out, if the camera is on it.
            const int body = m_mapHovered >= 0 ? m_mapHovered : m_mapSelected;
            if (shown->Find(body) != nullptr)
            {
                ShowMapBody(body);
                changed = true;
            }
        }
        else if (m_mapLevel == MapLevel::Body && past > 0)
        {
            ShowMapSystem(m_mapSystem, m_mapSelected);
            changed = true;
        }
        if (changed)
        {
            PlayNamed("UI/click", m_renderEye, 0.35f, 0.8f, false);
            ImGui::End();
            return;
        }
    }

    // --- Over the picture ---
    ImFont* font = ImGui::GetFont();
    (void)font;
    const float small = ImGui::GetFontSize() * 0.85f;
    const float tiny = ImGui::GetFontSize() * 0.72f;
    if (m_mapLevel == MapLevel::Galaxy)
    {
        const glm::vec3 focus = m_mapView.FocusPoint();
        const float reach = std::clamp(m_mapView.Distance() * 1.3f, 30.0f, 180.0f);
        const float closeness = std::pow(40.0f / std::max(m_mapView.Distance(), 1.0f), 0.35f);
        const glm::vec3 ship = Travel::GalaxyPosition(m_campaign, m_universe);
        int labels = 0;
        int drawnStars = 0;
        for (const SystemId& id : m_mapNearSystems)
        {
            const SystemGlance& glance = m_universe.Glance(id);
            const float t = 1.0f - glm::length(glm::vec2(glance.position.x - focus.x, glance.position.z - focus.z)) / reach;
            ImVec2 point;
            if (t <= 0.0f || !toScreen(glance.position, point))
            {
                continue;
            }
            const uint64_t packed = id.Packed();
            const float fade = std::min(1.0f, t * 1.8f);
            const float radius = std::clamp(2.6f * closeness * std::pow(std::max(glance.luminosity, 0.01f), 0.12f), 1.2f, 7.0f);
            const glm::vec3 tint = glm::mix(glance.starColor, glm::vec3(1.0f), 0.25f);
            // Far out there are thousands: a small one is a dot, and only the larger get a glow -- the picture has only
            // so many corners to draw with.
            if (++drawnStars > 2500)
            {
                break;
            }
            if (radius < 2.2f)
            {
                draw->AddCircleFilled(point, radius + 0.3f, Colour(tint, 0.95f * fade), 6);
            }
            else
            {
                draw->AddCircleFilled(point, radius * 2.6f, Colour(tint, 0.12f * fade), 12);
                draw->AddCircleFilled(point, radius, Colour(tint, 0.95f * fade), 10);
                draw->AddCircleFilled(point, radius * 0.45f, IM_COL32(255, 255, 255, static_cast<int>(230.0f * fade)), 6);
            }

            const bool here = packed == m_campaign.system && !m_campaign.travel.interstellar;
            // The one system the ship is bound for: the crossing plotted, or the one it is on.
            const bool course = (m_campaign.plan.set && m_campaign.plan.toSystem && packed == m_campaign.plan.system) ||
                                (!m_campaign.plan.set && m_campaign.travel.interstellar && packed == m_campaign.travel.toSystem);
            const bool picked = m_mapHasPickedSystem && packed == m_mapPickedSystem;
            const bool hover = m_mapHasHoverSystem && packed == m_mapHoverSystem;
            const bool visited = (m_campaign.Known(packed, -1) & CampaignState::kKnownVisited) != 0;
            if (visited)
            {
                draw->AddCircle(point, radius + 5.0f, Faded(IM_COL32(220, 226, 230, 150), fade), 20, 1.0f);
            }
            if (course)
            {
                Diamond(draw, point, radius + 12.0f, kGo, false);
                LabelUnder(draw, point, radius + 14.0f, kGo, "DESTINATION", kDim, "", tiny, tiny);
            }
            if (picked)
            {
                Brackets(draw, point, radius + 11.0f, kAmber);
            }
            else if (hover)
            {
                Brackets(draw, point, radius + 9.0f, IM_COL32(255, 255, 255, 200));
            }
            const bool named = here || course || picked || hover || (labels < 30 && fade > 0.6f && m_mapView.Distance() < 55.0f);
            if (named)
            {
                ++labels;
                const ImU32 colour = picked || course ? kAmber : hover ? IM_COL32(255, 255, 255, 255) : Faded(visited ? kText : kDim, fade);
                Label(draw, {point.x + radius + 7.0f, point.y - small * 0.55f}, colour, glance.name.c_str(), small);
                if (packed == SystemId{}.Packed())
                {
                    Label(draw, {point.x + radius + 7.0f, point.y + small * 0.45f}, Faded(kDim, fade), "HOME", tiny);
                }
            }
        }
        // The ship: in a system, its mark beside that system's star (whose name is beside it the other way) and its name under
        // it; between the stars, where it is, aimed where it is going.
        ImVec2 at;
        if (toScreen(ship, at))
        {
            ImVec2 ahead;
            glm::vec2 way{0.0f, -1.0f};
            if (m_campaign.travel.interstellar && toScreen(m_campaign.travel.toGalaxy, ahead) && std::hypot(ahead.x - at.x, ahead.y - at.y) > 1.0f)
            {
                way = glm::normalize(glm::vec2(ahead.x - at.x, ahead.y - at.y));
            }
            const ImVec2 mark = m_campaign.travel.interstellar ? at : ImVec2{at.x - 16.0f, at.y};
            draw->AddCircle(mark, 9.0f, Faded(kShipColour, 0.5f), 0, 1.0f);
            ShipMark(draw, mark, way, 5.0f, kShipColour);
            LabelUnder(draw, at, 12.0f, kShipColour, "YOUR SHIP", kDim, m_campaign.travel.interstellar ? "crossing" : "", tiny, tiny);
        }
        // The drive's reach, marked on its ring.
        if (const float reach = Travel::CrossingRange(DriveTier()); reach > 0.0f)
        {
            ImVec2 point;
            if (toScreen({ship.x + reach, 0.0f, ship.z}, point))
            {
                Label(draw, {point.x + 4.0f, point.y - tiny}, IM_COL32(236, 156, 64, 170), ("DRIVE REACH " + std::to_string(static_cast<int>(reach)) + " LY").c_str(),
                      tiny);
            }
        }
    }
    else if (m_mapLevel == MapLevel::System)
    {
        for (const Body& body : shown->bodies)
        {
            const SystemMapView::Drawn& at = drawn[body.index];
            const bool picked = body.index == m_mapSelected;
            const bool hover = body.index == m_mapHovered;
            ImVec2 centre;
            if (!toScreen(at.at, centre))
            {
                continue;
            }
            if (picked || hover)
            {
                ImVec2 edge;
                const float r = toScreen(at.at + shot.Up() * at.radius, edge) ? std::max(std::hypot(edge.x - centre.x, edge.y - centre.y), 6.0f) : 10.0f;
                Brackets(draw, centre, r + 6.0f, picked ? kAmber : IM_COL32(255, 255, 255, 190));
            }
            // A planet's name over it; a moon's to its side, only close in or when picked out.
            if (body.kind != BodyKind::Planet && !picked && !hover && m_mapView.Distance() > 11.0f)
            {
                continue;
            }
            ImVec2 point;
            const bool beside = body.kind != BodyKind::Planet;
            if (!toScreen(beside ? at.at : at.at + shot.Up() * (at.radius * 1.35f), point))
            {
                continue;
            }
            const uint8_t known = m_campaign.Known(m_mapSystem, body.index);
            const ImU32 colour = picked ? kAmber : hover ? IM_COL32(255, 255, 255, 255) : known != 0 ? kText : kDim;
            const ImVec2 extent = TextSize(body.name.c_str(), small);
            // Beside it, past its edge as drawn, however close the camera is.
            ImVec2 rim;
            const float across = toScreen(at.at + shot.Right() * at.radius, rim) ? std::abs(rim.x - point.x) : 0.0f;
            const ImVec2 corner = beside ? ImVec2{point.x + across + 10.0f, point.y - extent.y * 0.5f} : ImVec2{point.x - extent.x * 0.5f, point.y - extent.y};
            Label(draw, corner, colour, body.name.c_str(), small);
            // The world with an outpost on it: a mark over its name.
            if (body.index == shown->hub)
            {
                const ImVec2 word = TextSize("OUTPOST", tiny);
                const float left = corner.x + extent.x * 0.5f - (word.x + 12.0f) * 0.5f;
                Diamond(draw, {left + 4.0f, corner.y - word.y * 0.5f - 2.0f}, 3.5f, kHub, true);
                Label(draw, {left + 12.0f, corner.y - word.y - 2.0f}, kHub, "OUTPOST", tiny);
            }
        }
        // The distance rings' marks.
        for (const float au : {1.0f, 5.0f, 20.0f, 50.0f})
        {
            ImVec2 point;
            const glm::vec3 at = SystemMapView::Place({au, 0.0f, 0.0f});
            if (glm::length(at) < glm::length(drawn.empty() ? glm::vec3(0.0f) : drawn.back().at) * 1.6f && toScreen(at, point))
            {
                Label(draw, {point.x + 4.0f, point.y}, IM_COL32(150, 160, 180, 110), (Number(au, 0) + " AU").c_str(), tiny);
            }
        }
        if (ours)
        {
            // The one place the ship is going: the course plotted, or the one it is on.
            const int destination = m_campaign.plan.set && !m_campaign.plan.toSystem ? m_campaign.plan.body
                                    : m_campaign.travel.underway                     ? m_campaign.travel.target
                                                                                     : -1;
            ImVec2 point;
            if (destination >= 0 && destination < static_cast<int>(drawn.size()) && toScreen(drawn[static_cast<size_t>(destination)].at, point))
            {
                ImVec2 edge;
                const SystemMapView::Drawn& at = drawn[static_cast<size_t>(destination)];
                const float r = toScreen(at.at + shot.Up() * at.radius, edge) ? std::max(std::hypot(edge.x - point.x, edge.y - point.y), 6.0f) : 10.0f;
                Diamond(draw, point, r + 12.0f, kGo, false);
                LabelUnder(draw, point, r + 14.0f, kGo, m_campaign.travel.underway && !m_campaign.plan.set ? "COURSE" : "DESTINATION", kDim,
                           m_campaign.plan.set && !m_campaign.travel.underway ? "plotted: set out at the helm" : "", tiny, tiny);
            }
            // The ship: at a body, its mark beside the body and what it is doing under it (the body's name is over it); under way,
            // its mark where it is, aimed where it is going.
            const Body* at = m_campaign.travel.underway ? nullptr : shown->Find(m_campaign.body);
            if (at != nullptr && toScreen(drawn[static_cast<size_t>(at->index)].at, point))
            {
                ImVec2 edge;
                const SystemMapView::Drawn& body = drawn[static_cast<size_t>(at->index)];
                const float r = toScreen(body.at + shot.Up() * body.radius, edge) ? std::max(std::hypot(edge.x - point.x, edge.y - point.y), 6.0f) : 10.0f;
                const ImVec2 beside{point.x - r - 16.0f, point.y};
                draw->AddCircle(beside, 9.0f, Faded(kShipColour, 0.5f), 0, 1.0f);
                ShipMark(draw, beside, {0.0f, -1.0f}, 5.0f, kShipColour);
                const int region = m_campaign.travel.region;
                const std::string doing = ShipLanded() && region >= 0 && region < static_cast<int>(at->regions.size())
                                              ? "landed at " + at->regions[static_cast<size_t>(region)].designation
                                              : "in orbit";
                LabelUnder(draw, point, r + 8.0f, kShipColour, "YOUR SHIP", kDim, doing, tiny, tiny);
            }
            const glm::vec3 ship = SystemMapView::ShipAt(drawn, -1, Travel::ShipPosition(m_campaign, *shown));
            if (at == nullptr && toScreen(ship, point))
            {
                glm::vec2 way{0.0f, -1.0f};
                ImVec2 ahead;
                if (m_campaign.travel.underway && m_campaign.travel.target >= 0 && toScreen(drawn[static_cast<size_t>(m_campaign.travel.target)].at, ahead) &&
                    std::hypot(ahead.x - point.x, ahead.y - point.y) > 1.0f)
                {
                    way = glm::normalize(glm::vec2(ahead.x - point.x, ahead.y - point.y));
                }
                draw->AddCircle(point, 11.0f, Faded(kShipColour, 0.5f), 0, 1.0f);
                ShipMark(draw, point, way, 6.0f, kShipColour);
                LabelUnder(draw, point, 14.0f, kShipColour, "YOUR SHIP", kDim, m_campaign.travel.underway ? "under way" : "holding", small, tiny);
            }
        }
    }
    else if (globe != nullptr)
    {
        const glm::vec3 eye = glm::normalize(shot.Eye());
        const glm::vec3 sun = SunOverBody(*shown, *globe);
        const bool heading = ours && (m_campaign.travel.underway ? m_campaign.travel.target == globe->index : m_campaign.body == globe->index);
        for (int i = 0; i < static_cast<int>(globe->regions.size()); ++i)
        {
            if (!MapAreaKnown(m_mapSystem, *globe, i))
            {
                continue;
            }
            const LandingRegion& region = globe->regions[static_cast<size_t>(i)];
            const glm::vec3 at = AreaOnGlobe(region);
            const float facing = glm::dot(at, eye);
            ImVec2 point;
            if (facing < 0.05f || !toScreen(at * 1.01f, point))
            {
                continue;
            }
            const float fade = std::clamp((facing - 0.05f) * 5.0f, 0.0f, 1.0f);
            const bool day = glm::dot(at, sun) > 0.0f;
            const bool picked = i == m_mapRegion;
            const bool hover = i == m_mapHoverRegion;
            const bool chosen = heading && m_campaign.travel.region == i;
            const bool port = RegionIsPort(*globe, i);
            const ImU32 colour = Faded(chosen ? kGo : picked ? kAmber : port ? kHub : day ? kText : kNight, fade);
            if (port)
            {
                // A hub: where the ship itself sets down.
                Diamond(draw, point, 5.0f, colour, true);
                Diamond(draw, point, 9.0f, colour, false);
            }
            else
            {
                draw->AddCircleFilled(point, 4.0f, colour);
                draw->AddCircle(point, 8.0f, colour, 0, 1.5f);
            }
            if (picked || hover)
            {
                Brackets(draw, point, 13.0f, Faded(picked ? kAmber : IM_COL32(255, 255, 255, 200), fade));
            }
            Label(draw, {point.x + 14.0f, point.y - small * 0.55f}, colour, region.designation.c_str(), small);
            const bool shipHere = chosen && port && ShipLanded();
            const std::string under =
                std::string(shipHere ? "YOUR SHIP IS HERE   " : chosen ? (port ? "LANDING HERE   " : "GOING DOWN HERE   ") : port ? OutpostTag(region) : "") +
                (day ? "DAY" : "NIGHT");
            if (shipHere)
            {
                ShipMark(draw, {point.x - 20.0f, point.y}, {0.0f, -1.0f}, 5.0f, Faded(kShipColour, fade));
            }
            Label(draw, {point.x + 14.0f, point.y + small * 0.45f}, Faded(chosen ? kGo : kDim, fade), under.c_str(), tiny);
        }
        // The ship going round (standing at its area, the area's own mark says so).
        if (ours && !m_campaign.travel.underway && m_campaign.body == globe->index && !ShipLanded())
        {
            const glm::vec3 at = ShipOverGlobe(*globe);
            // Hidden behind the globe when it is round the far side.
            const glm::vec3 eyeAt = shot.Eye();
            const glm::vec3 toShip = at - eyeAt;
            const float along = -glm::dot(eyeAt, glm::normalize(toShip));
            const bool behind = along > 0.0f && along < glm::length(toShip) && glm::length(eyeAt + glm::normalize(toShip) * along) < 1.0f;
            ImVec2 point;
            if (!behind && toScreen(at, point))
            {
                draw->AddCircle(point, 11.0f, Faded(kShipColour, 0.5f), 0, 1.0f);
                ShipMark(draw, point, {0.0f, -1.0f}, 6.0f, kShipColour);
                LabelUnder(draw, point, 14.0f, kShipColour, "YOUR SHIP", kDim, "in orbit", small, tiny);
            }
        }
    }
    // What is under the pointer, in a line or two, by it.
    if (hovered && m_mapDragButton < 0)
    {
        std::string tip;
        if (m_mapLevel == MapLevel::Galaxy && m_mapHasHoverSystem)
        {
            const SystemGlance& glance = m_universe.Glance(SystemId::Unpack(m_mapHoverSystem));
            const float lightYears = glm::length(glance.position - Travel::GalaxyPosition(m_campaign, m_universe));
            tip = glance.name + "\n" + Number(lightYears, 1) + " light years" +
                  (m_mapHoverSystem == m_campaign.system ? "   the ship is here" : "   crossing " + About(Travel::InterstellarSeconds(lightYears, DriveTier())));
        }
        else if (m_mapLevel == MapLevel::System && m_mapHovered >= 0)
        {
            const Body& body = shown->bodies[static_cast<size_t>(m_mapHovered)];
            tip = body.name + "\n" + (body.kind == BodyKind::Moon ? "Moon" : body.gas ? "Gas giant" : "Planet");
            if (ours && !(m_campaign.body == body.index && !m_campaign.travel.underway))
            {
                const float distance = glm::length(shown->Position(body.index, m_campaign.clock) - Travel::ShipPosition(m_campaign, *shown));
                tip += "   " + Number(distance, 2) + " AU, " + About(Travel::Seconds(distance * 1.15f, DriveTier()));
            }
            tip += "\nDouble-click to see its surface";
        }
        else if (m_mapLevel == MapLevel::Body && globe != nullptr && m_mapHoverRegion >= 0)
        {
            const LandingRegion& region = globe->regions[static_cast<size_t>(m_mapHoverRegion)];
            const RegionKindDef* kind = m_universeData.RegionKind(region.kind);
            tip = region.designation + "\n" + (kind != nullptr ? kind->name : region.kind) +
                  (glm::dot(AreaOnGlobe(region), SunOverBody(*shown, *globe)) > 0.0f ? "   day" : "   night");
        }
        if (!tip.empty())
        {
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }
    // The fade between scales: darkening into the dive, lifting off the new one.
    {
        const float dark = m_mapTransition.active ? glm::smoothstep(0.05f, 0.28f, m_mapTransition.time) : glm::smoothstep(0.0f, 1.0f, m_mapFadeIn);
        if (dark > 0.001f)
        {
            draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32(0, 0, 0, static_cast<int>(dark * 235.0f)));
        }
    }
    ImGui::End();

    DrawMapBars(shown);
    if (m_mapLevel == MapLevel::Galaxy)
    {
        DrawMapGalaxyPanels();
    }
    else if (m_mapLevel == MapLevel::System)
    {
        DrawMapSystemPanels(*shown);
    }
    else
    {
        DrawMapBodyPanels(*shown);
    }
}

// --- The panels -----------------------------------------------------------------------------------------------------

void PredationGame::DrawMapBars(const StarSystem* shown)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->Pos;
    const ImVec2 size = viewport->Size;

    // Along the top: where on the map this is, as a trail to go back up by; how the ship stands; the way out.
    ImGui::SetNextWindowPos({origin.x + 16.0f, origin.y + 14.0f});
    ImGui::SetNextWindowSize({size.x - 32.0f, 46.0f});
    if (ImGui::Begin("##mapbar", nullptr, kPanel | ImGuiWindowFlags_NoScrollbar))
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kAmberText, "NAVIGATION");
        ImGui::SameLine(0.0f, 18.0f);
        const auto crumb = [&](const std::string& text, bool current)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, current ? kAmberText : ImVec4(0.85f, 0.88f, 0.9f, 1.0f));
            const bool pressed = ImGui::Button(text.c_str()) && !current;
            ImGui::PopStyleColor(2);
            return pressed;
        };
        if (crumb("GALAXY", m_mapLevel == MapLevel::Galaxy))
        {
            ShowMapGalaxy(shown != nullptr ? shown->galaxy : Travel::GalaxyPosition(m_campaign, m_universe), 30.0f);
        }
        if (shown != nullptr)
        {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextColored(kDimText, ">");
            ImGui::SameLine(0.0f, 2.0f);
            if (crumb(shown->name, m_mapLevel == MapLevel::System))
            {
                ShowMapSystem(m_mapSystem, m_mapSelected);
            }
            if (m_mapLevel == MapLevel::Body)
            {
                if (const Body* body = shown->Find(m_mapSelected))
                {
                    ImGui::SameLine(0.0f, 2.0f);
                    ImGui::TextColored(kDimText, ">");
                    ImGui::SameLine(0.0f, 2.0f);
                    crumb(body->name, true);
                }
            }
        }
        const std::string status = ShipStatus();
        const float statusWidth = ImGui::CalcTextSize(status.c_str()).x;
        const bool canCancel = m_campaign.travel.underway && !m_campaign.travel.interstellar && m_campaign.travel.target >= 0;
        const float closeWidth = 96.0f;
        const float cancelWidth = canCancel ? 150.0f : 0.0f;
        const float statusAt = std::max(ImGui::GetCursorPosX() + 380.0f, (size.x - 32.0f - statusWidth) * 0.5f);
        ImGui::SameLine(statusAt);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(m_campaign.travel.underway ? kAmberText : ImVec4(0.85f, 0.88f, 0.9f, 1.0f), "%s", status.c_str());
        // The map is everybody's: who has just moved it, when it was somebody else.
        if (m_mapMovedFor > 0.0f && m_mapMovedBy >= 0)
        {
            std::string name = "Player " + std::to_string(m_mapMovedBy + 1);
            for (const RemotePlayerView& remote : RemotePlayers())
            {
                name = remote.id == m_mapMovedBy ? remote.name : name;
            }
            ImGui::SameLine(0.0f, 18.0f);
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.62f, std::min(m_mapMovedFor, 1.0f)), "moved by %s", name.c_str());
        }
        ImGui::SameLine(size.x - 32.0f - closeWidth - cancelWidth - 24.0f);
        if (canCancel)
        {
            if (ImGui::Button("Call off course", {cancelWidth - 8.0f, 0.0f}))
            {
                AskCampaign(CampaignAction::CancelCourse);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Close   Esc", {closeWidth, 0.0f}))
        {
            CloseSystemMap();
        }
    }
    ImGui::End();

    // Under it, the course plotted: what to, and setting out on it (as the helm does) or clearing it.
    if (m_campaign.plan.set)
    {
        const std::string plan = "Plotted:  " + PlanName();
        const float planWidth = ImGui::CalcTextSize(plan.c_str()).x + 260.0f;
        ImGui::SetNextWindowPos({origin.x + size.x * 0.5f - planWidth * 0.5f, origin.y + 66.0f});
        ImGui::SetNextWindowSize({planWidth, 42.0f});
        if (ImGui::Begin("##mapplan", nullptr, kPanel | ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kGoText, "%s", plan.c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(m_map != MapChoice::Ship || m_cine.Active());
            if (ImGui::Button(m_campaign.travel.underway ? "Change course" : "Set out"))
            {
                SetOut();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                AskCampaign(CampaignAction::ClearPlot);
            }
        }
        ImGui::End();
    }

    // Down in the corner: what the marks on the map mean, at this scale.
    {
        struct Key
        {
            int mark; // 0 outpost, 1 ship, 2 destination, 3 been there, 4 a place to go down, 5 drive's reach, 6 the charts' reach
            const char* words;
        };
        std::vector<Key> keys;
        if (m_mapLevel == MapLevel::Galaxy)
        {
            keys = {{1, "Your ship"}, {2, "Destination"}, {3, "Been there"}, {5, "How far the drive reaches"}, {6, "How far the charts reach"}};
        }
        else if (m_mapLevel == MapLevel::System)
        {
            keys = {{1, "Your ship"}, {2, "Destination"}, {0, "Outpost: the ship lands there"}};
        }
        else
        {
            keys = {{1, "Your ship"}, {0, "Outpost: the ship lands there"}, {4, "A place to go down in the shuttle"}};
        }
        ImGui::SetNextWindowPos({origin.x + size.x - 16.0f, origin.y + size.y - 64.0f}, ImGuiCond_Always, {1.0f, 1.0f});
        ImGui::SetNextWindowBgAlpha(0.7f);
        if (ImGui::Begin("##maplegend", nullptr, kPanel | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs))
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float line = ImGui::GetTextLineHeight();
            for (const Key& key : keys)
            {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const ImVec2 middle{at.x + 9.0f, at.y + line * 0.5f};
                switch (key.mark)
                {
                case 0: Diamond(draw, middle, 4.5f, kHub, true); break;
                case 1: ShipMark(draw, middle, {0.0f, -1.0f}, 4.5f, kShipColour); break;
                case 2: Diamond(draw, middle, 6.0f, kGo, false); break;
                case 3: draw->AddCircle(middle, 5.5f, IM_COL32(220, 226, 230, 150), 16, 1.0f); break;
                case 4: draw->AddCircleFilled(middle, 3.0f, kText); draw->AddCircle(middle, 6.0f, kText, 0, 1.2f); break;
                case 5: draw->AddLine({middle.x - 7.0f, middle.y}, {middle.x + 7.0f, middle.y}, IM_COL32(236, 156, 64, 200), 1.5f); break;
                default:
                    draw->AddLine({middle.x - 7.0f, middle.y}, {middle.x - 2.0f, middle.y}, IM_COL32(120, 170, 220, 160), 1.5f);
                    draw->AddLine({middle.x + 2.0f, middle.y}, {middle.x + 7.0f, middle.y}, IM_COL32(120, 170, 220, 160), 1.5f);
                    break;
                }
                ImGui::SetCursorScreenPos({at.x + 24.0f, at.y});
                ImGui::TextColored(kDimText, "%s", key.words);
            }
        }
        ImGui::End();
    }

    // Along the bottom: what the mouse and keys do here.
    const char* help = m_mapLevel == MapLevel::Body
                           ? "Click  pick an area    Double-click  turn to it    Drag / WASD  turn the globe    Wheel  zoom, out to the system    "
                             "Backspace  up    H  ship    Esc  close"
                           : "Click  pick    Double-click  open    Right drag  move    Left drag  turn    WASD  move    Q E  turn    "
                             "Wheel  zoom, on to the next scale    F  focus    H  ship    Backspace  up    Esc  close";
    const float width = std::min(ImGui::CalcTextSize(help).x + 32.0f, size.x - 32.0f);
    ImGui::SetNextWindowPos({origin.x + size.x * 0.5f, origin.y + size.y - 14.0f}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowSize({width, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.7f);
    if (ImGui::Begin("##maphelp", nullptr, kPanel | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs))
    {
        ImGui::TextColored(kDimText, "%s", help);
    }
    ImGui::End();
}

void PredationGame::DrawMapGalaxyPanels()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->Pos;
    const ImVec2 size = viewport->Size;
    const glm::vec3 ship = Travel::GalaxyPosition(m_campaign, m_universe);
    const bool aboard = m_map == MapChoice::Ship;

    // Down the left: systems near the ship, or found by name, and the ones been to.
    ImGui::SetNextWindowPos({origin.x + 16.0f, origin.y + 74.0f});
    ImGui::SetNextWindowSize({310.0f, size.y - 74.0f - 70.0f});
    if (ImGui::Begin("##mapgalaxylist", nullptr, kPanel))
    {
        Title("Galaxy", "The ship's charts, as far as they go");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##find", "Find a system by name", m_mapSearch, sizeof(m_mapSearch));
        // Worked out again only when the ship has moved on or the search has changed: it is a lot of systems.
        const std::string search = m_mapSearch;
        if (glm::length(ship - m_mapListedFrom) > 2.0f || search != m_mapListedFor)
        {
            m_mapListedFrom = ship;
            m_mapListedFor = search;
            m_mapListed.clear();
            for (const SystemId& id : m_universe.Near(ship, search.empty() ? 45.0f : 160.0f))
            {
                if (SystemCharted(id.Packed()) && (search.empty() || Contains(m_universe.Glance(id).name, search.c_str())))
                {
                    m_mapListed.push_back(id);
                }
                if (m_mapListed.size() >= 60)
                {
                    break;
                }
            }
        }
        Section(search.empty() ? "NEAREST" : "FOUND");
        if (ImGui::BeginChild("##systems", {0.0f, -150.0f}))
        {
            if (m_mapListed.empty())
            {
                Wrapped(kDimText, "Nothing by that name within 160 light years.");
            }
            for (const SystemId& id : m_mapListed)
            {
                const SystemGlance& glance = m_universe.Glance(id);
                const uint64_t packed = id.Packed();
                ImGui::PushID(std::to_string(packed).c_str());
                const bool picked = m_mapHasPickedSystem && m_mapPickedSystem == packed;
                const ImVec2 corner = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##system", picked, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_mapPickedSystem = packed;
                    m_mapHasPickedSystem = true;
                    m_mapView.Focus(glance.position, std::min(m_mapView.Distance(), 40.0f));
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        ShowMapSystem(packed, -1);
                        ImGui::PopID();
                        break;
                    }
                }
                const bool hover = ImGui::IsItemHovered();
                if (hover)
                {
                    m_mapHoverSystem = packed;
                    m_mapHasHoverSystem = true;
                }
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const float line = ImGui::GetTextLineHeight();
                draw->AddCircleFilled({corner.x + 7.0f, corner.y + line * 0.5f}, 4.0f, Colour(glm::mix(glance.starColor, glm::vec3(1.0f), 0.2f), 1.0f));
                draw->AddText({corner.x + 20.0f, corner.y}, picked ? kAmber : kText, glance.name.c_str());
                std::string tag;
                if (packed == m_campaign.system && !m_campaign.travel.interstellar)
                {
                    tag = "HERE";
                }
                else if (m_campaign.travel.interstellar && packed == m_campaign.travel.toSystem)
                {
                    tag = "COURSE";
                }
                else
                {
                    tag = Number(glm::length(glance.position - ship), 1) + " LY";
                }
                const ImVec2 tagSize = ImGui::CalcTextSize(tag.c_str());
                const float right = corner.x + ImGui::GetContentRegionAvail().x;
                draw->AddText({right - tagSize.x - 4.0f, corner.y}, tag == "HERE" || tag == "COURSE" ? kAmber : kDim, tag.c_str());
                const uint8_t known = m_campaign.Known(packed, -1);
                if ((known & (CampaignState::kKnownVisited | CampaignState::kKnownRecords)) != 0)
                {
                    const char* word = (known & CampaignState::kKnownVisited) != 0 ? "visited" : "on file";
                    draw->AddText({right - tagSize.x - 4.0f - ImGui::CalcTextSize(word).x - 10.0f, corner.y}, kDim, word);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        Section("BEEN TO");
        if (ImGui::BeginChild("##visited", {0.0f, 0.0f}))
        {
            for (const auto& [key, bits] : m_campaign.known)
            {
                const size_t colon = key.find(':');
                if (colon == std::string::npos || key.compare(colon, std::string::npos, ":-1") != 0 || (bits & CampaignState::kKnownVisited) == 0)
                {
                    continue;
                }
                const uint64_t packed = std::stoull(key.substr(0, colon));
                const SystemGlance& glance = m_universe.Glance(SystemId::Unpack(packed));
                ImGui::PushID(key.c_str());
                if (ImGui::Selectable(glance.name.c_str(), m_mapHasPickedSystem && m_mapPickedSystem == packed))
                {
                    m_mapPickedSystem = packed;
                    m_mapHasPickedSystem = true;
                    m_mapView.Focus(glance.position, std::min(m_mapView.Distance(), 40.0f));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();

    // Down the right: the system picked out.
    if (!m_mapHasPickedSystem)
    {
        return;
    }
    const SystemGlance& glance = m_universe.Glance(SystemId::Unpack(m_mapPickedSystem));
    ImGui::SetNextWindowPos({origin.x + size.x - 16.0f - 340.0f, origin.y + 74.0f});
    ImGui::SetNextWindowSizeConstraints({340.0f, 0.0f}, {340.0f, 10000.0f});
    if (ImGui::Begin("##mapgalaxyinfo", nullptr, kPanel | ImGuiWindowFlags_AlwaysAutoResize))
    {
        const StarDef* star = nullptr;
        for (const StarDef& def : m_universeData.stars)
        {
            star = def.id == glance.star ? &def : star;
        }
        Title(glance.name, star != nullptr ? star->name : glance.star);
        const bool here = m_mapPickedSystem == m_campaign.system && !m_campaign.travel.interstellar;
        const bool course = m_campaign.travel.interstellar && m_mapPickedSystem == m_campaign.travel.toSystem;
        const bool visited = (m_campaign.Known(m_mapPickedSystem, -1) & CampaignState::kKnownVisited) != 0;
        Section("SYSTEM");
        const float distance = glm::length(glance.position - ship);
        Row("Status", here ? "The ship is here" : course ? "On course" : visited ? "Been here" : "Not been here");
        if (!here)
        {
            Row("Distance", Number(distance, 1) + " light years");
            Row("Crossing", About(Travel::InterstellarSeconds(distance, DriveTier())));
        }
        if (visited || here)
        {
            if (const StarSystem* system = m_universe.System(m_mapPickedSystem))
            {
                int planets = 0;
                int moons = 0;
                for (const Body& body : system->bodies)
                {
                    planets += body.kind == BodyKind::Planet ? 1 : 0;
                    moons += body.kind == BodyKind::Moon ? 1 : 0;
                }
                Row("Bodies", std::to_string(planets) + " planets, " + std::to_string(moons) + " moons");
                const Body* hubWorld = system->Find(system->hub);
                const bool hasHub = hubWorld != nullptr && system->hubRegion >= 0 && system->hubRegion < static_cast<int>(hubWorld->regions.size());
                Row("Outpost", hasHub ? "Yes: ships land there" : "None", true);
                if (hasHub)
                {
                    Row("Run by", OperatorOf(hubWorld->regions[static_cast<size_t>(system->hubRegion)]), true);
                }
            }
        }
        else if ((m_campaign.Known(m_mapPickedSystem, -1) & CampaignState::kKnownRecords) != 0)
        {
            if (const StarSystem* system = m_universe.System(m_mapPickedSystem))
            {
                Row("Bodies", std::to_string(system->bodies.size()) + " on CIRRA's records");
            }
        }
        else
        {
            Row("Bodies", "", false);
        }
        Row("CIRRA", MapRecordsText(m_mapPickedSystem, -1), true);
        ImGui::Spacing();
        ImGui::Spacing();
        if (ImGui::Button("Open the system   Enter", {-1.0f, 32.0f}))
        {
            ShowMapSystem(m_mapPickedSystem, -1);
        }
        if (course)
        {
            Wrapped(kAmberText, "On course. %s left.", About((1.0f - Travel::CrossingDone(m_campaign)) * m_campaign.travel.duration).c_str());
        }
        else if (!here)
        {
            const float reach = Travel::CrossingRange(DriveTier());
            const bool canCross = reach > 0.0f && distance <= reach;
            const bool plotted = m_campaign.plan.set && m_campaign.plan.toSystem && m_campaign.plan.system == m_mapPickedSystem;
            ImGui::BeginDisabled(!aboard || m_cine.Active() || !canCross || plotted);
            if (ImGui::Button(plotted ? "Plotted -- set out from the helm" : "Plot course for this system", {-1.0f, 32.0f}))
            {
                AskSystemCourse(m_mapPickedSystem);
            }
            ImGui::EndDisabled();
            if (reach <= 0.0f)
            {
                Wrapped(kDimText, "Crossing between the stars needs an upgraded drive.");
            }
            else if (!canCross)
            {
                Wrapped(kDimText, "Beyond the drive's reach of %d light years. A better drive goes further.", static_cast<int>(reach));
            }
            if (!aboard)
            {
                Wrapped(kDimText, "Courses are set aboard the ship.");
            }
        }
    }
    ImGui::End();
}

void PredationGame::DrawMapSystemPanels(const StarSystem& system)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->Pos;
    const ImVec2 size = viewport->Size;
    const std::vector<SystemMapView::Drawn> drawn = SystemMapView::Layout(system, m_campaign.clock);
    const bool ours = m_mapSystem == m_campaign.system && !m_campaign.travel.interstellar;
    const bool aboard = m_map == MapChoice::Ship;
    const StarDef* star = nullptr;
    for (const StarDef& def : m_universeData.stars)
    {
        star = def.id == system.star ? &def : star;
    }

    // Down the left: the bodies, each moon under its planet.
    ImGui::SetNextWindowPos({origin.x + 16.0f, origin.y + 74.0f});
    ImGui::SetNextWindowSize({310.0f, size.y - 74.0f - 70.0f});
    if (ImGui::Begin("##mapbodies", nullptr, kPanel))
    {
        Title(system.name, star != nullptr ? star->name : system.star);
        Section("BODIES");
        if (ImGui::BeginChild("##bodylist", {0.0f, -40.0f}))
        {
            for (const Body& body : system.bodies)
            {
                ImGui::PushID(body.index);
                const bool picked = body.index == m_mapSelected;
                const ImVec2 corner = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##body", picked, ImGuiSelectableFlags_AllowDoubleClick))
                {
                    m_mapSelected = body.index;
                    m_mapView.Focus(drawn[body.index].at, drawn[body.index].radius * 12.0f);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        ShowMapBody(body.index);
                        ImGui::PopID();
                        break;
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    m_mapHovered = body.index;
                }
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const float line = ImGui::GetTextLineHeight();
                const float indent = body.kind == BodyKind::Planet ? 0.0f : 16.0f;
                const ImVec2 icon{corner.x + indent + 7.0f, corner.y + line * 0.5f};
                {
                    const glm::vec3 tint = body.gas ? glm::mix(body.groundA, body.groundB, 0.5f) : glm::mix(body.groundA, body.ocean, body.oceanAmount * 0.6f);
                    draw->AddCircleFilled(icon, body.kind == BodyKind::Moon ? 3.5f : 5.0f, Colour(tint, 1.0f));
                    if (body.index == system.hub)
                    {
                        draw->AddCircle(icon, 8.0f, IM_COL32(150, 220, 255, 200), 0, 1.2f);
                    }
                }
                const uint8_t known = m_campaign.Known(m_mapSystem, body.index);
                draw->AddText({corner.x + indent + 20.0f, corner.y}, picked ? kAmber : known != 0 ? kText : kDim, body.name.c_str());
                const char* tag = nullptr;
                ImU32 tagColour = kAmber;
                if (ours && !m_campaign.travel.underway && body.index == m_campaign.body)
                {
                    tag = ShipLanded() ? "LANDED" : "HERE";
                }
                else if (ours && m_campaign.travel.underway && body.index == m_campaign.travel.target)
                {
                    tag = "COURSE";
                }
                else if ((known & CampaignState::kKnownVisited) != 0)
                {
                    tag = "visited";
                    tagColour = kDim;
                }
                if (tag != nullptr)
                {
                    const float right = corner.x + ImGui::GetContentRegionAvail().x;
                    draw->AddText({right - ImGui::CalcTextSize(tag).x - 4.0f, corner.y}, tagColour, tag);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (ImGui::Button("Whole system", {136.0f, 0.0f}))
        {
            float outermost = 10.0f;
            for (const SystemMapView::Drawn& at : drawn)
            {
                outermost = std::max(outermost, glm::length(at.at));
            }
            m_mapView.Focus(glm::vec3(0.0f), outermost * 2.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("Galaxy", {-1.0f, 0.0f}))
        {
            ShowMapGalaxy(system.galaxy, 30.0f);
        }
    }
    ImGui::End();

    // Down the right: what is known of the one picked out, and what can be done about it.
    const Body* picked = system.Find(m_mapSelected);
    if (picked == nullptr)
    {
        return;
    }
    ImGui::SetNextWindowPos({origin.x + size.x - 16.0f - 340.0f, origin.y + 74.0f});
    ImGui::SetNextWindowSizeConstraints({340.0f, 0.0f}, {340.0f, 10000.0f});
    if (ImGui::Begin("##mapinfo", nullptr, kPanel | ImGuiWindowFlags_AlwaysAutoResize))
    {
        const uint8_t known = m_campaign.Known(m_mapSystem, picked->index);
        const bool records = (known & CampaignState::kKnownRecords) != 0;
        const bool scanned = (known & (CampaignState::kKnownScanned | CampaignState::kKnownVisited)) != 0;
        const bool deep = (known & (CampaignState::kKnownDeep | CampaignState::kKnownVisited)) != 0;
        std::string what;
        if (picked->kind == BodyKind::Moon)
        {
            what = "Moon of " + system.bodies[static_cast<size_t>(picked->parent)].name;
        }
        else
        {
            what = (picked->gas ? "Gas giant, " : "Planet, ") + Number(picked->orbit, 2) + " AU from the star";
        }
        Title(picked->name, what);
        Section("SURVEY");
        Row("CIRRA", MapRecordsText(m_mapSystem, picked->index), true);
        {
            const BiomeDef* biome = m_universeData.Biome(picked->biome);
            Row("Type", biome != nullptr ? biome->name : picked->biome, picked->gas || scanned || records);
            Row("Size", Number(picked->radius, 2) + " Earth radii", true);
            if (!picked->gas)
            {
                const AtmosphereDef* air = m_universeData.Atmosphere(picked->atmosphere);
                Row("Atmosphere", air != nullptr ? air->name : picked->atmosphere, scanned);
                Row("Temperature", std::to_string(static_cast<int>(std::round(picked->temperature))) + " C", scanned);
                const WeatherDef* weather = m_universeData.Weather(picked->weather);
                Row("Weather", weather != nullptr ? weather->name : picked->weather, scanned);
                const TerrainDef* terrain = m_universeData.Terrain(picked->terrain);
                Row("Terrain", terrain != nullptr ? terrain->name : picked->terrain, deep);
                const CivilizationDef* civ = m_universeData.Civilization(picked->civilization);
                std::string settled = civ != nullptr ? civ->name : std::string("None");
                if (!records && scanned && civ != nullptr && !civ->inhabited)
                {
                    settled = civ->records ? "Structures detected" : "None detected";
                }
                Row("Settlement", settled, records || scanned);
                Row("Day", About(picked->day), scanned || records);
            }
            std::string traits;
            for (const std::string& id : picked->specials)
            {
                const SpecialDef* special = m_universeData.Special(id);
                const bool visible = id == "rings" || id == "aurora";
                if (deep || (visible && scanned))
                {
                    traits += (traits.empty() ? "" : ", ") + (special != nullptr ? special->name : id);
                }
            }
            if (!traits.empty())
            {
                Row("Notable", traits, true);
            }
        }

        const bool here = ours && !m_campaign.travel.underway && picked->index == m_campaign.body;
        const bool heading = ours && m_campaign.travel.underway && m_campaign.travel.target == picked->index;
        if (ours && !here)
        {
            const float distance = glm::length(system.Position(picked->index, m_campaign.clock) - Travel::ShipPosition(m_campaign, system));
            Section("FROM THE SHIP");
            Row("Distance", Number(distance, 2) + " AU");
            Row("Flight", About(Travel::Seconds(distance * 1.15f, DriveTier())));
        }

        // Places to go down.
        if (picked->Landable())
        {
            int areas = 0;
            for (int i = 0; i < static_cast<int>(picked->regions.size()); ++i)
            {
                areas += MapAreaKnown(m_mapSystem, *picked, i) ? 1 : 0;
            }
            Section("LANDING AREAS");
            Wrapped(areas > 0 ? ImVec4(0.85f, 0.88f, 0.9f, 1.0f) : kDimText, "%s",
                               areas > 0 ? (std::to_string(areas) + (areas == 1 ? " area known" : " areas known")).c_str()
                                         : scanned ? "None found yet." : "None charted. Go closer to look.");
        }

        ImGui::Spacing();
        ImGui::Spacing();
        if (ImGui::Button(picked->Landable() ? "View the surface   Enter" : "View it closer   Enter", {-1.0f, 32.0f}))
        {
            ShowMapBody(picked->index);
            ImGui::End();
            return;
        }
        if (!ours)
        {
            Wrapped(kDimText, "The ship is not in this system.");
            if (!(m_campaign.travel.interstellar && m_campaign.travel.toSystem == m_mapSystem))
            {
                const float reach = Travel::CrossingRange(DriveTier());
                const float lightYears = glm::length(system.galaxy - Travel::GalaxyPosition(m_campaign, m_universe));
                const bool canCross = reach > 0.0f && lightYears <= reach;
                ImGui::BeginDisabled(!aboard || m_cine.Active() || !canCross);
                if (ImGui::Button("Plot course for this system", {-1.0f, 32.0f}))
                {
                    AskSystemCourse(m_mapSystem);
                }
                ImGui::EndDisabled();
                if (reach <= 0.0f)
                {
                    Wrapped(kDimText, "Crossing between the stars needs an upgraded drive.");
                }
                else if (!canCross)
                {
                    Wrapped(kDimText, "Beyond the drive's reach of %d light years.", static_cast<int>(reach));
                }
            }
        }
        else if (here)
        {
            Wrapped(kAmberText, "%s", ShipLanded()   ? "The ship is landed here. Pick a place on the surface to go elsewhere."
                                                 : m_shipReady ? "In orbit. The shuttle is ready in the hangar."
                                                               : "In orbit.");
        }
        else if (heading)
        {
            Wrapped(kAmberText, "On course.");
        }
        else
        {
            const bool plotted = m_campaign.plan.set && !m_campaign.plan.toSystem && m_campaign.plan.body == picked->index;
            if (plotted)
            {
                Wrapped(kGoText, "Course plotted. Set out from the helm in the cockpit.");
            }
            ImGui::BeginDisabled(!aboard || m_cine.Active() || plotted || m_campaign.travel.interstellar);
            if (ImGui::Button(plotted ? "Plotted" : "Plot course", {-1.0f, 32.0f}))
            {
                // Down to the first area the shuttle can go down to, until one is picked on the surface.
                int region = -1;
                for (int i = 0; i < static_cast<int>(picked->regions.size()) && region < 0; ++i)
                {
                    region = RegionLandable(*picked, i) && !RegionIsPort(*picked, i) ? i : -1;
                }
                AskCampaign(CampaignAction::PlotCourse, picked->index, region);
                PlayNamed("UI/confirm", m_renderEye, 0.6f, 1.0f, false);
            }
            ImGui::EndDisabled();
            if (!aboard)
            {
                Wrapped(kDimText, "Courses are set aboard the ship.");
            }
        }
    }
    ImGui::End();
}

void PredationGame::DrawMapBodyPanels(const StarSystem& system)
{
    const Body* body = system.Find(m_mapSelected);
    if (body == nullptr)
    {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 origin = viewport->Pos;
    const ImVec2 size = viewport->Size;
    const bool ours = m_mapSystem == m_campaign.system && !m_campaign.travel.interstellar;
    const bool aboard = m_map == MapChoice::Ship;
    const bool here = ours && !m_campaign.travel.underway && m_campaign.body == body->index;
    const bool heading = ours && m_campaign.travel.underway && m_campaign.travel.target == body->index;
    const glm::vec3 sun = SunOverBody(system, *body);

    // The local time at a place: noon where the star is overhead.
    const auto localTime = [&](const LandingRegion& region)
    {
        const float sunLon = std::atan2(sun.z, sun.x);
        const float hourAngle = std::remainder(region.latLon.y * kDegrees - sunLon, kTau);
        float hours = 12.0f - hourAngle / kTau * 24.0f;
        hours = std::fmod(hours + 24.0f, 24.0f);
        char text[16];
        std::snprintf(text, sizeof(text), "%02d:%02d", static_cast<int>(hours), static_cast<int>(std::fmod(hours, 1.0f) * 60.0f));
        return std::string(text);
    };

    // Down the left: the areas known on it.
    ImGui::SetNextWindowPos({origin.x + 16.0f, origin.y + 74.0f});
    ImGui::SetNextWindowSize({310.0f, size.y - 74.0f - 70.0f});
    if (ImGui::Begin("##mapareas", nullptr, kPanel))
    {
        Title(body->name, body->kind == BodyKind::Moon ? "Moon of " + system.bodies[static_cast<size_t>(body->parent)].name : system.name);
        Section("LANDING AREAS");
        int known = 0;
        int hidden = 0;
        for (int i = 0; i < static_cast<int>(body->regions.size()); ++i)
        {
            if (!MapAreaKnown(m_mapSystem, *body, i))
            {
                ++hidden;
                continue;
            }
            ++known;
            const LandingRegion& region = body->regions[static_cast<size_t>(i)];
            ImGui::PushID(i);
            const ImVec2 corner = ImGui::GetCursorScreenPos();
            const float line = ImGui::GetTextLineHeight();
            if (ImGui::Selectable("##area", i == m_mapRegion, ImGuiSelectableFlags_None, {0.0f, line * 2.0f + 2.0f}))
            {
                m_mapRegion = i;
                m_mapView.FaceFrom(AreaOnGlobe(region) + glm::vec3(0.0f, 0.2f, 0.0f));
            }
            if (ImGui::IsItemHovered())
            {
                m_mapHoverRegion = i;
            }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const bool day = glm::dot(AreaOnGlobe(region), sun) > 0.0f;
            const bool chosen = (here || heading) && m_campaign.travel.region == i;
            draw->AddCircleFilled({corner.x + 7.0f, corner.y + line * 0.5f}, 4.0f, chosen ? kGo : day ? kText : kNight);
            draw->AddText({corner.x + 20.0f, corner.y}, i == m_mapRegion ? kAmber : kText, region.designation.c_str());
            const RegionKindDef* kind = m_universeData.RegionKind(region.kind);
            const std::string under = (kind != nullptr ? kind->name : region.kind) + "   " + (day ? "day " : "night ") + localTime(region);
            draw->AddText({corner.x + 20.0f, corner.y + line + 1.0f}, kDim, under.c_str());
            ImGui::PopID();
        }
        if (!body->Landable())
        {
            Wrapped(kDimText, "%s", body->gas ? "A gas giant: nothing to land on." : "Nothing to land on.");
        }
        else if (known == 0)
        {
            ImGui::TextWrapped("None found yet. The ship's sensors look for places to go down once it is in orbit.");
        }
        if (hidden > 0 && known > 0)
        {
            ImGui::Spacing();
            Wrapped(kDimText, "There may be more. Better sensors find more from orbit.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Back to the system", {-1.0f, 0.0f}))
        {
            ShowMapSystem(m_mapSystem, body->index);
        }
    }
    ImGui::End();

    // Down the right: the area picked out, and going there.
    if (m_mapRegion < 0 || m_mapRegion >= static_cast<int>(body->regions.size()) || !MapAreaKnown(m_mapSystem, *body, m_mapRegion))
    {
        return;
    }
    const LandingRegion& region = body->regions[static_cast<size_t>(m_mapRegion)];
    ImGui::SetNextWindowPos({origin.x + size.x - 16.0f - 340.0f, origin.y + 74.0f});
    ImGui::SetNextWindowSizeConstraints({340.0f, 0.0f}, {340.0f, 10000.0f});
    if (ImGui::Begin("##maparea", nullptr, kPanel | ImGuiWindowFlags_AlwaysAutoResize))
    {
        const RegionKindDef* kind = m_universeData.RegionKind(region.kind);
        Title(region.designation, kind != nullptr ? kind->name : region.kind);
        Section("THE AREA");
        Row("Location", LatLonText(region.latLon));
        const bool day = glm::dot(AreaOnGlobe(region), sun) > 0.0f;
        Row("Local time", localTime(region) + (day ? "   daylight" : "   night"));
        const uint8_t known = m_campaign.Known(m_mapSystem, body->index);
        const bool scanned = (known & (CampaignState::kKnownScanned | CampaignState::kKnownVisited)) != 0;
        const WeatherDef* weather = m_universeData.Weather(body->weather);
        Row("Weather", weather != nullptr ? weather->name : body->weather, scanned);
        Row("Temperature", std::to_string(static_cast<int>(std::round(body->temperature + (std::abs(region.latLon.x) > 55.0f ? -25.0f : 0.0f)))) + " C",
            scanned);
        ImGui::Spacing();
        ImGui::Spacing();
        const bool chosen = (here || heading) && m_campaign.travel.region == m_mapRegion;
        const bool port = RegionIsPort(*body, m_mapRegion);
        if (port)
        {
            Wrapped(kDimText, "An outpost: the ship itself lands here.");
            if (const std::string runs = OperatorOf(region); !runs.empty())
            {
                Row("Run by", runs, true);
            }
        }
        if (!ours)
        {
            Wrapped(kDimText, "The ship is not in this system. Cross to it first.");
        }
        else if (chosen && port)
        {
            Wrapped(kGoText, "%s", ShipLanded() ? "The ship is landed here." : "The ship lands here when it arrives.");
        }
        else if (chosen)
        {
            Wrapped(kGoText, "%s", here ? (m_shipReady ? "The shuttle goes down here. It is ready in the hangar." : "The shuttle goes down here.")
                                                     : "The shuttle goes down here when the ship arrives.");
        }
        else
        {
            ImGui::BeginDisabled(!aboard || m_cine.Active() || !RegionLandable(*body, m_mapRegion) || m_campaign.travel.interstellar);
            const bool plotted = m_campaign.plan.set && !m_campaign.plan.toSystem && m_campaign.plan.body == body->index && m_campaign.plan.region == m_mapRegion;
            const char* go = port ? (here || heading ? "Land the ship here" : plotted ? "Plotted" : "Plot course to land here")
                                  : (here || heading ? "Go down here" : plotted ? "Plotted" : "Plot course to go down here");
            if (ImGui::Button(go, {-1.0f, 32.0f}) && !plotted)
            {
                AskCampaign(here || heading ? CampaignAction::SetCourse : CampaignAction::PlotCourse, body->index, m_mapRegion);
                PlayNamed("UI/confirm", m_renderEye, 0.6f, 1.0f, false);
            }
            if (plotted)
            {
                Wrapped(kGoText, "Course plotted. Set out from the helm in the cockpit.");
            }
            ImGui::EndDisabled();
            if (!aboard)
            {
                Wrapped(kDimText, "Courses are set aboard the ship.");
            }
        }
    }
    ImGui::End();
}

} // namespace pred
