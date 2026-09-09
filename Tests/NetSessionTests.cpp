#include "Engine/Net/Transport.h"
#include "Engine/Physics/PhysicsWorld.h"
#include "Game/Net/NetSession.h"
#include "Game/Net/Prediction.h"
#include "Game/Player/PlayerController.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <memory>
#include <vector>

using namespace pred;

// A host and a client in one process, over a transport that can be made as bad as we like.
//
// The point of these tests is that prediction is only exercised by latency: on a perfect link the
// client and the host agree because nothing has had time to disagree. Everything here therefore
// runs with a round trip a real player would have.
namespace
{

constexpr float kTick = 1.0f / 60.0f;

// One machine: a physics world, a floor, and a player in it.
struct Machine
{
    PhysicsWorld physics;
    PlayerController player;
    PlayerConfig config;

    explicit Machine(const glm::vec3& spawn = {0.0f, 0.05f, 0.0f})
    {
        PhysicsWorld::Settings settings;
        settings.workerThreads = 1;
        REQUIRE(physics.Init(settings));
        physics.CreateBox({60.0f, 0.5f, 60.0f}, Transform{{0.0f, -0.5f, 0.0f}}, BodyMotion::Static);
        physics.OptimizeBroadPhase();
        REQUIRE(player.Init(physics, config, spawn));
    }

    ~Machine()
    {
        player.Shutdown();
        physics.Shutdown();
    }

    void Step(const PlayerInput& input)
    {
        physics.Step(kTick);
        player.Step(input, kTick);
    }
};

PlayerInput WalkForward()
{
    PlayerInput input;
    input.move = {0.0f, 1.0f};
    return input;
}

// Runs both ends for `ticks` fixed ticks, with the client predicting and the host authoritative.
struct Link
{
    Machine hostMachine;
    Machine clientMachine;
    NetHost host;
    NetClient client;
    uint32_t tick = 0;

    Link(uint16_t port, const NetConditions& conditions)
    {
        auto hostTransport = CreateLoopbackTransport(1234u);
        auto clientTransport = CreateLoopbackTransport(5678u);
        hostTransport->SetConditions(conditions);
        clientTransport->SetConditions(conditions);

        NetHost::Config hostConfig;
        hostConfig.port = port;
        REQUIRE(host.Start(std::move(hostTransport), hostConfig, hostMachine.physics, hostMachine.config,
                           {0.0f, 0.05f, 0.0f}));

        NetClient::Config clientConfig;
        REQUIRE(client.Connect(std::move(clientTransport), "loopback", port, "tester", clientConfig));
    }

    void Run(int ticks, const PlayerInput& clientInput, const PlayerInput& hostInput = {})
    {
        for (int i = 0; i < ticks; ++i)
        {
            ++tick;
            hostMachine.Step(hostInput);
            host.Tick(tick, hostMachine.player.State(), kTick);

            clientMachine.physics.Step(kTick);
            client.Tick(clientInput, clientMachine.player, kTick);
            client.UpdateInterpolation(kTick);
        }
    }

    // Where the host thinks the client's player is.
    glm::vec3 HostViewOfClient() const
    {
        REQUIRE(host.Remotes().size() == 1);
        return host.Remotes()[0].position;
    }
};

} // namespace

// --- The prediction buffer ---------------------------------------------------------------------

TEST_CASE("The prediction buffer keeps ticks in order and forgets settled ones", "[net][prediction]")
{
    PredictionBuffer buffer;
    PlayerInput input;
    PlayerState state;

    for (uint32_t i = 1; i <= 10; ++i)
    {
        state.position.x = static_cast<float>(i);
        buffer.Record(i, input, state);
    }
    CHECK(buffer.Size() == 10);
    CHECK(buffer.OldestSequence() == 1);
    CHECK(buffer.NewestSequence() == 10);

    REQUIRE(buffer.Find(4) != nullptr);
    CHECK(buffer.Find(4)->state.position.x == 4.0f);
    CHECK(buffer.Find(99) == nullptr);

    const std::vector<PredictedTick> after = buffer.After(7);
    REQUIRE(after.size() == 3);
    CHECK(after[0].sequence == 8);
    CHECK(after[2].sequence == 10);

    buffer.DropTo(7);
    CHECK(buffer.Size() == 3);
    CHECK(buffer.OldestSequence() == 8);
    CHECK(buffer.Find(4) == nullptr);
}

TEST_CASE("The prediction buffer never grows past its capacity", "[net][prediction]")
{
    PredictionBuffer buffer;
    PlayerInput input;
    PlayerState state;
    for (uint32_t i = 1; i <= PredictionBuffer::kCapacity * 2; ++i)
    {
        buffer.Record(i, input, state);
    }
    CHECK(buffer.Size() == PredictionBuffer::kCapacity);
    CHECK(buffer.NewestSequence() == PredictionBuffer::kCapacity * 2);
    // The oldest ticks are the ones dropped: a correction that far behind is unrecoverable anyway.
    CHECK(buffer.OldestSequence() == PredictionBuffer::kCapacity + 1);
}

TEST_CASE("Replaying a tick replaces what was remembered about it", "[net][prediction]")
{
    PredictionBuffer buffer;
    PlayerInput input;
    PlayerState state;
    state.position.x = 1.0f;
    buffer.Record(5, input, state);

    state.position.x = 2.0f;
    buffer.UpdateState(5, state);
    REQUIRE(buffer.Find(5) != nullptr);
    CHECK(buffer.Find(5)->state.position.x == 2.0f);
    CHECK(buffer.Size() == 1);
}

// --- Joining -----------------------------------------------------------------------------------

TEST_CASE("A client joins and is given a player slot", "[net][session]")
{
    NetConditions perfect;
    Link link(41001, perfect);

    CHECK_FALSE(link.client.Connected());
    link.Run(4, PlayerInput{});

    CHECK(link.client.Connected());
    CHECK(link.client.PlayerId() == 1);
    CHECK(link.host.ConnectedCount() == 1);
}

TEST_CASE("A fifth player is turned away rather than squeezed in", "[net][session]")
{
    Machine hostMachine;
    NetHost host;
    NetHost::Config config;
    config.port = 41002;
    REQUIRE(host.Start(CreateLoopbackTransport(), config, hostMachine.physics, hostMachine.config,
                       {0.0f, 0.05f, 0.0f}));

    std::vector<std::unique_ptr<NetClient>> clients;
    for (int i = 0; i < 4; ++i)
    {
        auto client = std::make_unique<NetClient>();
        REQUIRE(client->Connect(CreateLoopbackTransport(), "loopback", 41002, "extra", NetClient::Config{}));
        clients.push_back(std::move(client));
    }

    Machine clientMachine;
    for (int i = 0; i < 8; ++i)
    {
        host.Tick(static_cast<uint32_t>(i + 1), hostMachine.player.State(), kTick);
        for (auto& client : clients)
        {
            client->Tick(PlayerInput{}, clientMachine.player, kTick);
        }
    }

    // Three clients plus the host is a full game. The fourth is refused and told why.
    CHECK(host.ConnectedCount() == 3);
    int refused = 0;
    for (auto& client : clients)
    {
        if (client->Rejection() == JoinRejection::ServerFull)
        {
            ++refused;
        }
    }
    CHECK(refused == 1);
}

// --- Prediction --------------------------------------------------------------------------------

TEST_CASE("A client moves at once rather than waiting for the host", "[net][session]")
{
    NetConditions laggy;
    laggy.latencyMs = 80.0f; // a round trip of 160 ms, which would be a sixth of a second of nothing
    Link link(41003, laggy);

    link.Run(12, PlayerInput{});
    REQUIRE(link.client.Connected());

    const glm::vec3 before = link.clientMachine.player.State().position;
    link.Run(6, WalkForward()); // a tenth of a second, less than one round trip
    const glm::vec3 after = link.clientMachine.player.State().position;

    // The player is already moving even though the host cannot possibly have answered yet.
    CHECK(glm::length(after - before) > 0.02f);
}

TEST_CASE("The host arrives at the same place the client predicted", "[net][session]")
{
    NetConditions laggy;
    laggy.latencyMs = 60.0f;
    laggy.jitterMs = 20.0f;
    Link link(41004, laggy);

    link.Run(20, PlayerInput{});
    REQUIRE(link.client.Connected());

    link.Run(120, WalkForward()); // two seconds of walking
    link.Run(40, PlayerInput{});  // and long enough for the host to catch up and answer

    const glm::vec3 predicted = link.clientMachine.player.State().position;
    const glm::vec3 authoritative = link.HostViewOfClient();

    // Both ran the same movement code over the same inputs, so they end up in the same place. The
    // tolerance is the buffered input the host has not run yet, not a disagreement.
    INFO("predicted " << predicted.z << " authoritative " << authoritative.z);
    CHECK(glm::length(predicted - authoritative) < 0.25f);
    CHECK(predicted.z < -1.0f); // forward is negative Z, so it really did walk
}

TEST_CASE("A wrong guess is corrected and the remaining inputs replayed", "[net][session]")
{
    NetConditions laggy;
    laggy.latencyMs = 70.0f;
    Link link(41005, laggy);
    link.Run(20, PlayerInput{});
    REQUIRE(link.client.Connected());

    link.Run(30, WalkForward());

    // Shove the client's prediction somewhere the host never put it, the way a missed collision or
    // a dropped input would.
    PlayerState wrong = link.clientMachine.player.State();
    wrong.position += glm::vec3(0.0f, 0.0f, -1.5f);
    link.clientMachine.player.RestoreState(wrong);

    const uint32_t before = link.client.CorrectionCount();
    link.Run(30, WalkForward());

    // The host's answer disagreed, so the client took the host state and replayed on top of it.
    CHECK(link.client.CorrectionCount() > before);

    // Standing still lets the host run everything it has buffered. While the player is walking the
    // client is legitimately a round trip ahead, and comparing the two then would be measuring the
    // latency rather than the correction.
    link.Run(40, PlayerInput{});
    const glm::vec3 predicted = link.clientMachine.player.State().position;
    INFO("predicted " << predicted.z << " authoritative " << link.HostViewOfClient().z);
    CHECK(glm::length(predicted - link.HostViewOfClient()) < 0.1f);
}

TEST_CASE("A correction is hidden in the drawing rather than snapping the camera", "[net][session]")
{
    NetConditions laggy;
    laggy.latencyMs = 70.0f;
    Link link(41006, laggy);
    link.Run(20, PlayerInput{});
    REQUIRE(link.client.Connected());
    link.Run(20, WalkForward());

    PlayerState wrong = link.clientMachine.player.State();
    wrong.position += glm::vec3(0.0f, 0.0f, -0.4f);
    link.clientMachine.player.RestoreState(wrong);
    const uint32_t before = link.client.CorrectionCount();
    link.Run(20, WalkForward());

    REQUIRE(link.client.CorrectionCount() > before);
    // Whatever the correction moved, the visual offset absorbed it and is now decaying towards
    // nothing rather than having been applied in one frame.
    const float carried = glm::length(link.client.VisualOffset());
    link.Run(40, WalkForward());
    CHECK(glm::length(link.client.VisualOffset()) < carried);
}

TEST_CASE("Losing input packets does not stop the player", "[net][session]")
{
    NetConditions lossy;
    lossy.latencyMs = 50.0f;
    lossy.jitterMs = 30.0f;
    lossy.lossPercent = 25.0f; // one packet in four, which is a genuinely bad connection
    Link link(41007, lossy);

    link.Run(30, PlayerInput{});
    REQUIRE(link.client.Connected());

    link.Run(180, WalkForward());
    link.Run(60, PlayerInput{});

    // Every input packet repeats the last three ticks, so a quarter of them going missing costs
    // almost nothing: the host still ran nearly every tick.
    const glm::vec3 authoritative = link.HostViewOfClient();
    CHECK(authoritative.z < -3.0f);
    CHECK(glm::length(link.clientMachine.player.State().position - authoritative) < 0.6f);
}

// --- Seeing other people ----------------------------------------------------------------------

TEST_CASE("A client sees the host move, interpolated and slightly behind", "[net][session]")
{
    NetConditions laggy;
    laggy.latencyMs = 40.0f;
    Link link(41008, laggy);
    link.Run(20, PlayerInput{});
    REQUIRE(link.client.Connected());

    link.Run(120, PlayerInput{}, WalkForward()); // the host walks, the client stands still

    REQUIRE(link.client.Remotes().size() == 1);
    const RemotePlayerView& hostView = link.client.Remotes()[0];
    CHECK(hostView.id == 0);

    const glm::vec3 truth = link.hostMachine.player.State().position;
    // Drawn behind where the host actually is, by the interpolation delay and the latency, but on
    // the same path rather than lagging further and further behind.
    CHECK(hostView.position.z > truth.z);
    CHECK(hostView.position.z - truth.z < 1.0f);
    CHECK(hostView.position.z < -1.0f);
}

TEST_CASE("A client is never shown its own body as a remote player", "[net][session]")
{
    NetConditions perfect;
    Link link(41009, perfect);
    link.Run(30, WalkForward(), WalkForward());

    for (const RemotePlayerView& view : link.client.Remotes())
    {
        CHECK(view.id != link.client.PlayerId());
    }
}

TEST_CASE("A disconnecting client is dropped by the host", "[net][session]")
{
    NetConditions perfect;
    Link link(41010, perfect);
    link.Run(10, PlayerInput{});
    REQUIRE(link.host.ConnectedCount() == 1);

    link.client.Disconnect();
    for (int i = 0; i < 10; ++i)
    {
        ++link.tick;
        link.host.Tick(link.tick, link.hostMachine.player.State(), kTick);
    }
    CHECK(link.host.ConnectedCount() == 0);
    CHECK(link.host.Remotes().empty());
}

TEST_CASE("World changes reach every client, and losses do not lose them", "[net][session]")
{
    // A door opening is an event, not a value that will be sent again. If it goes missing the door
    // is shut on that player's screen for the rest of the game, so these go on the channel that
    // resends. This runs on a link that drops a third of everything.
    NetConditions awful;
    awful.latencyMs = 40.0f;
    awful.jitterMs = 30.0f;
    awful.lossPercent = 33.0f;
    Link link(41011, awful);

    link.Run(40, PlayerInput{});
    REQUIRE(link.client.Connected());

    for (uint8_t i = 0; i < 6; ++i)
    {
        WorldEventMessage event;
        event.kind = WorldEventKind::DoorMoved;
        event.index = i;
        event.flag = i % 2 == 0;
        link.host.Broadcast(event);
    }

    std::vector<WorldEventMessage> received;
    for (int i = 0; i < 400 && received.size() < 6; ++i)
    {
        link.Run(1, PlayerInput{});
        for (const WorldEventMessage& event : link.client.TakeWorldEvents())
        {
            received.push_back(event);
        }
    }

    REQUIRE(received.size() == 6);
    for (uint8_t i = 0; i < 6; ++i)
    {
        INFO("event " << static_cast<int>(i));
        CHECK(received[i].kind == WorldEventKind::DoorMoved);
        CHECK(received[i].index == i); // in the order they happened
        CHECK(received[i].flag == (i % 2 == 0));
    }
}

TEST_CASE("A client asks the host to open things rather than opening them", "[net][session]")
{
    NetConditions laggy;
    laggy.latencyMs = 50.0f;
    Link link(41012, laggy);
    link.Run(40, PlayerInput{});
    REQUIRE(link.client.Connected());

    link.client.SendInteract(1 /* door */, 3);
    link.Run(30, PlayerInput{});

    const std::vector<NetHost::InteractRequest> requests = link.host.TakeInteractRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].player == link.client.PlayerId());
    CHECK(requests[0].kind == 1);
    CHECK(requests[0].index == 3);

    // Reading them clears them, so one press is one request.
    CHECK(link.host.TakeInteractRequests().empty());
}

TEST_CASE("A shot reaches the host with its aim intact", "[net][session]")
{
    NetConditions laggy;
    laggy.latencyMs = 60.0f;
    laggy.lossPercent = 20.0f;
    Link link(41013, laggy);
    link.Run(40, PlayerInput{});
    REQUIRE(link.client.Connected());

    ShotMessage shot;
    shot.shotNumber = 7;
    shot.origin = {1.0f, 1.6f, -2.0f};
    shot.direction = glm::normalize(glm::vec3(0.1f, 0.0f, -1.0f));
    link.client.SendShot(shot);

    std::vector<NetHost::ShotRequest> requests;
    for (int i = 0; i < 200 && requests.empty(); ++i)
    {
        link.Run(1, PlayerInput{});
        requests = link.host.TakeShotRequests();
    }

    REQUIRE(requests.size() == 1);
    CHECK(requests[0].player == link.client.PlayerId());
    CHECK(requests[0].shot.shotNumber == 7);
    CHECK(glm::distance(requests[0].shot.direction, shot.direction) < 0.01f);
}

TEST_CASE("Loose objects are replicated as state", "[net][session]")
{
    NetConditions laggy;
    laggy.latencyMs = 30.0f;
    laggy.lossPercent = 15.0f;
    Link link(41014, laggy);
    link.Run(40, PlayerInput{});
    REQUIRE(link.client.Connected());

    WorldStateMessage state;
    state.count = 3;
    for (uint8_t i = 0; i < 3; ++i)
    {
        state.bodies[i].id = i;
        state.bodies[i].position = {static_cast<float>(i) * 2.0f, 0.5f, -1.0f};
    }
    link.host.SendWorldState(state);

    for (int i = 0; i < 100 && !link.client.HasWorldState(); ++i)
    {
        link.Run(1, PlayerInput{});
    }

    REQUIRE(link.client.HasWorldState());
    const WorldStateMessage& got = link.client.LatestWorldState();
    REQUIRE(got.count == 3);
    CHECK(got.bodies[2].position.x == Catch::Approx(4.0f).margin(0.002));
}

TEST_CASE("The host can rewind to where a client was looking", "[net][session][lag]")
{
    // A client draws everyone else a fixed delay behind the newest snapshot, and that snapshot took
    // a one-way trip to reach it. It therefore aims at where somebody was. Testing a shot against
    // the present punishes a player for their own connection, so the host keeps a second of history
    // and rewinds to the moment the shot was aimed.
    NetConditions laggy;
    laggy.latencyMs = 90.0f;
    Link link(41015, laggy);
    link.Run(40, PlayerInput{});
    REQUIRE(link.client.Connected());

    // The host walks steadily while the client stands still and watches.
    link.Run(150, PlayerInput{}, WalkForward());

    const uint32_t renderTick = link.client.RenderTick();
    REQUIRE(renderTick > 0);

    const std::vector<NetHost::PlayerPose> then = link.host.PosesAt(renderTick);
    const std::vector<NetHost::PlayerPose> now = link.host.PosesAt(link.host.CurrentTick());
    REQUIRE_FALSE(then.empty());
    REQUIRE_FALSE(now.empty());

    // The host is player zero in both.
    const float thenZ = then[0].position.z;
    const float nowZ = now[0].position.z;
    INFO("client was looking at z " << thenZ << ", host is now at z " << nowZ);

    // The rewind actually goes back: the host has moved on since the moment the client was drawing.
    CHECK(nowZ < thenZ - 0.05f);

    // And it matches where the client is actually drawing the host, which is the whole point.
    REQUIRE(link.client.Remotes().size() == 1);
    const float drawnZ = link.client.Remotes()[0].position.z;
    INFO("client is drawing the host at z " << drawnZ);
    CHECK(std::abs(drawnZ - thenZ) < 0.15f);
}

TEST_CASE("A rewind further back than anyone could need is refused", "[net][session][lag]")
{
    // The obvious way to cheat with lag compensation is to claim you were looking a long way into
    // the past and shoot people where they used to be. The clamp is what stops that.
    NetConditions laggy;
    laggy.latencyMs = 40.0f;
    Link link(41016, laggy);
    link.Run(40, PlayerInput{});
    REQUIRE(link.client.Connected());
    link.Run(120, PlayerInput{}, WalkForward());

    const uint32_t now = link.host.CurrentTick();
    const std::vector<NetHost::PlayerPose> honest = link.host.PosesAt(now - 10);
    const std::vector<NetHost::PlayerPose> absurd = link.host.PosesAt(1); // "I was looking at the start"
    REQUIRE_FALSE(honest.empty());
    REQUIRE_FALSE(absurd.empty());

    // The absurd claim is answered with the oldest rewind anyone is allowed, not with tick one.
    const std::vector<NetHost::PlayerPose> limit = link.host.PosesAt(now - 15);
    INFO("absurd " << absurd[0].position.z << ", limit " << limit[0].position.z);
    CHECK(std::abs(absurd[0].position.z - limit[0].position.z) < 0.01f);
    // And the honest one is more recent than the limit, so the clamp is not just flattening
    // everything to one answer.
    CHECK(honest[0].position.z < limit[0].position.z - 0.01f);
}

TEST_CASE("What a client holds reaches the host and the other clients", "[net][session]")
{
    // The host cannot see inside another machine, so unless a client says what it is holding
    // nobody else ever sees it holding anything. This has been reported fixed once already, so it
    // gets a test rather than an assurance.
    NetConditions laggy;
    laggy.latencyMs = 45.0f;
    laggy.lossPercent = 10.0f;
    Link link(41017, laggy);
    link.Run(40, PlayerInput{});
    REQUIRE(link.client.Connected());

    // A rifle, half raised, mid-reload.
    link.client.SetHeld(7, 0.5f, true, 0.4f);
    link.Run(60, PlayerInput{});

    REQUIRE(link.host.Remotes().size() == 1);
    const RemotePlayerView& seen = link.host.Remotes()[0];
    INFO("host sees item " << static_cast<int>(seen.heldItem) << " aim " << seen.aim);
    CHECK(seen.heldItem == 7);
    CHECK(seen.aim == Catch::Approx(0.5f).margin(0.05));
    CHECK(seen.reloading);
    CHECK(seen.reloadProgress == Catch::Approx(0.4f).margin(0.05));

    // Putting it away is seen too.
    link.client.SetHeld(0, 0.0f, false, 0.0f);
    link.Run(60, PlayerInput{});
    CHECK(link.host.Remotes()[0].heldItem == 0);
    CHECK(link.host.Remotes()[0].aim == Catch::Approx(0.0f).margin(0.05));
}

TEST_CASE("Aim reaches other players as a number, not a switch", "[net][session]")
{
    // Sights come up over a fifth of a second. Sent as a yes or no, everyone else watches them snap.
    NetConditions laggy;
    laggy.latencyMs = 40.0f;
    Link link(41018, laggy);
    link.Run(40, PlayerInput{});
    REQUIRE(link.client.Connected());

    std::vector<float> seen;
    for (int i = 0; i <= 10; ++i)
    {
        link.client.SetHeld(3, static_cast<float>(i) / 10.0f, false, 0.0f);
        link.Run(12, PlayerInput{});
        seen.push_back(link.host.Remotes()[0].aim);
    }

    // It climbed rather than jumping from nothing to everything.
    int distinct = 0;
    for (size_t i = 1; i < seen.size(); ++i)
    {
        if (std::abs(seen[i] - seen[i - 1]) > 0.02f)
        {
            ++distinct;
        }
    }
    INFO("saw " << distinct << " separate steps between hip and sights");
    CHECK(distinct >= 6);
}

TEST_CASE("A climb is visible to everyone else", "[net][session]")
{
    // A climb moves the character along a fixed path. Sent as position alone it reads as a body
    // sliding up a wall, so the fact of it, how far through it is, and where the ledge is all go
    // over the wire while it lasts.
    NetConditions laggy;
    laggy.latencyMs = 40.0f;
    Link link(41019, laggy);
    link.Run(40, PlayerInput{});
    REQUIRE(link.client.Connected());

    // Put the host mid-climb by hand: what is being tested is the replication, not the ledge search.
    PlayerState& state = link.hostMachine.player.State();
    state.mantling = true;
    state.mantleTime = 0.4f;
    state.mantleDuration = 0.8f;
    state.mantleEdge = {1.5f, 1.1f, -2.5f};

    for (int i = 0; i < 40; ++i)
    {
        link.Run(1, PlayerInput{});
        state.mantling = true; // the harness steps the controller, which would end it
        state.mantleTime = 0.4f;
        state.mantleDuration = 0.8f;
        state.mantleEdge = {1.5f, 1.1f, -2.5f};
    }

    REQUIRE(link.client.Remotes().size() == 1);
    const RemotePlayerView& seen = link.client.Remotes()[0];
    INFO("client sees mantling=" << seen.mantling << " phase " << seen.mantlePhase << " edge "
                                 << seen.mantleEdge.x << "," << seen.mantleEdge.y);
    CHECK(seen.mantling);
    CHECK(seen.mantlePhase == Catch::Approx(0.5f).margin(0.05));
    CHECK(seen.mantleEdge.x == Catch::Approx(1.5f).margin(0.01));
    CHECK(seen.mantleEdge.y == Catch::Approx(1.1f).margin(0.01));
}
