// Resources: deposits out on a site's ground, taken out of it a sample at a time (an item to carry), stowed in the ship's hold
// as the shuttle leaves with whoever carries them aboard, and sold at an outpost from the ship's terminal (Game/Campaign/Resources).
// A reason to go to a world besides a contract, and different worlds for different things.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Render/Primitives.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{

void PredationGame::LoadResourceRules()
{
    const std::filesystem::path file = Paths::AssetsRoot() / "Data" / "resources.json";
    std::string error;
    if (!m_resourceRules.LoadFromFile(file, &error))
    {
        PRED_LOG_WARN(Gameplay, "Resources: {} -- none on any site", error);
    }
    m_app->GetFileWatcher().Watch(file, [this](const std::filesystem::path& path)
                                  {
                                      std::string why;
                                      if (!m_resourceRules.LoadFromFile(path, &why))
                                      {
                                          PRED_LOG_WARN(Gameplay, "Resources: {}", why);
                                      }
                                  });
}

void PredationGame::ClearDeposits()
{
    for (const DepositProp& prop : m_depositProps)
    {
        m_interactions.Unregister(prop.rock);
        for (const Entity entity : prop.parts)
        {
            m_scene.Destroy(entity);
        }
    }
    m_depositProps.clear();
    m_deposits.clear();
}

void PredationGame::BuildDeposits()
{
    ClearDeposits();
    if (!m_facility.Built())
    {
        return;
    }
    const SiteMap::Look& look = m_facility.GetLook();
    m_deposits = m_resourceRules.Place(m_facility.Plan(), m_facility.Seed(), look.biome, m_missionPlan.markers);
    if (m_deposits.empty())
    {
        return;
    }
    // Each a low outcrop of the world's own rock, the stuff showing in it in its own colour: a few lumps and a few seams.
    MeshLibrary& meshes = m_app->GetMeshes();
    const MeshHandle lump = meshes.Upload(Primitives::Ellipsoid({0.5f, 0.32f, 0.42f}, 12, 8), "deposit_lump");
    const MeshHandle seam = meshes.Upload(Primitives::Box({0.12f, 0.34f, 0.5f}), "deposit_seam");
    Material rock = Material::Diffuse(look.rock * 0.9f, 0.9f);
    for (size_t i = 0; i < m_deposits.size(); ++i)
    {
        Deposit& deposit = m_deposits[i];
        deposit.at.y = m_facility.Terrain().Height(deposit.at.x, deposit.at.z);
        const ResourceDef* def = m_resourceRules.Find(deposit.key);
        Material vein = Material::Diffuse(def != nullptr ? def->color : glm::vec3(0.6f), 0.35f);
        vein.metallic = 0.3f;
        // (Ice and crystal catch the light; the rest only shows its colour.)
        if (deposit.key == "res_ice" || deposit.key == "res_crystal")
        {
            vein.emissive = vein.baseColor * 0.12f;
        }
        DepositProp prop;
        const glm::quat turn = glm::angleAxis(deposit.turn, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 lumps[4] = {{0.0f, 0.12f, 0.0f}, {0.62f, 0.02f, 0.25f}, {-0.48f, 0.0f, -0.35f}, {0.15f, -0.02f, -0.62f}};
        const float sizes[4] = {1.4f, 0.9f, 1.0f, 0.75f};
        for (int k = 0; k < 4; ++k)
        {
            Transform placed;
            placed.position = deposit.at + turn * lumps[k];
            placed.rotation = turn * glm::angleAxis(0.7f * static_cast<float>(k), glm::vec3(0.0f, 1.0f, 0.0f));
            placed.scale = glm::vec3(sizes[k]);
            const Entity part = m_scene.CreateMeshEntity("deposit_rock", placed, lump, rock);
            prop.parts.push_back(part);
            if (k == 0)
            {
                prop.rock = part;
            }
        }
        for (int k = 0; k < 3; ++k)
        {
            Transform placed;
            const float a = deposit.turn + static_cast<float>(k) * 2.1f;
            placed.position = deposit.at + glm::vec3(std::cos(a) * 0.38f, 0.28f, std::sin(a) * 0.38f);
            placed.rotation = glm::angleAxis(a, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(0.5f, glm::vec3(1.0f, 0.0f, 0.0f));
            const Entity part = m_scene.CreateMeshEntity("deposit_seam", placed, seam, vein);
            prop.parts.push_back(part);
            prop.seams.push_back(part);
        }
        for (const Entity part : prop.parts)
        {
            if (MeshRenderer* renderer = m_scene.GetMeshRenderer(part))
            {
                renderer->levelGeometry = true;
            }
        }
        Interactable interactable;
        interactable.entity = prop.rock;
        interactable.kind = InteractionKind::Deposit;
        interactable.payload = static_cast<int>(i);
        interactable.verb = "Extract";
        interactable.name = def != nullptr ? def->name : deposit.key;
        interactable.focusOffset = {0.0f, 0.25f, 0.0f};
        interactable.range = 2.6f;
        interactable.enabled = deposit.units > 0;
        m_interactions.Register(interactable);
        m_depositProps.push_back(std::move(prop));
    }
    PRED_LOG_INFO(Gameplay, "{} deposits on site {}; the first {} at {:.1f} {:.1f} {:.1f}", m_deposits.size(), m_facility.Seed(), m_deposits.front().key,
                  m_deposits.front().at.x, m_deposits.front().at.y, m_deposits.front().at.z);
}

void PredationGame::ShowDeposit(int index)
{
    if (index < 0 || index >= static_cast<int>(m_deposits.size()) || index >= static_cast<int>(m_depositProps.size()))
    {
        return;
    }
    // Worked out: its seams gone, nothing more to take.
    const bool left = m_deposits[static_cast<size_t>(index)].units > 0;
    for (const Entity seam : m_depositProps[static_cast<size_t>(index)].seams)
    {
        if (MeshRenderer* renderer = m_scene.GetMeshRenderer(seam))
        {
            renderer->visible = left;
        }
    }
    if (Interactable* interactable = m_interactions.Find(m_depositProps[static_cast<size_t>(index)].rock))
    {
        interactable->enabled = left;
    }
}

bool PredationGame::PerformDepositInteraction(int index, uint8_t player)
{
    // The host's: a sample broken out of it, set down on top of it to be picked up, until there is no more.
    if (!IsAuthority() || index < 0 || index >= static_cast<int>(m_deposits.size()))
    {
        return false;
    }
    Deposit& deposit = m_deposits[static_cast<size_t>(index)];
    const ItemId item = m_items.IdOf(deposit.key);
    if (deposit.units <= 0 || item == kInvalidItem)
    {
        return false;
    }
    --deposit.units;
    // On the ground beside it, on the side of whoever broke it out (its rocks are not solid: one set on top fell into them).
    glm::vec3 toward = PlayerPosition(player) - deposit.at;
    toward.y = 0.0f;
    toward = glm::length(toward) > 0.1f ? glm::normalize(toward) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 at = deposit.at + toward * 1.35f;
    at.y = m_facility.Terrain().Height(at.x, at.z) + 0.35f;
    const int pickup = m_world.SpawnPickup(m_scene, m_app->GetMeshes(), m_app->GetPhysics(), m_interactions, m_items, item, 1, at, toward * 0.6f + glm::vec3(0.0f, 0.8f, 0.0f));
    ShareSound("World/breaker", deposit.at, 0.6f);
    if (m_sessionMode == SessionMode::Host)
    {
        if (pickup >= 0)
        {
            WorldEventMessage spawned;
            spawned.kind = WorldEventKind::PickupSpawned;
            spawned.index = static_cast<uint8_t>(pickup);
            spawned.item = static_cast<uint16_t>(item);
            spawned.other = 1;
            spawned.position = at;
            m_host.Broadcast(spawned);
        }
        SendDeposit(index);
    }
    ShowDeposit(index);
    PRED_LOG_INFO(Gameplay, "Player {} took a sample of {} ({} left)", player, deposit.key, deposit.units);
    return true;
}

void PredationGame::SendDeposit(int index, int player)
{
    if (m_sessionMode != SessionMode::Host || index < 0 || index >= static_cast<int>(m_deposits.size()))
    {
        return;
    }
    WorldEventMessage event;
    event.kind = WorldEventKind::Deposit;
    event.index = static_cast<uint8_t>(index);
    event.other = static_cast<uint8_t>(std::max(m_deposits[static_cast<size_t>(index)].units, 0));
    event.quiet = true;
    if (player >= 0)
    {
        m_host.SendTo(static_cast<uint8_t>(player), event);
        return;
    }
    m_host.Broadcast(event);
}

void PredationGame::ApplyDepositEvent(const WorldEventMessage& event)
{
    if (event.index < m_deposits.size())
    {
        m_deposits[event.index].units = event.other;
        ShowDeposit(event.index);
    }
}

bool PredationGame::IsSample(ItemId item) const
{
    const ItemDefinition* def = m_items.Get(item);
    return def != nullptr && m_resourceRules.Find(def->key) != nullptr;
}

void PredationGame::StowSamples(uint8_t aboard)
{
    // Whoever is aboard as the shuttle goes: what they carry of what was taken out of the ground goes into the ship's hold. (Those
    // left behind keep theirs: it is lost with them.)
    int stowed = 0;
    const bool mineAboard = ((aboard >> LocalPlayerId()) & 1u) != 0;
    for (int slot = 0; mineAboard && slot < m_inventory.SlotCount(); ++slot)
    {
        const ItemId item = m_inventory.At(slot).item;
        const int count = m_inventory.At(slot).count;
        if (count > 0 && IsSample(item))
        {
            if (IsAuthority() && m_campaignOpen)
            {
                m_campaign.cargo[m_items.Get(item)->key] += count;
                stowed += count;
            }
            m_inventory.RemoveFromSlot(slot, count);
        }
    }
    if (m_sessionMode == SessionMode::Host)
    {
        for (const RemotePlayerView& remote : RemotePlayers())
        {
            if (((aboard >> remote.id) & 1u) == 0u)
            {
                continue;
            }
            for (const ResourceDef& def : m_resourceRules.All())
            {
                const ItemId item = m_items.IdOf(def.key);
                const int carried = item != kInvalidItem ? m_host.CarriedCount(remote.id, static_cast<uint16_t>(item)) : 0;
                if (carried > 0)
                {
                    m_host.TakeCarried(remote.id, static_cast<uint16_t>(item), carried);
                    if (m_campaignOpen)
                    {
                        m_campaign.cargo[def.key] += carried;
                        stowed += carried;
                    }
                }
            }
        }
    }
    if (stowed > 0 && IsAuthority() && m_campaignOpen)
    {
        PRED_LOG_INFO(Gameplay, "{} samples stowed in the hold ({} credits' worth there now)", stowed, m_resourceRules.ValueOf(m_campaign.cargo));
        CampaignChanged();
    }
}

bool PredationGame::CanSellCargo() const
{
    // At an outpost: somebody there to buy it.
    return m_campaignOpen && ShipLanded() && !Descending() && !m_campaign.cargo.empty();
}

void PredationGame::SellCargo()
{
    if (!IsAuthority() || !CanSellCargo())
    {
        return;
    }
    const int64_t paid = m_resourceRules.ValueOf(m_campaign.cargo);
    int units = 0;
    for (const auto& [key, count] : m_campaign.cargo)
    {
        units += count;
    }
    m_campaign.credits += paid;
    m_campaign.cargo.clear();
    m_campaign.AddLog("cargo", "", "Cargo sold", std::to_string(units) + " samples sold for " + std::to_string(paid) + " credits.");
    PRED_LOG_INFO(Gameplay, "Sold the hold: {} samples for {} credits", units, paid);
    PlayNamed("UI/confirm", m_renderEye, 0.6f, 1.0f, false);
    CampaignChanged();
}

} // namespace pred
