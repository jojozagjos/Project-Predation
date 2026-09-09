#include "Game/Player/PlayerTypes.h"

#include "Engine/Core/Log.h"

#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace pred
{

const char* PlayerStanceName(PlayerStance stance)
{
    switch (stance)
    {
    case PlayerStance::Standing:
        return "standing";
    case PlayerStance::Crouching:
        return "crouching";
    case PlayerStance::Prone:
        return "prone";
    }
    return "unknown";
}

float PlayerState::HorizontalSpeed() const
{
    return glm::length(glm::vec2(velocity.x, velocity.z));
}

float PlayerConfig::HeightForStance(PlayerStance stance) const
{
    switch (stance)
    {
    case PlayerStance::Crouching:
        return crouchHeight;
    case PlayerStance::Prone:
        return proneHeight;
    case PlayerStance::Standing:
    default:
        return standHeight;
    }
}

float PlayerConfig::EyeHeightForStance(PlayerStance stance) const
{
    switch (stance)
    {
    case PlayerStance::Crouching:
        return crouchEyeHeight;
    case PlayerStance::Prone:
        return proneEyeHeight;
    case PlayerStance::Standing:
    default:
        return standEyeHeight;
    }
}

float PlayerConfig::StrideLength(float speed) const
{
    return std::clamp(strideLengthBase + strideLengthPerSpeed * speed, 0.45f, strideLengthMax);
}

float PlayerConfig::StanceFraction(float speed) const
{
    // Walking pace keeps both feet down for part of the cycle; by sprint speed the stances no
    // longer overlap and there is a moment with neither foot on the ground.
    const float t = std::clamp((speed - walkSpeed) / std::max(sprintSpeed - walkSpeed, 0.1f), 0.0f, 1.0f);
    return stanceFractionWalk + (stanceFractionRun - stanceFractionWalk) * t;
}

float PlayerConfig::SpeedForStance(PlayerStance stance, bool sprint, bool walk) const
{
    switch (stance)
    {
    case PlayerStance::Crouching:
        return crouchSpeed;
    case PlayerStance::Prone:
        return proneSpeed;
    case PlayerStance::Standing:
    default:
        break;
    }
    // Sprinting and slow-walking are only meaningful while standing.
    if (walk)
    {
        return walkSpeed;
    }
    return sprint ? sprintSpeed : moveSpeed;
}

namespace
{

// Reads a value if present, otherwise leaves the existing default alone. Missing keys are not
// errors: a partial player.json overriding two values is a legitimate and useful thing to write.
template <typename T>
void ReadField(const nlohmann::json& node, const char* key, T& out)
{
    if (const auto it = node.find(key); it != node.end() && !it->is_null())
    {
        try
        {
            out = it->get<T>();
        }
        catch (const nlohmann::json::exception& ex)
        {
            PRED_LOG_WARN(Gameplay, "player config: field '{}' has the wrong type ({})", key, ex.what());
        }
    }
}

} // namespace

bool PlayerConfig::LoadFromFile(const std::filesystem::path& file)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        PRED_LOG_WARN(Gameplay, "Player config not found, using built-in defaults: {}", file.string());
        return false;
    }

    const nlohmann::json json = nlohmann::json::parse(stream, nullptr, false, true);
    if (json.is_discarded() || !json.is_object())
    {
        PRED_LOG_ERROR(Gameplay, "Player config is not valid JSON: {}", file.string());
        return false;
    }

    if (const auto it = json.find("speeds"); it != json.end())
    {
        ReadField(*it, "walk", walkSpeed);
        ReadField(*it, "move", moveSpeed);
        ReadField(*it, "sprint", sprintSpeed);
        ReadField(*it, "crouch", crouchSpeed);
        ReadField(*it, "prone", proneSpeed);
        ReadField(*it, "backward_scale", backwardScale);
        ReadField(*it, "strafe_scale", strafeScale);
    }
    if (const auto it = json.find("acceleration"); it != json.end())
    {
        ReadField(*it, "ground", groundAcceleration);
        ReadField(*it, "ground_decel", groundDeceleration);
        ReadField(*it, "air", airAcceleration);
    }
    if (const auto it = json.find("jump"); it != json.end())
    {
        ReadField(*it, "height", jumpHeight);
        ReadField(*it, "gravity_scale", gravityScale);
        ReadField(*it, "fall_gravity_scale", fallGravityScale);
        ReadField(*it, "coyote_time", coyoteTime);
        ReadField(*it, "buffer_time", jumpBufferTime);
    }
    if (const auto it = json.find("capsule"); it != json.end())
    {
        ReadField(*it, "stand_height", standHeight);
        ReadField(*it, "crouch_height", crouchHeight);
        ReadField(*it, "prone_height", proneHeight);
        ReadField(*it, "radius", radius);
        ReadField(*it, "step_height", stepHeight);
        ReadField(*it, "max_slope_angle", maxSlopeAngle);
    }
    if (const auto it = json.find("camera"); it != json.end())
    {
        ReadField(*it, "stand_eye_height", standEyeHeight);
        ReadField(*it, "crouch_eye_height", crouchEyeHeight);
        ReadField(*it, "prone_eye_height", proneEyeHeight);
        ReadField(*it, "eye_transition_speed", eyeTransitionSpeed);
        ReadField(*it, "step_smooth_speed", stepSmoothSpeed);
        ReadField(*it, "step_smooth_max", stepSmoothMax);
        ReadField(*it, "landing_dip_per_speed", landingDipPerSpeed);
        ReadField(*it, "landing_dip_max", landingDipMax);
        ReadField(*it, "landing_recover_speed", landingRecoverSpeed);
        ReadField(*it, "bob_amount", bobAmount);
        ReadField(*it, "bob_walk_lower", bobWalkLower);
        ReadField(*it, "bob_sprint_scale", bobSprintScale);
        ReadField(*it, "max_pitch_degrees", maxPitchDegrees);
    }
    if (const auto it = json.find("stride"); it != json.end())
    {
        ReadField(*it, "length_base", strideLengthBase);
        ReadField(*it, "length_per_speed", strideLengthPerSpeed);
        ReadField(*it, "length_max", strideLengthMax);
        ReadField(*it, "stance_fraction_walk", stanceFractionWalk);
        ReadField(*it, "stance_fraction_run", stanceFractionRun);
    }
    if (const auto it = json.find("fall_damage"); it != json.end())
    {
        ReadField(*it, "enabled", fallDamageEnabled);
        ReadField(*it, "min_speed", fallDamageMinSpeed);
        ReadField(*it, "lethal_speed", fallDamageLethalSpeed);
    }

    PRED_LOG_INFO(Gameplay, "Loaded player config from {}", file.string());
    return true;
}

bool PlayerConfig::SaveToFile(const std::filesystem::path& file) const
{
    nlohmann::json json;
    json["speeds"] = {{"walk", walkSpeed},         {"move", moveSpeed},
                      {"sprint", sprintSpeed},     {"crouch", crouchSpeed},
                      {"prone", proneSpeed},       {"backward_scale", backwardScale},
                      {"strafe_scale", strafeScale}};
    json["acceleration"] = {
        {"ground", groundAcceleration}, {"ground_decel", groundDeceleration}, {"air", airAcceleration}};
    json["jump"] = {{"height", jumpHeight},
                    {"gravity_scale", gravityScale},
                    {"fall_gravity_scale", fallGravityScale},
                    {"coyote_time", coyoteTime},
                    {"buffer_time", jumpBufferTime}};
    json["capsule"] = {{"stand_height", standHeight}, {"crouch_height", crouchHeight},
                       {"prone_height", proneHeight}, {"radius", radius},
                       {"step_height", stepHeight},   {"max_slope_angle", maxSlopeAngle}};
    json["camera"] = {{"stand_eye_height", standEyeHeight},
                      {"crouch_eye_height", crouchEyeHeight},
                      {"prone_eye_height", proneEyeHeight},
                      {"eye_transition_speed", eyeTransitionSpeed},
                      {"step_smooth_speed", stepSmoothSpeed},
                      {"step_smooth_max", stepSmoothMax},
                      {"landing_dip_per_speed", landingDipPerSpeed},
                      {"landing_dip_max", landingDipMax},
                      {"landing_recover_speed", landingRecoverSpeed},
                      {"bob_amount", bobAmount},
                      {"bob_walk_lower", bobWalkLower},
                      {"bob_sprint_scale", bobSprintScale},
                      {"max_pitch_degrees", maxPitchDegrees}};
    json["stride"] = {{"length_base", strideLengthBase},
                      {"length_per_speed", strideLengthPerSpeed},
                      {"length_max", strideLengthMax},
                      {"stance_fraction_walk", stanceFractionWalk},
                      {"stance_fraction_run", stanceFractionRun}};
    json["fall_damage"] = {
        {"enabled", fallDamageEnabled}, {"min_speed", fallDamageMinSpeed}, {"lethal_speed", fallDamageLethalSpeed}};

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream stream(file);
    if (!stream.is_open())
    {
        PRED_LOG_ERROR(Gameplay, "Cannot write player config: {}", file.string());
        return false;
    }
    stream << json.dump(2) << '\n';
    PRED_LOG_INFO(Gameplay, "Saved player config to {}", file.string());
    return true;
}

} // namespace pred
