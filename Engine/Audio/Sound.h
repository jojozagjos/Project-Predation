#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// A sound, as the mixer wants it: mono samples at a known rate.
//
// Mono on purpose. Everything in this game that makes a noise makes it somewhere, and a stereo
// source has already decided which ear it belongs in; positioning it afterwards means fighting a
// decision somebody else made. Stereo is what comes out of the mixer, not what goes into it.
struct SoundData
{
    std::vector<float> samples;
    int sampleRate = 48000;

    float Seconds() const
    {
        return sampleRate > 0 ? static_cast<float>(samples.size()) / static_cast<float>(sampleRate)
                              : 0.0f;
    }
};

// How to make one.
//
// The sounds are generated rather than recorded, for the same reason the models are built out of
// boxes: there is no sound designer on this project yet, and a placeholder that can be tuned from a
// text file while the game runs is worth more right now than a library of wav files that can only
// be replaced. Every field is something a person can hear the effect of.
//
// A gunshot is noise with a bite on the front and a fast decay. A footstep is the same thing
// quieter, shorter and with the top taken off. A door is a low tone that falls as it swings. A
// pickup is a short tone that rises. That is four of the five sounds this game currently needs.
struct SoundRecipe
{
    float seconds = 0.40f;   // how long the whole thing lasts
    float attack = 0.002f;   // how long it takes to reach full volume
    float decay = 0.25f;     // how long it takes to fall to a third of it
    float noise = 1.0f;      // how much of it is hiss
    float tone = 0.0f;       // and how much is a note
    float toneHz = 220.0f;   // what note, at the start
    float toneDrop = 0.0f;   // how far it slides over its life, in octaves; negative rises
    float lowpass = 1.0f;    // 1 is untouched, towards 0 takes the top off
    float bite = 0.0f;       // a click on the very front, which is most of what makes a bang a bang
    uint32_t seed = 1;       // so the same recipe always makes the same sound
};

// Builds the samples. Deterministic: the same recipe and rate always give the same buffer, so a
// test can assert on one and two machines in a game never disagree about what a gunshot sounds like.
SoundData Synthesise(const SoundRecipe& recipe, int sampleRate);

// Reads a table of recipes from JSON, as the rest of the game's tuning is read. Returns how many
// were understood; anything malformed is skipped with a warning rather than taking the file down.
struct SoundLibraryEntry
{
    std::string name;
    SoundRecipe recipe;
};
std::vector<SoundLibraryEntry> LoadSoundRecipes(const std::string& jsonText);

} // namespace pred
