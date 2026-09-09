#pragma once

#include "Engine/Application.h"
#include "Engine/Render/Camera.h"
#include "Engine/Scene/Scene.h"
#include "Game/Interaction/InteractionSystem.h"
#include "Game/Items/Inventory.h"
#include "Game/Items/ItemDatabase.h"
#include "Game/Items/ItemIcons.h"
#include "Game/Player/PlayerBody.h"
#include "Game/Player/PlayerController.h"
#include "Game/Weapons/ShotResolver.h"
#include "Game/Weapons/WeaponDatabase.h"
#include "Game/Weapons/WeaponSystem.h"
#include "Game/World/TestMap.h"
#include "Tools/ModelEditor/ModelEditor.h"
#include "Game/World/WorldObjects.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <vector>

namespace pred
{

// Milestone 3 game: a first-person player controller in the developer test map, with a free-flying
// inspection camera available on a key.
class PredationGame final : public Game
{
public:
    bool OnInit(Application& app) override;
    void OnShutdown() override;
    void OnEvent(const SDL_Event& event) override;
    void OnFixedUpdate(double fixedDt) override;
    void OnUpdate(double dt, double alpha) override;
    void OnRender() override;
    void OnImGui() override;

private:
    struct DynamicProp
    {
        Entity entity;
        BodyHandle body;
    };

    void RegisterCommands();
    void DrawDebugOverlays();
    void SyncDynamicProps();
    void SpawnProp(bool sphere, float impulse);
    void ClearProps();

    void SampleLook(float dt);
    PlayerInput BuildPlayerInput();
    void TryInteract();
    // Equips whatever weapon the selected inventory slot carries, or nothing if it carries none.
    void SyncEquippedWeapon();
    // Applies the rounds fired this tick. Only the authority may call this; it is the one place a
    // shot turns into an effect on the world.
    void ResolveShots();
    // Fades the drawn tracers. Presentation only.
    void AgeTracers(float dt);
    const WeaponDefinition* EquippedWeapon() const;
    // Where the barrel is and which way it points, including recoil.
    glm::vec3 MuzzlePosition() const;
    glm::vec3 AimDirection() const;
    void DropSelected();
    void EnterHidingSpot(int index);
    void LeaveHidingSpot();
    void DrawHud();
    void DrawInventoryPanel();
    // Draws the item as it actually looks, from the offscreen atlas. Falls back to nothing rather
    // than a stand-in shape: an icon that disagrees with the object is worse than no icon.
    void DrawItemIcon(ItemId item, float boxSize) const;
    void UpdateMouseCapture();
    // Logs any level geometry that intersects other level geometry. Overlapping static solids read
    // as modelling errors from inside and can shove the player, so the build reports its own.
    void ReportMapOverlaps(float minPenetration);
    // Best-effort name for a physics body, by finding the nearest scene entity. Bodies are not
    // named, and several can belong to one entity, so this is a diagnostic aid rather than a lookup.
    std::string DescribeBody(BodyHandle body) const;
    void ReloadPlayerConfig();
    void DrawPlayerPanel();

    Application* m_app = nullptr;
    Scene m_scene;

    PlayerController m_player;
    PlayerBody m_body;
    glm::vec3 m_spawnPoint{0.0f, 0.5f, TestMapSpec::kSpawnZ};

    ItemDatabase m_items;
    ItemIcons m_itemIcons;
    Inventory m_inventory;

    // The weapon simulation is deliberately separate from the controller. It is a pure function of
    // state, input and time, which is what lets a client run it the instant the trigger goes down
    // and a host run the identical code to decide what the round hit.
    WeaponDatabase m_weaponData;
    WeaponState m_weapon;
    std::vector<FireEvent> m_shots;
    // Where the last few rounds went, purely so they can be drawn. Never read by the simulation.
    struct Tracer
    {
        glm::vec3 from{0.0f};
        glm::vec3 to{0.0f};
        bool hit = false;
        float age = 0.0f;
    };
    std::vector<Tracer> m_tracers;
    InteractionSystem m_interactions;
    WorldObjects m_world;
    // -1 when not hidden. While hidden the player holds still inside the locker.
    int m_hidingSpot = -1;
    bool m_inventoryOpen = false;

    // First person for play, third person for watching the body animate, fly to inspect the level.
    enum class CameraMode : uint8_t
    {
        FirstPerson,
        ThirdPerson,
        Fly
    };
    CameraMode m_cameraMode = CameraMode::FirstPerson;
    float m_thirdPersonDistance = 3.2f;
    // Third-person orbit, applied on top of the look angles. Holding the look button turns the
    // camera around the character without turning the character, so the animation can be inspected
    // from any side.
    float m_orbitYaw = 0.0f;
    float m_orbitPitch = 0.0f;

    void SetCameraMode(CameraMode mode);
    const char* CameraModeName() const;

    FlyCamera m_camera;

    // The model and animation editor. It runs inside the game so that what is built in it is drawn
    // by the same renderer and the same shader the world uses.
    ModelEditor m_editor;

    // Mouse capture has exactly one owner. `m_wantMouseCaptured` is the player's intent, toggled by
    // Escape and by clicking back into the world. The effective state additionally requires window
    // focus and that no debug UI is using the pointer, so the console and the tuning panels are
    // always reachable.
    bool m_wantMouseCaptured = true;
    bool m_mouseCaptured = false;
    bool m_windowFocused = true;
    // The first motion event after relative mode is enabled can carry the whole distance from
    // wherever the cursor was, which would snap the view. Swallow one frame of delta.
    bool m_discardNextMouseDelta = false;

    // Look angles are sampled every frame, outside the fixed tick, so aiming is never limited by
    // the simulation rate.
    float m_lookYaw = 0.0f;
    float m_lookPitch = 0.0f;

    // Latched between frames so a press that happens between two ticks is never dropped.
    bool m_jumpLatch = false;
    bool m_crouchPressLatch = false;
    bool m_pronePressLatch = false;
    // Debug stance override, so stances can be inspected without holding a key.
    glm::vec2 m_debugMove{0.0f};
    // A reload request holds for a moment rather than a single tick, so pressing it while the
    // trigger is down or mid-shot still reloads as soon as the weapon can accept it.
    int m_reloadLatch = 0;
    // Console-driven trigger, so firing can be exercised in a headless capture.
    int m_debugTriggerTicks = 0;
    // Visual recoil: one on the frame a round leaves, decaying away. Separate from the weapon's own
    // recoil, which moves the aim; this only moves the model.
    float m_weaponKick = 0.0f;
    // Runs 0 to 1 as a weapon is brought up. Reset whenever what is held changes, so swapping is a
    // movement rather than one model being substituted for another.
    float m_weaponDraw = 1.0f;
    // Console-driven aim, so the sighted hold can be inspected in a headless capture.
    bool m_debugAim = false;
    bool m_forceCrouch = false;
    bool m_forceProne = false;
    bool m_crouchToggleState = false;
    bool m_proneToggleState = false;
    bool m_sprintToggleState = false;

    std::vector<DynamicProp> m_props;
    MeshHandle m_propSphereMesh;
    MeshHandle m_propBoxMesh;
    double m_time = 0.0;
};

} // namespace pred
