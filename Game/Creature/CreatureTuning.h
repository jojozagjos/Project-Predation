#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pred
{

// The numbers every creature's mind shares, read from Assets/Data/creatures.json.
//
// What makes one creature differ from another is its seed (CreatureTraits). What is here is what they
// all have in common -- how far an eye reaches, how long a glimpse takes to become a sighting, how long
// a patient one will wait -- which is what gets tuned while playing. Editing the file while the game
// runs applies at once. Every value has the default it had when it was a constant in the code, so a
// test that never loads the file sees the creature it always did.
struct CreatureTuning
{
    // Sight: how far, how wide, how poorly at the edge, and how close counts as touching.
    float sightRange = 26.0f;       // metres, for eyes of ordinary sharpness
    float halfFieldDegrees = 65.0f; // either side of straight ahead
    float edgeOfView = 0.35f;       // how well it sees at the very edge, against 1 dead ahead
    float closeSense = 1.6f;        // metres: known about whichever way it faces
    // Making somebody out: how fast a steady look fills the meter, how fast looking away empties it,
    // and how full it has to be to be worth going to look at.
    float exposureGain = 2.4f;
    float exposureDecay = 0.6f;
    float suspicion = 0.35f;
    // Hearing through a wall: this share of how far it would carry in the open.
    float throughWalls = 0.5f;
    // How much better something else has to score before it drops what it is doing.
    float commitment = 0.12f;
    // Waiting, in seconds: a stalker's patience, and a creature lying in wait, each the base plus the
    // share of the rest its own patience gives it.
    float stalkPatience = 6.0f;
    float stalkPatienceRange = 18.0f;
    float ambushPatience = 15.0f;
    float ambushPatienceRange = 35.0f;
    // Seconds between one attempt at luring somebody with a voice and the next.
    float lureEvery = 50.0f;
};

// What the creatures use now.
const CreatureTuning& Tuning();
void SetTuning(const CreatureTuning& tuning);

// Reads creatures.json into `out`, starting from what is in it already. False when the file cannot be
// read; keys it does not know are named in `warnings`.
bool LoadCreatureTuning(const std::filesystem::path& file, CreatureTuning& out, std::vector<std::string>* warnings = nullptr);

} // namespace pred
