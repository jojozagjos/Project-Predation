#pragma once

#include "Engine/Audio/Sound.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// The placeholder sound designer.
//
// Every sound the game plays is a wav in Assets/Audio, and a recording always beats what is made here.
// Until there are recordings, this is what makes the files: a sound described as layers in
// Assets/Data/Sounds/<Category>.json, rendered offline by the `sound_bake` console command, several takes of
// each so nothing repeats. The game never runs any of this -- it reads the files.
//
// A layer is one source -- noise, an oscillator, a voice, a click -- shaped by an envelope, a pitch that
// moves, a filter that moves, and optionally repeated: a rattle is one click repeated with a jittered
// gap, a gunshot is a crack, a boom and a tail, a growl is a voice with a rough pitch pushed through a
// throat. A patch is the layers summed, given a room, and normalised. It is small on purpose: every field
// is something a person can hear change.
struct SoundLayer
{
    enum class Source : uint8_t
    {
        Noise,    // hiss, coloured by `color`
        Sine,
        Saw,
        Square,
        Triangle,
        Fm,       // a sine whose pitch is swung by another at `fmRatio` times it: bells, metal, rings
        Voice,    // a rough pulse through three formants: throats, growls, breath with a note in it
        Click,    // a single impulse, for the filter to ring: ticks, latches, casings
    };
    Source source = Source::Noise;

    float start = 0.0f;  // seconds into the sound
    float length = 0.3f; // seconds it lasts, release included
    float gain = 1.0f;

    // Up over `attack`, held for `hold`, then falling towards `sustain` with a time constant of `decay`,
    // and faded out over the last few milliseconds so nothing ends on a click.
    float attack = 0.002f;
    float hold = 0.0f;
    float decay = 0.1f;
    float sustain = 0.0f;

    // Pitch in hertz, from `hz` to `hzEnd` along a curve (1 straight, above 1 late, below early).
    float hz = 220.0f;
    float hzEnd = -1.0f; // negative: the same as `hz`
    float curve = 1.0f;
    float vibratoHz = 0.0f;
    float vibratoDepth = 0.0f; // semitones
    float jitter = 0.0f;       // random pitch roughness, 0 to 1: what makes a growl a growl

    int color = 0; // noise: 0 white, 1 pink, 2 brown

    // A state-variable filter, swept from `cutoff` to `cutoffEnd`. 0 none, 1 low pass, 2 high pass,
    // 3 band pass.
    int filter = 0;
    float cutoff = 2000.0f;
    float cutoffEnd = -1.0f; // negative: the same as `cutoff`
    float q = 0.7f;

    float fmRatio = 1.0f;
    float fmIndex = 0.0f;
    float fmIndexEnd = -1.0f; // negative: the same as `fmIndex`

    // Up to three formants for a voice, in hertz; zero is unused. Their width in hertz.
    float formants[3] = {0.0f, 0.0f, 0.0f};
    float formantWidth = 120.0f;
    float breath = 0.0f; // how much of the voice is air rather than note, 0 to 1

    float amHz = 0.0f; // tremolo, flutter, purr
    float amDepth = 0.0f;
    float drive = 0.0f; // saturation, 0 to 1

    // The layer again, `repeats` times, each `interval` seconds after the last, give or take
    // `intervalJitter` of it, `repeatGain` as loud, and up to `repeatPitch` semitones off.
    int repeats = 1;
    float interval = 0.1f;
    float intervalJitter = 0.0f;
    float repeatGain = 1.0f;
    float repeatPitch = 0.0f;
};

struct SoundPatch
{
    std::string name;
    float seconds = 1.0f;
    int variants = 3;    // takes written, each a little different
    float vary = 0.05f;  // how different: pitch and timing, as a fraction
    float gain = 0.8f;   // the peak each take is brought to
    bool loop = false;   // crossfaded end into start, for something that plays continuously
    float room = 0.0f;   // how much of the room is heard, 0 to 1
    float roomSize = 0.5f; // 0 a cupboard, 1 a hangar
    uint32_t seed = 1;
    std::vector<SoundLayer> layers;
};

// Reads every patch in one file of the library. Anything malformed is skipped and named in `errors`.
// `category`, when given, is put in front of every name: the file Weapons.json holding "shot" is the
// patch "Weapons/shot".
std::vector<SoundPatch> LoadSoundPatches(const std::string& jsonText, std::vector<std::string>* errors = nullptr,
                                         const std::string& category = {});

// The whole library: every .json in a folder -- Assets/Data/Sounds, one file per category, named for it
// -- read with its file's name as the category. Problems are named with the file they are in; two
// patches with the same name are one of them.
std::vector<SoundPatch> LoadSoundLibrary(const std::string& folder, std::vector<std::string>* errors = nullptr);

// One take of a patch. The same patch, take and rate always give the same samples.
SoundData RenderPatch(const SoundPatch& patch, int take, int sampleRate);

} // namespace pred
