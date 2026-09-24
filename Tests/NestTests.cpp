#include "Engine/Net/BitStream.h"
#include "Engine/Render/Mesh.h"
#include "Game/Net/Protocol.h"
#include "Game/World/HiveMesh.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <glm/geometric.hpp>

#include <vector>

using namespace pred;

// A nest is a heart on a wall and growth spread from it. None of it may reach behind the surface it is
// on -- walls are thin, and a heart on one side must not show through on the other -- and none of it
// is solid, so it cannot trap the creature that built it.

TEST_CASE("A nest's heart stands out from its wall, and its roots spread flat across it", "[nest][mesh]")
{
    const NestHeartMeshes meshes = BuildNestHeart(1234);
    REQUIRE(meshes.heart.TriangleCount() > 500);
    REQUIRE(meshes.roots.TriangleCount() > 500);

    const AABB heart = meshes.heart.ComputeBounds();
    INFO("heart from " << heart.min.x << "," << heart.min.y << "," << heart.min.z << " to " << heart.max.x << ","
                       << heart.max.y << "," << heart.max.z);
    CHECK(heart.min.z > -0.04f);
    // The middle of it -- what is shot at, lit and heard -- is inside it.
    CHECK(heart.min.x < 0.0f);
    CHECK(heart.max.x > 0.0f);
    CHECK(heart.min.y < kNestHeartUp);
    CHECK(heart.max.y > kNestHeartUp);
    CHECK(heart.max.z > kNestHeartOut);
    // About the size of a chest.
    CHECK(heart.max.z - heart.min.z < 0.8f);
    CHECK(heart.max.y - heart.min.y > 0.6f);

    const AABB roots = meshes.roots.ComputeBounds();
    CHECK(roots.min.z > -0.04f);
    CHECK(roots.max.z < 0.45f);
    CHECK(roots.max.x - roots.min.x > 2.4f);
    CHECK(roots.max.y - roots.min.y > 2.4f);
}

TEST_CASE("Growth lies on its surface and each nest's is its own", "[nest][mesh]")
{
    const MeshData first = BuildNestGrowth(77, 0);
    const MeshData again = BuildNestGrowth(77, 0);
    const MeshData other = BuildNestGrowth(77, 1);
    const MeshData elsewhere = BuildNestGrowth(78, 0);
    REQUIRE(first.TriangleCount() > 200);

    const AABB bounds = first.ComputeBounds();
    CHECK(bounds.min.y > -0.04f);
    CHECK(bounds.max.y < 0.6f);
    CHECK(bounds.max.x - bounds.min.x > 1.8f);

    CHECK(MeshFingerprintForTesting(first) == MeshFingerprintForTesting(again));
    CHECK(MeshFingerprintForTesting(first) != MeshFingerprintForTesting(other));
    CHECK(MeshFingerprintForTesting(first) != MeshFingerprintForTesting(elsewhere));
}

namespace
{

WorldEventMessage RoundTrip(const WorldEventMessage& sent)
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
}

} // namespace

TEST_CASE("A nest arrives as old as it is, so a newcomer sees it grown as far as everybody else",
          "[nest][net][protocol]")
{
    WorldEventMessage built;
    built.kind = WorldEventKind::NestBuilt;
    built.index = 3;
    built.item = 0xA11E;
    built.position = {-14.0f, 0.0f, 22.5f};
    built.amount = 187.4f;
    built.quiet = true;
    const WorldEventMessage received = RoundTrip(built);
    CHECK(received.kind == WorldEventKind::NestBuilt);
    CHECK(received.index == 3);
    CHECK(received.item == 0xA11E);
    CHECK(received.quiet);
    CHECK(received.amount == Catch::Approx(187.0f).margin(1.0f));
    CHECK(glm::distance(received.position, built.position) < 0.01f);
}

TEST_CASE("A heart's wounds are sent as how much is left, and only nothing left is dead", "[nest][net][protocol]")
{
    WorldEventMessage wounded;
    wounded.kind = WorldEventKind::NestWounded;
    wounded.index = 2;
    wounded.player = 3;
    wounded.amount = 0.62f;
    WorldEventMessage received = RoundTrip(wounded);
    CHECK(received.kind == WorldEventKind::NestWounded);
    CHECK(received.index == 2);
    CHECK(received.player == 3);
    CHECK(received.amount == Catch::Approx(0.62f).margin(0.01f));

    // A sliver left is still alive.
    wounded.amount = 0.004f;
    CHECK(RoundTrip(wounded).amount > 0.0f);
    wounded.amount = 0.0f;
    CHECK(RoundTrip(wounded).amount == 0.0f);
}
