#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace pred
{

// A cinematic: a sequence of shots, and of things moving, lighting, sounding and happening, laid out on a timeline.
// Everything that goes between the parts of a deployment -- the ship leaving, arriving, the craft going down, the
// crawler coming over the ridge, the doors opening -- is one of these, played by CinematicPlayer.
//
// Data, not code: each is a file in Assets/Cinematics, edited by hand in the cinematic editor. It is written in terms
// of *anchors* -- named places the game supplies when it plays it, such as "pad" or "facility_entrance" -- and of the
// *actors* it moves, so one file fits every generated site: the mission decides where things are, the file decides how
// they are shown, and hand edits to the file are never overwritten by what a mission generates. A key whose anchor is
// empty is in the world. A key's anchor may also name an actor, which carries it along: a camera riding on a vehicle.
//
// Only data and sampling here, with nothing that needs the engine, so all of it can be tested.

// How a value goes from one key to the next.
struct Ease
{
    enum class Kind : uint8_t
    {
        Linear,
        Step,  // holds, then jumps at the next key
        In,    // slow away
        Out,   // slow into the key
        InOut,
        Curve  // a cubic Bezier from (0,0) to (1,1) through `a` and `b`, as CSS writes them
    };
    Kind kind = Kind::InOut;
    glm::vec2 a{0.42f, 0.0f};
    glm::vec2 b{0.58f, 1.0f};

    float Apply(float t) const;
    static const char* Name(Kind kind);
};

// A place and a turn, in the world.
struct CinePose
{
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};

    glm::vec3 Apply(const glm::vec3& local) const { return position + rotation * local; }
};

// Turns as the editor shows them: degrees about x (pitch), y (yaw) and z (roll), applied roll, then pitch, then yaw,
// so a camera's yaw turns it about the world's up whatever it is pitched to. A turn of zero faces -z.
glm::quat TurnFromDegrees(const glm::vec3& degrees);
glm::vec3 DegreesFromTurn(const glm::quat& turn);

struct TransformKey
{
    float time = 0.0f;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // degrees, see TurnFromDegrees
    Ease ease;
};

struct CameraKey
{
    float time = 0.0f;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f}; // degrees; ignored while the camera is looking at something, except its roll
    float fov = 60.0f;        // horizontal, degrees
    float focus = 0.0f;       // the distance in focus, 0 for none (for when there is a depth of field)
    Ease ease;
};

struct FloatKey
{
    float time = 0.0f;
    float value = 0.0f;
    Ease ease;
};

// A camera: where it is and where it points, over time. `anchor` is what its keys are measured from; `lookAt`, when
// given, is an actor or an anchor it keeps pointed at (plus `lookOffset`, in that thing's own frame).
struct CameraTrack
{
    std::string name;
    std::string anchor;
    std::string lookAt;
    glm::vec3 lookOffset{0.0f};
    std::vector<CameraKey> keys;
};

// Which camera the picture is from, from `time`: a cut, or a blend from the one before over `blend` seconds.
struct Shot
{
    float time = 0.0f;
    std::string camera;
    float blend = 0.0f;
    Ease ease;
};

// Something the cinematic brings or moves: a model (Assets/Models/<model>.json) put in the world for it, or -- with
// `bind` -- something the game already has, such as the site's shuttle, moved where it is.
struct ActorDef
{
    std::string name;
    std::string model;
    std::string bind;
};

// An actor moved by keys, measured from `anchor`.
struct ActorTrack
{
    std::string actor;
    std::string anchor;
    std::vector<TransformKey> keys;
};

// An actor driven along one of the paths the game supplies (a crawler's route from the pad to a building), from
// `start` to `end`, eased, facing the way it goes, `lift` metres over the path.
struct PathFollow
{
    std::string actor;
    std::string path;
    float start = 0.0f;
    float end = 1.0f;
    Ease ease;
    float lift = 0.0f;
    bool face = true;
};

// One of an actor's model's clips played from `time`: doors, a ramp, clamps.
struct ClipEvent
{
    float time = 0.0f;
    std::string actor;
    std::string clip;
    float speed = 1.0f;
};

// A sound from `time`: in the head when `at` is empty, or from an actor or an anchor. Music is heard however far.
struct SoundEvent
{
    float time = 0.0f;
    std::string sound;
    float volume = 1.0f;
    std::string at;
    bool music = false;
};

// Something for the game to do at `time`: open the doors, hand control back, stop the world, start it again. The
// game decides what each name means (see CinematicPlayer).
struct Marker
{
    float time = 0.0f;
    std::string name;
    std::string value;
};

// Words on the screen from `time` for `duration`: a title card typed out a letter at a time, or a caption. Lines may
// name what the game fills in: {planet}, {site}, {region}, {local_time}, {conditions}, {sector}.
struct TextItem
{
    float time = 0.0f;
    float duration = 5.0f;
    std::string style = "title_card"; // title_card, caption
    std::vector<std::string> lines;
    float typeSpeed = 28.0f; // letters a second; 0 shows them all at once
};

// A light an actor has, or one of the world's, brought up and down.
struct LightTrack
{
    std::string light;
    std::vector<FloatKey> intensity;
};

// A particle effect from `time` (engine exhaust, snow thrown up), at an actor or an anchor.
struct ParticleEvent
{
    float time = 0.0f;
    std::string effect;
    std::string at;
    glm::vec3 offset{0.0f};
    float duration = 1.0f;
};

struct Cinematic
{
    std::string name;
    float duration = 10.0f;
    // Whether the world stops while it plays (the players are not in it: they are aboard, being carried).
    bool pausesGameplay = true;
    // How long the picture takes at the end to go back to the player's own eyes, rather than cutting to them.
    float blendOut = 1.0f;
    bool loop = false;

    std::vector<ActorDef> actors;
    std::vector<CameraTrack> cameras;
    std::vector<Shot> shots;
    std::vector<ActorTrack> actorTracks;
    std::vector<PathFollow> paths;
    std::vector<ClipEvent> clips;
    std::vector<SoundEvent> sounds;
    std::vector<Marker> markers;
    std::vector<TextItem> texts;
    std::vector<LightTrack> lights;
    std::vector<ParticleEvent> particles;
    // Shaking the picture: how hard (degrees, and a tenth of that in metres) and how fast (per second).
    std::vector<FloatKey> shake;
    std::vector<FloatKey> shakeSpeed;
    // Over the picture: black (0 none, 1 all) and the bars top and bottom (0 none, 1 full).
    std::vector<FloatKey> fade;
    std::vector<FloatKey> letterbox;
    // How far the fog is pushed back (1 as it is, 2 twice as far), and how much the dark is lifted (1 as it is), for a
    // shot that has to show a place the night would otherwise hide.
    std::vector<FloatKey> fogScale;
    std::vector<FloatKey> ambientScale;

    bool LoadFromFile(const std::filesystem::path& file, std::string* error = nullptr);
    bool SaveToFile(const std::filesystem::path& file) const;
    bool FromJsonText(const std::string& text, std::string* error = nullptr);
    std::string ToJsonText() const;

    // Keeps every list of keys and events in time order, and the duration covering all of them.
    void Tidy();

    const CameraTrack* FindCamera(const std::string& camera) const;
    const ActorDef* FindActor(const std::string& actor) const;
};

// What the game supplies when it plays one: its anchors, its paths, and what the words fill in.
struct CinematicBindings
{
    std::map<std::string, CinePose> anchors;
    std::map<std::string, std::vector<glm::vec3>> paths;
    std::map<std::string, std::string> words;
    // Where each actor is before anything moves it: a bound one, where the game has it.
    std::map<std::string, CinePose> actorRest;
};

// The picture's camera at a moment.
struct CameraState
{
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // looks down its own -z
    float fov = 60.0f;
    float focus = 0.0f;
};

CameraState LerpCamera(const CameraState& a, const CameraState& b, float t);

// Sampling a cinematic at a moment. Anchors that are not bound are the world's origin.
class CinematicSampler
{
public:
    CinematicSampler(const Cinematic& cinematic, const CinematicBindings& bindings) : m_cinematic(cinematic), m_bindings(bindings) {}

    // An actor's pose: from its path while it is on one, from its keys otherwise, and where it rests without either.
    CinePose Actor(const std::string& actor, float time) const;
    // What an anchor name means now: an actor (where it is at this moment), a bound anchor, or the world.
    CinePose Frame(const std::string& anchor, float time) const;
    // One camera, on its own.
    CameraState Camera(const CameraTrack& camera, float time) const;
    // The picture: the shot at this moment, blended from the one before while its blend lasts. `shot` and `blend`
    // say which camera and how far into its blend (1 when it is not blending).
    CameraState Picture(float time, std::string* shot = nullptr, float* blend = nullptr) const;
    float Shake(float time) const;
    float ShakeSpeed(float time) const;
    float Fade(float time) const;
    float Letterbox(float time) const;
    float FogScale(float time) const;
    float AmbientScale(float time) const;
    float LightIntensity(const LightTrack& light, float time) const;
    // A line of text with its {words} filled in, typed out as far as it has got by `time`.
    std::string Text(const TextItem& item, size_t line, float time) const;
    std::string Fill(const std::string& line) const;
    // Where a sound or a particle effect is: an actor, an anchor, or nowhere (in the head).
    bool Where(const std::string& at, float time, glm::vec3& out) const;

private:
    const Cinematic& m_cinematic;
    const CinematicBindings& m_bindings;
};

// A value from keys at a moment, eased from the key before into the key after.
float SampleFloat(const std::vector<FloatKey>& keys, float time, float none);
// A point along a path, by how far along it is (0 the start, 1 the end), and which way it goes there.
glm::vec3 AlongPath(const std::vector<glm::vec3>& path, float fraction, glm::vec3* heading = nullptr);

// Everything that happens in (from, to]: the markers, sounds, clips, particles and texts that start in it, in time
// order, for the player to act on as the time passes them.
struct CinematicHappening
{
    enum class Kind : uint8_t
    {
        Marker,
        Sound,
        Clip,
        Particle,
        Text
    };
    Kind kind = Kind::Marker;
    float time = 0.0f;
    size_t index = 0; // into the list of that kind
};
std::vector<CinematicHappening> HappeningsBetween(const Cinematic& cinematic, float from, float to);

} // namespace pred
