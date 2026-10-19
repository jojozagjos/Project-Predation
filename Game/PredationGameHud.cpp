#include "Game/PredationGame.h"
#include "Game/OverlayLayer.h"
#include "Game/PredationGameShared.h"

#include "Engine/Render/DepthConvention.h"
#include "Game/Creature/CreatureTuning.h"

#include "Engine/Core/BuildInfo.h"
#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Paths.h"
#include "Engine/Debug/DebugCategories.h"
#include "Engine/Assets/GltfImport.h"
#include "Engine/Debug/ImGuiLayer.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Primitives.h"
#include "Game/Weapons/WeaponAppearance.h"
#include "Game/World/TestMap.h"
#include "Game/World/KestrelStation.h"
#include "Game/World/Surfaces.h"
#include "Engine/Audio/Sound.h"
#include "Engine/Debug/FrameStats.h"
#include "Engine/Platform/Window.h"
#include "Engine/Audio/SoundDesign.h"

#include <SDL3/SDL_events.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <limits>
#include <string>


// What is drawn over the game: the HUD, the inventory, the player panels and the debug overlays.

namespace pred
{

void PredationGame::DrawDebugOverlays()
{
    DebugDraw& draw = m_app->GetDebugDraw();
    DrawCinematicPaths(draw);

    if (m_editor.IsOpen())
    {
        m_editor.DrawOverlays(draw);
        return;
    }

    // What the creature sees, hears and means to do, and the surface it walks on.
    DrawCreatureOverlays(draw);

    // A round is drawn as something that travels, with a short lit section and a mark where it
    // arrives. Lighting the whole line at once, which is what this used to do, draws a diagram of
    // a shot rather than a shot, and it stayed on screen long enough to read as a laser.
    for (const Tracer& tracer : m_tracers)
    {
        const glm::vec3 travel = tracer.to - tracer.from;
        const float distance = glm::length(travel);
        if (distance < 1e-3f)
        {
            continue;
        }
        const glm::vec3 direction = travel / distance;
        const float head = tracer.age * kTracerSpeed;

        if (head < distance)
        {
            const float tail = std::max(head - kTracerLength, 0.0f);
            // Dimmer the further it has gone, so a long shot thins out down range.
            const float bright = 1.0f - 0.40f * (head / distance);
            const auto level = [bright](float channel)
            { return static_cast<uint8_t>(std::clamp(channel * bright, 0.0f, 255.0f)); };
            draw.Line(tracer.from + direction * tail, tracer.from + direction * head,
                      Color::RGBA(level(255.0f), level(226.0f), level(150.0f)));
        }
        else if (tracer.hit && tracer.surface && !tracer.marked)
        {
            // Arrived. The round leaves a hole where it landed and that is the whole of the effect:
            // a burst that flashes and vanishes says only that something happened, and a hole says
            // what happened and where, and is still saying it a minute later. Reading a room by the
            // marks in it is worth more to this game than a spark is.
            //
            // `marked` rather than a test on the age, so it happens on the one frame the round
            // arrives rather than on every frame the tracer survives afterwards.
            const_cast<Tracer&>(tracer).marked = true;
            //
            // Where the round struck is taken from the body it struck, as that body stands now, when
            // there is one that moves: the shove from this same round has had the whole flight of
            // the drawn tracer to carry it away from the point in the world where it was hit.
            glm::vec3 at = tracer.to;
            glm::vec3 facing = tracer.normal;
            const PhysicsWorld& physics = m_app->GetPhysics();
            if (tracer.anchored && physics.IsValid(tracer.body))
            {
                const Transform body = physics.GetTransform(tracer.body);
                at = body.position + body.rotation * tracer.localTo;
                facing = body.rotation * tracer.localNormal;
            }
            PlaceBulletHole(at, facing, tracer.body);
        }
    }

    // The line a round was actually traced along, which is not the line it is drawn along: rounds
    // are traced from the eye so the crosshair tells the truth, and drawn from the muzzle so they
    // look right. Seeing both at once is the only way to check that difference has not become a
    // lie, and it is a developer's question, so it lives behind a toggle rather than on screen.
    if (DebugCategories::IsEnabled(DebugCategory::Combat))
    {
        for (const Tracer& tracer : m_tracers)
        {
            draw.Line(tracer.origin, tracer.to, Color::kMagenta);
            draw.Line(tracer.from, tracer.to, tracer.hit ? Color::kYellow : Color::kGrey);
            if (tracer.hit)
            {
                draw.Sphere(tracer.to, 0.045f, Color::kRed, 8);
            }
        }
    }

    // The sockets on the weapon as it is actually held, with a line to the hand that is meant to be
    // at each. Placing a grip is a matter of looking at where the hand lands, and there is nothing
    // else that shows both at once.
    if (m_screen == Screen::Editor && m_benchSockets && m_editorBodyBuilt && m_editorBody.HasWeapon())
    {
        const glm::quat hold = m_editorBody.WeaponRotation();
        const glm::vec3 origin = m_editorBody.WeaponOrigin();
        const auto mark = [&](const glm::vec3& socket, BoneIndex bone, uint32_t colour)
        {
            const glm::vec3 world = origin + hold * socket;
            draw.Sphere(world, 0.018f, colour, 8);
            draw.Line(world, m_editorBody.GetPose().GlobalPosition(bone), colour);
        };
        mark(m_editorBody.Weapon().triggerGrip, m_editorBody.Rig().hand[1], Color::kGreen);
        mark(m_editorBody.Weapon().supportGrip, m_editorBody.Rig().hand[0], Color::kCyan);
        draw.Sphere(origin + hold * m_editorBody.Weapon().muzzle, 0.014f, Color::kYellow, 8);
        draw.Axes(glm::translate(glm::mat4(1.0f), origin) * glm::mat4_cast(hold), 0.12f);
    }

    if (cv_showGrid.Get())
    {
        draw.Grid(24.0f, 1.0f, 0.02f);
        draw.Axes(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.02f, 0.0f)), 1.5f);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Physics))
    {
        m_app->GetPhysics().DebugDraw(draw);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Player) && m_cameraMode != CameraMode::FirstPerson)
    {
        // Only worth drawing the capsule when it is not wrapped around the camera.
        m_player.DebugDraw(draw);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Animation))
    {
        m_body.DebugDraw(draw);
    }

    if (DebugCategories::IsEnabled(DebugCategory::Rendering))
    {
        const MeshLibrary& meshes = m_app->GetMeshes();
        m_scene.ForEachMeshRenderer(
            [&](Entity, const Transform& transform, const MeshRenderer& renderer)
            {
                const Mesh* mesh = meshes.Get(renderer.mesh);
                if (mesh == nullptr || !mesh->bounds.IsValid())
                {
                    return;
                }
                // Drawn in the entity's own frame. Building an axis-aligned box from the position
                // and scale alone ignored the rotation, so anything turned showed a bounds box
                // sitting at an angle to the collider it was supposed to describe.
                const glm::mat4 model =
                    transform.Matrix() * glm::translate(glm::mat4(1.0f), mesh->bounds.Center());
                draw.BoxOriented(model, mesh->bounds.HalfExtents(), Color::kCyan);
            });
    }
}

void PredationGame::DrawPlayerPanel()
{
    PlayerConfig& config = m_player.Config();
    const PlayerState& state = m_player.State();
    const PlayerController::Debug& diagnostics = m_player.Diagnostics();

    ImGui::Text("Stance %-9s %s", PlayerStanceName(state.stance),
                state.stanceBlocked ? "(blocked overhead)" : "");
    ImGui::Text("Speed %5.2f / %5.2f m/s   %s", diagnostics.horizontalSpeed, diagnostics.targetSpeed,
                state.grounded ? "grounded" : (state.onSteepSlope ? "on steep slope" : "airborne"));
    ImGui::Text("Position %.2f %.2f %.2f", state.position.x, state.position.y, state.position.z);
    ImGui::Text("Vertical %6.2f m/s   slope %4.1f deg   step %+.3f m", state.velocity.y,
                diagnostics.slopeAngleDegrees, diagnostics.lastStepUp);
    ImGui::Text("Health %.0f %s", state.health, state.alive ? "" : "(dead)");
    ImGui::TextUnformatted(m_mouseCaptured ? "Mouse captured. Escape frees the cursor."
                                           : "Cursor free. Escape or click the world to look again.");
    if (state.landingImpactSpeed > 0.0f)
    {
        ImGui::Text("Last landing impact %.1f m/s", state.landingImpactSpeed);
    }

    ImGui::Separator();
    if (ImGui::Button("Respawn"))
    {
        RespawnLocalPlayer(m_spawnPoint);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save to player.json"))
    {
        config.SaveToFile(PlayerConfigPath());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
    {
        ReloadPlayerConfig();
    }

    bool capsuleChanged = false;

    if (ImGui::CollapsingHeader("Speeds", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Move", &config.moveSpeed, 0.5f, 10.0f, "%.2f m/s");
        ImGui::SliderFloat("Sprint", &config.sprintSpeed, 0.5f, 14.0f, "%.2f m/s");
        ImGui::SliderFloat("Slow walk", &config.walkSpeed, 0.2f, 6.0f, "%.2f m/s");
        ImGui::SliderFloat("Crouch", &config.crouchSpeed, 0.2f, 6.0f, "%.2f m/s");
        ImGui::SliderFloat("Prone", &config.proneSpeed, 0.1f, 3.0f, "%.2f m/s");
        ImGui::SliderFloat("Backward scale", &config.backwardScale, 0.3f, 1.0f, "%.2f");
        ImGui::SliderFloat("Strafe scale", &config.strafeScale, 0.3f, 1.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Acceleration", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Ground accel", &config.groundAcceleration, 5.0f, 200.0f, "%.0f m/s2");
        ImGui::SliderFloat("Ground decel", &config.groundDeceleration, 5.0f, 200.0f, "%.0f m/s2");
        ImGui::SliderFloat("Air accel", &config.airAcceleration, 0.0f, 60.0f, "%.1f m/s2");
    }

    if (ImGui::CollapsingHeader("Jump and gravity", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Jump height", &config.jumpHeight, 0.1f, 2.5f, "%.2f m");
        ImGui::SliderFloat("Gravity scale", &config.gravityScale, 0.5f, 4.0f, "%.2f");
        ImGui::SliderFloat("Fall gravity scale", &config.fallGravityScale, 0.5f, 5.0f, "%.2f");
        ImGui::SliderFloat("Coyote time", &config.coyoteTime, 0.0f, 0.4f, "%.3f s");
        ImGui::SliderFloat("Jump buffer", &config.jumpBufferTime, 0.0f, 0.4f, "%.3f s");
    }

    if (ImGui::CollapsingHeader("Capsule"))
    {
        capsuleChanged |= ImGui::SliderFloat("Stand height", &config.standHeight, 1.0f, 2.4f, "%.2f m");
        capsuleChanged |= ImGui::SliderFloat("Crouch height", &config.crouchHeight, 0.6f, 1.6f, "%.2f m");
        capsuleChanged |= ImGui::SliderFloat("Prone height", &config.proneHeight, 0.3f, 1.0f, "%.2f m");
        capsuleChanged |= ImGui::SliderFloat("Radius", &config.radius, 0.15f, 0.6f, "%.2f m");
        ImGui::SliderFloat("Step up", &config.stepHeight, 0.05f, 0.8f, "%.2f m");
        ImGui::SliderFloat("Max slope", &config.maxSlopeAngle, 20.0f, 70.0f, "%.0f deg");
        ImGui::TextUnformatted("Step up and slope apply on respawn.");
    }

    if (ImGui::CollapsingHeader("Camera feel", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Stand eye", &config.standEyeHeight, 1.0f, 2.0f, "%.2f m");
        ImGui::SliderFloat("Crouch eye", &config.crouchEyeHeight, 0.5f, 1.5f, "%.2f m");
        ImGui::SliderFloat("Prone eye", &config.proneEyeHeight, 0.1f, 1.0f, "%.2f m");
        ImGui::SliderFloat("Eye transition", &config.eyeTransitionSpeed, 1.0f, 30.0f, "%.1f");
        ImGui::SliderFloat("Step smoothing", &config.stepSmoothSpeed, 1.0f, 40.0f, "%.1f");
        ImGui::SliderFloat("Landing dip", &config.landingDipPerSpeed, 0.0f, 0.05f, "%.4f m per m/s");
        ImGui::SliderFloat("Landing dip max", &config.landingDipMax, 0.0f, 0.6f, "%.2f m");
        ImGui::SliderFloat("Landing recovery", &config.landingRecoverSpeed, 1.0f, 25.0f, "%.1f");
        ImGui::SliderFloat("Footfall dip", &config.bobAmount, 0.0f, 0.16f, "%.3f m");
        ImGui::SliderFloat("Walk lower", &config.bobWalkLower, 0.0f, 0.16f, "%.3f m");
        ImGui::SliderFloat("Dip at speed", &config.bobSprintScale, 1.0f, 3.0f, "%.2f x");
        ImGui::SliderFloat("Max pitch", &config.maxPitchDegrees, 45.0f, 89.9f, "%.1f deg");
    }

    if (ImGui::CollapsingHeader("Fall damage"))
    {
        ImGui::Checkbox("Enabled", &config.fallDamageEnabled);
        ImGui::SliderFloat("Free below", &config.fallDamageMinSpeed, 1.0f, 25.0f, "%.1f m/s");
        ImGui::SliderFloat("Lethal at", &config.fallDamageLethalSpeed, 5.0f, 60.0f, "%.1f m/s");
    }

    if (ImGui::CollapsingHeader("Body and gait", ImGuiTreeNodeFlags_DefaultOpen))
    {
        PlayerBody::Config& body = m_body.Tuning();
        bool visible = body.visible;
        if (ImGui::Checkbox("Show body", &visible))
        {
            m_body.SetVisible(m_scene, visible);
        }
        ImGui::SliderFloat("Stride base", &config.strideLengthBase, 0.4f, 2.0f, "%.2f m");
        ImGui::SliderFloat("Stride per m/s", &config.strideLengthPerSpeed, 0.0f, 0.6f, "%.3f m");
        ImGui::SliderFloat("Stance walk", &config.stanceFractionWalk, 0.4f, 0.85f, "%.2f");
        ImGui::SliderFloat("Stance run", &config.stanceFractionRun, 0.3f, 0.7f, "%.2f");
        ImGui::SliderFloat("Foot lift", &body.stepHeight, 0.0f, 0.4f, "%.3f m");
        ImGui::SliderFloat("Step reach", &body.stepReachMargin, 0.7f, 0.99f, "%.2f");
        ImGui::SliderFloat("Hip sway", &body.hipSwayAmount, 0.0f, 0.12f, "%.3f m");
        ImGui::SliderFloat("Hip bob", &body.hipBobAmount, 0.0f, 0.12f, "%.3f m");
        ImGui::SliderFloat("Lean per m/s", &body.leanPerSpeed, 0.0f, 6.0f, "%.2f deg");
        ImGui::SliderFloat("Max lean", &body.maxLean, 0.0f, 40.0f, "%.0f deg");
        ImGui::SliderFloat("Arm swing", &body.armSwingDegrees, 0.0f, 60.0f, "%.0f deg");
        ImGui::SliderFloat("Foot smoothing", &body.footPlantSmoothing, 2.0f, 60.0f, "%.0f");
        ImGui::TextUnformatted("Skeleton overlay: enable the ANIMATION debug category.");
    }

    if (capsuleChanged)
    {
        m_player.ApplyConfigToCharacter();
    }
}

void PredationGame::DrawCondition()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    // What shape the player is in, bottom left. Two bars and no numbers: how hurt you are and how
    // much running is left are things to glance at, and a number invites arithmetic in a game that
    // is meant to be making you panic.
    const float left = viewport->Pos.x + 26.0f;
    const float bottom = viewport->Pos.y + viewport->Size.y - 26.0f;
    constexpr float kWidth = 168.0f;
    constexpr float kHeight = 7.0f;
    constexpr float kGap = 9.0f;

    const auto bar = [&](float y, float fill, ImU32 colour)
    {
        const ImVec2 from{left, y};
        const ImVec2 to{left + kWidth, y + kHeight};
        draw->AddRectFilled(from, to, IM_COL32(14, 16, 20, 150), 2.0f);
        if (fill > 0.0f)
        {
            draw->AddRectFilled(from, {left + kWidth * std::clamp(fill, 0.0f, 1.0f), to.y}, colour, 2.0f);
        }
        draw->AddRect(from, to, IM_COL32(120, 126, 138, 130), 2.0f);
    };

    const PlayerState& state = m_player.State();
    const float health = std::clamp(state.health / 100.0f, 0.0f, 1.0f);
    // Reddens as it empties, so the colour says the same thing as the length.
    const ImU32 healthColour = health > 0.6f   ? IM_COL32(150, 190, 160, 220)
                               : health > 0.3f ? IM_COL32(210, 180, 110, 225)
                                               : IM_COL32(205, 90, 80, 235);
    bar(bottom - kHeight, health, healthColour);

    // Stamina is only worth the space when it is not full, or when it has run out, which is exactly
    // when the player needs to know about it.
    if (state.stamina < 0.999f)
    {
        const ImU32 staminaColour =
            state.winded ? IM_COL32(200, 120, 90, 220) : IM_COL32(140, 165, 195, 200);
        bar(bottom - kHeight * 2.0f - kGap, state.stamina, staminaColour);
    }
    // The torch's cell, while it is on and whenever it is getting low.
    if (m_torchOn || m_torchCharge < 0.5f)
    {
        const ImU32 torchColour = m_torchCharge > 0.2f ? IM_COL32(215, 200, 130, 200) : IM_COL32(215, 120, 80, 225);
        bar(bottom - kHeight * 3.0f - kGap * 2.0f, m_torchCharge, torchColour);
    }
}

void PredationGame::DrawHud()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 centre{viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f};
    constexpr ImGuiWindowFlags kHudFlags = ImGuiWindowFlags_NoDecoration |
                                           ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground;

    // Reticle. The gap opens with the weapon's current cone, so the crosshair says where rounds can
    // actually go rather than always promising the centre of the screen.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    // The microphone, top left, whenever voice could be sending.
    //
    // Everything about a microphone is otherwise invisible until somebody else says whether they
    // could hear you, which is a slow way to find out that Windows never granted the game the
    // device. This says three things at a glance: whether the microphone is open at all, how loud it
    // is hearing you, and whether what it hears is actually going out.
    //
    // Mostly a testing tool, which is why it is plain and in a corner rather than designed.
    //
    // Only with somebody to talk to. Alone it said "nobody to hear you" in the corner of every single
    // player game, which is true and of no use to anybody.
    if (cv_micMeter.Get() && m_screen == Screen::Playing && m_sessionMode != SessionMode::Offline)
    {
        const bool open = m_microphone.Running();
        const float level = std::clamp(m_voiceLevel, 0.0f, 1.0f);

        const ImVec2 origin{viewport->Pos.x + 18.0f, viewport->Pos.y + 18.0f};
        constexpr float kWidth = 150.0f;
        constexpr float kHeight = 9.0f;
        draw->AddRectFilled(origin, {origin.x + kWidth, origin.y + kHeight},
                            IM_COL32(18, 20, 26, 190), 2.0f);

        // Green while it is going out, amber while it is heard but held back by the gate, grey when
        // the microphone is shut. The colour is the state; the length is the loudness.
        const ImU32 colour = !open              ? IM_COL32(90, 94, 104, 200)
                             : m_voiceSending   ? IM_COL32(120, 210, 130, 230)
                                                : IM_COL32(220, 180, 90, 230);
        if (level > 0.001f)
        {
            draw->AddRectFilled(origin, {origin.x + kWidth * level, origin.y + kHeight}, colour, 2.0f);
        }

        // Where the gate opens, so a threshold can be set by watching your own voice cross a line
        // rather than by guessing at a number.
        if (cv_voiceOpenMic.Get())
        {
            const float mark = origin.x + kWidth * std::clamp(cv_voiceThreshold.Get(), 0.0f, 1.0f);
            draw->AddLine({mark, origin.y - 2.0f}, {mark, origin.y + kHeight + 2.0f},
                          IM_COL32(240, 200, 120, 220), 1.5f);
        }

        const std::string talkKey = "hold " + KeyFor(m_app->GetInput(), "voice") + " to talk";
        const std::string what = !cv_voiceEnabled.Get() ? std::string("voice off")
                                 : !open ? (cv_voiceOpenMic.Get() ? std::string("mic not working") : talkKey)
                                 : m_voiceSending ? std::string("talking")
                                                  : std::string("listening");
        draw->AddText({origin.x, origin.y + kHeight + 3.0f}, IM_COL32(200, 205, 215, 200),
                      what.c_str());
    }

    // The version, bottom left, quietly.
    //
    // It is on the title screen too, but by the time somebody is describing a problem they are
    // usually in the game and not looking at the menu. A screenshot of a bug that says which build
    // took it is worth a round of "which version were you running"; this whole session has had two
    // of those.
    {
        const char* version = BuildInfo::Label();
        const ImVec2 size = ImGui::CalcTextSize(version);
        draw->AddText({viewport->Pos.x + 10.0f, viewport->Pos.y + viewport->Size.y - size.y - 8.0f},
                      IM_COL32(180, 190, 200, 90), version);
    }
    // Held by a creature, or wrapped up at its nest: what is happening, and what to do about it.
    if (m_screen == Screen::Playing && m_player.State().alive)
    {
        bool held = false;
        bool cocooned = false;
        float struggle = -1.0f;
        if (IsAuthority())
        {
            if (const auto grip = m_grips.find(LocalPlayerId()); grip != m_grips.end())
            {
                held = true;
                struggle = grip->second.struggle;
            }
            cocooned = std::any_of(m_cocoons.begin(), m_cocoons.end(),
                                   [&](const Cocoon& cocoon) { return cocoon.player == LocalPlayerId(); });
        }
        else if (m_sessionMode == SessionMode::Client)
        {
            cocooned = m_client.CocoonedByHost();
            held = m_client.HeldByHost() && !cocooned;
            struggle = held ? m_client.StruggleFromHost() : -1.0f;
        }
        if (held || cocooned)
        {
            const float t = static_cast<float>(m_time);
            const ImVec2 middle{centre.x, viewport->Pos.y + viewport->Size.y * 0.74f};
            if (cocooned)
            {
                // Wrapped up: nothing to press. Only the others can help.
                const char* line = "WRAPPED UP AT THE NEST";
                const char* hint = "somebody has to cut you free";
                ImGui::SetWindowFontScale(1.0f);
                const ImVec2 size = ImGui::CalcTextSize(line);
                const ImVec2 hintSize = ImGui::CalcTextSize(hint);
                draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.6f, {middle.x - size.x * 0.8f, middle.y - 18.0f},
                              IM_COL32(225, 70, 60, static_cast<int>(200 + 55 * std::sin(t * 3.0f))), line);
                draw->AddText({middle.x - hintSize.x * 0.5f, middle.y + 18.0f}, IM_COL32(220, 200, 200, 200), hint);
            }
            else
            {
                // Struggling: the key to hammer in a ring that fills as you work free and drains as you
                // tire, jerked each time you press it.
                const float progress = std::clamp(struggle, 0.0f, 1.0f);
                const float jolt = m_struggleKick;
                const ImVec2 at{middle.x + m_struggleSide * jolt * 10.0f, middle.y - jolt * 6.0f};
                const float radius = 44.0f + 6.0f * jolt;
                const float beat = 0.5f + 0.5f * std::sin(t * 9.0f);
                draw->AddCircleFilled(at, radius + 10.0f, IM_COL32(10, 4, 4, 170), 48);
                draw->AddCircle(at, radius, IM_COL32(90, 30, 28, 220), 48, 7.0f);
                if (progress > 0.001f)
                {
                    draw->PathArcTo(at, radius, -glm::half_pi<float>(), -glm::half_pi<float>() + glm::two_pi<float>() * progress, 48);
                    const int green = static_cast<int>(120 + 120 * progress);
                    draw->PathStroke(IM_COL32(255, green, static_cast<int>(80 * progress), 255), 0, 7.0f);
                }
                const std::string key = KeyFor(m_app->GetInput(), "jump");
                const float keyScale = 1.8f + 0.25f * jolt + 0.1f * beat;
                const ImVec2 keySize = ImGui::CalcTextSize(key.c_str());
                draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * keyScale,
                              {at.x - keySize.x * keyScale * 0.5f, at.y - keySize.y * keyScale * 0.5f},
                              IM_COL32(255, 235, 225, 255), key.c_str());
                const char* line = "BREAK FREE";
                const ImVec2 lineSize = ImGui::CalcTextSize(line);
                draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.3f, {middle.x - lineSize.x * 0.65f, at.y - radius - 42.0f},
                              IM_COL32(235, 80, 70, static_cast<int>(180 + 75 * beat)), line);
                const char* hint = "hammer it -- or somebody can shoot it off you";
                const ImVec2 hintSize = ImGui::CalcTextSize(hint);
                draw->AddText({middle.x - hintSize.x * 0.5f, at.y + radius + 18.0f}, IM_COL32(210, 190, 190, 170), hint);
            }
        }
    }

    const ImU32 reticleColor = IM_COL32(230, 230, 235, 150);
    float gap = 2.0f;
    if (const WeaponDefinition* weapon = EquippedWeapon())
    {
        // Half the vertical field of view maps to half the screen height, so a cone in degrees
        // converts to pixels through the same projection the world is drawn with.
        const float halfFov = glm::radians(cv_fov.Get()) * 0.5f;
        const float aspect = viewport->Size.x / std::max(viewport->Size.y, 1.0f);
        const float verticalHalfFov = std::atan(std::tan(halfFov) / std::max(aspect, 0.01f));
        const float spread = glm::radians(WeaponSim::CurrentSpread(*weapon, m_weapon));
        gap = 2.0f + std::tan(spread) / std::max(std::tan(verticalHalfFov), 1e-3f) * viewport->Size.y * 0.5f;
        gap = std::min(gap, viewport->Size.y * 0.25f);
    }
    const float tick = 5.0f;
    // Dead, there is nothing in anybody's hands to aim.
    if (cv_crosshair.Get() && m_player.State().alive)
    {
        draw->AddLine({centre.x - gap - tick, centre.y}, {centre.x - gap, centre.y}, reticleColor, 1.5f);
        draw->AddLine({centre.x + gap, centre.y}, {centre.x + gap + tick, centre.y}, reticleColor, 1.5f);
        draw->AddLine({centre.x, centre.y - gap - tick}, {centre.x, centre.y - gap}, reticleColor, 1.5f);
        draw->AddLine({centre.x, centre.y + gap}, {centre.x, centre.y + gap + tick}, reticleColor, 1.5f);
    }
    if (cv_showFps.Get())
    {
        // Averaged, so it can be read: a number that changes every frame is a blur.
        const float ms = FrameStats::Instance().AverageFrameMs();
        char fps[32];
        std::snprintf(fps, sizeof(fps), "%.0f fps", ms > 0.0f ? 1000.0f / ms : 0.0f);
        const ImVec2 size = ImGui::CalcTextSize(fps);
        draw->AddText({viewport->Pos.x + viewport->Size.x - size.x - 12.0f, viewport->Pos.y + 10.0f},
                      IM_COL32(220, 220, 225, 170), fps);
    }
    // Interaction prompt, just below the reticle.
    const InteractionSystem::Focus& focus = m_interactions.CurrentFocus();
    // Nothing in the middle of the view from inside a locker: it is the slits you are looking at.
    // Nor over a panel that is open.
    const std::string prompt = m_hidingSpot >= 0 || m_loadoutOpen || m_mapOpen || m_inventoryOpen || m_paused || m_settingsOpen || m_cine.Active()
                                   ? std::string()
                                   : focus.prompt;
    if (!prompt.empty())
    {
        // On the thing itself: the door, the locker, the crate. A prompt under the crosshair says what
        // can be done and leaves the player to work out to what; one sitting on the handle says both.
        // Under the crosshair still when the thing has no place on screen -- leaving a locker from the
        // inside, or something at the very edge of the view.
        ImVec2 at{centre.x, centre.y + 42.0f};
        if (m_hidingSpot < 0)
        {
            const Interactable* target = m_interactions.Find(focus.entity);
            const Transform* where = m_scene.GetTransform(focus.entity);
            if (target != nullptr && where != nullptr)
            {
                const glm::vec4 clip = m_viewProjection * glm::vec4(where->position + target->focusOffset, 1.0f);
                if (clip.w > 0.05f)
                {
                    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
                    const ImVec2 screen{viewport->Pos.x + (ndc.x * 0.5f + 0.5f) * viewport->Size.x,
                                        viewport->Pos.y + (0.5f - ndc.y * 0.5f) * viewport->Size.y};
                    const float margin = 80.0f;
                    if (screen.x > viewport->Pos.x + margin && screen.x < viewport->Pos.x + viewport->Size.x - margin &&
                        screen.y > viewport->Pos.y + margin && screen.y < viewport->Pos.y + viewport->Size.y - margin)
                    {
                        at = screen;
                    }
                }
            }
        }
        // A key in a box, then what it does: the look of every prompt, so the eye learns it once.
        // A locked door with the keycard in hand is opened by swiping it: the fire button, not interact.
        const bool swipe = m_hidingSpot < 0 && focus.kind == InteractionKind::Door && m_heldItem != kInvalidItem &&
                           m_heldItem == m_items.IdOf("keycard") && m_world.GetDoor(focus.payload) != nullptr &&
                           m_world.GetDoor(focus.payload)->locked;
        const std::string key = KeyFor(m_app->GetInput(), swipe ? "fire" : "interact");
        // Larger than the rest of the HUD's text, and on a darker backing with an edge to it: it is read
        // at a glance, against whatever the thing is -- a grey locker in a grey room was the worst of it.
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * 1.3f;
        const ImVec2 keySize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, key.c_str());
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, prompt.c_str());
        const float pad = 7.0f;
        const float keyBox = std::max(keySize.x, keySize.y) + pad * 2.0f;
        const float width = keyBox + pad + textSize.x + pad * 2.0f;
        const float height = keyBox;
        const ImVec2 corner{at.x - width * 0.5f, at.y - height * 0.5f};
        const OverlayLayer layer("##prompt");
        ImDrawList* list = layer.Draw();
        list->AddRectFilled(corner, {corner.x + width, corner.y + height}, IM_COL32(6, 7, 10, 225), 4.0f);
        list->AddRect(corner, {corner.x + width, corner.y + height}, IM_COL32(225, 205, 150, 150), 4.0f, 0, 1.5f);
        list->AddRectFilled({corner.x + 2.0f, corner.y + 2.0f}, {corner.x + keyBox - 2.0f, corner.y + height - 2.0f},
                            IM_COL32(225, 205, 150, 245), 3.0f);
        list->AddText(font, fontSize, {corner.x + (keyBox - keySize.x) * 0.5f, corner.y + (height - keySize.y) * 0.5f},
                      IM_COL32(20, 20, 24, 255), key.c_str());
        list->AddText(font, fontSize, {corner.x + keyBox + pad, corner.y + (height - textSize.y) * 0.5f},
                      IM_COL32(245, 245, 248, 255), prompt.c_str());
    }

    // Hotbar: one icon per slot, with the selected one picked out and stack counts in the corner.
    constexpr float kSlotSize = 46.0f;
    constexpr float kSlotGap = 6.0f;

    ImGui::SetNextWindowPos({centre.x, viewport->Pos.y + viewport->Size.y - 16.0f}, ImGuiCond_Always,
                            {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.0f);
    // Dead, what they carried is on the floor where they fell, and so is the bar that showed it.
    const bool showHands = m_player.State().alive;
    if (showHands && ImGui::Begin("##Hotbar", nullptr, kHudFlags))
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        for (int i = 0; i < m_inventory.SlotCount(); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0f, kSlotGap);
            }

            const Inventory::Slot& slot = m_inventory.At(i);
            const bool selected = i == m_inventory.SelectedSlot();
            const ImVec2 origin = ImGui::GetCursorScreenPos();

            list->AddRectFilled(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                                IM_COL32(18, 20, 26, selected ? 190 : 130), 4.0f);
            list->AddRect(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                          selected ? IM_COL32(235, 205, 130, 235) : IM_COL32(120, 124, 134, 150), 4.0f,
                          0, selected ? 2.0f : 1.0f);

            if (const ItemDefinition* definition = m_items.Get(slot.item); definition != nullptr)
            {
                DrawItemIcon(slot.item, kSlotSize);
                if (slot.count > 1)
                {
                    const std::string count = std::to_string(slot.count);
                    list->AddText({origin.x + kSlotSize - 6.0f - ImGui::CalcTextSize(count.c_str()).x,
                                   origin.y + kSlotSize - 17.0f},
                                  IM_COL32(240, 240, 245, 235), count.c_str());
                }
            }

            const std::string number = std::to_string(i + 1);
            list->AddText({origin.x + 4.0f, origin.y + 2.0f},
                          selected ? IM_COL32(235, 205, 130, 220) : IM_COL32(150, 154, 164, 170),
                          number.c_str());

            ImGui::Dummy({kSlotSize, kSlotSize});
        }
    }
    if (showHands)
    {
        ImGui::End();
    }

    // What has just been picked out of the bar, named over it for a moment and fading: the bar is pictures, and a
    // picture of a box is not always enough to say which box.
    {
        const Inventory::Slot& chosen = m_inventory.Selected();
        const ItemId chosenItem = chosen.IsEmpty() ? kInvalidItem : chosen.item;
        const int chosenSlot = m_inventory.SelectedSlot();
        if (chosenSlot != m_namedSlot || chosenItem != m_namedItem)
        {
            m_namedSlot = chosenSlot;
            m_namedItem = chosenItem;
            m_namedFor = chosenItem != kInvalidItem ? 2.2f : 0.0f;
        }
        m_namedFor = std::max(m_namedFor - m_lastFrameSeconds, 0.0f);
        const ItemDefinition* named = m_items.Get(m_namedItem);
        if (showHands && named != nullptr && m_namedFor > 0.0f)
        {
            const float fade = std::clamp(m_namedFor / 0.6f, 0.0f, 1.0f);
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize() * 1.15f;
            const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, named->name.c_str());
            const ImVec2 at{centre.x - extent.x * 0.5f, viewport->Pos.y + viewport->Size.y - 16.0f - kSlotSize - 12.0f - extent.y};
            const OverlayLayer layer("##named");
            ImDrawList* nameList = layer.Draw();
            nameList->AddText(font, size, {at.x + 1.5f, at.y + 1.5f}, IM_COL32(0, 0, 0, static_cast<int>(200 * fade)), named->name.c_str());
            nameList->AddText(font, size, at, IM_COL32(236, 238, 242, static_cast<int>(255 * fade)), named->name.c_str());
        }
    }

    // How the player is doing. Nothing to do with what is in their hands, which is where this was
    // and why it only appeared when a weapon was out.
    if (m_player.State().alive)
    {
        DrawCondition();
    }

    // Ammunition, bottom right, away from the hotbar. Reads magazine over reserve, the way a
    // shooter always has, and says so plainly while the magazine is out.
    if (const WeaponDefinition* weapon = EquippedWeapon())
    {
        ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x - 24.0f,
                                 viewport->Pos.y + viewport->Size.y - 20.0f},
                                ImGuiCond_Always, {1.0f, 1.0f});
        ImGui::SetNextWindowBgAlpha(0.0f);
        if (ImGui::Begin("##Ammo", nullptr, kHudFlags))
        {
            if (m_weapon.IsReloading())
            {
                ImGui::TextColored({0.85f, 0.80f, 0.55f, 1.0f}, "Reloading");
            }
            else
            {
                const ImVec4 colour = m_weapon.rounds == 0 ? ImVec4(0.88f, 0.42f, 0.38f, 1.0f)
                                                           : ImVec4(0.90f, 0.91f, 0.94f, 1.0f);
                ImGui::TextColored(colour, "%d / %d", m_weapon.rounds, m_weapon.reserve);
            }
            ImGui::TextDisabled("%s  %s", weapon->name.c_str(), FireModeName(weapon->mode));
        }
        ImGui::End();
    }

    DrawDroneHud();
    DrawMissionHud();
    DrawShipHud();
    DrawCampaignNotice();
    DrawTitleCard();
    DrawSubtitle();
    DrawPlayerList();

    // Whose eyes these are, and how to move to somebody else's. Without it a dead player is looking
    // through a stranger with no way to tell whose view it is or that it can be changed.
    // And when you are back. Nothing to press: it happens on its own, and this says so.
    if (!m_player.State().alive && (m_respawnTimer > 0.0f || m_deadForGood) && !m_supportDrone.Active())
    {
        ImGui::SetNextWindowPos({centre.x, viewport->Pos.y + viewport->Size.y * 0.32f}, ImGuiCond_Always,
                                {0.5f, 0.5f});
        if (ImGui::Begin("##Respawning", nullptr, kHudFlags))
        {
            ImGui::SetWindowFontScale(1.4f);
            ImGui::TextColored({0.88f, 0.42f, 0.36f, 1.0f}, "You died");
            ImGui::SetWindowFontScale(1.0f);
            if (!m_deadForGood)
            {
                ImGui::TextDisabled("Back in %d", static_cast<int>(std::ceil(m_respawnTimer)));
            }
            else if (m_everybodyDownFor > 0.0f)
            {
                ImGui::TextDisabled("Everybody is down. Back in %d", static_cast<int>(std::ceil(m_everybodyDownFor)));
            }
        }
        ImGui::End();
    }

    if (!m_player.State().alive && m_spectating >= 0)
    {
        std::string name = "a teammate";
        for (const RemotePlayerView& other : RemotePlayers())
        {
            if (static_cast<int>(other.id) == m_spectating && !other.name.empty())
            {
                name = other.name;
                break;
            }
        }
        ImGui::SetNextWindowPos({centre.x, viewport->Pos.y + viewport->Size.y - 110.0f},
                                ImGuiCond_Always, {0.5f, 1.0f});
        if (ImGui::Begin("##Spectating", nullptr, kHudFlags))
        {
            ImGui::TextColored({0.82f, 0.84f, 0.88f, 1.0f}, "Watching %s", name.c_str());
            ImGui::TextDisabled("[%s]  next", KeyFor(m_app->GetInput(), "interact").c_str());
        }
        ImGui::End();
    }

    if (m_inventoryOpen)
    {
        DrawInventoryPanel();
    }
    if (m_loadoutOpen)
    {
        DrawLoadoutPanel();
    }
}

void PredationGame::DrawPlayerList()
{
    // Only in a game with other people in it. On your own it is a list of one, which is a panel
    // asking the player to look at their own name.
    if (m_sessionMode == SessionMode::Offline)
    {
        return;
    }
    const std::vector<RemotePlayerView>& others = RemotePlayers();
    if (others.empty())
    {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Under the frame counter when there is one, rather than on top of it.
    const float below = cv_showFps.Get() ? 34.0f : 16.0f;
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x - 16.0f, viewport->Pos.y + below},
                            ImGuiCond_Always, {1.0f, 0.0f});
    ImGui::SetNextWindowBgAlpha(0.35f);
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (!ImGui::Begin("##Players", nullptr, kFlags))
    {
        ImGui::End();
        return;
    }

    // One row per player, this machine first. Health as a short bar rather than a number: from
    // across the screen a bar says "hurt" at a glance and a number has to be read.
    const auto row = [](const char* name, float health, bool alive, int pingMs, bool self,
                        bool spectated)
    {
        ImDrawList* list = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        constexpr float kBarWidth = 54.0f;
        constexpr float kBarHeight = 4.0f;

        const ImVec4 nameColour = !alive     ? ImVec4(0.62f, 0.40f, 0.38f, 1.0f)
                                 : spectated ? ImVec4(0.95f, 0.85f, 0.55f, 1.0f)
                                 : self      ? ImVec4(0.88f, 0.92f, 0.96f, 1.0f)
                                             : ImVec4(0.74f, 0.78f, 0.84f, 1.0f);
        ImGui::TextColored(nameColour, "%s%s", name, self ? " (you)" : "");
        ImGui::SameLine(132.0f);

        if (alive)
        {
            const ImVec2 barAt{origin.x + 132.0f, origin.y + ImGui::GetTextLineHeight() * 0.5f - 1.0f};
            const float fraction = glm::clamp(health / 100.0f, 0.0f, 1.0f);
            list->AddRectFilled(barAt, {barAt.x + kBarWidth, barAt.y + kBarHeight},
                                IM_COL32(60, 64, 72, 180));
            const ImU32 fill = fraction > 0.6f   ? IM_COL32(120, 190, 130, 220)
                               : fraction > 0.3f ? IM_COL32(210, 185, 110, 220)
                                                 : IM_COL32(205, 100, 90, 230);
            list->AddRectFilled(barAt, {barAt.x + kBarWidth * fraction, barAt.y + kBarHeight}, fill);
            ImGui::Dummy({kBarWidth, ImGui::GetTextLineHeight()});
        }
        else
        {
            ImGui::TextColored({0.62f, 0.40f, 0.38f, 1.0f}, "down");
        }

        ImGui::SameLine(200.0f);
        if (self && pingMs < 0)
        {
            // The host is not waiting on anybody, so it has no round trip worth printing.
            ImGui::TextDisabled("host");
        }
        else
        {
            const ImVec4 colour = pingMs < 80    ? ImVec4(0.55f, 0.75f, 0.58f, 1.0f)
                                  : pingMs < 180 ? ImVec4(0.82f, 0.76f, 0.50f, 1.0f)
                                                 : ImVec4(0.85f, 0.50f, 0.45f, 1.0f);
            ImGui::TextColored(colour, "%d ms", pingMs);
        }
    };

    const bool hosting = m_sessionMode == SessionMode::Host;
    row(PlayerName().c_str(), m_player.State().health, m_player.State().alive,
        hosting ? -1 : static_cast<int>(m_client.PingMs()), true, false);
    for (const RemotePlayerView& other : others)
    {
        const bool watched = m_spectating >= 0 && static_cast<int>(other.id) == m_spectating;
        row(other.name.c_str(), other.health, other.alive, static_cast<int>(other.pingMs), false,
            watched);
    }

    ImGui::End();
}

void PredationGame::DrawItemIcon(ItemId item, float boxSize) const
{
    const ItemIcons::Icon* icon = m_itemIcons.Find(item);
    if (icon == nullptr || !m_itemIcons.IsReady())
    {
        return;
    }

    // Inset a little, so the render's own framing does not touch the slot border.
    const float inset = boxSize * 0.06f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // A pale backing, lighter at the top like a lit shelf, so a dark object has something to stand
    // against. The weapons are near-black metal and polymer, which is right for them and made them
    // black shapes on a black slot: the icon was there, in its real colours, and could not be seen.
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        {origin.x + 2.0f, origin.y + 2.0f}, {origin.x + boxSize - 2.0f, origin.y + boxSize - 2.0f},
        IM_COL32(128, 134, 144, 150), IM_COL32(128, 134, 144, 150), IM_COL32(74, 78, 88, 150),
        IM_COL32(74, 78, 88, 150));
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(ImGuiLayer::TextureId(m_itemIcons.Texture())),
                                        {origin.x + inset, origin.y + inset},
                                        {origin.x + boxSize - inset, origin.y + boxSize - inset},
                                        {icon->uv0.x, icon->uv0.y}, {icon->uv1.x, icon->uv1.y});
}

void PredationGame::MoveInventorySlot(int from, int to)
{
    // What is in the hands is written back first, so its magazine goes with it to the new slot.
    if (m_ammoSlot != Inventory::kNoSlot && m_weapon.HasWeapon())
    {
        m_inventory.SetSlotAmmo(m_ammoSlot, m_weapon.rounds, m_weapon.reserve);
    }
    if (!m_inventory.Move(m_items, from, to))
    {
        return;
    }
    if (m_ammoSlot == from)
    {
        m_ammoSlot = to;
    }
    else if (m_ammoSlot == to)
    {
        m_ammoSlot = from;
    }
}

void PredationGame::DrawInventoryPanel()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->Pos.x + viewport->Size.x * 0.5f,
                             viewport->Pos.y + viewport->Size.y * 0.5f},
                            ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("Inventory", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        constexpr float kSlotSize = 76.0f;
        constexpr int kColumns = 3;

        for (int i = 0; i < m_inventory.SlotCount(); ++i)
        {
            if (i % kColumns != 0)
            {
                ImGui::SameLine(0.0f, 10.0f);
            }
            ImGui::BeginGroup();

            const Inventory::Slot& slot = m_inventory.At(i);
            const bool selected = i == m_inventory.SelectedSlot();
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* list = ImGui::GetWindowDrawList();

            list->AddRectFilled(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                                IM_COL32(26, 28, 36, 220), 4.0f);
            list->AddRect(origin, {origin.x + kSlotSize, origin.y + kSlotSize},
                          selected ? IM_COL32(235, 205, 130, 235) : IM_COL32(110, 114, 124, 160), 4.0f,
                          0, selected ? 2.0f : 1.0f);

            const ItemDefinition* definition = m_items.Get(slot.item);
            if (definition != nullptr)
            {
                DrawItemIcon(slot.item, kSlotSize);
                // The count rides on the icon, as it does on the hotbar, so the caption below only
                // ever has to hold a name.
                if (slot.count > 1)
                {
                    const std::string count = std::to_string(slot.count);
                    list->AddText({origin.x + kSlotSize - 6.0f - ImGui::CalcTextSize(count.c_str()).x,
                                   origin.y + kSlotSize - 17.0f},
                                  IM_COL32(240, 240, 245, 235), count.c_str());
                }
            }

            // An invisible button over the slot makes it clickable for selecting.
            ImGui::InvisibleButton(("##slot" + std::to_string(i)).c_str(), {kSlotSize, kSlotSize});
            // Dragged somewhere else: the slot goes where it is dropped. Selecting waits for the
            // button to come up without having moved, so starting a drag does not draw the thing.
            if (definition != nullptr && ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload("PRED_SLOT", &i, sizeof(int));
                ImGui::TextUnformatted(definition->name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("PRED_SLOT"))
                {
                    MoveInventorySlot(*static_cast<const int*>(payload->Data), i);
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsItemDeactivated() && ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 36.0f)
            {
                // Clicking what is already out puts it away, the same as pressing its number, and
                // like the number it does nothing while a climb has the hands.
                if (HandsAreFree())
                {
                    m_inventory.SelectSlot(m_inventory.SelectedSlot() == i ? Inventory::kNoSlot : i);
                }
            }
            if (definition != nullptr && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", definition->name.c_str());
            }

            // Up to two lines, broken between words, with anything that still will not fit cut short
            // and marked as cut. Clipping one line at the slot edge, which is what this did, reads
            // as a rendering fault rather than as an abbreviation: "Access Keycard" came out as
            // "Access Keyc" with the c half drawn.
            const std::string label = definition != nullptr ? definition->name : std::string("Empty");
            const float lineHeight = ImGui::GetTextLineHeight();
            const ImU32 colour =
                definition != nullptr ? IM_COL32(220, 222, 230, 255) : IM_COL32(115, 118, 128, 255);
            const ImVec2 caption = ImGui::GetCursorScreenPos();

            std::vector<std::string> lines;
            {
                std::string current;
                size_t at = 0;
                while (at <= label.size() && lines.size() < 2)
                {
                    const size_t space = label.find(' ', at);
                    const std::string word = label.substr(at, space - at);
                    const std::string candidate = current.empty() ? word : current + " " + word;
                    if (!current.empty() && ImGui::CalcTextSize(candidate.c_str()).x > kSlotSize)
                    {
                        lines.push_back(current);
                        current = word;
                    }
                    else
                    {
                        current = candidate;
                    }
                    if (space == std::string::npos)
                    {
                        break;
                    }
                    at = space + 1;
                }
                if (lines.size() < 2 && !current.empty())
                {
                    lines.push_back(current);
                }
            }
            for (std::string& line : lines)
            {
                // A single word longer than the slot has nowhere to break, so it is shortened.
                while (line.size() > 1 && ImGui::CalcTextSize((line + ".").c_str()).x > kSlotSize)
                {
                    line.pop_back();
                }
                if (ImGui::CalcTextSize(line.c_str()).x > kSlotSize)
                {
                    line += ".";
                }
            }
            for (size_t line = 0; line < lines.size(); ++line)
            {
                list->AddText({caption.x, caption.y + lineHeight * static_cast<float>(line)}, colour,
                              lines[line].c_str());
            }
            // Always two lines' worth, so a one-word name and a two-word one leave the grid level.
            ImGui::Dummy({kSlotSize, lineHeight * 2.0f});

            ImGui::EndGroup();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Tab closes.  G drops the selected item.");
    }
    ImGui::End();
}

void PredationGame::OnImGui()
{
    // The editor is a tool, not part of the game's own interface, so it is drawn whether or not the
    // debug overlay is showing and it takes the screen to itself.
    if (m_editor.IsOpen())
    {
        m_editor.DrawUi(m_editorScene, m_app->GetMeshes());
        // Beside it, because it answers the one question the editor cannot: where does the hand end
        // up. Only for something held: a ship has no grip to be placed.
        if (m_editor.IsWeapon())
        {
            DrawWeaponBench();
            DrawEditorFirstPerson();
        }
        return;
    }

    // The boot screen first, once, over everything.
    if (!m_bootDone && DrawBootScreen())
    {
        return;
    }
    MenuSounds();
    if (m_screen == Screen::Title)
    {
        DrawTitleScreen();
        return;
    }

    // The HUD is part of the game, not the debug overlay, so it is always drawn -- except while a cinematic has the players,
    // when there is nothing for it to say but what the intercom does.
    if (m_cameraMode != CameraMode::Fly && !m_cine.Active() && !m_mapOpen)
    {
        DrawHud();
    }
    else if (m_cine.Active())
    {
        DrawSubtitle();
    }
    DrawCinematicOverlay();
    DrawCinematicDebug();
    DrawCinematicEditor();

    DrawSystemMap();
    if (m_paused)
    {
        DrawPauseMenu();
    }

    // Outside the overlay's switch, because it is asked for by name and read while playing.
    DrawBrainInspector();

    if (!m_app->IsOverlayVisible())
    {
        KeepDebugOnTop();
        return;
    }

#if !PRED_DEV_TOOLS
    // A player's F3: the frame rate the engine already shows, and the connection under it. The
    // tuning panels below are for building the game, and in a player's hands a slider that moves
    // the sun or the sprint speed is a way to break it rather than a setting.
    DrawConnectionReadout();
#else

    DrawNetworkPanel();
    DrawSoundPanel();

    ImGui::SetNextWindowPos(ImVec2(8.0f, 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Player"))
    {
        if (m_cameraMode == CameraMode::Fly)
        {
            ImGui::TextUnformatted("Free camera. P cycles first person, third person and this.");
            ImGui::Text("Camera %.2f %.2f %.2f", m_camera.position.x, m_camera.position.y,
                        m_camera.position.z);
            ImGui::Separator();
        }
        DrawPlayerPanel();

        // What the mixer is doing. Sound is the one subsystem with nothing to look at, so the
        // numbers are the only way to tell a silent bug from a quiet moment.
        ImGui::Separator();
        AudioEngine& audio = m_app->GetAudio();
        const AudioEngine::Stats sound = audio.GetStats();
        ImGui::Text("Audio %s  %d voices, %u played", audio.HasDevice() ? "on" : "OFF (no device)",
                    sound.voices, sound.started);
        if (sound.stolen > 0 || sound.clipped > 0)
        {
            ImGui::TextDisabled("%u cut short, %u samples limited", sound.stolen, sound.clipped);
        }
        float volume = audio.MasterGain();
        if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.5f, "%.2f"))
        {
            audio.SetMasterGain(volume);
        }
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(420.0f, 470.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("World", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const SceneRenderer::Stats& stats = m_app->GetSceneRenderer().LastStats();
        ImGui::Text("Entities %zu   submitted %zu   triangles %zu", m_scene.EntityCount(),
                    stats.meshesSubmitted, stats.trianglesSubmitted);
        ImGui::Text("Props %zu", m_props.size());

        float fov = cv_fov.Get();
        if (ImGui::SliderFloat("FOV (horizontal)", &fov, 60.0f, 120.0f, "%.0f"))
        {
            cv_fov.Set(fov);
        }
        float sensitivity = cv_mouseSensitivity.Get();
        if (ImGui::SliderFloat("Sensitivity", &sensitivity, 0.02f, 0.5f, "%.3f"))
        {
            cv_mouseSensitivity.Set(sensitivity);
        }
        bool invert = cv_invertY.Get();
        if (ImGui::Checkbox("Invert Y", &invert))
        {
            cv_invertY.Set(invert);
        }
        bool crouchToggle = cv_crouchToggle.Get();
        if (ImGui::Checkbox("Crouch toggles", &crouchToggle))
        {
            cv_crouchToggle.Set(crouchToggle);
        }
        bool sprintToggle = cv_sprintToggle.Get();
        if (ImGui::Checkbox("Sprint toggles", &sprintToggle))
        {
            cv_sprintToggle.Set(sprintToggle);
        }

        ImGui::Separator();
        Environment& environment = m_scene.GetEnvironment();
        float sunIntensity = cv_sunIntensity.Get();
        if (ImGui::SliderFloat("Sun intensity", &sunIntensity, 0.0f, 8.0f, "%.2f"))
        {
            cv_sunIntensity.Set(sunIntensity);
        }
        ImGui::ColorEdit3("Fog colour", &environment.fogColor.r);
        float fogStart = cv_fogStart.Get();
        if (ImGui::SliderFloat("Fog start", &fogStart, 0.0f, 100.0f, "%.1f m"))
        {
            cv_fogStart.Set(fogStart);
        }
        float fogEnd = cv_fogEnd.Get();
        if (ImGui::SliderFloat("Fog end", &fogEnd, 1.0f, 400.0f, "%.1f m"))
        {
            cv_fogEnd.Set(fogEnd);
        }

        bool grid = cv_showGrid.Get();
        if (ImGui::Checkbox("Reference grid", &grid))
        {
            cv_showGrid.Set(grid);
        }
        ImGui::SameLine();
        bool wireframe = cv_wireframe.Get();
        if (ImGui::Checkbox("Wireframe", &wireframe))
        {
            cv_wireframe.Set(wireframe);
        }
        ImGui::SameLine();
        bool fullbright = cv_fullbright.Get();
        if (ImGui::Checkbox("Fullbright", &fullbright))
        {
            cv_fullbright.Set(fullbright);
        }
    }
    ImGui::End();
#endif
    KeepDebugOnTop();
}

void PredationGame::KeepDebugOnTop()
{
    // The developer's windows -- the F3 overlay, the panels, the console last -- over everything else, whatever is open: the
    // map, a cinematic, the title. (They are made before most of the game's windows, which otherwise came up over them.)
    for (const char* name : {"##DebugOverlay", "Player", "World", "Sound", "Network", "Hold it", "First person", "Cinematic",
                             "Creature brain", "##PredationConsole", "##PredationConsoleSuggestions"})
    {
        if (ImGuiWindow* window = ImGui::FindWindowByName(name); window != nullptr && window->Active)
        {
            ImGui::BringWindowToDisplayFront(window);
        }
    }
}


} // namespace pred
