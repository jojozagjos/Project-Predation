#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Net/BitStream.h"
#include "Game/Net/Protocol.h"
#include "Game/Player/SupportDrone.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

using namespace pred;

// The drone a dead player drives: a thing on the floor with tracks, which goes where it is driven, can be
// knocked over, gets itself back up, and shuts down for a while when it is hurt enough.

namespace
{

struct Floor
{
    PhysicsWorld physics;
    Scene scene;
    MeshLibrary meshes;

    Floor()
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        meshes.SetHeadless(true);
        Transform slab;
        slab.position = {0.0f, -0.5f, 0.0f};
        physics.CreateBox({30.0f, 0.5f, 30.0f}, slab, BodyMotion::Static);
        physics.OptimizeBroadPhase();
    }

    void Run(SupportDrone& drone, const SupportDrone::Controls& controls, float seconds)
    {
        constexpr float dt = 1.0f / 60.0f;
        for (int i = 0; i < static_cast<int>(seconds / dt); ++i)
        {
            drone.Step(physics, controls, dt);
            physics.Step(dt);
            drone.UpdateVisual(scene, physics, dt);
        }
    }
};

bool Upright(const SupportDrone& drone)
{
    return (drone.Rotation() * glm::vec3(0.0f, 1.0f, 0.0f)).y > 0.9f;
}

} // namespace

TEST_CASE("A support drone settles on its tracks and drives where its driver looks", "[drone]")
{
    Floor floor;
    SupportDrone drone;
    drone.Deploy(floor.scene, floor.meshes, floor.physics, {0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(drone.Active());
    floor.Run(drone, {}, 1.0f);
    CHECK(Upright(drone));
    // Resting on the floor: its middle half its height up, and its camera about knee high.
    CHECK(drone.Position().y > 0.05f);
    CHECK(drone.Position().y < 0.15f);
    CHECK(drone.Eye().y > 0.3f);
    CHECK(drone.Eye().y < 0.55f);

    // Looking a quarter turn right and pushed forward, it turns and goes that way (+X).
    SupportDrone::Controls controls;
    controls.move = {0.0f, 1.0f};
    controls.lookYaw = glm::half_pi<float>();
    const glm::vec3 start = drone.Position();
    floor.Run(drone, controls, 3.0f);
    const glm::vec3 moved = drone.Position() - start;
    INFO("moved " << moved.x << ", " << moved.z);
    CHECK(moved.x > 3.5f);
    CHECK(std::abs(moved.z) < 1.0f);
    CHECK(Upright(drone));
    // Its camera looks where its driver does, whichever way the chassis happens to be pointing.
    CHECK(drone.LookDirection().x > 0.95f);

    // Pulled back, it reverses rather than turning round.
    controls.move = {0.0f, -1.0f};
    const glm::vec3 before = drone.Position();
    floor.Run(drone, controls, 1.5f);
    CHECK(drone.Position().x < before.x - 1.5f);
    CHECK(drone.LookDirection().x > 0.95f);

    // Let go, it stops.
    floor.Run(drone, {}, 0.2f);
    const glm::vec3 stopped = drone.Position();
    SupportDrone::Controls idle;
    idle.lookYaw = glm::half_pi<float>();
    floor.Run(drone, idle, 1.0f);
    CHECK(glm::distance(drone.Position(), stopped) < 0.1f);
}

TEST_CASE("A support drone driven at a ledge stops at it, and hops up onto it", "[drone]")
{
    Floor floor;
    // Knee high to a person, a little over twice the drone's own height, a metre in front of it.
    Transform ledge;
    ledge.position = {0.0f, 0.2f, -2.5f};
    floor.physics.CreateBox({2.0f, 0.2f, 1.5f}, ledge, BodyMotion::Static);
    SupportDrone drone;
    drone.Deploy(floor.scene, floor.meshes, floor.physics, {0.0f, 0.0f, 0.0f}, 0.0f);
    floor.Run(drone, {}, 0.5f);

    SupportDrone::Controls forward;
    forward.move = {0.0f, 1.0f};
    floor.Run(drone, forward, 1.5f);
    INFO("at " << drone.Position().y << " up, " << drone.Position().z);
    CHECK(drone.Position().z > -1.0f);
    CHECK(drone.Position().y < 0.2f);

    // Up against it, a hop and still driving: over the edge and onto the top.
    SupportDrone::Controls hop = forward;
    hop.jump = true;
    floor.Run(drone, hop, 0.1f);
    floor.Run(drone, forward, 1.5f);
    INFO("then at " << drone.Position().y << " up, " << drone.Position().z);
    CHECK(drone.Position().y > 0.4f);
    CHECK(drone.Position().z < -1.5f);
    CHECK(Upright(drone));
}

TEST_CASE("A support drone knocked over rights itself, and its picture goes over with it", "[drone]")
{
    Floor floor;
    SupportDrone drone;
    drone.Deploy(floor.scene, floor.meshes, floor.physics, {0.0f, 0.0f, 0.0f}, 0.0f);
    floor.Run(drone, {}, 0.5f);

    // Laid on its side.
    Transform side;
    side.position = {0.0f, 0.25f, 0.0f};
    side.rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    floor.physics.SetTransform(drone.Body(), side);
    floor.physics.SetLinearVelocity(drone.Body(), glm::vec3(0.0f));
    floor.physics.SetAngularVelocity(drone.Body(), glm::vec3(0.0f));
    floor.Run(drone, {}, 1.0f);
    REQUIRE_FALSE(Upright(drone));
    CHECK(drone.Upended());
    // On its side, the camera's up is along the floor: the picture is on its side.
    const glm::mat4 view = drone.ViewMatrix();
    const glm::vec3 cameraUp{view[0][1], view[1][1], view[2][1]};
    CHECK(std::abs(cameraUp.y) < 0.5f);

    // Asked, it kicks itself back over.
    SupportDrone::Controls right;
    right.rightItself = true;
    floor.Run(drone, right, 0.1f);
    floor.Run(drone, {}, 2.0f);
    CHECK(Upright(drone));

    // On its back, left alone, it gets up by itself in a few seconds -- trying again if once is not enough.
    Transform back;
    back.position = {0.0f, 0.3f, 0.0f};
    back.rotation = glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    floor.physics.SetTransform(drone.Body(), back);
    floor.physics.SetLinearVelocity(drone.Body(), glm::vec3(0.0f));
    floor.physics.SetAngularVelocity(drone.Body(), glm::vec3(0.0f));
    floor.Run(drone, {}, 10.0f);
    CHECK(Upright(drone));
}

TEST_CASE("A support drone hurt enough shuts down, reboots, and comes back part mended", "[drone]")
{
    Floor floor;
    SupportDrone drone;
    drone.Deploy(floor.scene, floor.meshes, floor.physics, {0.0f, 0.0f, 0.0f}, 0.0f);
    floor.Run(drone, {}, 0.5f);

    // Struck: shoved along and hurt, not yet out.
    const glm::vec3 before = drone.Position();
    drone.Hit(floor.physics, {40.0f, 10.0f, 0.0f}, 40.0f);
    floor.Run(drone, {}, 1.0f);
    CHECK(drone.Position().x > before.x + 0.5f);
    CHECK(drone.Health() == SupportDrone::kMaxHealth - 40.0f);
    CHECK_FALSE(drone.Disabled());

    // Struck again, harder than it has left: it shuts down and cannot be driven.
    drone.Hit(floor.physics, glm::vec3(0.0f), 80.0f);
    REQUIRE(drone.Disabled());
    CHECK(drone.Health() == 0.0f);
    floor.Run(drone, {}, 3.0f); // settle, and right itself is not something it does while out
    const glm::vec3 out = drone.Position();
    SupportDrone::Controls drive;
    drive.move = {0.0f, 1.0f};
    floor.Run(drone, drive, 2.0f);
    CHECK(glm::distance(drone.Position(), out) < 0.2f);
    CHECK(drone.RebootProgress() > 0.4f);
    CHECK(drone.RebootProgress() < 0.6f);

    // Rebooted, it is back with some of its health and drives again.
    floor.Run(drone, {}, SupportDrone::kRebootSeconds);
    CHECK_FALSE(drone.Disabled());
    CHECK(drone.Health() == SupportDrone::kHealthAfterReboot);
}

TEST_CASE("A support drone's state reaches the host with its owner's input, and everybody else in snapshots",
          "[drone][net][protocol]")
{
    DroneState drone;
    drone.active = true;
    drone.position = {12.5f, 3.2f, -40.25f};
    // On its side, turned.
    drone.rotation = glm::angleAxis(1.2f, glm::vec3(0.0f, 1.0f, 0.0f)) *
                     glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
    drone.headYaw = -0.8f;
    drone.headPitch = 0.5f;
    drone.health = 42.0f;
    drone.rebootLeft = 6.5f;
    drone.lightOn = true;

    const auto same = [&](const DroneState& back)
    {
        CHECK(back.active);
        CHECK(glm::distance(back.position, drone.position) < 0.01f);
        // The same way up and the same way round: the same rotation, or its negative.
        CHECK(std::abs(glm::dot(back.rotation, drone.rotation)) > 0.999f);
        CHECK(std::abs(back.headYaw - drone.headYaw) < 0.02f);
        CHECK(std::abs(back.headPitch - drone.headPitch) < 0.02f);
        CHECK(std::abs(back.health - drone.health) < 1.0f);
        CHECK(std::abs(back.rebootLeft - drone.rebootLeft) < 0.2f);
        CHECK(back.lightOn);
    };

    InputMessage input;
    input.count = 1;
    input.drone = drone;
    BitWriter up;
    WriteInput(up, input);
    const std::vector<uint8_t>& upBytes = up.Finish();
    BitReader upReader(upBytes.data(), upBytes.size());
    InputMessage upBack;
    REQUIRE(ReadInput(upReader, upBack));
    same(upBack.drone);

    SnapshotMessage snapshot;
    snapshot.count = 2;
    snapshot.players[0].playerId = 0;
    snapshot.players[1].playerId = 2;
    snapshot.players[1].alive = false;
    snapshot.players[1].drone = drone;
    BitWriter down;
    WriteSnapshot(down, snapshot);
    const std::vector<uint8_t>& downBytes = down.Finish();
    BitReader downReader(downBytes.data(), downBytes.size());
    SnapshotMessage downBack;
    REQUIRE(ReadSnapshot(downReader, downBack));
    CHECK_FALSE(downBack.players[0].drone.active);
    same(downBack.players[1].drone);
}

TEST_CASE("A drone struck, and a death that is for good, reach everybody", "[drone][net][protocol]")
{
    const auto roundTrip = [](const WorldEventMessage& sent)
    {
        BitWriter writer;
        WriteMessageHeader(writer, MessageType::WorldEvent);
        WriteWorldEvent(writer, sent);
        const std::vector<uint8_t>& bytes = writer.Finish();
        BitReader reader(bytes.data(), bytes.size());
        MessageType type = MessageType::Count;
        REQUIRE(ReadMessageHeader(reader, type));
        WorldEventMessage received;
        REQUIRE(ReadWorldEvent(reader, received));
        return received;
    };

    WorldEventMessage hit;
    hit.kind = WorldEventKind::DroneHit;
    hit.player = 3;
    hit.direction = {-30.0f, 22.0f, 12.5f};
    hit.amount = 34.0f;
    const WorldEventMessage hitBack = roundTrip(hit);
    CHECK(hitBack.kind == WorldEventKind::DroneHit);
    CHECK(hitBack.player == 3);
    CHECK(glm::distance(hitBack.direction, hit.direction) < 0.1f);
    CHECK(std::abs(hitBack.amount - 34.0f) < 0.5f);

    WorldEventMessage died;
    died.kind = WorldEventKind::PlayerDied;
    died.player = 1;
    died.amount = 0.0f;
    died.flag = true;
    CHECK(roundTrip(died).flag);
    died.flag = false;
    died.amount = 6.0f;
    const WorldEventMessage back = roundTrip(died);
    CHECK_FALSE(back.flag);
    CHECK(back.amount == 6.0f);
}

TEST_CASE("Somebody else's drone is shown where their machine says, and follows it", "[drone]")
{
    Floor floor;
    SupportDrone proxy;
    const glm::quat turned = glm::angleAxis(0.7f, glm::vec3(0.0f, 1.0f, 0.0f));
    proxy.DeployProxy(floor.scene, floor.meshes, floor.physics, {2.0f, 0.09f, 3.0f}, turned);
    REQUIRE(proxy.Active());
    REQUIRE(proxy.IsProxy());
    constexpr float dt = 1.0f / 60.0f;
    const glm::vec3 there{4.0f, 0.09f, 3.5f};
    for (int i = 0; i < 60; ++i)
    {
        proxy.SetPose(there, turned, 0.4f, -0.2f);
        proxy.SetStatus(55.0f, 0.0f);
        proxy.lightOn = true;
        floor.physics.Step(dt);
        proxy.UpdateVisual(floor.scene, floor.physics, dt);
    }
    CHECK(glm::distance(proxy.Position(), there) < 0.05f);
    CHECK(proxy.Health() == 55.0f);
    // Its body went with it, so a round aimed at it finds it there.
    const RayHit hit = floor.physics.RayCast(there + glm::vec3(0.0f, 2.0f, 0.0f), {0.0f, -1.0f, 0.0f}, 3.0f);
    REQUIRE(hit);
    CHECK(hit.body == proxy.Body());
    proxy.Remove(floor.scene, floor.physics);
    CHECK_FALSE(proxy.Active());
}

TEST_CASE("A support drone driven up stairs rides up them by itself", "[drone]")
{
    // Stairs as the buildings have them, eighteen centimetres a step: it rides up each, as tracks do, without being
    // made to hop at every one -- which is where it used to stop.
    Floor floor;
    for (int step = 0; step < 6; ++step)
    {
        Transform tread;
        const float top = 0.18f * static_cast<float>(step + 1);
        tread.position = {0.0f, top * 0.5f, -1.5f - 0.3f * static_cast<float>(step) - 1.5f};
        floor.physics.CreateBox({1.0f, top * 0.5f, 1.5f}, tread, BodyMotion::Static);
    }
    SupportDrone drone;
    drone.Deploy(floor.scene, floor.meshes, floor.physics, {0.0f, 0.0f, 0.0f}, 0.0f);
    floor.Run(drone, {}, 0.5f);
    SupportDrone::Controls forward;
    forward.move = {0.0f, 1.0f};
    // Up to the top, a tenth of a second at a time, upright all the way.
    float highest = 0.0f;
    bool stayedUpright = true;
    for (int i = 0; i < 40 && highest < 1.1f; ++i)
    {
        floor.Run(drone, forward, 0.1f);
        highest = std::max(highest, drone.Position().y);
        stayedUpright = stayedUpright && Upright(drone);
    }
    INFO("highest " << highest << ", at " << drone.Position().y << " up, " << drone.Position().z);
    CHECK(highest > 1.0f);
    CHECK(stayedUpright);
}
