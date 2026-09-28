#include "Game/Cinematic/Cinematic.h"

#include <nlohmann/json.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace pred
{

using nlohmann::json;

// --- Easing ------------------------------------------------------------------------------------------

namespace
{

float Bezier(float a, float b, float s)
{
    // One axis of a cubic Bezier from 0 to 1 with its handles at a and b.
    const float u = 1.0f - s;
    return 3.0f * u * u * s * a + 3.0f * u * s * s * b + s * s * s;
}

} // namespace

float Ease::Apply(float t) const
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (kind)
    {
    case Kind::Linear: return t;
    case Kind::Step: return t >= 1.0f ? 1.0f : 0.0f;
    case Kind::In: return t * t * t;
    case Kind::Out:
    {
        const float u = 1.0f - t;
        return 1.0f - u * u * u;
    }
    case Kind::InOut: return t * t * (3.0f - 2.0f * t);
    case Kind::Curve:
    {
        // The curve's x is time and its y is how far: find where x is t, by halving, which cannot fail to converge as
        // Newton's method can on a steep handle.
        float low = 0.0f;
        float high = 1.0f;
        float s = t;
        for (int i = 0; i < 28; ++i)
        {
            s = (low + high) * 0.5f;
            if (Bezier(a.x, b.x, s) < t)
            {
                low = s;
            }
            else
            {
                high = s;
            }
        }
        return Bezier(a.y, b.y, s);
    }
    }
    return t;
}

const char* Ease::Name(Kind kind)
{
    switch (kind)
    {
    case Kind::Linear: return "linear";
    case Kind::Step: return "step";
    case Kind::In: return "in";
    case Kind::Out: return "out";
    case Kind::InOut: return "in_out";
    case Kind::Curve: return "curve";
    }
    return "in_out";
}

// --- Turns -------------------------------------------------------------------------------------------

glm::quat TurnFromDegrees(const glm::vec3& degrees)
{
    // Yaw positive to the right, as a player's look is; pitch positive up; roll positive tips the top of the picture
    // to the left.
    const glm::quat yaw = glm::angleAxis(glm::radians(-degrees.y), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::quat pitch = glm::angleAxis(glm::radians(degrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat roll = glm::angleAxis(glm::radians(degrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
    return yaw * pitch * roll;
}

glm::vec3 DegreesFromTurn(const glm::quat& turn)
{
    const glm::vec3 forward = turn * glm::vec3(0.0f, 0.0f, -1.0f);
    const float yaw = std::atan2(forward.x, -forward.z);
    const float pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    // What is left once the yaw and the pitch are taken back out is the roll.
    const glm::quat level = glm::angleAxis(-yaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(pitch, glm::vec3(1.0f, 0.0f, 0.0f));
    const glm::quat left = glm::inverse(level) * turn;
    const glm::vec3 right = left * glm::vec3(1.0f, 0.0f, 0.0f);
    const float roll = std::atan2(right.y, right.x);
    return {glm::degrees(pitch), glm::degrees(yaw), glm::degrees(roll)};
}

// --- Keys ------------------------------------------------------------------------------------------

namespace
{

// The key at or before `time` and the one after it, and how far between them, eased by the later one.
template <typename Key>
bool Segment(const std::vector<Key>& keys, float time, size_t& from, size_t& to, float& t)
{
    if (keys.empty())
    {
        return false;
    }
    if (time <= keys.front().time)
    {
        from = to = 0;
        t = 0.0f;
        return true;
    }
    if (time >= keys.back().time)
    {
        from = to = keys.size() - 1;
        t = 0.0f;
        return true;
    }
    const auto after = std::upper_bound(keys.begin(), keys.end(), time, [](float value, const Key& key) { return value < key.time; });
    to = static_cast<size_t>(after - keys.begin());
    from = to - 1;
    const float span = keys[to].time - keys[from].time;
    t = span > 1.0e-6f ? keys[to].ease.Apply((time - keys[from].time) / span) : 1.0f;
    return true;
}

template <typename Key>
void SortByTime(std::vector<Key>& keys)
{
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
}

} // namespace

float SampleFloat(const std::vector<FloatKey>& keys, float time, float none)
{
    size_t from = 0;
    size_t to = 0;
    float t = 0.0f;
    if (!Segment(keys, time, from, to, t))
    {
        return none;
    }
    return keys[from].value + (keys[to].value - keys[from].value) * t;
}

glm::vec3 AlongPath(const std::vector<glm::vec3>& path, float fraction, glm::vec3* heading)
{
    if (path.empty())
    {
        if (heading != nullptr)
        {
            *heading = {0.0f, 0.0f, -1.0f};
        }
        return glm::vec3(0.0f);
    }
    if (path.size() == 1)
    {
        if (heading != nullptr)
        {
            *heading = {0.0f, 0.0f, -1.0f};
        }
        return path.front();
    }
    float total = 0.0f;
    for (size_t i = 1; i < path.size(); ++i)
    {
        total += glm::distance(path[i - 1], path[i]);
    }
    float left = std::clamp(fraction, 0.0f, 1.0f) * total;
    for (size_t i = 1; i < path.size(); ++i)
    {
        const float length = glm::distance(path[i - 1], path[i]);
        if (left <= length || i + 1 == path.size())
        {
            const glm::vec3 direction = length > 1.0e-5f ? (path[i] - path[i - 1]) / length : glm::vec3(0.0f, 0.0f, -1.0f);
            if (heading != nullptr)
            {
                *heading = direction;
            }
            return path[i - 1] + direction * std::min(left, length);
        }
        left -= length;
    }
    return path.back();
}

CameraState LerpCamera(const CameraState& a, const CameraState& b, float t)
{
    CameraState out;
    out.position = glm::mix(a.position, b.position, t);
    out.rotation = glm::slerp(a.rotation, b.rotation, t);
    out.fov = a.fov + (b.fov - a.fov) * t;
    out.focus = a.focus + (b.focus - a.focus) * t;
    return out;
}

// --- Sampling ----------------------------------------------------------------------------------------

namespace
{

CinePose Facing(const glm::vec3& position, const glm::vec3& heading)
{
    CinePose pose;
    pose.position = position;
    if (glm::length(heading) > 1.0e-5f)
    {
        const glm::vec3 direction = glm::normalize(heading);
        const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        pose.rotation = glm::quatLookAtRH(direction, up);
    }
    return pose;
}

} // namespace

CinePose CinematicSampler::Frame(const std::string& anchor, float time) const
{
    if (anchor.empty())
    {
        return {};
    }
    if (m_cinematic.FindActor(anchor) != nullptr)
    {
        return Actor(anchor, time);
    }
    const auto found = m_bindings.anchors.find(anchor);
    return found != m_bindings.anchors.end() ? found->second : CinePose{};
}

CinePose CinematicSampler::Actor(const std::string& actor, float time) const
{
    // On a path now.
    const PathFollow* before = nullptr; // the last one it finished
    const PathFollow* first = nullptr;  // the first it will start
    for (const PathFollow& follow : m_cinematic.paths)
    {
        if (follow.actor != actor)
        {
            continue;
        }
        const auto path = m_bindings.paths.find(follow.path);
        if (path == m_bindings.paths.end())
        {
            continue;
        }
        if (time >= follow.start && time <= follow.end)
        {
            const float span = std::max(follow.end - follow.start, 1.0e-4f);
            glm::vec3 heading;
            const glm::vec3 at = AlongPath(path->second, follow.ease.Apply((time - follow.start) / span), &heading);
            CinePose pose = Facing(at + glm::vec3(0.0f, follow.lift, 0.0f), follow.face ? glm::vec3(heading.x, 0.0f, heading.z) : glm::vec3(0.0f));
            if (!follow.face)
            {
                pose.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            return pose;
        }
        if (follow.end < time && (before == nullptr || follow.end > before->end))
        {
            before = &follow;
        }
        if (follow.start > time && (first == nullptr || follow.start < first->start))
        {
            first = &follow;
        }
    }
    // Moved by keys.
    for (const ActorTrack& track : m_cinematic.actorTracks)
    {
        if (track.actor != actor || track.keys.empty() || track.anchor == actor)
        {
            continue;
        }
        size_t from = 0;
        size_t to = 0;
        float t = 0.0f;
        Segment(track.keys, time, from, to, t);
        const CinePose frame = Frame(track.anchor, time);
        const glm::vec3 local = glm::mix(track.keys[from].position, track.keys[to].position, t);
        const glm::vec3 degrees = glm::mix(track.keys[from].rotation, track.keys[to].rotation, t);
        return {frame.Apply(local), frame.rotation * TurnFromDegrees(degrees)};
    }
    // Between paths, or before or after them: where the one it finished left it, or where the next will start it.
    const PathFollow* hold = before != nullptr ? before : first;
    if (hold != nullptr)
    {
        const auto path = m_bindings.paths.find(hold->path);
        glm::vec3 heading;
        const glm::vec3 at = AlongPath(path->second, hold == before ? 1.0f : 0.0f, &heading);
        return Facing(at + glm::vec3(0.0f, hold->lift, 0.0f), hold->face ? glm::vec3(heading.x, 0.0f, heading.z) : glm::vec3(0.0f));
    }
    const auto rest = m_bindings.actorRest.find(actor);
    return rest != m_bindings.actorRest.end() ? rest->second : CinePose{};
}

CameraState CinematicSampler::Camera(const CameraTrack& camera, float time) const
{
    CameraState state;
    if (camera.keys.empty())
    {
        return state;
    }
    size_t from = 0;
    size_t to = 0;
    float t = 0.0f;
    Segment(camera.keys, time, from, to, t);
    const CameraKey& a = camera.keys[from];
    const CameraKey& b = camera.keys[to];
    const CinePose frame = Frame(camera.anchor, time);
    const glm::vec3 degrees = glm::mix(a.rotation, b.rotation, t);
    state.position = frame.Apply(glm::mix(a.position, b.position, t));
    state.fov = a.fov + (b.fov - a.fov) * t;
    state.focus = a.focus + (b.focus - a.focus) * t;
    if (!camera.lookAt.empty())
    {
        const CinePose target = Frame(camera.lookAt, time);
        const glm::vec3 direction = target.Apply(camera.lookOffset) - state.position;
        if (glm::length(direction) > 1.0e-4f)
        {
            state.rotation = Facing(state.position, direction).rotation *
                             glm::angleAxis(glm::radians(degrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
            return state;
        }
    }
    state.rotation = frame.rotation * TurnFromDegrees(degrees);
    return state;
}

CameraState CinematicSampler::Picture(float time, std::string* shot, float* blend) const
{
    const std::vector<Shot>& shots = m_cinematic.shots;
    if (blend != nullptr)
    {
        *blend = 1.0f;
    }
    if (shots.empty())
    {
        if (!m_cinematic.cameras.empty())
        {
            if (shot != nullptr)
            {
                *shot = m_cinematic.cameras.front().name;
            }
            return Camera(m_cinematic.cameras.front(), time);
        }
        return {};
    }
    size_t current = 0;
    for (size_t i = 0; i < shots.size(); ++i)
    {
        if (shots[i].time <= time)
        {
            current = i;
        }
    }
    const CameraTrack* now = m_cinematic.FindCamera(shots[current].camera);
    if (shot != nullptr)
    {
        *shot = shots[current].camera;
    }
    if (now == nullptr)
    {
        return {};
    }
    const CameraState picture = Camera(*now, time);
    const float into = time - shots[current].time;
    if (current > 0 && shots[current].blend > 0.0f && into < shots[current].blend)
    {
        const CameraTrack* was = m_cinematic.FindCamera(shots[current - 1].camera);
        if (was != nullptr)
        {
            const float t = shots[current].ease.Apply(std::max(into, 0.0f) / shots[current].blend);
            if (blend != nullptr)
            {
                *blend = t;
            }
            return LerpCamera(Camera(*was, time), picture, t);
        }
    }
    return picture;
}

float CinematicSampler::Shake(float time) const
{
    return SampleFloat(m_cinematic.shake, time, 0.0f);
}

float CinematicSampler::ShakeSpeed(float time) const
{
    return SampleFloat(m_cinematic.shakeSpeed, time, 14.0f);
}

float CinematicSampler::Fade(float time) const
{
    return std::clamp(SampleFloat(m_cinematic.fade, time, 0.0f), 0.0f, 1.0f);
}

float CinematicSampler::Letterbox(float time) const
{
    return std::clamp(SampleFloat(m_cinematic.letterbox, time, 0.0f), 0.0f, 1.0f);
}

float CinematicSampler::LightIntensity(const LightTrack& light, float time) const
{
    return SampleFloat(light.intensity, time, 1.0f);
}

std::string CinematicSampler::Fill(const std::string& line) const
{
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '{')
        {
            const size_t close = line.find('}', i + 1);
            if (close != std::string::npos)
            {
                const auto word = m_bindings.words.find(line.substr(i + 1, close - i - 1));
                if (word != m_bindings.words.end())
                {
                    out += word->second;
                    i = close;
                    continue;
                }
            }
        }
        out += line[i];
    }
    return out;
}

std::string CinematicSampler::Text(const TextItem& item, size_t line, float time) const
{
    if (line >= item.lines.size() || time < item.time)
    {
        return {};
    }
    const std::string full = Fill(item.lines[line]);
    if (item.typeSpeed <= 0.0f)
    {
        return full;
    }
    // The lines are typed one after another, with a short pause between them.
    float typed = (time - item.time) * item.typeSpeed;
    for (size_t i = 0; i < line; ++i)
    {
        typed -= static_cast<float>(Fill(item.lines[i]).size()) + 6.0f;
    }
    if (typed <= 0.0f)
    {
        return {};
    }
    // A hair over, so that a letter due exactly now is not lost to the arithmetic.
    return full.substr(0, std::min(full.size(), static_cast<size_t>(typed + 1.0e-3f)));
}

bool CinematicSampler::Where(const std::string& at, float time, glm::vec3& out) const
{
    if (at.empty())
    {
        return false;
    }
    if (m_cinematic.FindActor(at) == nullptr && m_bindings.anchors.count(at) == 0)
    {
        return false;
    }
    out = Frame(at, time).position;
    return true;
}

std::vector<CinematicHappening> HappeningsBetween(const Cinematic& cinematic, float from, float to)
{
    std::vector<CinematicHappening> out;
    const auto collect = [&](CinematicHappening::Kind kind, size_t count, auto timeOf)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const float time = timeOf(i);
            if (time > from && time <= to)
            {
                out.push_back({kind, time, i});
            }
        }
    };
    collect(CinematicHappening::Kind::Marker, cinematic.markers.size(), [&](size_t i) { return cinematic.markers[i].time; });
    collect(CinematicHappening::Kind::Sound, cinematic.sounds.size(), [&](size_t i) { return cinematic.sounds[i].time; });
    collect(CinematicHappening::Kind::Clip, cinematic.clips.size(), [&](size_t i) { return cinematic.clips[i].time; });
    collect(CinematicHappening::Kind::Particle, cinematic.particles.size(), [&](size_t i) { return cinematic.particles[i].time; });
    collect(CinematicHappening::Kind::Text, cinematic.texts.size(), [&](size_t i) { return cinematic.texts[i].time; });
    std::stable_sort(out.begin(), out.end(), [](const CinematicHappening& a, const CinematicHappening& b) { return a.time < b.time; });
    return out;
}

// --- The file ----------------------------------------------------------------------------------------

namespace
{

json Vec(const glm::vec3& v)
{
    return json::array({v.x, v.y, v.z});
}

glm::vec3 ReadVec(const json& j, const char* key, const glm::vec3& none)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3)
    {
        return none;
    }
    return {j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>()};
}

json WriteEase(const Ease& ease)
{
    if (ease.kind == Ease::Kind::Curve)
    {
        return json::array({ease.a.x, ease.a.y, ease.b.x, ease.b.y});
    }
    return Ease::Name(ease.kind);
}

Ease ReadEase(const json& j, const char* key = "ease")
{
    Ease ease;
    if (!j.contains(key))
    {
        return ease;
    }
    const json& value = j[key];
    if (value.is_array() && value.size() == 4)
    {
        ease.kind = Ease::Kind::Curve;
        ease.a = {value[0].get<float>(), value[1].get<float>()};
        ease.b = {value[2].get<float>(), value[3].get<float>()};
        return ease;
    }
    const std::string name = value.is_string() ? value.get<std::string>() : std::string();
    for (const Ease::Kind kind : {Ease::Kind::Linear, Ease::Kind::Step, Ease::Kind::In, Ease::Kind::Out, Ease::Kind::InOut})
    {
        if (name == Ease::Name(kind))
        {
            ease.kind = kind;
        }
    }
    return ease;
}

json WriteFloats(const std::vector<FloatKey>& keys)
{
    json out = json::array();
    for (const FloatKey& key : keys)
    {
        out.push_back({{"t", key.time}, {"value", key.value}, {"ease", WriteEase(key.ease)}});
    }
    return out;
}

std::vector<FloatKey> ReadFloats(const json& j, const char* key)
{
    std::vector<FloatKey> keys;
    if (!j.contains(key) || !j[key].is_array())
    {
        return keys;
    }
    for (const json& entry : j[key])
    {
        keys.push_back({entry.value("t", 0.0f), entry.value("value", 0.0f), ReadEase(entry)});
    }
    return keys;
}

json WriteTransforms(const std::vector<TransformKey>& keys)
{
    json out = json::array();
    for (const TransformKey& key : keys)
    {
        out.push_back({{"t", key.time}, {"position", Vec(key.position)}, {"rotation", Vec(key.rotation)}, {"ease", WriteEase(key.ease)}});
    }
    return out;
}

std::vector<TransformKey> ReadTransforms(const json& j)
{
    std::vector<TransformKey> keys;
    if (!j.contains("keys") || !j["keys"].is_array())
    {
        return keys;
    }
    for (const json& entry : j["keys"])
    {
        keys.push_back({entry.value("t", 0.0f), ReadVec(entry, "position", glm::vec3(0.0f)), ReadVec(entry, "rotation", glm::vec3(0.0f)), ReadEase(entry)});
    }
    return keys;
}

template <typename T>
const json& ListOr(const json& j, const char* key)
{
    static const json kEmpty = json::array();
    return j.contains(key) && j[key].is_array() ? j[key] : kEmpty;
}

} // namespace

bool Cinematic::FromJsonText(const std::string& text, std::string* error)
{
    const json j = json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object())
    {
        if (error != nullptr)
        {
            *error = "not a JSON object";
        }
        return false;
    }
    *this = Cinematic{};
    name = j.value("name", std::string());
    duration = j.value("duration", 10.0f);
    pausesGameplay = j.value("pauses_gameplay", true);
    blendOut = j.value("blend_out", 1.0f);
    loop = j.value("loop", false);
    for (const json& entry : ListOr<ActorDef>(j, "actors"))
    {
        actors.push_back({entry.value("name", std::string()), entry.value("model", std::string()), entry.value("bind", std::string())});
    }
    for (const json& entry : ListOr<CameraTrack>(j, "cameras"))
    {
        CameraTrack camera;
        camera.name = entry.value("name", std::string());
        camera.anchor = entry.value("anchor", std::string());
        camera.lookAt = entry.value("look_at", std::string());
        camera.lookOffset = ReadVec(entry, "look_offset", glm::vec3(0.0f));
        for (const json& key : ListOr<CameraKey>(entry, "keys"))
        {
            camera.keys.push_back({key.value("t", 0.0f), ReadVec(key, "position", glm::vec3(0.0f)), ReadVec(key, "rotation", glm::vec3(0.0f)),
                                   key.value("fov", 60.0f), key.value("focus", 0.0f), ReadEase(key)});
        }
        cameras.push_back(std::move(camera));
    }
    for (const json& entry : ListOr<Shot>(j, "shots"))
    {
        shots.push_back({entry.value("t", 0.0f), entry.value("camera", std::string()), entry.value("blend", 0.0f), ReadEase(entry)});
    }
    for (const json& entry : ListOr<ActorTrack>(j, "actor_tracks"))
    {
        actorTracks.push_back({entry.value("actor", std::string()), entry.value("anchor", std::string()), ReadTransforms(entry)});
    }
    for (const json& entry : ListOr<PathFollow>(j, "paths"))
    {
        PathFollow follow;
        follow.actor = entry.value("actor", std::string());
        follow.path = entry.value("path", std::string());
        follow.start = entry.value("start", 0.0f);
        follow.end = entry.value("end", 1.0f);
        follow.ease = ReadEase(entry);
        follow.lift = entry.value("lift", 0.0f);
        follow.face = entry.value("face", true);
        paths.push_back(follow);
    }
    for (const json& entry : ListOr<ClipEvent>(j, "clips"))
    {
        clips.push_back({entry.value("t", 0.0f), entry.value("actor", std::string()), entry.value("clip", std::string()), entry.value("speed", 1.0f)});
    }
    for (const json& entry : ListOr<SoundEvent>(j, "sounds"))
    {
        sounds.push_back({entry.value("t", 0.0f), entry.value("sound", std::string()), entry.value("volume", 1.0f), entry.value("at", std::string()),
                          entry.value("music", false)});
    }
    for (const json& entry : ListOr<Marker>(j, "markers"))
    {
        markers.push_back({entry.value("t", 0.0f), entry.value("name", std::string()), entry.value("value", std::string())});
    }
    for (const json& entry : ListOr<TextItem>(j, "texts"))
    {
        TextItem item;
        item.time = entry.value("t", 0.0f);
        item.duration = entry.value("duration", 5.0f);
        item.style = entry.value("style", std::string("title_card"));
        item.typeSpeed = entry.value("type_speed", 28.0f);
        for (const json& line : ListOr<std::string>(entry, "lines"))
        {
            if (line.is_string())
            {
                item.lines.push_back(line.get<std::string>());
            }
        }
        texts.push_back(std::move(item));
    }
    for (const json& entry : ListOr<LightTrack>(j, "lights"))
    {
        lights.push_back({entry.value("light", std::string()), ReadFloats(entry, "intensity")});
    }
    for (const json& entry : ListOr<ParticleEvent>(j, "particles"))
    {
        particles.push_back({entry.value("t", 0.0f), entry.value("effect", std::string()), entry.value("at", std::string()),
                             ReadVec(entry, "offset", glm::vec3(0.0f)), entry.value("duration", 1.0f)});
    }
    shake = ReadFloats(j, "shake");
    shakeSpeed = ReadFloats(j, "shake_speed");
    fade = ReadFloats(j, "fade");
    letterbox = ReadFloats(j, "letterbox");
    Tidy();
    return true;
}

std::string Cinematic::ToJsonText() const
{
    json j;
    j["name"] = name;
    j["duration"] = duration;
    j["pauses_gameplay"] = pausesGameplay;
    j["blend_out"] = blendOut;
    j["loop"] = loop;
    j["actors"] = json::array();
    for (const ActorDef& actor : actors)
    {
        json entry{{"name", actor.name}};
        if (!actor.model.empty())
        {
            entry["model"] = actor.model;
        }
        if (!actor.bind.empty())
        {
            entry["bind"] = actor.bind;
        }
        j["actors"].push_back(entry);
    }
    j["cameras"] = json::array();
    for (const CameraTrack& camera : cameras)
    {
        json entry{{"name", camera.name}, {"anchor", camera.anchor}, {"look_at", camera.lookAt}, {"look_offset", Vec(camera.lookOffset)}};
        entry["keys"] = json::array();
        for (const CameraKey& key : camera.keys)
        {
            entry["keys"].push_back({{"t", key.time},
                                     {"position", Vec(key.position)},
                                     {"rotation", Vec(key.rotation)},
                                     {"fov", key.fov},
                                     {"focus", key.focus},
                                     {"ease", WriteEase(key.ease)}});
        }
        j["cameras"].push_back(entry);
    }
    j["shots"] = json::array();
    for (const Shot& shot : shots)
    {
        j["shots"].push_back({{"t", shot.time}, {"camera", shot.camera}, {"blend", shot.blend}, {"ease", WriteEase(shot.ease)}});
    }
    j["actor_tracks"] = json::array();
    for (const ActorTrack& track : actorTracks)
    {
        j["actor_tracks"].push_back({{"actor", track.actor}, {"anchor", track.anchor}, {"keys", WriteTransforms(track.keys)}});
    }
    j["paths"] = json::array();
    for (const PathFollow& follow : paths)
    {
        j["paths"].push_back({{"actor", follow.actor},
                              {"path", follow.path},
                              {"start", follow.start},
                              {"end", follow.end},
                              {"ease", WriteEase(follow.ease)},
                              {"lift", follow.lift},
                              {"face", follow.face}});
    }
    j["clips"] = json::array();
    for (const ClipEvent& clip : clips)
    {
        j["clips"].push_back({{"t", clip.time}, {"actor", clip.actor}, {"clip", clip.clip}, {"speed", clip.speed}});
    }
    j["sounds"] = json::array();
    for (const SoundEvent& sound : sounds)
    {
        j["sounds"].push_back({{"t", sound.time}, {"sound", sound.sound}, {"volume", sound.volume}, {"at", sound.at}, {"music", sound.music}});
    }
    j["markers"] = json::array();
    for (const Marker& marker : markers)
    {
        j["markers"].push_back({{"t", marker.time}, {"name", marker.name}, {"value", marker.value}});
    }
    j["texts"] = json::array();
    for (const TextItem& item : texts)
    {
        j["texts"].push_back(
            {{"t", item.time}, {"duration", item.duration}, {"style", item.style}, {"type_speed", item.typeSpeed}, {"lines", item.lines}});
    }
    j["lights"] = json::array();
    for (const LightTrack& light : lights)
    {
        j["lights"].push_back({{"light", light.light}, {"intensity", WriteFloats(light.intensity)}});
    }
    j["particles"] = json::array();
    for (const ParticleEvent& particle : particles)
    {
        j["particles"].push_back(
            {{"t", particle.time}, {"effect", particle.effect}, {"at", particle.at}, {"offset", Vec(particle.offset)}, {"duration", particle.duration}});
    }
    j["shake"] = WriteFloats(shake);
    j["shake_speed"] = WriteFloats(shakeSpeed);
    j["fade"] = WriteFloats(fade);
    j["letterbox"] = WriteFloats(letterbox);
    return j.dump(2);
}

bool Cinematic::LoadFromFile(const std::filesystem::path& file, std::string* error)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        if (error != nullptr)
        {
            *error = "not found: " + file.string();
        }
        return false;
    }
    std::stringstream text;
    text << stream.rdbuf();
    if (!FromJsonText(text.str(), error))
    {
        if (error != nullptr)
        {
            *error += ": " + file.string();
        }
        return false;
    }
    if (name.empty())
    {
        name = file.stem().string();
    }
    return true;
}

bool Cinematic::SaveToFile(const std::filesystem::path& file) const
{
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
    {
        return false;
    }
    stream << ToJsonText() << "\n";
    return static_cast<bool>(stream);
}

void Cinematic::Tidy()
{
    float last = 0.0f;
    const auto reach = [&](float time) { last = std::max(last, time); };
    for (CameraTrack& camera : cameras)
    {
        SortByTime(camera.keys);
        if (!camera.keys.empty())
        {
            reach(camera.keys.back().time);
        }
    }
    SortByTime(shots);
    for (ActorTrack& track : actorTracks)
    {
        SortByTime(track.keys);
        if (!track.keys.empty())
        {
            reach(track.keys.back().time);
        }
    }
    for (const PathFollow& follow : paths)
    {
        reach(follow.end);
    }
    SortByTime(clips);
    SortByTime(sounds);
    SortByTime(markers);
    SortByTime(texts);
    for (const TextItem& item : texts)
    {
        reach(item.time + item.duration);
    }
    for (LightTrack& light : lights)
    {
        SortByTime(light.intensity);
    }
    SortByTime(particles);
    SortByTime(shake);
    SortByTime(shakeSpeed);
    SortByTime(fade);
    SortByTime(letterbox);
    for (const Marker& marker : markers)
    {
        reach(marker.time);
    }
    duration = std::max(duration, last);
}

const CameraTrack* Cinematic::FindCamera(const std::string& camera) const
{
    for (const CameraTrack& track : cameras)
    {
        if (track.name == camera)
        {
            return &track;
        }
    }
    return nullptr;
}

const ActorDef* Cinematic::FindActor(const std::string& actor) const
{
    for (const ActorDef& def : actors)
    {
        if (def.name == actor)
        {
            return &def;
        }
    }
    return nullptr;
}

} // namespace pred
