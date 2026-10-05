#include "Game/Campaign/CampaignStore.h"

#include "Engine/Core/JsonText.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

namespace pred
{

namespace
{

int64_t NowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ReadFile(const std::filesystem::path& file, std::string& out)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    out = text.str();
    return true;
}

// One save file: when it was written and the campaign in it.
struct SaveFile
{
    bool good = false;
    int64_t savedAt = 0;
    nlohmann::json campaign;
};

SaveFile ReadSave(const std::filesystem::path& file)
{
    SaveFile out;
    std::string text;
    if (!ReadFile(file, text))
    {
        return out;
    }
    try
    {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_object() || !root.contains("campaign") || !root["campaign"].is_object())
        {
            return out;
        }
        out.savedAt = root.value("savedAt", int64_t{0});
        out.campaign = root["campaign"];
        out.good = true;
    }
    catch (const std::exception&)
    {
    }
    return out;
}

} // namespace

std::string WriteFileSafely(const std::filesystem::path& file, const std::string& text)
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path spare = file.string() + ".tmp";
    {
        std::ofstream stream(spare, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return "could not write " + spare.string();
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream)
        {
            return "could not finish writing " + spare.string() + " (is the disk full?)";
        }
    }
    const std::filesystem::path backup = file.string() + ".bak";
    if (std::filesystem::exists(file, ec))
    {
        std::filesystem::remove(backup, ec);
        std::filesystem::rename(file, backup, ec);
        if (ec)
        {
            return "could not keep the last save as a backup: " + ec.message();
        }
    }
    std::filesystem::rename(spare, file, ec);
    if (ec)
    {
        return "could not put the save in place: " + ec.message();
    }
    return {};
}

CampaignStore::CampaignStore(std::filesystem::path root) : m_root(std::move(root))
{
    if (m_root.empty())
    {
        m_root = Paths::UserDataDir() / "Campaigns";
    }
}

CampaignStore::~CampaignStore()
{
    Finish();
}

std::filesystem::path CampaignStore::FileFor(const std::string& folder, bool autosave) const
{
    return m_root / folder / (autosave ? "autosave.json" : "campaign.json");
}

std::vector<CampaignSlot> CampaignStore::List() const
{
    std::vector<CampaignSlot> slots;
    std::error_code ec;
    if (!std::filesystem::is_directory(m_root, ec))
    {
        return slots;
    }
    for (const auto& entry : std::filesystem::directory_iterator(m_root, ec))
    {
        if (!entry.is_directory())
        {
            continue;
        }
        const std::string folder = entry.path().filename().string();
        const SaveFile manual = ReadSave(FileFor(folder, false));
        const SaveFile automatic = ReadSave(FileFor(folder, true));
        const bool useAuto = automatic.good && (!manual.good || automatic.savedAt > manual.savedAt);
        const SaveFile& newest = useAuto ? automatic : manual;
        if (!newest.good)
        {
            continue;
        }
        CampaignSlot slot;
        slot.folder = folder;
        slot.name = newest.campaign.value("name", folder);
        slot.played = newest.campaign.value("played", 0.0);
        slot.credits = newest.campaign.value("credits", int64_t{0});
        slot.savedAt = newest.savedAt;
        slot.newestIsAutosave = useAuto;
        slot.universeSeed = newest.campaign.value("universeSeed", uint64_t{0});
        if (const auto location = newest.campaign.find("location"); location != newest.campaign.end() && location->is_object())
        {
            slot.system = location->value("system", uint64_t{0});
            slot.body = location->value("body", -1);
            slot.landed = location->value("landed", false);
        }
        if (const auto travel = newest.campaign.find("travel"); travel != newest.campaign.end() && travel->is_object() &&
                                                                 travel->value("underway", false))
        {
            slot.underway = true;
            slot.body = travel->value("target", -1);
        }
        slots.push_back(slot);
    }
    std::sort(slots.begin(), slots.end(), [](const CampaignSlot& a, const CampaignSlot& b) { return a.savedAt > b.savedAt; });
    return slots;
}

std::string CampaignStore::NewFolder(const std::string& name) const
{
    // Letters, numbers and underscores only: a folder name every file system takes.
    std::string base;
    for (const char c : name)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        {
            base.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        else if (!base.empty() && base.back() != '_')
        {
            base.push_back('_');
        }
        if (base.size() >= 32)
        {
            break;
        }
    }
    while (!base.empty() && base.back() == '_')
    {
        base.pop_back();
    }
    if (base.empty())
    {
        base = "campaign";
    }
    std::error_code ec;
    std::string folder = base;
    for (int n = 2; std::filesystem::exists(m_root / folder, ec); ++n)
    {
        folder = base + "_" + std::to_string(n);
    }
    return folder;
}

bool CampaignStore::Load(const std::string& folder, CampaignState& out, std::string* error) const
{
    // Newest first, then the backups of each.
    std::vector<SaveFile> saves{ReadSave(FileFor(folder, false)), ReadSave(FileFor(folder, true))};
    std::sort(saves.begin(), saves.end(), [](const SaveFile& a, const SaveFile& b) { return a.good && (!b.good || a.savedAt > b.savedAt); });
    saves.push_back(ReadSave(FileFor(folder, false).string() + ".bak"));
    saves.push_back(ReadSave(FileFor(folder, true).string() + ".bak"));
    std::string why = "no save found in " + (m_root / folder).string();
    for (const SaveFile& save : saves)
    {
        if (!save.good)
        {
            continue;
        }
        std::string problem;
        if (CampaignState::FromJson(save.campaign, out, &problem))
        {
            if (!problem.empty())
            {
                PRED_LOG_WARN(Gameplay, "Campaign '{}': {}", folder, problem);
            }
            return true;
        }
        why = problem;
    }
    if (error != nullptr)
    {
        *error = why;
    }
    return false;
}

void CampaignStore::Save(const std::string& folder, const CampaignState& state, bool autosave)
{
    Finish();
    nlohmann::json root;
    root["savedAt"] = NowSeconds();
    root["kind"] = autosave ? "auto" : "manual";
    root["campaign"] = state.ToJson();
    // Readable, as the data files are: a save can be looked into, and mended, by hand.
    std::string text = JsonText(root, 5);
    const std::filesystem::path file = FileFor(folder, autosave);
    m_pending = std::async(std::launch::async, [file, text = std::move(text)]() { return WriteFileSafely(file, text); });
}

bool CampaignStore::Saving() const
{
    return m_pending.valid() && m_pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

std::string CampaignStore::Finish()
{
    if (m_pending.valid())
    {
        m_lastError = m_pending.get();
        if (!m_lastError.empty())
        {
            PRED_LOG_ERROR(Gameplay, "Saving the campaign failed: {}", m_lastError);
        }
        return m_lastError;
    }
    return {};
}

bool CampaignStore::Delete(const std::string& folder) const
{
    if (folder.empty() || folder.find("..") != std::string::npos || folder.find('/') != std::string::npos || folder.find('\\') != std::string::npos)
    {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove_all(m_root / folder, ec);
    return !ec;
}

} // namespace pred
