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


// The weapon bench and the model editor's first-person view.

namespace pred
{

namespace
{

// The panel's own resolution. Small on purpose: it is redrawn every frame, and what it is for is
// the shape and placement of a hold, which reads at this size.
constexpr uint16_t kEditorEyeWidth = 640;
// The height is the window's, worked out per frame: see RenderEditorFirstPerson.

} // namespace

// The weapon bench.
//
// Placing a grip socket is not something that can be done by looking at the model on its own: the
// only question that matters is where the hand ends up, and the only way to answer it is to hold
// the thing. So the bench points a weapon at a model without a restart, drives everything the
// weapon does, and draws the sockets on the weapon as it is held, with the distance from each one
// to the hand that is supposed to be at it.
// The editor's try-it panel: someone holding what is being built.
//
// Placing a grip cannot be done by looking at the model. The only question is where the hand ends
// up, so the panel shows the distance from each socket to the hand meant to be at it, draws the line
// between them in the world, and plays every movement the weapon has on demand. Move a socket, watch
// the number come down.
void PredationGame::DrawWeaponBench()
{
    if (m_screen != Screen::Editor || !m_editorBodyBuilt)
    {
        return;
    }

    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 372.0f, 24.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({350.0f, 520.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Hold it"))
    {
        ImGui::End();
        return;
    }

    // Everything in here wraps to the panel. The explanations are the useful part of this window
    // and a sentence that runs off the right edge is worse than no sentence: it reads as a bug.
    ImGui::PushTextWrapPos(0.0f);

    ImGui::TextDisabled("Whatever is open in the editor, in someone's hands.");
    ImGui::Separator();

    // What is in the hands: the model being edited, or one of the items that is not a weapon.
    //
    // One list rather than a list and a checkbox and a second list. They were separate because a
    // weapon and an item are placed by different machinery, and a person holding one thing at a
    // time does not care about that. Choosing an item also takes the weapon out of the hands, which
    // it has to: a weapon takes both of them and the one-handed carry never runs while one is held,
    // so the item was placed and then never posed and never appeared.
    {
        std::vector<const ItemDefinition*> holdable;
        for (const ItemDefinition& item : m_items.All())
        {
            if (item.id != kInvalidItem && !item.key.empty() &&
                m_weaponData.ForItem(item.key) == kInvalidWeapon)
            {
                holdable.push_back(&item);
            }
        }
        m_benchItem = std::clamp(m_benchItem, -1, static_cast<int>(holdable.size()) - 1);

        const auto label = [](const ItemDefinition* item)
        { return item->name.empty() ? item->key.c_str() : item->name.c_str(); };
        const char* current = m_benchItem < 0 ? "the model in the editor"
                                              : label(holdable[static_cast<size_t>(m_benchItem)]);
        if (ImGui::BeginCombo("In the hands", current))
        {
            if (ImGui::Selectable("the model in the editor", m_benchItem < 0))
            {
                m_benchItem = -1;
                m_benchItemHeld = false;
            }
            for (int i = 0; i < static_cast<int>(holdable.size()); ++i)
            {
                ImGui::PushID(i);
                if (ImGui::Selectable(label(holdable[static_cast<size_t>(i)]), i == m_benchItem))
                {
                    m_benchItem = i;
                    m_benchItemHeld = false;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        if (m_benchItem >= 0)
        {
            const ItemDefinition* chosen = holdable[static_cast<size_t>(m_benchItem)];
            if (ItemDefinition* editable = m_items.Mutable(chosen->id))
            {
                bool moved =
                    ImGui::DragFloat3("Offset", &editable->holdOffset.x, 0.002f, -0.5f, 0.5f, "%.3f m");
                moved |= ImGui::DragFloat3("Turn", &editable->holdRotation.x, 1.0f, -180.0f, 180.0f,
                                           "%.0f deg");
                moved |= ImGui::DragFloat3("Hand", &editable->holdHand.x, 0.002f, -0.5f, 0.5f, "%.3f m");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Where the hand itself goes: right, up and forward of where things are usually carried.");
                }
                if (moved)
                {
                    m_editorBody.SetHeldItemPlacement(editable->holdOffset, editable->holdRotation, editable->holdHand);
                }
                if (ImGui::Button("Write to items.json"))
                {
                    const std::filesystem::path file = Paths::AssetsRoot() / "Data" / "items.json";
                    m_app->GetConsole().Print(m_items.SaveHoldPlacements(file)
                                                  ? "Hold placements written to items.json"
                                                  : "Could not write items.json");
                }
                ImGui::TextDisabled("The offset is from the fingers and the turn is in the hand's "
                                    "own frame, so both follow the arm wherever it goes.");

                // Its use: what it does, how long it takes, and the hand's movement through it, as
                // keys that can be played, scrubbed, changed and added to here, and written back.
                ItemUse& use = editable->use;
                if (use.kind != ItemUseKind::None &&
                    ImGui::CollapsingHeader((std::string("Use: ") + ItemUseKindName(use.kind)).c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                {
                    const bool twoHalves = !use.secondMotion.empty();
                    if (twoHalves)
                    {
                        ImGui::Checkbox("The second half (thrown)", &m_benchUseSecond);
                    }
                    else
                    {
                        m_benchUseSecond = false;
                    }
                    float& seconds = m_benchUseSecond ? use.secondSeconds : use.seconds;
                    std::vector<ItemMotionKey>& keys = m_benchUseSecond ? use.secondMotion : use.motion;
                    ImGui::SliderFloat("Takes", &seconds, 0.1f, 5.0f, "%.2f s");
                    if (ImGui::Button(m_benchUseTime >= 0.0f ? "Playing..." : "Play"))
                    {
                        m_benchUseTime = 0.0f;
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::SliderFloat("##scrub", &m_benchUseScrub, 0.0f, 1.0f, "at %.2f"))
                    {
                        m_benchUseTime = -1.0f;
                    }
                    int remove = -1;
                    for (size_t k = 0; k < keys.size(); ++k)
                    {
                        ImGui::PushID(static_cast<int>(k) + 1000);
                        ImGui::SetNextItemWidth(60.0f);
                        ImGui::DragFloat("##t", &keys[k].t, 0.005f, 0.0f, 1.0f, "%.2f");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("go"))
                        {
                            m_benchUseScrub = keys[k].t;
                            m_benchUseTime = -1.0f;
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("x"))
                        {
                            remove = static_cast<int>(k);
                        }
                        ImGui::DragFloat3("move", &keys[k].offset.x, 0.002f, -0.6f, 0.6f, "%.3f");
                        ImGui::DragFloat3("turn", &keys[k].turn.x, 0.5f, -180.0f, 180.0f, "%.0f");
                        ImGui::PopID();
                    }
                    if (remove >= 0)
                    {
                        keys.erase(keys.begin() + remove);
                    }
                    if (ImGui::Button("Add a key where the scrubber is"))
                    {
                        ItemMotionKey key = SampleMotion(keys, m_benchUseScrub);
                        key.t = m_benchUseScrub;
                        keys.push_back(key);
                    }
                    std::sort(keys.begin(), keys.end(), [](const ItemMotionKey& a, const ItemMotionKey& b) { return a.t < b.t; });
                    ImGui::TextDisabled("Each key is where the hand has the item that far through the use: metres "
                                        "right, up and forward of the view, and a turn. Written with the button above.");
                }
            }
        }
    }
    ImGui::Separator();

    // What the body is doing. Nothing simulates in here, so every movement is a button: the point
    // is to watch one thing at a time and as often as you like.
    const char* stances[] = {"Standing", "Crouching", "Prone"};
    ImGui::Combo("Stance", &m_editorStance, stances, 3);
    ImGui::Checkbox("Walk on the spot", &m_editorWalking);
    ImGui::SliderFloat("Sights", &m_editorAim, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("Follow the timeline", &m_benchFollowTimeline);
    ImGui::SetItemTooltip("%s", "Plays the clip open in the Animation panel at its playhead, so scrubbing "
                                "the timeline moves the hands in front of you.");

    if (ImGui::Button("Reload"))
    {
        m_editorReload = 0.0f;
        m_editorReloadEmpty = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from empty"))
    {
        m_editorReload = 0.0f;
        m_editorReloadEmpty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Draw"))
    {
        m_editorDraw = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Put away"))
    {
        m_editorHolster = 1.0f;
        m_editorHolstering = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Fire"))
    {
        m_editorKick = 1.0f;
    }
    if (m_editorReload >= 0.0f)
    {
        ImGui::ProgressBar(m_editorReload, {-1.0f, 0.0f}, "reloading");
    }

    ImGui::Separator();

    // The clips the model carries, each playable on demand. A clip that only runs when it happens
    // to be named after something the weapon already does is most of a clip system nobody can use.
    const ModelAsset& model = m_editor.Model();
    if (model.clips.empty())
    {
        ImGui::TextDisabled("No clips on this model, so nothing moves on it: reloading and drawing "
                            "are the model's own now, not something written into the game. Add one "
                            "under Animation and name it reload, equip, unequip or fire. A fire "
                            "clip plays on top of the recoil rather than instead of it.");
    }
    else
    {
        int clipIndex = 0;
        for (const AnimationClip& clip : model.clips)
        {
            // By position, not by name. A clip whose name has been cleared would push an empty id.
            ImGui::PushID(clipIndex++);
            const bool playing = m_editorClip == clip.name;
            if (ImGui::Button(playing ? "Stop" : "Play"))
            {
                m_editorClip = playing ? std::string() : clip.name;
                m_editorClipTime = 0.0f;
            }
            ImGui::SameLine();
            ImGui::Text("%s  %.2fs  %d tracks", clip.name.c_str(), clip.duration,
                        static_cast<int>(clip.tracks.size()));
            if (playing)
            {
                ImGui::SliderFloat("##scrub", &m_editorClipTime, 0.0f, 1.0f, "%.2f");
                ImGui::SameLine();
                ImGui::Checkbox("Run", &m_editorClipPlaying);
            }
            ImGui::PopID();
        }
        if (model.FindPart("magazine") == nullptr)
        {
            ImGui::TextColored({0.90f, 0.70f, 0.45f, 1.0f},
                               "No part called 'magazine', so the built-in reload has nothing to "
                               "take out. Rename the part that is one and it will move.");
        }
    }

    ImGui::Separator();
    if (m_editorBody.HasWeapon())
        {
        ImGui::Checkbox("Draw sockets and the hands they belong to", &m_benchSockets);
        ImGui::TextDisabled("The carry socket is where the weapon sits: move it and the gun moves on "
                            "the screen. The grip is where the trigger hand closes: move that and the "
                            "hand moves along the weapon. Neither drags the other any more.");
        const WeaponVisual& visual = m_editorBody.Weapon();
        const glm::quat hold = m_editorBody.WeaponRotation();
        const glm::vec3 origin = m_editorBody.WeaponOrigin();
        // Two separate questions, and they used to be answered by one number that could not tell them
        // apart. A wrist sits a hand's length behind whatever the palm closes on, so a hand properly on
        // a grip still reads several centimetres away from it, and a support hand that slid back down
        // the barrel because the socket was out of reach read as a much larger miss than a hand that
        // simply could not get there. The useful readings are whether the fingers close on the socket,
        // and how hard the arm is working to hold that.
        const auto report = [&](const char* label, const glm::vec3& socket, int side)
        {
            const glm::vec3 world = origin + hold * socket;
            const glm::vec3 wrist = m_editorBody.GetPose().GlobalPosition(m_editorBody.Rig().hand[side]);
            const glm::vec3 shoulder =
                m_editorBody.GetPose().GlobalPosition(m_editorBody.Rig().shoulder[side]);
            const float gap = glm::distance(world, wrist);
            const float span =
                m_editorBody.Rig().upperArmLength + m_editorBody.Rig().lowerArmLength;
            const float working = glm::distance(wrist, shoulder) / std::max(span, 1e-3f);
    
            // A hand is about eleven centimetres from wrist to closed fingers, so anything inside that
            // is a hand on the grip. Beyond it the arm went somewhere else.
            const bool held = gap < 0.11f;
            ImGui::TextColored(held ? ImVec4(0.65f, 0.85f, 0.65f, 1.0f) : ImVec4(0.90f, 0.70f, 0.45f, 1.0f),
                               held ? "%s hand is on its socket, wrist %.1f cm behind it"
                                    : "%s hand ended up %.1f cm from its socket",
                               label, gap * 100.0f);
            ImGui::TextColored(working < 0.95f ? ImVec4(0.65f, 0.85f, 0.65f, 1.0f)
                                               : ImVec4(0.90f, 0.70f, 0.45f, 1.0f),
                               "   arm at %.0f%% of its reach", working * 100.0f);
        };
        report("Trigger", visual.triggerGrip, 1);
        report("Support", visual.supportGrip, 0);
    
        // What is wrong with the hold, said where the hold is being looked at. The same faults are
        // reported in the editor's Model panel, which is collapsed most of the time and is not where
        // anybody is looking while they place a grip.
        {
            const ImVec4 warn{0.95f, 0.55f, 0.45f, 1.0f};
            if (visual.muzzle.z <= visual.triggerGrip.z)
            {
                ImGui::TextColored(warn, "The muzzle socket is behind the grip, so this is being held "
                                         "by the barrel and pointed at its own stock. Turn the model "
                                         "round under Model, or set the grip's Turn to 0, 180, 0.");
            }
            const float across = std::max(std::abs(visual.triggerGrip.x), std::abs(visual.supportGrip.x));
            if (across > 0.08f)
            {
                ImGui::TextColored(warn,
                                   "A grip sits %.0f cm off the middle of the weapon. Hands close on the "
                                   "centre line of a gun, and an offset there twists the whole hold.",
                                   across * 100.0f);
            }
        }
        ImGui::TextDisabled("The support hand slides back down the barrel when its socket is out of "
                            "reach, so a socket out past the end of the handguard shows as a hand that "
                            "is not on it rather than as an arm stretched to nothing. An arm at its "
                            "full reach is the one to move: that is where the elbow locks straight and "
                            "the hold stops looking like a hold.");
    }


    ImGui::Separator();
    ImGui::TextDisabled("The weapon this model is worn by, for the game:");
    // Every row carries its own id, so two weapons that happen to share a name cannot collide.
    // The database's "no weapon" placeholder used to be in this list and had to be skipped by hand
    // here -- offering it crashed the game outright, because a row with an empty name is a row with
    // an empty id, and an empty id at the root of a popup is the one thing ImGui refuses. All()
    // no longer hands it out, so there is nothing to remember.
    const std::span<const WeaponDefinition> weapons = m_weaponData.All();
    if (!weapons.empty())
    {
        m_benchWeapon = std::clamp(m_benchWeapon, 0, static_cast<int>(weapons.size()) - 1);
        const auto label = [&](int index)
        {
            const WeaponDefinition& weapon = weapons[static_cast<size_t>(index)];
            return weapon.name.empty() ? weapon.key : weapon.name;
        };
        if (ImGui::BeginCombo("Weapon", label(m_benchWeapon).c_str()))
        {
            for (int i = 0; i < static_cast<int>(weapons.size()); ++i)
            {
                ImGui::PushID(i);
                if (ImGui::Selectable(label(i).c_str(), i == m_benchWeapon))
                {
                    m_benchWeapon = i;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Assign this model to it"))
        {
            AssignModelToWeapon(weapons[static_cast<size_t>(m_benchWeapon)].id);
        }
        ImGui::TextDisabled("Not written to weapons.json: set it there to keep it.");
    }

    ImGui::PopTextWrapPos();
    ImGui::End();
}

void PredationGame::RenderEditorFirstPerson()
{
    if (m_screen != Screen::Editor || !m_editorFirstPerson || !m_editorBodyBuilt)
    {
        return;
    }

    // The panel is the shape of the game's window, not a fixed 16:9. How much of a weapon is on
    // screen depends entirely on how wide the screen is, so a hold lined up against a panel of the
    // wrong shape comes out somewhere else in the game. The target is remade when the window
    // changes shape, which is rare enough to cost nothing.
    const Renderer& renderer = m_app->GetRenderer();
    const float windowAspect = renderer.Height() > 0 ? static_cast<float>(renderer.Width()) /
                                                           static_cast<float>(renderer.Height())
                                                     : 16.0f / 9.0f;
    const auto wantedHeight = static_cast<uint16_t>(
        std::clamp(static_cast<int>(std::lround(kEditorEyeWidth / std::max(windowAspect, 0.2f))), 180,
                   1200));
    if (wantedHeight != m_editorEyeHeightPixels)
    {
        DestroyEditorFirstPerson();
        m_editorEyeHeightPixels = wantedHeight;
    }

    if (!bgfx::isValid(m_editorEyeBuffer))
    {
        const uint64_t targetFlags = BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
        bgfx::TextureHandle attachments[2] = {
            bgfx::createTexture2D(kEditorEyeWidth, m_editorEyeHeightPixels, false, 1,
                                  bgfx::TextureFormat::BGRA8, targetFlags),
            bgfx::createTexture2D(kEditorEyeWidth, m_editorEyeHeightPixels, false, 1,
                                  Depth::Format(), BGFX_TEXTURE_RT_WRITE_ONLY)};
        if (!bgfx::isValid(attachments[0]) || !bgfx::isValid(attachments[1]))
        {
            PRED_LOG_ERROR(Render, "Editor first-person panel: could not create its render target");
            m_editorFirstPerson = false;
            return;
        }
        // The framebuffer takes ownership of both, so destroying it destroys them.
        m_editorEyeBuffer = bgfx::createFrameBuffer(2, attachments, true);
        m_editorEyeTexture = attachments[0];
        if (!bgfx::isValid(m_editorEyeBuffer))
        {
            PRED_LOG_ERROR(Render, "Editor first-person panel: could not create its framebuffer");
            m_editorFirstPerson = false;
            return;
        }
    }

    const PlayerView& view = m_editorView;
    const glm::vec3 forward = view.Forward();
    const glm::mat4 viewMatrix =
        glm::lookAtRH(view.eyePosition, view.eyePosition + forward, glm::vec3(0.0f, 1.0f, 0.0f));
    // The game's own field of view, and the near plane it uses, because that is what decides how
    // much of a receiver the camera cuts through.
    const float aspect =
        static_cast<float>(kEditorEyeWidth) / static_cast<float>(m_editorEyeHeightPixels);
    const float vertical = 2.0f * std::atan(std::tan(glm::radians(cv_fov.Get()) * 0.5f) / aspect);
    const glm::mat4 projection = Depth::Perspective(vertical, aspect, 0.05f, 200.0f);

    bgfx::setViewFrameBuffer(Renderer::kViewOffscreenLive, m_editorEyeBuffer);
    bgfx::setViewRect(Renderer::kViewOffscreenLive, 0, 0, kEditorEyeWidth, m_editorEyeHeightPixels);
    bgfx::setViewClear(Renderer::kViewOffscreenLive, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x14181cff,
                       Depth::Clear(), 0);
    bgfx::setViewTransform(Renderer::kViewOffscreenLive, glm::value_ptr(viewMatrix),
                           glm::value_ptr(projection));
    m_app->GetSceneRenderer().Draw(Renderer::kViewOffscreenLive, m_editorScene, m_app->GetMeshes(),
                                   view.eyePosition);
}

void PredationGame::DrawEditorFirstPerson()
{
    if (m_screen != Screen::Editor || !m_editorBodyBuilt)
    {
        return;
    }

    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 492.0f, 470.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({470.0f, 350.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("First person"))
    {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Draw it", &m_editorFirstPerson);
    ImGui::SameLine();
    ImGui::Checkbox("Centre lines", &m_editorEyeReticle);
    if (m_editorFirstPerson && bgfx::isValid(m_editorEyeTexture))
    {
        // Fitted to the panel, keeping the shape of the frame it is standing in for. A stretched
        // one would answer the on-screen question wrongly, which is the question it exists for.
        const float aspect =
            static_cast<float>(kEditorEyeWidth) / static_cast<float>(m_editorEyeHeightPixels);
        const ImVec2 room = ImGui::GetContentRegionAvail();
        const float width = std::min(room.x, std::max(room.y, 1.0f) * aspect);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, width / aspect);
        ImGui::Image(static_cast<ImTextureID>(ImGuiLayer::TextureId(m_editorEyeTexture)), size);

        // Where the view axis is, drawn over the picture. Lining a sight up means putting it on
        // that axis, and by eye alone the middle of a small panel is a guess.
        if (m_editorEyeReticle)
        {
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 centre(at.x + size.x * 0.5f, at.y + size.y * 0.5f);
            constexpr ImU32 kLine = IM_COL32(120, 230, 140, 150);
            draw->AddLine({at.x, centre.y}, {at.x + size.x, centre.y}, kLine);
            draw->AddLine({centre.x, at.y}, {centre.x, at.y + size.y}, kLine);
            draw->AddCircle(centre, size.y * 0.06f, kLine, 0, 1.0f);
        }
    }
    else if (!m_editorFirstPerson)
    {
        ImGui::TextDisabled("Off. The panel is redrawn every frame, so leave it off while working "
                            "on something else.");
    }

    ImGui::End();
}

void PredationGame::DestroyEditorFirstPerson()
{
    if (bgfx::isValid(m_editorEyeBuffer))
    {
        bgfx::destroy(m_editorEyeBuffer);
    }
    m_editorEyeBuffer = BGFX_INVALID_HANDLE;
    m_editorEyeTexture = BGFX_INVALID_HANDLE;
}

void PredationGame::AssignModelToWeapon(WeaponId weapon)
{
    WeaponDefinition* chosen = m_weaponData.Mutable(weapon);
    if (chosen == nullptr)
    {
        m_app->GetConsole().PrintError("No such weapon to assign a model to");
        return;
    }

    const std::string name = m_editor.Model().name;
    if (name.empty())
    {
        m_app->GetConsole().PrintError("The model has no name yet: give it one and save it first");
        return;
    }

    chosen->model = name;
    // Anything already built from the old model file is now wrong, so the cache goes and the next
    // weapon built reads the file again.
    ForgetWeaponModels();
    ApplyWeaponClipTimings();
    m_app->GetConsole().Print(chosen->name + " now wears " + name);
}

// The preview body in the editor: someone to hold what is being built.
//
// Driven from numbers rather than from a controller, because there is nothing to control. The body
// is the game's own, posed by the same code that poses the player, so a grip that looks held here is
// held in the game. A separate mannequin drawn by separate code would answer a different question.
void PredationGame::UpdateEditorBody(float frameDeltaSeconds)
{
    if (!m_editorBodyBuilt)
    {
        return;
    }

    m_editorClock += frameDeltaSeconds;

    const PlayerConfig& config = m_player.Config();
    const PlayerStance stance = m_editorStance == 2   ? PlayerStance::Prone
                                : m_editorStance == 1 ? PlayerStance::Crouching
                                                      : PlayerStance::Standing;
    m_editorState.stance = stance;
    m_editorState.grounded = true;
    m_editorState.alive = true;
    // Aside from the origin, where the model itself is laid out. Standing the two in the same place
    // means the body is always in front of the thing being edited. Beside anything bigger, just clear of it, for
    // its size to be judged against somebody's.
    m_editorState.position = {1.1f, 0.0f, 0.0f};
    AABB modelBounds;
    if (!m_editor.IsWeapon() && m_editor.ModelBounds(modelBounds))
    {
        m_editorState.position = {modelBounds.max.x + 0.8f, 0.0f, (modelBounds.min.z + modelBounds.max.z) * 0.5f};
    }

    // Walking on the spot. The stride advances but the body does not travel, so the gait can be
    // watched from one place rather than chased across the floor.
    const float speed = m_editorWalking ? config.SpeedForStance(stance, false, false) : 0.0f;
    m_editorState.velocity = glm::vec3(0.0f, 0.0f, -speed);
    if (m_editorWalking)
    {
        m_editorState.strideDistance += speed * frameDeltaSeconds;
        m_editorState.stridePhase = glm::fract(
            m_editorState.stridePhase +
            speed * frameDeltaSeconds / std::max(config.StrideLength(speed, stance), 0.05f));
    }

    m_editorView.eyeHeight = config.EyeHeightForStance(stance);
    m_editorView.renderPosition = m_editorState.position;
    m_editorView.eyePosition = m_editorState.position + glm::vec3(0.0f, m_editorView.eyeHeight, 0.0f);
    // Facing straight ahead. It used to be turned a little towards the model, on the idea that the
    // weapon reads better presented than seen end-on, and what it actually looked like was a body
    // standing crooked for no reason anyone could see.
    m_editorView.yaw = 0.0f;
    m_editorState.yaw = 0.0f;
    m_editorView.pitch = 0.0f;
    m_editorView.leanRoll = 0.0f;

    // What it is holding is whatever the editor has open, rebuilt whenever the editor says the model
    // changed. That is the whole loop: move a socket, see the hand move.
    const ModelAsset& model = m_editor.Model();
    const bool geometryChanged = m_editor.TakeGeometryChanged();
    const bool anythingChanged = m_editor.TakePreviewChanged();

    // A vehicle or a prop is not held: empty hands, and nothing of a weapon's played on them.
    if (!m_editor.IsWeapon())
    {
        if (m_editorBody.HasWeapon())
        {
            m_editorBody.SetWeapon(m_editorScene, m_app->GetMeshes(), nullptr);
        }
        if (m_benchItemHeld)
        {
            m_editorBody.ClearHeldItem(m_editorScene);
            m_benchItemHeld = false;
        }
        m_editorBody.Update(m_editorScene, m_editorState, m_editorView, config, m_app->GetPhysics(), frameDeltaSeconds);
        return;
    }

    // One thing in the hands at a time.
    //
    // A weapon takes both of them, and the one-handed carry never runs while one is held, so an
    // item put in the hand alongside a weapon was placed and then never posed: it stayed at the
    // origin and nobody ever saw it. Choosing an item takes the weapon away and choosing the model
    // brings it back.
    std::vector<const ItemDefinition*> holdable;
    for (const ItemDefinition& item : m_items.All())
    {
        if (item.id != kInvalidItem && !item.key.empty() &&
            m_weaponData.ForItem(item.key) == kInvalidWeapon)
        {
            holdable.push_back(&item);
        }
    }
    const int itemIndex =
        m_benchItem >= 0 && m_benchItem < static_cast<int>(holdable.size()) ? m_benchItem : -1;

    if (itemIndex >= 0)
    {
        if (m_editorBody.HasWeapon())
        {
            m_editorBody.SetWeapon(m_editorScene, m_app->GetMeshes(), nullptr);
        }
        // Built once rather than every frame: making a mesh per frame is how an editor comes to
        // feel slow, and dragging an offset does not change the mesh.
        if (!m_benchItemHeld)
        {
            const ItemDefinition* item = holdable[static_cast<size_t>(itemIndex)];
            m_editorBody.SetHeldItem(m_editorScene, m_app->GetMeshes(), item->key,
                                     ItemMesh(*item, &m_weaponData), ItemMaterial(*item));
            m_editorBody.SetHeldItemPlacement(item->holdOffset, item->holdRotation, item->holdHand);
            m_benchItemHeld = true;
        }
        // Its use, playing or scrubbed.
        const ItemDefinition* item = holdable[static_cast<size_t>(itemIndex)];
        const bool second = m_benchUseSecond && !item->use.secondMotion.empty();
        const float seconds = std::max(second ? item->use.secondSeconds : item->use.seconds, 0.05f);
        float at = m_benchUseScrub;
        if (m_benchUseTime >= 0.0f)
        {
            m_benchUseTime += frameDeltaSeconds;
            at = m_benchUseTime / seconds;
            m_benchUseScrub = std::min(at, 1.0f);
            if (at >= 1.0f)
            {
                m_benchUseTime = -1.0f;
            }
        }
        const ItemMotionKey pose = SampleMotion(second ? item->use.secondMotion : item->use.motion, at);
        m_editorBody.SetHeldItemMotion(pose.offset, pose.turn);
    }
    else
    {
        if (m_benchItemHeld)
        {
            m_editorBody.ClearHeldItem(m_editorScene);
            m_benchItemHeld = false;
        }
        if (geometryChanged || !m_editorBody.HasWeapon())
        {
            m_editorPreviewWeapon = WeaponDefinition{};
            m_editorPreviewWeapon.id = static_cast<WeaponId>(1);
            m_editorPreviewWeapon.key = model.name.empty() ? "preview" : model.name;
            m_editorPreviewWeapon.name = m_editorPreviewWeapon.key;
            m_editorPreviewWeapon.model = model.name;
            m_editorPreviewWeapon.reloadSeconds = 2.2f;
            m_editorPreviewWeapon.magazineSize = 30;
            ApplyClipTimings(m_editorPreviewWeapon, model);
            ForgetWeaponModels();
            m_editorBody.SetWeaponFromModel(m_editorScene, m_app->GetMeshes(), m_editorPreviewWeapon,
                                            model);
        }
    }

    if (anythingChanged && !geometryChanged)
    {
        // A socket or a clip. Nothing that is drawn has changed, so nothing that is drawn is
        // rebuilt: only the named points the hands are placed by are read again. Rebuilding the
        // meshes for a socket drag copied a quarter of a megabyte per part and destroyed and remade
        // every entity, on every frame of the drag.
        m_editorBody.RefreshWeaponSockets(m_editorPreviewWeapon, model);
    }

    // A clip being watched runs on its own clock, so it can be scrubbed or left running.
    if (!m_editorClip.empty() && m_editorClipPlaying)
    {
        const AnimationClip* watching = model.FindClip(m_editorClip);
        const float duration = watching != nullptr ? std::max(watching->duration, 0.05f) : 1.0f;
        m_editorClipTime = glm::fract(m_editorClipTime + frameDeltaSeconds / duration);
    }

    PlayerBody::WeaponPose pose;
    pose.aim = m_editorAim;
    pose.draw = m_editorDraw;
    pose.kick = m_editorKick;
    pose.reloading = m_editorReload >= 0.0f;
    pose.reload = m_editorReload;
    pose.reloadEmpty = m_editorReloadEmpty;
    pose.clip = m_editorClip;
    pose.clipProgress = m_editorClipTime;
    // What the timeline has open, at the playhead, unless a clip or a movement is being played from
    // here. Scrubbing the timeline then scrubs the hands in front of you.
    if (m_benchFollowTimeline && m_editorClip.empty() && m_editorReload < 0.0f)
    {
        if (const AnimationClip* editing = m_editor.SelectedClip())
        {
            pose.clip = editing->name;
            pose.clipProgress = editing->duration > 0.0f ? m_editor.Playhead() / editing->duration : 0.0f;
        }
    }
    pose.holster = m_editorHolster;
    m_editorBody.SetWeaponPose(pose);

    m_editorDraw = std::min(m_editorDraw + frameDeltaSeconds * 1.6f, 1.0f);
    if (m_editorHolstering)
    {
        m_editorHolster -= frameDeltaSeconds / 0.6f;
        if (m_editorHolster <= 0.0f)
        {
            // Held at nothing for a moment and then brought back out, so the put-away can be
            // watched over and over from one button.
            m_editorHolster = 1.0f;
            m_editorHolstering = false;
            m_editorDraw = 0.0f;
        }
    }
    m_editorKick = std::max(m_editorKick - frameDeltaSeconds * 7.0f, 0.0f);
    if (m_editorReload >= 0.0f)
    {
        // As long as the game would take: the clip's own length when the model has one.
        m_editorReload += frameDeltaSeconds / std::max(m_editorPreviewWeapon.ReloadSecondsFrom(m_editorReloadEmpty), 0.1f);
        if (m_editorReload > 1.0f)
        {
            m_editorReload = -1.0f;
        }
    }

    m_editorBody.Update(m_editorScene, m_editorState, m_editorView, config, m_app->GetPhysics(),
                        frameDeltaSeconds);
}

void PredationGame::OnFileDropped(const std::string& path)
{
#if PRED_DEV_TOOLS
    const std::filesystem::path file = path;
    if (!ModelEditor::IsImportableModel(file))
    {
        // Said rather than swallowed. A file that simply vanishes when you drop it on a window is
        // worse than one that is refused, because there is nothing to conclude from it.
        PRED_LOG_INFO(Asset, "Dropped {}, which is not a model the editor reads (OBJ, GLB or GLTF)",
                      file.filename().string());
        m_titleStatus = file.filename().string() + " is not an OBJ, GLB or GLTF.";
        return;
    }

    // Dropped anywhere but the editor, the editor is what was meant. Somebody dragging a model
    // onto the title screen has said what they want clearly enough to not be asked again.
    if (m_screen != Screen::Editor)
    {
        EnterEditor(std::string());
    }
    if (m_editor.DropFile(path))
    {
        PRED_LOG_INFO(Asset, "Imported {} by dropping it on the window", file.filename().string());
    }
#else
    // A shipped build has no editor to drop things into, and importing art at runtime is not a
    // thing a player does. Named in the log so a dropped file is not simply silence.
    PRED_LOG_INFO(Asset, "Ignoring dropped file {}: this build has no editor", path);
#endif
}

void PredationGame::EnterEditor(const std::string& modelName)
{
    // A place of its own rather than a mode switched on in the middle of a game. It has its own
    // scene, so what is being built stands against an empty floor instead of against whatever
    // happens to be at the spawn point, and nothing in the world simulates behind it.
    StopSession();
    m_screen = Screen::Editor;
    m_titleStatus.clear();
    m_inventoryOpen = false;

    m_editor.SetOpen(m_editorScene, true);
    if (!modelName.empty())
    {
        m_editor.Load(modelName);
    }

    if (!m_editorBodyBuilt)
    {
        // Someone to hold it. Built once and kept, because building a body is not free and the
        // editor is opened and closed a great deal.
        m_editorBody.Build(m_editorScene, m_app->GetMeshes(), m_player.Config());
        m_editorBody.SetTextureLibrary(m_app->GetTextures());
        m_editorBody.Tuning().hideHead = false;
        m_editorBodyBuilt = true;
    }
    m_editorState = PlayerState{};
    m_editorState.alive = true;
    m_editorState.grounded = true;
    m_editorClock = 0.0f;

    // Everything in an editor is done with the pointer, so the mouse is free and it is only taken
    // while the right button is held to look around.
    SetCameraMode(CameraMode::Fly);
    // Framed on the model, which sits at the origin, with the body it will be held by off to one
    // side and in shot.
    //
    // Written to the editor's camera, not the game's. The game's is copied from the editor's every
    // frame while the editor is open, so setting it here was undone one frame later and the view
    // jumped back to wherever the editor had last left it.
    m_editor.Camera().position = {0.30f, 0.62f, 1.15f};
    m_camera.position = m_editor.Camera().position;
    m_lookYaw = glm::radians(12.0f);
    m_lookPitch = glm::radians(-22.0f);
    m_wantMouseCaptured = false;
    UpdateMouseCapture();
    m_app->GetConsole().Print(
        "Model editor. WASD to move, Q and E for down and up, right mouse to look, Escape to leave.");
}


} // namespace pred
