// The title's campaign pages: beginning a campaign, choosing one to carry on with, and the lobby everybody waits in before
// the game starts. The title's own menu and the page for joining somebody else's game are in PredationGame.cpp.

#include "Game/PredationGame.h"

#include "Engine/Core/Log.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <random>
#include <string>

namespace pred
{

namespace
{

constexpr ImVec4 kAmberText{0.92f, 0.62f, 0.25f, 1.0f};
constexpr ImVec4 kWarningText{0.90f, 0.55f, 0.35f, 1.0f};

std::string PlayedFor(double seconds)
{
    const int minutes = static_cast<int>(seconds / 60.0);
    if (minutes < 60)
    {
        return std::to_string(minutes) + " min played";
    }
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min played";
}

std::string SavedAgo(int64_t savedAt)
{
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t ago = std::max<int64_t>(now - savedAt, 0);
    if (ago < 60)
    {
        return "saved just now";
    }
    if (ago < 3600)
    {
        return "saved " + std::to_string(ago / 60) + " min ago";
    }
    if (ago < 86400)
    {
        return "saved " + std::to_string(ago / 3600) + " h ago";
    }
    return "saved " + std::to_string(ago / 86400) + " days ago";
}

// A heading as the title sets one: spaced capitals, with an amber rule under it.
void Heading(const char* text)
{
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(text);
    ImGui::SetWindowFontScale(1.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled({at.x, at.y + 2.0f}, {at.x + 56.0f, at.y + 4.0f}, IM_COL32(230, 150, 60, 230));
    ImGui::Dummy({1.0f, 10.0f});
}

void Label(const char* text)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", text);
    ImGui::SameLine(110.0f);
}

} // namespace

std::string PredationGame::WhereIs(uint64_t universeSeed, uint64_t system, int body, bool underway, bool landed)
{
    // Worked out from the seed, as everything in a universe is: the system the save says, and the body in it.
    Universe scratch;
    Universe* universe = &m_universe;
    if (universeSeed != m_universe.Seed() || m_universe.Data() == nullptr)
    {
        scratch.Reset(universeSeed, &m_universeData);
        universe = &scratch;
    }
    const StarSystem* found = universe->System(system);
    if (found == nullptr)
    {
        return "Somewhere uncharted";
    }
    const Body* at = found->Find(body);
    if (underway)
    {
        return at != nullptr ? "Under way to " + at->name : "Under way in " + found->name;
    }
    if (at != nullptr && landed)
    {
        return "Landed on " + at->name;
    }
    return at != nullptr ? "In orbit of " + at->name : "Between the planets of " + found->name;
}

void PredationGame::ReadCampaignSlots()
{
    if (m_campaignSlotsRead)
    {
        return;
    }
    m_campaignSlots = m_campaignStore->List();
    m_campaignSlotsRead = true;
    m_campaignChoice = m_campaignSlots.empty() ? -1 : 0;
    m_campaignDeleteArmed.clear();
    m_campaignSlotWhere.clear();
    for (const CampaignSlot& slot : m_campaignSlots)
    {
        m_campaignSlotWhere.push_back(WhereIs(slot.universeSeed, slot.system, slot.body, slot.underway, slot.landed && !slot.underway));
    }
}

void PredationGame::DrawHostingChoice()
{
    // Who can come in, and what the game is called in their list.
    ImGui::Spacing();
    ImGui::TextDisabled("WHO CAN JOIN");
    if (ImGui::RadioButton("Only people I give the code to", !m_listPublicly))
    {
        m_listPublicly = false;
    }
    ImGui::BeginDisabled(!LobbyServerConfigured());
    if (ImGui::RadioButton("Anyone: show it in the public games", m_listPublicly))
    {
        m_listPublicly = true;
    }
    ImGui::EndDisabled();
    if (m_listPublicly)
    {
        Label("Game name");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##lobbyname", LobbyName().c_str(), m_lobbyName, sizeof(m_lobbyName));
    }
    ImGui::TextDisabled("Playing alone? Start the game from the lobby when it opens.");
}

void PredationGame::HostChosenCampaign()
{
    std::string error;
    if (!OpenChosenCampaign(error))
    {
        m_titleStatus = "That campaign could not be opened: " + error;
        return;
    }
    StartHosting();
    if (m_sessionMode != SessionMode::Host)
    {
        CloseCampaign();
    }
    m_campaignSlotsRead = false;
}

void PredationGame::DrawTitleNewCampaign()
{
    const ImVec2 wide{-1.0f, 38.0f};
    Heading("NEW CAMPAIGN");
    ImGui::TextDisabled("A ship of your own, a system to start in, and as far as you care to go.");
    ImGui::Spacing();
    Label("Name");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##campaignname", "Campaign", m_newCampaignName, sizeof(m_newCampaignName));
    Label("Seed");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##campaignseed", "random", m_newCampaignSeed, sizeof(m_newCampaignSeed), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::Dummy({0.0f, 0.0f});
    ImGui::TextDisabled("The same seed is the same universe. Leave it empty for a new one.");
    DrawHostingChoice();
    ImGui::Spacing();
    if (ImGui::Button("Create it and open the lobby", wide))
    {
        m_campaignChoice = -1;
        HostChosenCampaign();
    }
}

void PredationGame::DrawTitleLoadCampaign()
{
    const ImVec2 wide{-1.0f, 38.0f};
    ReadCampaignSlots();
    Heading("LOAD CAMPAIGN");
    if (m_campaignSlots.empty())
    {
        ImGui::TextDisabled("No campaigns saved on this computer yet.");
        return;
    }
    // A card a campaign: its name, where the ship is, and how long it has gone on.
    const float cardHeight = ImGui::GetTextLineHeightWithSpacing() * 3.0f + 10.0f;
    const float listHeight = std::min(static_cast<float>(m_campaignSlots.size()), 4.0f) * (cardHeight + 4.0f) + 6.0f;
    if (ImGui::BeginChild("##campaigncards", {-1.0f, listHeight}, ImGuiChildFlags_None))
    {
        for (size_t i = 0; i < m_campaignSlots.size(); ++i)
        {
            const CampaignSlot& slot = m_campaignSlots[i];
            ImGui::PushID(static_cast<int>(i));
            const bool chosen = m_campaignChoice == static_cast<int>(i);
            const ImVec2 corner = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##card", chosen, ImGuiSelectableFlags_AllowDoubleClick, {0.0f, cardHeight}))
            {
                m_campaignChoice = static_cast<int>(i);
                m_campaignDeleteArmed.clear();
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    HostChosenCampaign();
                    ImGui::PopID();
                    ImGui::EndChild();
                    return;
                }
            }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            if (chosen)
            {
                draw->AddRectFilled({corner.x, corner.y}, {corner.x + 3.0f, corner.y + cardHeight}, IM_COL32(230, 150, 60, 230));
            }
            const float line = ImGui::GetTextLineHeightWithSpacing();
            draw->AddText({corner.x + 14.0f, corner.y + 5.0f}, IM_COL32(232, 236, 238, 255), slot.name.c_str());
            const std::string where = (i < m_campaignSlotWhere.size() ? m_campaignSlotWhere[i] : std::string()) + "   " +
                                      std::to_string(slot.credits) + " credits";
            draw->AddText({corner.x + 14.0f, corner.y + 5.0f + line}, IM_COL32(170, 178, 184, 255), where.c_str());
            const std::string when = PlayedFor(slot.played) + ", " + SavedAgo(slot.savedAt) + (slot.newestIsAutosave ? " (autosave)" : "");
            draw->AddText({corner.x + 14.0f, corner.y + 5.0f + line * 2.0f}, IM_COL32(120, 128, 134, 255), when.c_str());
            ImGui::Dummy({0.0f, 4.0f});
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (m_campaignChoice >= 0 && m_campaignChoice < static_cast<int>(m_campaignSlots.size()))
    {
        // Deleting is for good: the button asks twice.
        const CampaignSlot& slot = m_campaignSlots[static_cast<size_t>(m_campaignChoice)];
        const bool armed = m_campaignDeleteArmed == slot.folder;
        ImGui::PushStyleColor(ImGuiCol_Text, armed ? kWarningText : ImVec4(0.6f, 0.63f, 0.66f, 1.0f));
        if (ImGui::SmallButton(armed ? "Delete it for good? Click again" : "Delete this campaign"))
        {
            if (armed)
            {
                m_campaignStore->Delete(slot.folder);
                m_campaignSlotsRead = false;
            }
            else
            {
                m_campaignDeleteArmed = slot.folder;
            }
        }
        ImGui::PopStyleColor();
    }
    DrawHostingChoice();
    ImGui::Spacing();
    ImGui::BeginDisabled(m_campaignChoice < 0);
    if (ImGui::Button("Open the lobby", wide))
    {
        HostChosenCampaign();
    }
    ImGui::EndDisabled();
}

void PredationGame::DrawLobby()
{
    const ImVec2 wide{-1.0f, 34.0f};

    // A guest still finding its way to the host, which can take a few seconds and can fail.
    if (m_joinTransport != nullptr)
    {
        Heading("JOINING");
        switch (m_lobby.Status())
        {
        case LobbyClient::State::Punching:
            ImGui::Text("Found \"%s\". Connecting to the host...", m_lobby.LobbyName().c_str());
            break;
        case LobbyClient::State::Failed:
            ImGui::TextColored(kWarningText, "%s", m_lobby.Message().c_str());
            break;
        default:
            ImGui::TextUnformatted("Looking for that game...");
            break;
        }
        ImGui::Spacing();
        if (ImGui::Button("Back", wide))
        {
            StopSession();
        }
        return;
    }

    const bool hosting = m_sessionMode == SessionMode::Host;
    // Whose game, and which campaign: what the ship is doing, and how far the crew has come.
    Heading(hosting ? "LOBBY" : "LOBBY");
    if (m_campaignOpen)
    {
        ImGui::SetWindowFontScale(1.2f);
        ImGui::TextUnformatted(m_campaign.name.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s   %lld credits   %s", WhereIs(m_campaign.universeSeed, m_campaign.system, m_campaign.travel.underway ? m_campaign.travel.target : m_campaign.body,
                                                              m_campaign.travel.underway, ShipLanded()).c_str(),
                            static_cast<long long>(m_campaign.credits), PlayedFor(m_campaign.played).c_str());
        if (!hosting)
        {
            ImGui::TextDisabled("%s's campaign", m_client.NameOf(0).c_str());
        }
    }
    else if (!hosting)
    {
        ImGui::Text("%s's game", m_client.NameOf(0).c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Who is here, and -- for the host -- how to let the rest in.
    if (ImGui::BeginTable("##lobbycolumns", hosting ? 2 : 1, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextColumn();
        DrawLobbyPlayers();
        if (hosting)
        {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("INVITE");
            DrawInvite();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    DrawLobbyRules();
    ImGui::Spacing();

    if (hosting)
    {
        if (ImGui::Button("Start", {-1.0f, 42.0f}))
        {
            StartTheGame();
            return;
        }
        ImGui::Spacing();
        if (ImGui::Button("Close the lobby", wide))
        {
            StopSession();
            CloseCampaign();
            m_titlePage = TitlePage::Root;
        }
    }
    else
    {
        ImGui::TextDisabled("Waiting for %s to start the game...", m_client.NameOf(0).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Leave", wide))
        {
            StopSession();
            m_titlePage = TitlePage::Browse;
        }
    }

    if (!m_titleStatus.empty())
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kWarningText, "%s", m_titleStatus.c_str());
        ImGui::PopTextWrapPos();
    }
}

void PredationGame::ContinueCampaign()
{
    ReadCampaignSlots();
    if (m_campaignSlots.empty())
    {
        m_titlePage = TitlePage::NewCampaign;
        return;
    }
    m_campaignChoice = 0;
    HostChosenCampaign();
}

} // namespace pred
