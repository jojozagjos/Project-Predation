#pragma once

#include "Game/Campaign/Campaign.h"

#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <vector>

namespace pred
{

// One campaign on disk, as the title lists it.
struct CampaignSlot
{
    std::string folder; // its folder's name, under the store's root
    std::string name;
    double played = 0.0;
    int64_t credits = 0;
    int64_t savedAt = 0; // seconds since 1970, of its newest save
    bool newestIsAutosave = false;
    uint64_t universeSeed = 0;
    uint64_t system = 0;
    int body = -1; // where it is, or where it is heading when under way
    bool underway = false;
    bool landed = false; // the ship itself on the ground, at a hub
};

// Where campaigns are kept: a folder each under the player's own data, with the host's own saves in it.
//
// Two saves a campaign -- the one saved by hand and the one the game saves on its own at the moments that matter --
// and loading takes whichever is newer and reads. Each file is written whole to a spare name and then put in place,
// the one it replaces kept as a backup, so a crash or a full disk mid-save leaves the last good one, never half of one.
// Writing happens on a worker thread: the save is turned to text where it is asked for (so what is written is the
// campaign as it was at that moment), and only the disk is left to the worker.
class CampaignStore
{
public:
    // The default root is Campaigns in the player's own data folder.
    explicit CampaignStore(std::filesystem::path root = {});
    ~CampaignStore();

    const std::filesystem::path& Root() const { return m_root; }
    // Every campaign there is, the most recently saved first.
    std::vector<CampaignSlot> List() const;
    // A folder name for a new campaign called `name`, not already used.
    std::string NewFolder(const std::string& name) const;
    // The newest good save of a campaign (the backups when both are damaged). False, with why, when there is none.
    bool Load(const std::string& folder, CampaignState& out, std::string* error = nullptr) const;
    // Saves it, by hand or on the game's own account, without waiting for the disk. A save asked for while one is
    // being written waits for that one first.
    void Save(const std::string& folder, const CampaignState& state, bool autosave);
    bool Saving() const;
    // Waits for any save being written; returns its error, empty when it went well.
    std::string Finish();
    // Gone for good: the folder and everything in it.
    bool Delete(const std::string& folder) const;

    // The file a save is written to, by hand or not.
    std::filesystem::path FileFor(const std::string& folder, bool autosave) const;

private:
    std::filesystem::path m_root;
    std::future<std::string> m_pending;
    std::string m_lastError;
};

// Writes `text` to `file` safely: whole to a spare name, then into place with what was there kept as `file`.bak.
// Returns an error, empty on success.
std::string WriteFileSafely(const std::filesystem::path& file, const std::string& text);

} // namespace pred
