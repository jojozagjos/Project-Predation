#pragma once

#include "Game/Campaign/Universe.h"

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pred
{

// Everything a campaign has that is not made from a seed: what the crew has, has found and has done.
//
// The universe is not in it (Universe makes that again from `universeSeed`), and neither is any landing region as
// built -- only what has been changed in one, by the region's key. So a save is small however far the crew has been,
// and the host sends this, not a world.
//
// Kept as plain data with a JSON form, and the JSON form is the save file. Anything a newer game wrote that this one
// does not know is kept in `extra` and written back, so a save is never quietly cut down by an older game; a version
// number lets a newer game bring an older save up to date.
struct CampaignState
{
    static constexpr int kVersion = 1;

    // What is known about a body, a bit each; more bits are knowing more.
    enum Knowledge : uint8_t
    {
        kKnownRecords = 1 << 0, // the Company's or others' records of it
        kKnownScanned = 1 << 1, // the ship's sensors have looked at it
        kKnownDeep = 1 << 2,    // and looked closely
        kKnownVisited = 1 << 3  // somebody has stood on it
    };

    // The ship's colours: its main hull, the trim, and the small accents.
    struct Colors
    {
        glm::vec3 primary{0.78f, 0.78f, 0.76f};
        glm::vec3 secondary{0.16f, 0.17f, 0.18f};
        glm::vec3 accent{0.92f, 0.52f, 0.14f};
    };

    // An entry in the ship's log: something found or learnt, kept so nobody has to write it down.
    struct LogEntry
    {
        uint32_t id = 0;
        std::string kind;      // "planet", "region", "poi", "signal", "clue", "record", "lead", ...
        std::string subject;   // what it is about: a body or region key (BodyKey, RegionKey), or empty
        std::string title;
        std::string text;
        double time = 0.0;     // on the campaign's clock
        bool important = false;
        bool pinned = false;
        std::vector<uint32_t> links; // other entries it is connected to
    };

    // Where the ship is going, while it is under way: where it is now in the system (astronomical units from the
    // star), how fast and which way, and what it is heading for.
    struct Travel
    {
        bool underway = false;
        glm::vec3 position{0.0f};
        glm::vec3 velocity{0.0f};
        int target = -1; // a body of the current system
        // Where to go down on the body it is at or heading for: one of its landing regions, or -1 for none chosen.
        int region = -1;
        // Between the stars: the system it is heading for, where in the galaxy it set out from and where that is (light
        // years), and when it set out and how long the crossing takes (the campaign's clock).
        bool interstellar = false;
        uint64_t toSystem = 0;
        glm::vec3 fromGalaxy{0.0f};
        glm::vec3 toGalaxy{0.0f};
        double departed = 0.0;
        float duration = 0.0f;
    };

    int version = kVersion;
    std::string name;
    uint64_t universeSeed = 0;
    double clock = 0.0;      // the campaign's own clock, seconds: what the planets turn by
    double played = 0.0;     // seconds actually played
    int64_t credits = 0;
    std::map<std::string, int> components;     // special parts, by id, and how many
    std::map<std::string, int> upgrades;       // ship upgrades, by category, at which tier
    Colors colors;
    // Where the ship is: its system and the body it is at (-1: out between them), and, landed, which region.
    uint64_t system = 0;
    int body = -1;
    int region = -1;
    // The ship itself on the ground, at the body's landing region travel.region (a hub's landing field), rather than in
    // orbit over it.
    bool landed = false;
    Travel travel;
    std::map<std::string, uint8_t> known;                // by BodyKey: Knowledge bits
    std::map<std::string, bool> regionsFound;            // by RegionKey: found (beyond the charted ones)
    std::map<std::string, nlohmann::json> regionChanges; // by RegionKey: what has been changed there
    std::vector<LogEntry> log;
    uint32_t nextLogId = 1;
    nlohmann::json story = nlohmann::json::object(); // story flags and progress, by name
    bool storyComplete = false;
    // What is kept aboard between expeditions: items by id and count.
    std::map<std::string, int> cargo;
    nlohmann::json extra = nlohmann::json::object();

    // A body's and a region's key: "system:body" and "system:body:region", the system as its packed number.
    static std::string BodyKey(uint64_t system, int body);
    static std::string RegionKey(uint64_t system, int body, int region);

    uint8_t Known(uint64_t system, int body) const;
    // Adds to what is known; true when that was news.
    bool Learn(uint64_t system, int body, uint8_t bits);
    bool RegionFound(uint64_t system, int body, int region, bool charted) const;
    bool FindRegion(uint64_t system, int body, int region);
    int Upgrade(const std::string& category) const;
    int Components(const std::string& id) const;

    // A new entry in the log, numbered; returns it. An entry with the same kind and subject and title as one already
    // there is not added again: the one there is returned.
    LogEntry& AddLog(const std::string& kind, const std::string& subject, const std::string& title, const std::string& text);
    LogEntry* FindLog(uint32_t id);

    nlohmann::json ToJson() const;
    // False, with why, when it cannot be read at all; a field it does not have keeps its default.
    static bool FromJson(const nlohmann::json& json, CampaignState& out, std::string* error = nullptr);

    // A campaign begun: its name and seed, the ship landed at home, at the settled world's hub, with what a crew starts
    // with, and what is on the charts known.
    static CampaignState Begin(const std::string& name, uint64_t seed, Universe& universe);
};

} // namespace pred
