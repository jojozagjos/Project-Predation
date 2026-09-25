#include "Game/Creature/CreatureRig.h"

#include "Engine/Animation/IK.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pred
{
namespace
{

float Wrap(float angle)
{
    return std::remainder(angle, glm::two_pi<float>());
}

float Smooth(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// How much of the gap to close this frame, for something easing towards a target at `rate`.
float Ease(float rate, float dt)
{
    return 1.0f - std::exp(-rate * std::max(dt, 0.0f));
}

glm::vec3 Apply(const glm::mat4& m, const glm::vec3& p)
{
    return glm::vec3(m * glm::vec4(p, 1.0f));
}

// A rotation about a point.
glm::mat4 RotateAbout(const glm::vec3& pivot, const glm::quat& turn)
{
    return glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(turn) * glm::translate(glm::mat4(1.0f), -pivot);
}

// A frame without the stretch breathing puts in it, for carrying other things rigidly.
glm::mat4 Unscaled(const glm::mat4& m)
{
    glm::mat4 out = m;
    for (int c = 0; c < 3; ++c)
    {
        const float length = glm::length(glm::vec3(m[c]));
        if (length > 1e-6f)
        {
            out[c] = m[c] / length;
        }
    }
    return out;
}

uint32_t Next(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

float Unit(uint32_t& state)
{
    return static_cast<float>(Next(state) & 0xFFFFFFu) / 16777215.0f;
}

} // namespace

const char* RigActionName(RigAction action)
{
    switch (action)
    {
    case RigAction::None:
        return "none";
    case RigAction::Swipe:
        return "swipe";
    case RigAction::Lunge:
        return "lunge";
    case RigAction::Bite:
        return "bite";
    case RigAction::Grab:
        return "grab";
    case RigAction::Carry:
        return "carry";
    case RigAction::Roar:
        return "roar";
    case RigAction::Bash:
        return "bash";
    case RigAction::Feed:
        return "feed";
    }
    return "?";
}

void CreatureRig::Init(const CreatureAnatomy& anatomy, const CreatureSkin& skin, const glm::vec3& position, float yaw)
{
    m_anatomy = &anatomy;
    m_skin = &skin;
    m_rest = anatomy.Rest();
    m_bones.resize(skin.bones.size());
    m_skinning.assign(skin.bones.size(), glm::mat4(1.0f));
    for (size_t b = 0; b < skin.bones.size(); ++b)
    {
        m_bones[b] = skin.bones[b].rest;
    }
    m_root = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(glm::angleAxis(-yaw, glm::vec3(0.0f, 1.0f, 0.0f)));
    m_feet.assign(m_rest.legs.size(), Foot{});
    for (size_t i = 0; i < m_rest.legs.size(); ++i)
    {
        const CreatureAnatomy::Leg& leg = m_rest.legs[i];
        Foot& foot = m_feet[i];
        foot.planted = Apply(m_root, leg.foot);
        foot.yaw = yaw;
        // A pair's two feet are opposite, and each pair is opposite the one in front of it: a diagonal
        // walk on four, two tripods on six, alternating on two.
        foot.group = (static_cast<int>(i / 2) + (leg.side > 0.0f ? 1 : 0)) % 2;
    }
    m_tailPoints.clear();
    m_tailVelocity.clear();
    if (!skin.tail.empty())
    {
        m_tailPoints.push_back(Apply(m_root, skin.bones[static_cast<size_t>(skin.tail.front())].head));
        for (const int bone : skin.tail)
        {
            m_tailPoints.push_back(Apply(m_root, skin.bones[static_cast<size_t>(bone)].tail));
        }
        m_tailVelocity.assign(m_tailPoints.size(), glm::vec3(0.0f));
    }
    m_lastYaw = yaw;
    m_random = anatomy.seed * 2654435761u + 1u;
    m_twitchAt = 1.5f + Unit(m_random) * 3.0f;
    m_initialised = true;
}

void CreatureRig::Flinch(int bone, const glm::vec3& direction, float strength)
{
    if (!m_initialised)
    {
        return;
    }
    // Knocked away from the hit, about an axis square to it: a round in the chest from the front tips
    // the chest back.
    const glm::vec3 local = glm::vec3(glm::inverse(m_root) * glm::vec4(direction, 0.0f));
    glm::vec3 axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), local);
    if (glm::length(axis) < 1e-4f)
    {
        axis = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    const bool head = bone >= 0 && static_cast<size_t>(bone) < m_skin->bones.size() &&
                      (m_skin->bones[static_cast<size_t>(bone)].kind == BoneKind::Head ||
                       m_skin->bones[static_cast<size_t>(bone)].kind == BoneKind::Neck);
    m_flinchVelocity += glm::normalize(axis) * strength * (head ? 9.0f : 5.0f);
}

void CreatureRig::PlaceFeet(const RigInput& input, float legScale)
{
    const float dt = std::max(input.dt, 0.0f);
    // Up, for the body: the world's up on the floor, out of the wall or down from the ceiling otherwise.
    // Everything about feet -- how far one is from where it should be, how high a step lifts it -- is
    // measured against this rather than against the world.
    const glm::vec3 surfaceUp = input.surface * glm::vec3(0.0f, 1.0f, 0.0f);
    const bool onFloor = surfaceUp.y > 0.95f;
    const glm::vec3 flatVelocity = input.velocity - surfaceUp * glm::dot(input.velocity, surfaceUp);
    const float speed = glm::length(flatVelocity);
    const glm::mat4 rootInverse = glm::inverse(m_root);
    const glm::vec3 localVelocity = glm::vec3(rootInverse * glm::vec4(flatVelocity, 0.0f));
    // A quicker step the faster it goes, and further ahead of the hip, so it runs rather than
    // shuffling fast.
    // Quicker steps while it turns, too: a body swinging round on feet that step at a walking pace leaves
    // them behind, twisted under it, which is what turning looked like.
    const float turning = std::abs(m_turnRate);
    const float stepTime = std::clamp(0.34f * legScale / (0.35f + speed * 0.5f + turning * 0.35f), 0.1f, 0.36f);
    // And put down where the body is turning to, not where it is: half a step ahead through the turn.
    const glm::quat turnAhead = glm::angleAxis(-m_turnRate * stepTime * 0.6f, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::vec3 lead = localVelocity * stepTime * 0.9f;
    if (glm::length(lead) > legScale * 0.55f)
    {
        lead = glm::normalize(lead) * legScale * 0.55f;
    }
    const float threshold = legScale * (speed > 0.2f ? 0.22f : 0.12f) + speed * 0.05f;

    int steppingGroup[2] = {0, 0};
    for (const Foot& foot : m_feet)
    {
        if (foot.stepping)
        {
            ++steppingGroup[foot.group];
        }
    }

    for (size_t i = 0; i < m_feet.size(); ++i)
    {
        Foot& foot = m_feet[i];
        const CreatureAnatomy::Leg& leg = m_rest.legs[i];
        glm::vec3 desired = Apply(m_root, turnAhead * glm::vec3(leg.foot.x, 0.0f, leg.foot.z) + lead);
        if (!onFloor)
        {
            // On a wall or a ceiling: wherever that surface actually is under the foot, and otherwise
            // the plane through its feet, which is where the root already put it.
            glm::vec3 hit;
            if (input.probe && input.probe(desired + surfaceUp * (m_anatomy->hipHeight + 0.3f), -surfaceUp,
                                           m_anatomy->hipHeight * 2.5f + 0.6f, hit))
            {
                desired = hit;
            }
        }
        else if (input.ground)
        {
            float height = 0.0f;
            if (input.ground(desired + glm::vec3(0.0f, m_anatomy->hipHeight + 0.3f, 0.0f), m_anatomy->hipHeight * 2.5f + 0.6f,
                             height))
            {
                desired.y = height;
            }
            else
            {
                desired.y = input.position.y;
            }
        }
        else
        {
            desired.y = input.position.y;
        }

        if (input.airborne > 0.0f)
        {
            // In the air: nothing to stand on. The feet tuck up under it, and step down wherever they
            // land.
            foot.reaching = true;
            foot.stepping = false;
            continue;
        }
        if (foot.reaching)
        {
            // Back from a reach or a jump: put down where it should be, in one short step from wherever
            // the hand or foot was.
            foot.reaching = false;
            foot.stepping = true;
            foot.progress = 0.0f;
            foot.duration = 0.16f;
            foot.from = foot.planted;
            foot.to = desired;
            foot.fromYaw = foot.yaw;
            foot.toYaw = input.yaw;
            continue;
        }
        if (foot.stepping)
        {
            foot.progress += dt / std::max(foot.duration, 0.05f);
            if (foot.progress >= 1.0f)
            {
                foot.stepping = false;
                foot.planted = foot.to;
                // Kept for whoever listens, and only so many: nothing asks in a test.
                if (m_footfalls.size() < 16)
                {
                    m_footfalls.push_back(foot.to);
                }
                foot.yaw = foot.toYaw;
                --steppingGroup[foot.group];
            }
            continue;
        }
        glm::vec3 apart = desired - foot.planted;
        const float heightApart = glm::dot(apart, surfaceUp);
        apart -= surfaceUp * heightApart;
        const float distance = glm::length(apart);
        const float twist = std::abs(Wrap(input.yaw - foot.yaw));
        const bool wants = distance > threshold || twist > 0.35f || std::abs(heightApart) > legScale * 0.35f;
        const bool mustNow = distance > threshold * 2.0f || twist > 0.8f;
        const bool otherDown = steppingGroup[1 - foot.group] == 0;
        if ((wants && otherDown) || mustNow)
        {
            foot.stepping = true;
            foot.progress = 0.0f;
            foot.duration = stepTime;
            foot.from = foot.planted;
            foot.to = desired;
            foot.fromYaw = foot.yaw;
            foot.toYaw = input.yaw;
            ++steppingGroup[foot.group];
        }
    }
}

void CreatureRig::Update(const RigInput& input)
{
    if (!m_initialised)
    {
        return;
    }
    const CreatureAnatomy& a = *m_anatomy;
    const CreatureSkin& skin = *m_skin;
    const float dt = std::max(input.dt, 0.0f);
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    const glm::vec3 surfaceUp = input.surface * up;
    m_root = glm::translate(glm::mat4(1.0f), input.position) * glm::mat4_cast(input.surface * glm::angleAxis(-input.yaw, up));
    const glm::mat4 rootInverse = glm::inverse(m_root);

    float legScale = 0.0f;
    for (const LegPair& pair : a.legs)
    {
        legScale += pair.upper + pair.lower;
    }
    legScale /= static_cast<float>(std::max<size_t>(a.legs.size(), 1));
    const glm::vec3 flatVelocity = input.velocity - surfaceUp * glm::dot(input.velocity, surfaceUp);
    const float speed = glm::length(flatVelocity);
    const float pace = std::clamp(speed / 1.2f, 0.0f, 1.0f);

    // Turning: how fast, smoothed, and the spine bent into it.
    const float turned = Wrap(input.yaw - m_lastYaw);
    m_lastYaw = input.yaw;
    const float rate = dt > 1e-5f ? turned / dt : 0.0f;
    m_turnRate += (rate - m_turnRate) * Ease(8.0f, dt);
    m_bend += (std::clamp(m_turnRate * 0.16f, -0.38f, 0.38f) - m_bend) * Ease(6.0f, dt);
    m_stride += speed * dt;

    PlaceFeet(input, legScale);

    // Where each foot is now, in the world.
    std::vector<glm::vec3> footWorld(m_feet.size());
    std::vector<float> footYaw(m_feet.size());
    for (size_t i = 0; i < m_feet.size(); ++i)
    {
        const Foot& foot = m_feet[i];
        if (foot.stepping)
        {
            const float s = Smooth(foot.progress);
            const float lift = legScale * 0.13f + 0.02f + (a.legs.empty() ? 0.0f : 0.0f);
            footWorld[i] = glm::mix(foot.from, foot.to, s) + surfaceUp * (lift * std::sin(glm::pi<float>() * std::clamp(foot.progress, 0.0f, 1.0f)));
            footYaw[i] = foot.fromYaw + Wrap(foot.toYaw - foot.fromYaw) * s;
        }
        else
        {
            footWorld[i] = foot.planted;
            footYaw[i] = foot.yaw;
        }
    }

    // How high the body rides: following the ground its feet are on, so it climbs a stair rather than
    // wading through it, and tipped to the slope between its front feet and its back ones.
    float footHeight = 0.0f;
    float frontHeight = 0.0f;
    float backHeight = 0.0f;
    int front = 0;
    int back = 0;
    int counted = 0;
    for (size_t i = 0; i < m_feet.size(); ++i)
    {
        if (m_feet[i].reaching)
        {
            continue;
        }
        const glm::vec3 footAt = m_feet[i].stepping ? glm::mix(m_feet[i].from, m_feet[i].to, Smooth(m_feet[i].progress)) : footWorld[i];
        const float height = glm::dot(footAt - input.position, surfaceUp);
        footHeight += height;
        ++counted;
        if (m_rest.legs[i].pair->along < 0.5f)
        {
            frontHeight += height;
            ++front;
        }
        else
        {
            backHeight += height;
            ++back;
        }
    }
    footHeight = counted > 0 ? footHeight / static_cast<float>(counted) : 0.0f;
    m_lift += (std::clamp(footHeight, -0.5f, 0.5f) * 0.7f - m_lift) * Ease(10.0f, dt);
    float pitchTarget = 0.0f;
    if (front > 0 && back > 0)
    {
        pitchTarget = std::atan2(frontHeight / static_cast<float>(front) - backHeight / static_cast<float>(back),
                                 std::max(a.length, 0.3f));
    }

    // The body's own motion on top of that.
    const float stepLength = std::max(legScale * 0.6f, 0.2f);
    const float bob = std::abs(std::sin(m_stride * glm::pi<float>() / stepLength)) * 0.03f * a.hipHeight * pace;
    const float crouchDrop = a.hipHeight * (0.35f * input.crouch + 0.32f * input.squeeze);
    const float draw = std::max(a.headLength * 2.0f, 0.6f);
    float forward = -0.25f * input.windup * draw * 0.4f; // drawn back before a strike
    float rise = 0.1f * input.windup * draw * 0.4f;
    const float phase = std::clamp(input.actionPhase, 0.0f, 1.0f);
    const float swing = std::sin(glm::pi<float>() * phase);
    switch (input.action)
    {
    case RigAction::Lunge:
        // Down and back to gather, then flung forward.
        forward += phase < 0.3f ? -0.1f * a.length * (phase / 0.3f) : 0.45f * a.length * Smooth((phase - 0.3f) / 0.3f) * (1.0f - Smooth((phase - 0.75f) / 0.25f));
        rise += phase < 0.3f ? -0.12f * a.hipHeight * (phase / 0.3f) : 0.0f;
        pitchTarget -= 0.15f * swing;
        break;
    case RigAction::Roar:
        pitchTarget += 0.35f * swing;
        rise += 0.12f * a.hipHeight * swing;
        break;
    case RigAction::Bash:
        forward += 0.22f * a.length * Smooth(phase / 0.4f) * (1.0f - Smooth((phase - 0.5f) / 0.5f));
        break;
    case RigAction::Swipe:
        pitchTarget += 0.08f * swing;
        break;
    default:
        break;
    }
    // Down over what it is eating: the whole body low and its front end lowest, so the mouth gets to it by
    // the body going down, not by the neck stretching out -- and staying down while it lifts its head to
    // look round, rather than standing up between mouthfuls.
    {
        const bool eating = input.action == RigAction::Feed;
        if (eating)
        {
            if (m_feed < 0.02f)
            {
                m_feedAt = input.actionTarget;
            }
            m_feedAt += (input.actionTarget - m_feedAt) * Ease(2.5f, dt);
        }
        m_feed += ((eating ? 1.0f : 0.0f) - m_feed) * Ease(eating ? 2.2f : 1.6f, dt);
        m_feedReach += ((eating && input.actionSide > 0 ? 1.0f : 0.0f) - m_feedReach) * Ease(3.0f, dt);
        rise -= 0.38f * a.hipHeight * m_feed;
        pitchTarget -= 0.55f * m_feed;
        forward += 0.08f * a.length * m_feed;
    }
    if (input.airborne > 0.0f)
    {
        // Nose up leaving the ground, nose down coming back to it.
        pitchTarget += 0.35f * std::cos(input.airborne * glm::pi<float>());
    }
    m_pitch += (std::clamp(pitchTarget, -0.6f, 0.6f) - m_pitch) * Ease(7.0f, dt);
    const float roll = std::sin(m_stride * glm::pi<float>() / stepLength) * 0.035f * pace;

    const glm::vec3 pivot{0.0f, a.hipHeight * 0.6f, 0.0f};
    const glm::mat4 body = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, m_lift + bob - crouchDrop + rise, -forward)) *
                           RotateAbout(pivot, glm::angleAxis(m_pitch, glm::vec3(1.0f, 0.0f, 0.0f)) *
                                                  glm::angleAxis(roll, glm::vec3(0.0f, 0.0f, 1.0f)));

    // The flinch, settling back on a spring.
    m_flinchVelocity += (-m_flinch * 90.0f - m_flinchVelocity * 12.0f) * dt;
    m_flinch += m_flinchVelocity * dt;
    if (glm::length(m_flinch) > 0.6f)
    {
        m_flinch = glm::normalize(m_flinch) * 0.6f;
    }
    const float flinchAngle = glm::length(m_flinch);
    const glm::quat flinch = flinchAngle > 1e-5f ? glm::angleAxis(flinchAngle, m_flinch / flinchAngle) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

    // --- The spine: carried by the body, bent into a turn, the chest breathing.
    const std::vector<int>& spine = skin.spine;
    std::array<glm::vec3, 4> joints{skin.bones[static_cast<size_t>(spine[0])].head, skin.bones[static_cast<size_t>(spine[1])].head,
                                    skin.bones[static_cast<size_t>(spine[2])].head, skin.bones[static_cast<size_t>(spine[2])].tail};
    const float middle = (joints[0].z + joints[3].z) * 0.5f;
    const std::array<float, 4> bendShare{-0.45f, -0.1f, 0.45f, 1.0f};
    for (size_t k = 0; k < joints.size(); ++k)
    {
        glm::vec3 p = joints[k];
        p = Apply(RotateAbout(glm::vec3(0.0f, p.y, middle), glm::angleAxis(-m_bend * bendShare[k], up)), p);
        joints[k] = Apply(body, p);
    }
    joints[3] = joints[2] + flinch * (joints[3] - joints[2]);
    const glm::vec3 bodyUp = glm::normalize(glm::vec3(body * glm::vec4(up, 0.0f)));
    for (size_t k = 0; k < 3; ++k)
    {
        m_bones[static_cast<size_t>(spine[k])] = BoneFrame(joints[k], joints[k + 1], bodyUp);
    }
    std::vector<glm::mat4> rigid(skin.bones.size(), glm::mat4(1.0f));
    for (size_t k = 0; k < 3; ++k)
    {
        const size_t b = static_cast<size_t>(spine[k]);
        rigid[b] = m_bones[b] * skin.bones[b].restInverse;
    }
    const float breath = std::sin(input.time * (1.6f + 1.8f * pace + 2.0f * input.windup)) * (0.018f + 0.01f * pace);
    {
        glm::mat4& chest = m_bones[static_cast<size_t>(skin.chest)];
        chest[0] *= 1.0f + breath;
        chest[2] *= 1.0f + breath;
    }

    // --- The neck and head: carried by the chest, drawn back to strike, turned to what it looks at.
    const glm::mat4& chestRigid = rigid[static_cast<size_t>(skin.chest)];
    const SkinBone& neckLow = skin.bones[static_cast<size_t>(skin.neck[0])];
    const SkinBone& neckHigh = skin.bones[static_cast<size_t>(skin.neck[1])];
    const SkinBone& headBone = skin.bones[static_cast<size_t>(skin.head)];
    const SkinBone& jawBone = skin.bones[static_cast<size_t>(skin.jaw)];
    std::array<glm::vec3, 4> neck{Apply(chestRigid, neckLow.head), Apply(chestRigid, neckLow.tail),
                                  Apply(chestRigid, neckHigh.tail), Apply(chestRigid, headBone.tail)};
    glm::vec3 hinge = Apply(chestRigid, jawBone.head);
    glm::vec3 chin = Apply(chestRigid, jawBone.tail);
    glm::vec3 headUp = glm::normalize(glm::vec3(chestRigid * glm::vec4(up, 0.0f)));

    // Where it wants to look, in its own frame, and how far it may turn its head to do it.
    float yawTarget = 0.12f * std::sin(input.time * 0.37f + static_cast<float>(a.seed % 17u));
    float pitchTarget2 = 0.05f * std::sin(input.time * 0.23f);
    if (input.look)
    {
        const glm::vec3 local = Apply(rootInverse, input.lookAt) - neck[2];
        const float horizontal = glm::length(glm::vec2(local.x, local.z));
        if (horizontal > 0.15f || std::abs(local.y) > 0.15f)
        {
            yawTarget = std::atan2(local.x, -local.z);
            pitchTarget2 = std::atan2(local.y, std::max(horizontal, 0.05f)) - m_pitch;
        }
    }
    // Every few seconds, a twitch: a quick jerk of the head that settles. Nothing alive holds still
    // like a statue, and nothing this alive moves smoothly.
    if (input.time >= m_twitchAt)
    {
        m_twitch = {(Unit(m_random) - 0.5f) * 0.7f, (Unit(m_random) - 0.5f) * 0.45f};
        m_twitchUntil = input.time + 0.25f + Unit(m_random) * 0.35f;
        m_twitchAt = input.time + 1.5f + Unit(m_random) * 4.5f;
    }
    if (input.time < m_twitchUntil)
    {
        yawTarget += m_twitch.x;
        pitchTarget2 += m_twitch.y;
    }
    yawTarget = std::clamp(Wrap(yawTarget), -1.35f, 1.35f);
    // Head down in something on the floor, it can bend a long way further than it ever looks down.
    pitchTarget2 = std::clamp(pitchTarget2, -0.8f - 0.65f * m_feed, 0.85f);
    const float headRate = input.time < m_twitchUntil ? 30.0f : 9.0f;
    m_headYaw += (yawTarget - m_headYaw) * Ease(headRate, dt);
    m_headPitch += (pitchTarget2 - m_headPitch) * Ease(headRate, dt);

    // Strikes with the head: drawn back, then snapped forward.
    glm::vec3 thrust{0.0f};
    const glm::vec3 headAhead = glm::normalize(neck[3] - neck[2]);
    if (input.action == RigAction::Bite || input.action == RigAction::Lunge)
    {
        const float s = phase < 0.4f ? -0.5f * Smooth(phase / 0.4f) : -0.5f + 1.5f * Smooth((phase - 0.4f) / 0.25f) * (1.0f - Smooth((phase - 0.7f) / 0.3f));
        thrust = headAhead * (s * a.neckLength * 0.9f);
    }
    else if (input.action == RigAction::Roar)
    {
        m_headPitch = glm::mix(m_headPitch, 0.7f, swing);
    }
    else if (input.action == RigAction::Feed)
    {
        // Head bent down into what it is eating (the look does the bending, the body the lowering) and
        // tearing at it: a short pull back and up, and a shake from side to side. Never a stretch.
        const float tear = std::sin(phase * glm::two_pi<float>());
        const glm::vec3 across = glm::normalize(glm::cross(headAhead, glm::vec3(0.0f, 1.0f, 0.0f)) + glm::vec3(1e-4f, 0.0f, 0.0f));
        thrust = (-headAhead * (0.1f * a.headLength * std::max(tear, 0.0f)) +
                  across * (0.05f * a.headLength * std::sin(phase * glm::two_pi<float>() * 2.0f)) +
                  glm::vec3(0.0f, 0.08f * a.headLength * std::max(tear, 0.0f), 0.0f)) *
                 m_feedReach;
    }
    thrust += glm::vec3(0.0f, 0.12f, 0.2f) * input.windup * a.headLength;
    for (size_t k = 1; k < neck.size(); ++k)
    {
        neck[k] += thrust * (static_cast<float>(k) / 3.0f);
    }
    hinge += thrust;
    chin += thrust;

    // Turned in three parts: the base of the neck, the top of it, and the head on the neck.
    const std::array<float, 3> share{0.3f, 0.3f, 0.4f};
    for (size_t k = 0; k < 3; ++k)
    {
        const glm::quat turn = glm::angleAxis(-m_headYaw * share[k], up) * glm::angleAxis(m_headPitch * share[k], glm::vec3(1.0f, 0.0f, 0.0f)) *
                               (k == 2 ? glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), flinch, 0.7f) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        const glm::mat4 about = RotateAbout(neck[k], turn);
        for (size_t j = k + 1; j < neck.size(); ++j)
        {
            neck[j] = Apply(about, neck[j]);
        }
        hinge = Apply(about, hinge);
        chin = Apply(about, chin);
        headUp = glm::normalize(turn * headUp);
    }
    // Its mouth in what it is eating: the neck bent, as two lengths that do not change, so that the front of
    // the face arrives at the place it is biting -- arched up at the middle the way a neck arches, never
    // stretched. Eased in and out with how far it is into it.
    if (m_feedReach > 0.01f)
    {
        const glm::vec3 mouthAt = Apply(rootInverse, m_feedAt) + glm::vec3(0.0f, 0.04f, 0.0f);
        const float first = glm::distance(neck[0], neck[2]);
        const float second = glm::distance(neck[2], neck[3]);
        const float toMiddle = glm::distance(neck[0], neck[1]);
        const TwoBoneIKResult bent =
            SolveTwoBoneIK(neck[0], mouthAt, glm::vec3(0.0f, 1.0f, 0.3f), std::max(first, 0.01f), std::max(second, 0.01f));
        const glm::vec3 oldHead = neck[3] - neck[2];
        const glm::vec3 bentMiddle = glm::mix(neck[2], bent.jointPosition, m_feedReach);
        const glm::vec3 bentFront = glm::mix(neck[3], bent.endPosition, m_feedReach);
        const glm::vec3 newHead = bentFront - bentMiddle;
        const glm::quat headTurn = glm::length(oldHead) > 1e-4f && glm::length(newHead) > 1e-4f
                                       ? RotationBetween(glm::normalize(oldHead), glm::normalize(newHead))
                                       : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        hinge = bentMiddle + headTurn * (hinge - neck[2]);
        chin = bentMiddle + headTurn * (chin - neck[2]);
        headUp = glm::normalize(headTurn * headUp);
        const glm::vec3 along = bentMiddle - neck[0];
        neck[1] = neck[0] + (glm::length(along) > 1e-4f ? glm::normalize(along) * toMiddle : neck[1] - neck[0]);
        neck[2] = bentMiddle;
        neck[3] = bentFront;
    }
    m_bones[static_cast<size_t>(skin.neck[0])] = BoneFrame(neck[0], neck[1], headUp);
    m_bones[static_cast<size_t>(skin.neck[1])] = BoneFrame(neck[1], neck[2], headUp);
    m_bones[static_cast<size_t>(skin.head)] = BoneFrame(neck[2], neck[3], headUp);

    // The jaw: hanging as open as this one's hangs, breathing, gaping to strike and to roar.
    // Shut or nearly at rest, breathing through it; a maw hangs open. Never shut further than shut.
    float gape = 1.5f + 9.0f * a.gape * a.gape + (a.headShape == HeadShape::Maw ? 10.0f : 0.0f) + 30.0f * input.windup +
                 std::sin(input.time * 1.9f + static_cast<float>(a.seed % 97u)) * 1.5f;
    switch (input.action)
    {
    case RigAction::Bite:
        gape += phase < 0.55f ? 40.0f * Smooth(phase / 0.4f) : 40.0f * (1.0f - Smooth((phase - 0.55f) / 0.1f)) - 8.0f;
        break;
    case RigAction::Lunge:
    case RigAction::Roar:
        gape += 45.0f * swing;
        break;
    case RigAction::Swipe:
    case RigAction::Grab:
        gape += 15.0f * swing;
        break;
    default:
        break;
    }
    const float jawLength = glm::distance(jawBone.head, jawBone.tail);
    gape *= std::clamp(0.3f / std::max(jawLength, 0.05f), 0.35f, 1.0f);
    gape = std::max(gape, 0.0f);
    const glm::vec3 headSide = glm::normalize(glm::vec3(m_bones[static_cast<size_t>(skin.head)][0]));
    chin = Apply(RotateAbout(hinge, glm::angleAxis(-(glm::radians(gape) - skin.restGape), headSide)), chin);
    m_bones[static_cast<size_t>(skin.jaw)] = BoneFrame(hinge, chin, headUp);

    // --- The tail: points in the world swinging after the body on springs.
    if (!skin.tail.empty())
    {
        const glm::mat4& pelvisRigid = rigid[static_cast<size_t>(skin.pelvis)];
        const float wag = (0.05f + 0.1f * pace + 0.15f * input.windup) * a.tailLength;
        std::vector<glm::vec3> target(m_tailPoints.size());
        target[0] = Apply(m_root, Apply(pelvisRigid, skin.bones[static_cast<size_t>(skin.tail.front())].head));
        for (size_t k = 0; k < skin.tail.size(); ++k)
        {
            const float u = static_cast<float>(k + 1) / static_cast<float>(skin.tail.size());
            const glm::vec3 rest = Apply(pelvisRigid, skin.bones[static_cast<size_t>(skin.tail[k])].tail);
            const glm::vec3 side = glm::vec3(std::sin(input.time * 1.4f - u * 2.0f) * wag * u * u, 0.0f, 0.0f);
            target[k + 1] = Apply(m_root, rest + side);
        }
        m_tailPoints[0] = target[0];
        for (size_t k = 1; k < m_tailPoints.size(); ++k)
        {
            const float stiffness = 60.0f - 30.0f * static_cast<float>(k) / static_cast<float>(m_tailPoints.size());
            m_tailVelocity[k] += ((target[k] - m_tailPoints[k]) * stiffness - m_tailVelocity[k] * 7.0f) * dt;
            m_tailPoints[k] += m_tailVelocity[k] * dt;
            const SkinBone& bone = skin.bones[static_cast<size_t>(skin.tail[k - 1])];
            const float length = glm::distance(bone.head, bone.tail);
            const glm::vec3 along = m_tailPoints[k] - m_tailPoints[k - 1];
            const float have = glm::length(along);
            m_tailPoints[k] = m_tailPoints[k - 1] + (have > 1e-5f ? along / have : glm::normalize(target[k] - target[k - 1])) * length;
            // Dragged along whatever it is on rather than through it.
            const float above = glm::dot(m_tailPoints[k] - input.position, surfaceUp);
            if (above < bone.radius * 0.8f)
            {
                m_tailPoints[k] += surfaceUp * (bone.radius * 0.8f - above);
            }
            if (glm::distance(m_tailPoints[k], target[k]) > a.tailLength * 1.5f)
            {
                m_tailPoints[k] = target[k];
                m_tailVelocity[k] = glm::vec3(0.0f);
            }
        }
        for (size_t k = 0; k < skin.tail.size(); ++k)
        {
            m_bones[static_cast<size_t>(skin.tail[k])] =
                BoneFrame(Apply(rootInverse, m_tailPoints[k]), Apply(rootInverse, m_tailPoints[k + 1]), bodyUp);
        }
    }

    // --- The limbs: from where the body carries each hip to where its foot is, bent at the knee.
    for (size_t i = 0; i < skin.limbs.size(); ++i)
    {
        const CreatureSkin::Limb& limb = skin.limbs[i];
        const SkinBone& upper = skin.bones[static_cast<size_t>(limb.upper)];
        const SkinBone& lower = skin.bones[static_cast<size_t>(limb.lower)];
        const SkinBone& end = skin.bones[static_cast<size_t>(limb.end)];
        const CreatureAnatomy::Leg& leg = m_rest.legs[i];
        const LegPair& pair = *leg.pair;
        const glm::vec3 hip = Apply(rigid[static_cast<size_t>(upper.parent)], upper.head);
        glm::vec3 foot = Apply(rootInverse, footWorld[i]);
        glm::vec3 toeForward{std::sin(footYaw[i] - input.yaw), 0.0f, -std::cos(footYaw[i] - input.yaw)};

        // An action takes a front limb off the floor. Which ones: a crawler's arms, or anything's front
        // pair.
        const bool forelimb = pair.arm || (pair.along < 0.2f && a.plan != BodyPlan::Biped);
        const glm::vec3 target = Apply(rootInverse, input.action == RigAction::Feed ? m_feedAt : input.actionTarget);
        float reach = 0.0f;
        glm::vec3 reachTo = foot;
        if (forelimb && input.action != RigAction::None && input.action != RigAction::Bite && input.action != RigAction::Roar)
        {
            const int side = leg.side > 0.0f ? 1 : -1;
            const glm::vec3 shoulder = hip;
            const float span = pair.upper + pair.lower;
            const auto clampReach = [&](const glm::vec3& want)
            {
                const glm::vec3 d = want - shoulder;
                const float length = glm::length(d);
                return length > span * 0.97f ? shoulder + d / length * span * 0.97f : want;
            };
            switch (input.action)
            {
            case RigAction::Swipe:
                if (side == input.actionSide)
                {
                    // Drawn up and back, raked across, and down again.
                    const glm::vec3 raised = shoulder + glm::vec3(side * span * 0.35f, span * 0.45f, span * 0.1f);
                    const glm::vec3 through = target + glm::vec3(-side * span * 0.35f, -span * 0.1f, 0.0f);
                    if (phase < 0.4f)
                    {
                        reachTo = glm::mix(foot, raised, Smooth(phase / 0.4f));
                    }
                    else if (phase < 0.65f)
                    {
                        reachTo = glm::mix(raised, through, Smooth((phase - 0.4f) / 0.25f));
                    }
                    else
                    {
                        reachTo = glm::mix(through, foot, Smooth((phase - 0.65f) / 0.35f));
                    }
                    reach = 1.0f;
                }
                break;
            case RigAction::Lunge:
                reachTo = glm::mix(foot, target + glm::vec3(side * 0.18f, 0.2f, 0.0f), Smooth((phase - 0.25f) / 0.3f));
                reach = phase > 0.25f && phase < 0.9f ? 1.0f : 0.0f;
                break;
            case RigAction::Grab:
            case RigAction::Carry:
            {
                // Each hand on its own side of what it is holding. With the sides the other way round it
                // carried people with its arms crossed over them, which is not how anything carries anything.
                const glm::vec3 hold = input.action == RigAction::Carry
                                           ? shoulder + glm::vec3(side * span * 0.16f, -span * 0.3f, -span * 0.6f)
                                           : target + glm::vec3(side * 0.18f, 0.05f, 0.0f);
                reachTo = glm::mix(foot, hold, input.action == RigAction::Carry ? 1.0f : Smooth(phase / 0.35f));
                reach = 1.0f;
                break;
            }
            case RigAction::Bash:
                reachTo = glm::mix(foot, shoulder + glm::vec3(side * 0.1f, -span * 0.2f, -span * 0.75f), swing);
                reach = swing > 0.1f ? 1.0f : 0.0f;
                break;
            case RigAction::Feed:
                // Forefeet planted on the floor under its shoulders and a little towards what it is eating,
                // holding it down at the near edge: not stretched out to it, and never over its back.
            {
                // At a comfortable reach from the shoulder, forward towards it and either side of it: close in
                // under the shoulder, a long arm folds up with its elbow over its back.
                const glm::vec3 below{shoulder.x, target.y, shoulder.z};
                const float height = std::max(shoulder.y - target.y, 0.0f);
                const float comfortable = span * 0.66f;
                const float ahead = std::sqrt(std::max(comfortable * comfortable - height * height, 0.04f));
                // Straight out ahead of its own shoulder, each hand on its own side of the body: aimed at the one
                // place it was eating, the two long arms crossed over each other.
                reachTo = glm::vec3(static_cast<float>(side) * (std::max(std::abs(shoulder.x), 0.2f) + 0.2f), target.y + 0.02f,
                                    shoulder.z - ahead);
                reach = 1.0f;
                break;
            }
            default:
                break;
            }
            // Feeding moves slowly: where each forefoot is going is followed, not jumped to, and it comes off
            // the floor and back down over a moment rather than in one frame.
            if (input.action == RigAction::Feed && reach > 0.0f)
            {
                Foot& state = m_feet[i];
                const glm::vec3 wanted = Apply(m_root, clampReach(reachTo));
                if (state.gentle < 0.01f)
                {
                    state.gentleAt = Apply(m_root, foot);
                }
                state.gentleAt += (wanted - state.gentleAt) * Ease(4.0f, dt);
                state.gentle += (1.0f - state.gentle) * Ease(3.0f, dt);
                reachTo = glm::mix(foot, Apply(rootInverse, state.gentleAt), state.gentle);
            }
            else
            {
                m_feet[i].gentle = 0.0f;
            }
            if (reach > 0.0f)
            {
                reachTo = clampReach(reachTo);
                foot = reachTo;
                const glm::vec3 toward = target - reachTo;
                if (glm::length(glm::vec2(toward.x, toward.z)) > 0.05f)
                {
                    toeForward = glm::normalize(glm::vec3(toward.x, 0.0f, toward.z));
                }
                m_feet[i].reaching = true;
            }
        }
        // Done feeding: each forefoot back to walking over a moment, not in one frame.
        if (forelimb && input.action == RigAction::None && m_feet[i].gentle > 0.0f && input.airborne <= 0.0f)
        {
            Foot& state = m_feet[i];
            state.gentle -= state.gentle * Ease(3.0f, dt);
            if (state.gentle < 0.03f)
            {
                state.gentle = 0.0f;
            }
            else
            {
                foot = glm::mix(foot, Apply(rootInverse, state.gentleAt), state.gentle);
                reach = 1.0f;
                state.reaching = true;
            }
        }
        if (input.airborne > 0.0f)
        {
            // Tucked up under the body in the air.
            const float tuck = std::sin(glm::pi<float>() * std::clamp(input.airborne, 0.0f, 1.0f));
            foot = hip + glm::vec3(0.0f, -(pair.upper + pair.lower) * (0.85f - 0.35f * tuck), pair.arm ? -0.15f * tuck : 0.1f * tuck);
            foot.y -= pair.thickness;
            m_feet[i].reaching = true;
            m_feet[i].planted = Apply(m_root, foot);
        }
        else if (reach > 0.0f)
        {
            m_feet[i].planted = Apply(m_root, glm::vec3(foot.x, foot.y - pair.thickness, foot.z));
        }

        const glm::vec3 ankleTarget = reach > 0.0f || input.airborne > 0.0f ? foot : foot + glm::vec3(0.0f, pair.thickness, 0.0f);
        // Down over something it is eating, its forelimbs brace with the elbows out to the sides, as a crouched
        // animal holds them, rather than folded up over its back.
        const glm::vec3 pole = forelimb && m_feed > 0.05f
                                   ? glm::normalize(glm::mix(LimbPole(a, leg), glm::vec3(leg.side, -0.35f, 0.35f), std::min(m_feed * 1.5f, 1.0f)))
                                   : LimbPole(a, leg);
        const TwoBoneIKResult ik = SolveTwoBoneIK(hip, ankleTarget, pole, pair.upper, pair.lower);
        const glm::vec3 knee = ik.jointPosition;
        const glm::vec3 ankle = ik.endPosition;
        const glm::vec3 toe = ankle + glm::vec3(0.0f, reach > 0.0f ? 0.0f : -pair.thickness * 0.5f, 0.0f) + toeForward * pair.foot;
        glm::vec3 bend = glm::cross(knee - hip, ankle - knee);
        bend = glm::length(bend) > 1e-6f ? glm::normalize(bend) : limb.bend;
        m_bones[static_cast<size_t>(limb.upper)] = BoneFrame(hip, knee, bend);
        m_bones[static_cast<size_t>(limb.lower)] = BoneFrame(knee, ankle, bend);
        m_bones[static_cast<size_t>(limb.end)] = BoneFrame(ankle, toe, bend);
        (void)lower;
        (void)end;
    }

    for (size_t b = 0; b < skin.bones.size(); ++b)
    {
        m_skinning[b] = m_bones[b] * skin.bones[b].restInverse;
    }
}

} // namespace pred
