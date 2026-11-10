#pragma once

#include "Game/Campaign/Campaign.h"
#include "Game/World/ScreenCanvas.h"

#include <functional>
#include <string>
#include <vector>

namespace pred
{

// The ship's terminal (Phase 4): a screen on the ops room's wall where the crew picks contracts and reads the ship's log -- its
// leads, its entries, the last expedition's debrief. Worked up close with the pointer (the camera brought to it, as a site's
// devices are); everybody else sees the same page on the wall from across the room, since what it shows is the campaign and a
// page, a row and a scroll the host keeps for everybody.
//
// Drawn on the CPU into a picture (ScreenCanvas) the size of its texture; a click is a point on that picture, answered from the
// buttons as last drawn. Nothing here needs the engine.
class ShipTerminal
{
public:
    static constexpr int kWide = 768;
    static constexpr int kHigh = 456;

    enum class Page : uint8_t
    {
        Contracts,
        Leads,
        Debrief,
        Cargo,
        Count
    };
    // What everybody sees: the page, its row picked out, and how far down its list is scrolled.
    struct View
    {
        Page page = Page::Contracts;
        int selected = 0;
        int scroll = 0;
        bool operator==(const View&) const = default;
    };
    // What a click on it asks for.
    struct Press
    {
        enum class Kind : uint8_t
        {
            None,
            Page,   // value: the page
            Select, // value: the row
            Scroll, // value: -1 up, 1 down
            Accept, // value: the contract taken on (and its course plotted)
            Plot,   // value: a contract's id, or with `lead` a lead's log entry: its course plotted
            Sell    // the hold sold (at an outpost)
        };
        Kind kind = Kind::None;
        int value = 0;
        bool lead = false;
    };
    // How what it shows is put into words: a place by its system, body and area; a world by its system and body; what a contract
    // asks for and the scan's read of its area; how a mission came out; somebody by their number; and whether a course is plotted
    // to an area now.
    struct Words
    {
        std::function<std::string(uint64_t, int, int)> area;
        std::function<std::string(uint64_t, int)> world;
        std::function<std::string(const std::string&)> wanted;
        std::function<std::string(uint64_t, int, int)> read;
        // How hard it looks (Difficulty): its rating, and the reasons.
        std::function<std::string(uint64_t, int, int)> difficulty;
        std::function<std::vector<std::string>(uint64_t, int, int)> difficultyWhy;
        std::function<std::string(const std::string&, bool)> result;
        std::function<std::string(int)> player;
        std::function<bool(uint64_t, int, int)> plotted;
        // The hold's: what each thing in it is called, what a unit of it fetches, and whether it can be sold here (at an outpost).
        std::function<std::string(const std::string&)> resourceName;
        std::function<int(const std::string&)> resourceValue;
        std::function<bool()> canSell;
    };

    // The page drawn; `hover` is a point on the picture under the pointer (none: below nought), lit where it is over a button.
    void Draw(ScreenCanvas& canvas, const CampaignState& campaign, const View& view, const Words& words, int hoverX = -1, int hoverY = -1);
    // What a click at a point on the picture presses, from the buttons as last drawn.
    Press Hit(int x, int y) const;

    // The rows of a page, in the order it lists them: contracts open first, then the last few done; the leads; the log, newest
    // first; what is in the hold. (The debrief has none.)
    static std::vector<const CampaignState::Contract*> ContractRows(const CampaignState& campaign);
    static std::vector<const CampaignState::LogEntry*> LeadRows(const CampaignState& campaign);
    static std::vector<std::pair<std::string, int>> CargoRows(const CampaignState& campaign);
    static int RowCount(const CampaignState& campaign, Page page);
    // How many rows show at once.
    static constexpr int kRows = 8;
    // A view kept inside its page's rows.
    static View Clamped(const CampaignState& campaign, View view);

private:
    struct Button
    {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        Press press;
    };
    std::vector<Button> m_buttons;
};

} // namespace pred
