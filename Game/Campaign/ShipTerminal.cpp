#include "Game/Campaign/ShipTerminal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace pred
{
namespace
{

const Rgb kBack{7, 11, 13};
const Rgb kPanel{13, 19, 22};
const Rgb kHeader{26, 17, 7};
const Rgb kAmber{255, 176, 64};
const Rgb kAmberDim{110, 74, 30};
const Rgb kPicked{44, 30, 10};
const Rgb kText{214, 222, 226};
const Rgb kDim{112, 124, 130};
const Rgb kFaint{48, 58, 64};
const Rgb kGreen{98, 226, 150};
const Rgb kRed{240, 92, 72};

constexpr int kListX0 = 14;
constexpr int kListX1 = 296;
constexpr int kTop = 80;
constexpr int kRowHigh = 40;
constexpr int kDetailX0 = 312;
constexpr int kDetailX1 = ShipTerminal::kWide - 14;
constexpr int kBottom = ShipTerminal::kHigh - 12;

// As the screen's letters have it: capitals, figures and a few marks; anything else left out.
std::string Up(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        const char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (Glyph(u) != nullptr || u == ' ')
        {
            out.push_back(u);
        }
    }
    return out;
}

// Cut to so many letters, with dots where it was cut.
std::string Fit(const std::string& text, int letters)
{
    return static_cast<int>(text.size()) <= letters ? text : text.substr(0, static_cast<size_t>(std::max(letters - 2, 0))) + "..";
}

// Into lines of so many letters at most, broken between words.
std::vector<std::string> Wrap(const std::string& text, int letters)
{
    std::vector<std::string> lines;
    std::string line;
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find(' ', at);
        end = end == std::string::npos ? text.size() : end;
        std::string word = text.substr(at, end - at);
        at = end + 1;
        while (static_cast<int>(word.size()) > letters)
        {
            if (!line.empty())
            {
                lines.push_back(line);
                line.clear();
            }
            lines.push_back(word.substr(0, static_cast<size_t>(letters)));
            word = word.substr(static_cast<size_t>(letters));
        }
        if (line.empty())
        {
            line = word;
        }
        else if (static_cast<int>(line.size() + 1 + word.size()) <= letters)
        {
            line += " " + word;
        }
        else
        {
            lines.push_back(line);
            line = word;
        }
    }
    if (!line.empty())
    {
        lines.push_back(line);
    }
    return lines;
}

// The campaign's clock as the log keeps it: the day, and the time on it.
std::string Stamp(double clock)
{
    const long long seconds = std::max(static_cast<long long>(clock), 0LL);
    char text[32];
    std::snprintf(text, sizeof(text), "DAY %lld  %02lld:%02lld", seconds / 86400 + 1, (seconds / 3600) % 24, (seconds / 60) % 60);
    return text;
}

bool Visited(const CampaignState& campaign, const std::string& key)
{
    const auto found = campaign.regionChanges.find(key);
    return found != campaign.regionChanges.end() && found->second.is_object() && found->second.value("visited", false);
}

// A lead's area, from its log entry's subject ("system:body:region"); false when it is not one.
bool LeadArea(const CampaignState::LogEntry& entry, uint64_t& system, int& body, int& region)
{
    unsigned long long s = 0;
    if (std::sscanf(entry.subject.c_str(), "%llu:%d:%d", &s, &body, &region) != 3)
    {
        return false;
    }
    system = s;
    return true;
}

} // namespace

std::vector<const CampaignState::Contract*> ShipTerminal::ContractRows(const CampaignState& campaign)
{
    using Status = CampaignState::Contract::Status;
    std::vector<const CampaignState::Contract*> rows;
    for (const CampaignState::Contract& contract : campaign.contracts)
    {
        if (contract.status != Status::Done)
        {
            rows.push_back(&contract);
        }
    }
    // Taken on first, then on offer, each newest first; then the last few done, newest first.
    std::stable_sort(rows.begin(), rows.end(), [](const CampaignState::Contract* a, const CampaignState::Contract* b)
                     { return a->status != b->status ? a->status == Status::Accepted : a->offered > b->offered; });
    std::vector<const CampaignState::Contract*> done;
    for (const CampaignState::Contract& contract : campaign.contracts)
    {
        if (contract.status == Status::Done)
        {
            done.push_back(&contract);
        }
    }
    std::sort(done.begin(), done.end(), [](const CampaignState::Contract* a, const CampaignState::Contract* b) { return a->done > b->done; });
    for (size_t i = 0; i < done.size() && i < 5; ++i)
    {
        rows.push_back(done[i]);
    }
    return rows;
}

std::vector<const CampaignState::LogEntry*> ShipTerminal::LeadRows(const CampaignState& campaign)
{
    std::vector<const CampaignState::LogEntry*> rows;
    for (auto it = campaign.log.rbegin(); it != campaign.log.rend(); ++it)
    {
        if (it->kind == "lead")
        {
            rows.push_back(&*it);
        }
    }
    // Those not yet been to first.
    std::stable_sort(rows.begin(), rows.end(), [&campaign](const CampaignState::LogEntry* a, const CampaignState::LogEntry* b)
                     { return !Visited(campaign, a->subject) && Visited(campaign, b->subject); });
    return rows;
}

std::vector<std::pair<std::string, int>> ShipTerminal::CargoRows(const CampaignState& campaign)
{
    std::vector<std::pair<std::string, int>> rows;
    for (const auto& [key, count] : campaign.cargo)
    {
        if (count > 0)
        {
            rows.push_back({key, count});
        }
    }
    return rows;
}

int ShipTerminal::RowCount(const CampaignState& campaign, Page page)
{
    switch (page)
    {
    case Page::Contracts: return static_cast<int>(ContractRows(campaign).size());
    case Page::Leads: return static_cast<int>(LeadRows(campaign).size());
    case Page::Cargo: return static_cast<int>(CargoRows(campaign).size());
    case Page::Debrief:
    case Page::Count: break;
    }
    return 0;
}

ShipTerminal::View ShipTerminal::Clamped(const CampaignState& campaign, View view)
{
    view.page = static_cast<Page>(std::min(static_cast<int>(view.page), static_cast<int>(Page::Count) - 1));
    const int rows = RowCount(campaign, view.page);
    view.selected = std::clamp(view.selected, 0, std::max(rows - 1, 0));
    view.scroll = std::clamp(view.scroll, 0, std::max(rows - kRows, 0));
    return view;
}

ShipTerminal::Press ShipTerminal::Hit(int x, int y) const
{
    for (auto it = m_buttons.rbegin(); it != m_buttons.rend(); ++it)
    {
        if (x >= it->x0 && x <= it->x1 && y >= it->y0 && y <= it->y1)
        {
            return it->press;
        }
    }
    return {};
}

void ShipTerminal::Draw(ScreenCanvas& canvas, const CampaignState& campaign, const View& shown, const Words& words, int hoverX, int hoverY)
{
    using Status = CampaignState::Contract::Status;
    m_buttons.clear();
    const View view = Clamped(campaign, shown);
    const auto over = [&](int x0, int y0, int x1, int y1) { return hoverX >= x0 && hoverX <= x1 && hoverY >= y0 && hoverY <= y1; };
    const auto box = [&](int x0, int y0, int x1, int y1, Rgb colour)
    { canvas.Box(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1), colour); };
    const auto fill = [&](int x0, int y0, int x1, int y1, Rgb colour)
    { canvas.Fill(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1), colour); };
    const auto text = [&](int x, int y, const std::string& line, Rgb colour, int scale) { canvas.Text(x, y, Up(line).c_str(), colour, scale); };
    // A button: lit under the pointer, and what pressing it asks.
    const auto button = [&](int x0, int y0, int x1, int y1, const std::string& label, Press press, bool strong)
    {
        const bool hot = over(x0, y0, x1, y1);
        fill(x0, y0, x1, y1, strong ? (hot ? Rgb{70, 46, 14} : kPicked) : (hot ? Rgb{28, 36, 40} : kPanel));
        box(x0, y0, x1, y1, strong ? kAmber : hot ? kDim : kFaint);
        const std::string shown = Up(label);
        canvas.Text((x0 + x1 - canvas.TextWidth(shown.c_str(), 2)) / 2, (y0 + y1) / 2 - 7, shown.c_str(), strong ? kAmber : kText, 2);
        m_buttons.push_back({x0, y0, x1, y1, press});
    };

    canvas.Clear(kBack);
    // Along the top: what it is, and the ship's credits.
    fill(0, 0, kWide, 32, kHeader);
    text(14, 9, "SHIP TERMINAL", kAmber, 2);
    const std::string credits = "CREDITS " + std::to_string(campaign.credits);
    text(kWide - 14 - canvas.TextWidth(credits.c_str(), 2), 9, credits, kText, 2);
    // The pages, as tabs.
    const std::array<const char*, 4> pages{"CONTRACTS", "FINDINGS", "DEBRIEF", "CARGO"};
    for (int i = 0; i < 4; ++i)
    {
        const int x0 = 14 + i * 186;
        const int x1 = x0 + 178;
        const bool on = static_cast<int>(view.page) == i;
        const bool hot = over(x0, 40, x1, 68);
        fill(x0, 40, x1, 68, on ? kPicked : hot ? Rgb{24, 30, 34} : kPanel);
        box(x0, 40, x1, 68, on ? kAmber : kFaint);
        canvas.Text((x0 + x1 - canvas.TextWidth(pages[static_cast<size_t>(i)], 2)) / 2, 47, pages[static_cast<size_t>(i)], on ? kAmber : kDim, 2);
        m_buttons.push_back({x0, 40, x1, 68, {Press::Kind::Page, i}});
    }

    // The list down the left: its rows, the picked one lit, and up and down below it when there are more than fit.
    const auto list = [&](int count, const std::function<void(int row, int x, int y, bool picked)>& draw)
    {
        fill(kListX0, kTop, kListX1, kBottom, kPanel);
        box(kListX0, kTop, kListX1, kBottom, kFaint);
        if (count == 0)
        {
            text(kListX0 + 12, kTop + 14, "NOTHING YET", kDim, 2);
            return;
        }
        for (int i = 0; i < kRows && view.scroll + i < count; ++i)
        {
            const int row = view.scroll + i;
            const int y0 = kTop + 4 + i * kRowHigh;
            const int y1 = y0 + kRowHigh - 4;
            const bool picked = row == view.selected;
            if (picked || over(kListX0 + 4, y0, kListX1 - 4, y1))
            {
                fill(kListX0 + 4, y0, kListX1 - 4, y1, picked ? kPicked : Rgb{22, 28, 32});
            }
            if (picked)
            {
                fill(kListX0 + 4, y0, kListX0 + 7, y1, kAmber);
            }
            draw(row, kListX0 + 14, y0 + 5, picked);
            m_buttons.push_back({kListX0 + 4, y0, kListX1 - 4, y1, {Press::Kind::Select, row}});
        }
        if (count > kRows)
        {
            const int y0 = kBottom - 26;
            const int middle = (kListX0 + kListX1) / 2;
            button(kListX0 + 6, y0, middle - 3, kBottom - 4, "UP", {Press::Kind::Scroll, -1}, false);
            button(middle + 3, y0, kListX1 - 6, kBottom - 4, "DOWN", {Press::Kind::Scroll, 1}, false);
        }
    };
    // The details down the right: lines of text from the top, a section's name over each part.
    int y = kTop + 14;
    const int letters = (kDetailX1 - kDetailX0 - 28) / 12;
    const auto heading = [&](const std::string& line)
    {
        text(kDetailX0 + 14, y, line, kAmber, 2);
        y += 22;
    };
    const auto section = [&](const std::string& name)
    {
        y += 6;
        text(kDetailX0 + 14, y, name, kDim, 1);
        y += 12;
    };
    const auto lines = [&](const std::string& body, Rgb colour, int most)
    {
        int n = 0;
        for (const std::string& line : Wrap(Up(body), letters))
        {
            if (n++ >= most)
            {
                break;
            }
            text(kDetailX0 + 14, y, line, colour, 2);
            y += 18;
        }
    };
    // How hard it looks: the rating, coloured by it, and the worst of the reasons.
    const auto difficulty = [&](uint64_t system, int body, int region, int most)
    {
        if (!words.difficulty)
        {
            return;
        }
        const std::string rating = words.difficulty(system, body, region);
        if (rating.empty())
        {
            return;
        }
        section("DIFFICULTY");
        const bool hard = rating.find("SEVERE") != std::string::npos || rating.find("EXTREME") != std::string::npos;
        text(kDetailX0 + 14, y, rating, hard ? kAmber : kText, 2);
        y += 18;
        if (words.difficultyWhy)
        {
            const std::vector<std::string> why = words.difficultyWhy(system, body, region);
            for (size_t i = 0; i < why.size() && static_cast<int>(i) < most; ++i)
            {
                text(kDetailX0 + 14, y, Fit(Up(why[i]), letters * 2 - 2), kDim, 1);
                y += 11;
            }
        }
    };
    const auto details = [&]
    {
        fill(kDetailX0, kTop, kDetailX1, kBottom, kPanel);
        box(kDetailX0, kTop, kDetailX1, kBottom, kFaint);
    };
    const auto action = [&](const std::string& label, Press press)
    { button(kDetailX0 + 14, kBottom - 46, kDetailX1 - 14, kBottom - 12, label, press, true); };
    const auto note = [&](const std::string& line, Rgb colour)
    {
        const std::string shown = Up(line);
        canvas.Text((kDetailX0 + kDetailX1 - canvas.TextWidth(shown.c_str(), 2)) / 2, kBottom - 36, shown.c_str(), colour, 2);
    };

    switch (view.page)
    {
    case Page::Contracts:
    {
        const std::vector<const CampaignState::Contract*> rows = ContractRows(campaign);
        list(static_cast<int>(rows.size()),
             [&](int row, int x, int yy, bool)
             {
                 const CampaignState::Contract& c = *rows[static_cast<size_t>(row)];
                 text(x, yy, Fit(Up(words.area(c.system, c.body, c.region)), 21), c.status == Status::Done ? kDim : kText, 2);
                 const char* tag = c.status == Status::Accepted ? "TAKEN ON" : c.status == Status::Done ? "DONE" : "OFFERED";
                 text(x, yy + 19, tag, c.status == Status::Accepted ? kAmber : c.status == Status::Done ? kGreen : kDim, 1);
                 const std::string pays = std::to_string(c.reward) + " CR";
                 text(kListX1 - 14 - canvas.TextWidth(pays.c_str(), 1), yy + 19, pays, kDim, 1);
             });
        details();
        if (rows.empty())
        {
            text(kDetailX0 + 14, y, "NO CONTRACTS ON OFFER.", kDim, 2);
            break;
        }
        const CampaignState::Contract& c = *rows[static_cast<size_t>(view.selected)];
        heading(Fit(Up(words.area(c.system, c.body, c.region)), letters));
        text(kDetailX0 + 14, y, Up(words.world(c.system, c.body)), kDim, 1);
        y += 14;
        section("WANTED");
        lines(words.wanted(c.kind), kText, 4);
        if (const std::string read = words.read(c.system, c.body, c.region); !read.empty())
        {
            section("SCAN");
            lines(read, kDim, 3);
        }
        difficulty(c.system, c.body, c.region, 2);
        section("PAYS");
        text(kDetailX0 + 14, y, std::to_string(c.reward) + " CREDITS", kGreen, 2);
        y += 18;
        if (c.status == Status::Done)
        {
            note("DONE AND PAID", kGreen);
        }
        else if (words.plotted(c.system, c.body, c.region))
        {
            note(c.status == Status::Accepted ? "TAKEN ON - COURSE PLOTTED" : "COURSE PLOTTED", kAmber);
        }
        else if (c.status == Status::Offered)
        {
            action("TAKE ON AND PLOT COURSE", {Press::Kind::Accept, static_cast<int>(c.id)});
        }
        else
        {
            action("PLOT COURSE", {Press::Kind::Plot, static_cast<int>(c.id)});
        }
        break;
    }

    case Page::Leads:
    {
        const std::vector<const CampaignState::LogEntry*> rows = LeadRows(campaign);
        list(static_cast<int>(rows.size()),
             [&](int row, int x, int yy, bool)
             {
                 const CampaignState::LogEntry& e = *rows[static_cast<size_t>(row)];
                 const bool been = Visited(campaign, e.subject);
                 text(x, yy, Fit(Up(e.title), 21), been ? kDim : kText, 2);
                 text(x, yy + 19, been ? "BEEN THERE" : "NOT YET VISITED", been ? kDim : kAmber, 1);
             });
        details();
        if (rows.empty())
        {
            text(kDetailX0 + 14, y, "NOTHING FOUND YET.", kDim, 2);
            y += 26;
            lines("Missions done sometimes point somewhere else. Those are kept here.", kDim, 4);
            break;
        }
        const CampaignState::LogEntry& e = *rows[static_cast<size_t>(view.selected)];
        heading(Fit(Up(e.title), letters));
        text(kDetailX0 + 14, y, Stamp(e.time), kDim, 1);
        y += 14;
        section("NOTE");
        lines(e.text, kText, 6);
        uint64_t system = 0;
        int body = -1;
        int region = -1;
        if (LeadArea(e, system, body, region))
        {
            if (const std::string read = words.read(system, body, region); !read.empty())
            {
                section("SCAN");
                lines(read, kDim, 3);
            }
            difficulty(system, body, region, 1);
            if (Visited(campaign, e.subject))
            {
                note("BEEN THERE", kDim);
            }
            else if (words.plotted(system, body, region))
            {
                note("COURSE PLOTTED", kAmber);
            }
            else
            {
                action("PLOT COURSE", {Press::Kind::Plot, static_cast<int>(e.id), true});
            }
        }
        break;
    }

    case Page::Cargo:
    {
        // What is in the hold: samples brought back from the sites, each kind and how much, what it all fetches -- and selling it
        // at an outpost (later, upgrades built from it).
        const std::vector<std::pair<std::string, int>> rows = CargoRows(campaign);
        const auto name = [&](const std::string& key) { return words.resourceName ? words.resourceName(key) : key; };
        const auto value = [&](const std::string& key) { return words.resourceValue ? words.resourceValue(key) : 0; };
        list(static_cast<int>(rows.size()),
             [&](int row, int x, int yy, bool)
             {
                 const auto& [key, count] = rows[static_cast<size_t>(row)];
                 text(x, yy, Fit(Up(name(key)), 21), kText, 2);
                 text(x, yy + 19, "x " + std::to_string(count), kDim, 1);
                 const std::string worth = std::to_string(value(key) * count) + " CR";
                 text(kListX1 - 14 - canvas.TextWidth(worth.c_str(), 1), yy + 19, worth, kDim, 1);
             });
        details();
        int64_t total = 0;
        for (const auto& [key, count] : rows)
        {
            total += static_cast<int64_t>(value(key)) * count;
        }
        if (rows.empty())
        {
            heading("THE HOLD IS EMPTY");
            lines("Samples taken out of deposits on a site and carried aboard the shuttle as it leaves come here.", kDim, 5);
            break;
        }
        const auto& [key, count] = rows[static_cast<size_t>(view.selected)];
        heading(Fit(Up(name(key)), letters));
        section("IN THE HOLD");
        text(kDetailX0 + 14, y, std::to_string(count) + (count == 1 ? " SAMPLE" : " SAMPLES"), kText, 2);
        y += 18;
        section("EACH FETCHES");
        text(kDetailX0 + 14, y, std::to_string(value(key)) + " CREDITS", kText, 2);
        y += 18;
        section("THE WHOLE HOLD");
        text(kDetailX0 + 14, y, std::to_string(total) + " CREDITS", kGreen, 2);
        y += 18;
        if (words.canSell && words.canSell())
        {
            action("SELL ALL - " + std::to_string(total) + " CREDITS", {Press::Kind::Sell, 0});
        }
        else
        {
            note("SOLD AT AN OUTPOST", kDim);
        }
        break;
    }

    case Page::Debrief:
    case Page::Count:
    {
        // The whole width: the last expedition, how it went, who came back, what it paid and what it added to the log.
        fill(kListX0, kTop, kDetailX1, kBottom, kPanel);
        box(kListX0, kTop, kDetailX1, kBottom, kFaint);
        const CampaignState::Debrief& d = campaign.debrief;
        const int x = kListX0 + 16;
        int yy = kTop + 14;
        if (!d.any)
        {
            text(x, yy, "NO EXPEDITION YET.", kDim, 2);
            break;
        }
        text(x, yy, "LAST EXPEDITION   " + Stamp(d.time), kDim, 1);
        yy += 14;
        text(x, yy, Fit(Up(words.area(d.system, d.body, d.region)), 60), kAmber, 2);
        yy += 20;
        text(x, yy, Up(words.world(d.system, d.body)), kDim, 1);
        yy += 20;
        text(x, yy, "RESULT", kDim, 1);
        text(x + 120, yy - 3, Up(words.result(d.kind, d.success)), d.success ? kGreen : kRed, 2);
        yy += 24;
        text(x, yy, "PAID", kDim, 1);
        // (The contract's pay, and on top of it what the optional goal's records drive paid.)
        const int64_t all = d.paid + d.extra;
        text(x + 120, yy - 3, all > 0 ? std::to_string(all) + " CREDITS" : std::string("NOTHING"), all > 0 ? kGreen : kDim, 2);
        if (d.extra > 0)
        {
            text(x + 440, yy, Up("incl. " + std::to_string(d.extra) + " for the records drive"), kDim, 1);
        }
        yy += 26;
        text(x, yy, "CREW", kDim, 1);
        int crewY = yy - 3;
        for (int p = 0; p < 8; ++p)
        {
            if (((d.crew >> p) & 1u) == 0u)
            {
                continue;
            }
            const bool back = ((d.aboard >> p) & 1u) != 0u;
            text(x + 120, crewY, Fit(Up(words.player(p)), 24), kText, 2);
            text(x + 440, crewY, back ? "BACK ABOARD" : "LEFT BEHIND", back ? kGreen : kRed, 2);
            crewY += 20;
        }
        yy = std::max(crewY, yy + 20) + 8;
        // What else it found: leads and anything else new (the expedition's own entry and its contract's are what is above).
        text(x, yy, "FOUND", kDim, 1);
        int logY = yy - 3;
        for (const uint32_t id : d.entries)
        {
            for (const CampaignState::LogEntry& e : campaign.log)
            {
                if (e.id == id && e.kind != "expedition" && e.kind != "contract" && logY < kBottom - 24)
                {
                    text(x + 120, logY, Fit(Up((e.kind == "lead" ? "FINDING: " : "") + e.title), 50), e.kind == "lead" ? kAmber : kText, 2);
                    logY += 20;
                }
            }
        }
        if (logY == yy - 3)
        {
            text(x + 120, logY, "NOTHING NEW", kDim, 2);
        }
        break;
    }
    }
    canvas.Lines();
}

} // namespace pred
