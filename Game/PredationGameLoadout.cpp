// The loadout locker: the gear room's one place for kit. Everybody chooses what they take down on a screen of their own,
// and draws it, handing back whatever they had from the locker before.

#include "Game/PredationGame.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Render/Primitives.h"
#include "Engine/Render/TextureLibrary.h"
#include "Game/World/ScreenCanvas.h"

#include <imgui.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace pred
{

namespace
{

CVar<std::string> cv_loadout{"game.loadout", std::string(), "The kit last drawn at the loadout locker, to start from next time",
                             CVarFlags::Archive};

// The locker's screen: its size on the locker's face and in pixels.
constexpr float kScreenW = 1.1f;
constexpr float kScreenH = 0.62f;
constexpr int kWide = 512;
constexpr int kHigh = 288;
// How far from its screen someone can be and still be using it.
constexpr float kLockerReach = 3.5f;

const Rgb kAmber{255, 170, 60};
const Rgb kAmberDim{120, 76, 24};
const Rgb kSoft{190, 196, 204};
const Rgb kFaint{90, 96, 104};

std::string Upper(std::string text)
{
    for (char& c : text)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

MeshData LockerQuad()
{
    MeshData mesh;
    const float w = kScreenW * 0.5f;
    const float h = kScreenH * 0.5f;
    const glm::vec3 normal{0.0f, 0.0f, 1.0f};
    mesh.vertices.push_back(MeshVertex{{-w, h, 0.0f}, normal, {0.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{w, h, 0.0f}, normal, {1.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{w, -h, 0.0f}, normal, {1.0f, 1.0f}});
    mesh.vertices.push_back(MeshVertex{{-w, -h, 0.0f}, normal, {0.0f, 1.0f}});
    mesh.indices = {0, 3, 2, 0, 2, 1, 0, 1, 2, 0, 2, 3};
    return mesh;
}

const ImVec4 kAmberText{1.0f, 0.67f, 0.24f, 1.0f};

} // namespace

void PredationGame::BuildLoadoutLocker()
{
    // Its screen: what it is, in letters big enough to read across the room, and what it gives out.
    ScreenCanvas canvas(kWide, kHigh);
    canvas.Clear({8, 10, 12});
    canvas.Fill(0.0f, 0.0f, static_cast<float>(kWide), 64.0f, {44, 26, 8});
    canvas.Text((kWide - canvas.TextWidth("LOADOUT", 6)) / 2, 11, "LOADOUT", kAmber, 6);
    canvas.Line(0.0f, 64.0f, static_cast<float>(kWide), 64.0f, kAmberDim, 2);
    const char* heading = "KIT ISSUE - ALL CREW";
    canvas.Text((kWide - canvas.TextWidth(heading, 2)) / 2, 78, heading, kSoft, 2);
    int row = 0;
    for (const ItemId id : Loadouts::Issued(m_items))
    {
        const ItemDefinition* item = m_items.Get(id);
        const int column = row % 2;
        const int line = row / 2;
        const int x = 24 + column * 248;
        const int y = 108 + line * 24;
        canvas.Fill(static_cast<float>(x), static_cast<float>(y + 4), static_cast<float>(x + 6), static_cast<float>(y + 10), kAmberDim);
        canvas.Text(x + 14, y, Upper(item->name).c_str(), kSoft, 2);
        ++row;
    }
    const char* footer = "CHOOSE YOUR KIT HERE";
    canvas.Fill(0.0f, static_cast<float>(kHigh - 40), static_cast<float>(kWide), static_cast<float>(kHigh), {30, 18, 6});
    canvas.Text((kWide - canvas.TextWidth(footer, 3)) / 2, kHigh - 31, footer, kAmber, 3);
    canvas.Box(1.0f, 1.0f, static_cast<float>(kWide - 2), static_cast<float>(kHigh - 2), kFaint);
    canvas.Lines();

    TextureLibrary& textures = m_app->GetTextures();
    m_loadoutTexture = textures.CreateDynamic(kWide, kHigh, "loadout_screen");
    textures.Update(m_loadoutTexture, canvas.image);

    const CinePose at = m_ship.LoadoutLocker();
    Transform transform;
    transform.position = at.position;
    transform.rotation = at.rotation;
    Material material = Material::Diffuse(glm::vec3(1.0f), 0.3f);
    material.baseColorTexture = m_loadoutTexture;
    material.emissive = glm::vec3(1.25f);
    material.emissiveTextured = true;
    m_loadoutScreen = m_scene.CreateMeshEntity("loadout_screen", transform, m_app->GetMeshes().Upload(LockerQuad(), "loadout_screen"),
                                               material);
    if (MeshRenderer* renderer = m_scene.GetMeshRenderer(m_loadoutScreen))
    {
        renderer->castsShadow = false;
    }

    Interactable interactable;
    interactable.entity = m_loadoutScreen;
    interactable.kind = InteractionKind::Loadout;
    interactable.verb = "Choose";
    interactable.name = "loadout";
    interactable.range = 2.6f;
    m_interactions.Register(interactable);
}

bool PredationGame::CarryingKit() const
{
    for (int i = 0; i < m_inventory.SlotCount(); ++i)
    {
        const ItemDefinition* item = m_items.Get(m_inventory.At(i).item);
        if (!m_inventory.At(i).IsEmpty() && item != nullptr && item->loadoutMax > 0)
        {
            return true;
        }
    }
    return false;
}

void PredationGame::OpenLoadout()
{
    // Starting from what was drawn last time (or the standard kit), cut to what there is room for beside whatever has
    // been found and is kept.
    Loadout start = Loadouts::FromText(m_items, cv_loadout.Get());
    if (start.empty())
    {
        start = Loadouts::Default(m_items);
    }
    start = Loadouts::Clamp(m_items, start, m_inventory.SlotCount() - Loadouts::KeptSlots(m_items, m_inventory));
    m_loadoutChoice.clear();
    for (const ItemId id : Loadouts::Issued(m_items))
    {
        const auto found = std::find_if(start.begin(), start.end(), [&](const LoadoutPick& pick) { return pick.item == id; });
        m_loadoutChoice.push_back({id, found != start.end() ? found->count : 0});
    }
    m_loadoutOpen = true;
    m_inventoryOpen = false;
    m_wantMouseCaptured = false;
    UpdateMouseCapture();
}

void PredationGame::CloseLoadout()
{
    if (!m_loadoutOpen)
    {
        return;
    }
    m_loadoutOpen = false;
    m_wantMouseCaptured = !m_paused;
    UpdateMouseCapture();
}

void PredationGame::DrawKit(const Loadout& kit)
{
    // Whatever is in the hands is put down first: it may be going back in the locker.
    m_inventory.SelectSlot(Inventory::kNoSlot);
    m_weapon = WeaponState{};
    m_ammoSlot = Inventory::kNoSlot;
    const Loadout drawn = Loadouts::Draw(m_items, m_inventory, kit);
    cv_loadout.Set(Loadouts::ToText(m_items, kit));
    if (m_sessionMode == SessionMode::Client)
    {
        LoadoutMessage message;
        for (const LoadoutPick& pick : drawn)
        {
            message.picks.push_back({static_cast<uint16_t>(pick.item), static_cast<uint8_t>(pick.count)});
        }
        m_client.SendLoadout(message);
    }
    PlaySound(m_sounds.locker.Pick(), m_ship.LoadoutLocker().position, 0.6f);
    PRED_LOG_INFO(Gameplay, "Drew a kit: {}", Loadouts::ToText(m_items, drawn));
    m_app->GetConsole().Print("Drew " + (drawn.empty() ? std::string("nothing") : Loadouts::ToText(m_items, drawn)));
}

void PredationGame::ServeLoadout(uint8_t player, const LoadoutMessage& message)
{
    // At the locker, or near enough allowing for the time it took to arrive.
    const glm::vec3 at = PlayerPosition(player);
    const glm::vec3 locker = m_ship.LoadoutLocker().position;
    if (glm::length(glm::vec2(at.x - locker.x, at.z - locker.z)) > kLockerReach + 2.0f || std::abs(at.y - locker.y) > 3.0f)
    {
        PRED_LOG_WARN(Network, "Player {} drew a kit away from the locker; ignored", player);
        return;
    }
    Loadout kit;
    for (const LoadoutMessage::Pick& pick : message.picks)
    {
        kit.push_back({static_cast<ItemId>(pick.item), pick.count});
    }
    kit = Loadouts::Clamp(m_items, kit, m_inventory.SlotCount());
    // Everything from the locker they had before went back into it: what they have of each is what they drew.
    for (const ItemId id : Loadouts::Issued(m_items))
    {
        const auto found = std::find_if(kit.begin(), kit.end(), [&](const LoadoutPick& pick) { return pick.item == id; });
        m_host.SetCarried(player, static_cast<uint16_t>(id), found != kit.end() ? found->count : 0);
    }
    PlaySound(m_sounds.locker.Pick(), locker, 0.6f);
}

void PredationGame::DrawLoadoutPanel()
{
    // Walked away from it, or something else has the picture: it closes.
    const glm::vec3 locker = m_ship.LoadoutLocker().position;
    if (m_screen != Screen::Playing || m_cine.Active() || !m_player.State().alive ||
        glm::distance(m_player.State().position + glm::vec3(0.0f, 1.5f, 0.0f), locker) > kLockerReach)
    {
        CloseLoadout();
        return;
    }

    const int slots = m_inventory.SlotCount();
    const int kept = Loadouts::KeptSlots(m_items, m_inventory);
    const int room = slots - kept;
    const int used = Loadouts::SlotsFor(m_items, m_loadoutChoice);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f}, ImGuiCond_Always,
                            {0.5f, 0.5f});
    ImGui::SetNextWindowSize({600.0f, 0.0f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (!ImGui::Begin("##Loadout", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoMove))
    {
        ImGui::End();
        return;
    }
    ImDrawList* list = ImGui::GetWindowDrawList();

    // The heading, and how full the bag will be: a box a slot, lit for each one the kit takes and grey for each taken by
    // something found.
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextColored(kAmberText, "LOADOUT");
    ImGui::SetWindowFontScale(1.0f);
    {
        constexpr float kBox = 16.0f;
        constexpr float kGap = 4.0f;
        const float width = static_cast<float>(slots) * (kBox + kGap) - kGap;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        const float top = ImGui::GetWindowPos().y + ImGui::GetStyle().WindowPadding.y + 4.0f;
        const std::string label = std::to_string(used + kept) + " / " + std::to_string(slots) + " slots";
        const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
        list->AddText({right - width - labelWidth - 10.0f, top}, IM_COL32(200, 204, 212, 255), label.c_str());
        for (int i = 0; i < slots; ++i)
        {
            const float x = right - width + static_cast<float>(i) * (kBox + kGap);
            const ImU32 colour = i < kept ? IM_COL32(120, 124, 132, 255) : i < kept + used ? IM_COL32(255, 170, 60, 255) : IM_COL32(40, 42, 48, 255);
            list->AddRectFilled({x, top}, {x + kBox, top + kBox}, colour, 2.0f);
        }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Choose what you take down. It replaces whatever you had from here; weapons come loaded.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();

    constexpr float kIcon = 52.0f;
    for (LoadoutPick& pick : m_loadoutChoice)
    {
        const ItemDefinition* item = m_items.Get(pick.item);
        if (item == nullptr)
        {
            continue;
        }
        ImGui::PushID(static_cast<int>(pick.item));
        const ImVec2 rowStart = ImGui::GetCursorScreenPos();
        if (pick.count > 0)
        {
            list->AddRectFilled({rowStart.x - 4.0f, rowStart.y - 2.0f}, {rowStart.x + ImGui::GetContentRegionAvail().x + 4.0f, rowStart.y + kIcon + 2.0f},
                                IM_COL32(255, 170, 60, 22), 3.0f);
        }
        DrawItemIcon(pick.item, kIcon);
        ImGui::Dummy({kIcon, kIcon});
        ImGui::SameLine(0.0f, 12.0f);
        ImGui::BeginGroup();
        ImGui::TextColored(pick.count > 0 ? ImVec4{0.94f, 0.95f, 0.97f, 1.0f} : ImVec4{0.6f, 0.62f, 0.66f, 1.0f}, "%s", item->name.c_str());
        std::string detail = item->blurb;
        if (const WeaponDefinition* weapon = m_weaponData.Get(m_weaponData.ForItem(item->key)))
        {
            detail += " " + std::to_string(weapon->magazineSize) + " in the magazine, " + std::to_string(weapon->reserveOnPickup) + " spare.";
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
        ImGui::TextDisabled("%s", detail.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        // Fewer, how many of the most there may be, more. More is only there while it fits: another of something already
        // taken may fit in its stack where a new thing would need a slot.
        const float controls = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - 118.0f;
        const float middle = rowStart.y + kIcon * 0.5f - ImGui::GetFrameHeight() * 0.5f;
        ImGui::SetCursorScreenPos({controls, middle});
        ImGui::BeginDisabled(pick.count <= 0);
        if (ImGui::Button("-", {26.0f, 0.0f}))
        {
            --pick.count;
        }
        ImGui::EndDisabled();
        const std::string count = std::to_string(pick.count) + " / " + std::to_string(item->loadoutMax);
        ImGui::SetCursorScreenPos({controls + 59.0f - ImGui::CalcTextSize(count.c_str()).x * 0.5f, middle});
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(count.c_str());
        ImGui::SetCursorScreenPos({controls + 92.0f, middle});
        const bool needsSlot = pick.count % item->maxStack == 0;
        ImGui::BeginDisabled(pick.count >= item->loadoutMax || (needsSlot && used >= room));
        if (ImGui::Button("+", {26.0f, 0.0f}))
        {
            ++pick.count;
        }
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos({rowStart.x, rowStart.y + kIcon + 8.0f});
        ImGui::Dummy({0.0f, 0.0f});
        ImGui::PopID();
    }

    // What was found and is not the locker's: it stays, and it is taking up room.
    if (kept > 0)
    {
        ImGui::Separator();
        std::string names;
        for (int i = 0; i < slots; ++i)
        {
            const ItemDefinition* item = m_items.Get(m_inventory.At(i).item);
            if (!m_inventory.At(i).IsEmpty() && item != nullptr && item->loadoutMax <= 0)
            {
                names += (names.empty() ? "" : ", ") + item->name;
            }
        }
        ImGui::TextDisabled("Kept (found, not from here): %s", names.c_str());
    }

    ImGui::Separator();
    if (ImGui::Button("Draw kit", {140.0f, 30.0f}))
    {
        Loadout kit;
        for (const LoadoutPick& pick : m_loadoutChoice)
        {
            if (pick.count > 0)
            {
                kit.push_back(pick);
            }
        }
        DrawKit(kit);
        CloseLoadout();
    }
    ImGui::SameLine();
    if (ImGui::Button("Standard kit", {120.0f, 30.0f}))
    {
        const Loadout standard = Loadouts::Clamp(m_items, Loadouts::Default(m_items), room);
        for (LoadoutPick& pick : m_loadoutChoice)
        {
            const auto found = std::find_if(standard.begin(), standard.end(), [&](const LoadoutPick& p) { return p.item == pick.item; });
            pick.count = found != standard.end() ? found->count : 0;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {90.0f, 30.0f}))
    {
        CloseLoadout();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  Esc closes");
    ImGui::End();
}

void PredationGame::RegisterLoadoutCommands()
{
    m_app->GetConsole().RegisterCommand("loadout", "Open the loadout locker's screen, wherever you are standing: loadout",
                            [this](const std::vector<std::string>&) { OpenLoadout(); });
    m_app->GetConsole().RegisterCommand(
        "loadout_draw", "Draw a kit as the locker would: loadout_draw <medkit:2,battery:2,...|standard>",
        [this](const std::vector<std::string>& args)
        {
            const std::string text = args.size() >= 2 ? args[1] : std::string("standard");
            DrawKit(text == "standard" ? Loadouts::Default(m_items) : Loadouts::FromText(m_items, text));
        },
        "loadout_draw <kit|standard>");
}

} // namespace pred
