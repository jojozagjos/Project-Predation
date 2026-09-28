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

TEST_CASE("A nest's skin grows over the surfaces it is planned on, and knows how far each part is from the heart",
          "[nest][mesh]")
{
    // A floor with the heart's patch at the origin, a patch two metres out, and a root between them.
    NestSkinPlan plan;
    plan.seed = 77;
    plan.pads.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 1.0f, 0.0f, 1.0f, 0.0f});
    plan.pads.push_back({{2.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 1.0f, 2.0f, 1.2f, 0.5f});
    plan.roots.push_back({{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.06f, 0.04f, 0.0f, 2.0f});
    const std::vector<MeshData> pieces = BuildNestSkin(plan);
    REQUIRE_FALSE(pieces.empty());
    float nearest = 1.0e9f;
    float farthest = 0.0f;
    float highest = 0.0f;
    for (const MeshData& piece : pieces)
    {
        REQUIRE(piece.TriangleCount() > 0);
        for (const MeshVertex& vertex : piece.vertices)
        {
            // Lying on the floor: not far above it.
            highest = std::max(highest, vertex.position.y);
            nearest = std::min(nearest, vertex.uv.x);
            farthest = std::max(farthest, vertex.uv.x);
            CHECK(vertex.uv.y >= 0.0f);
        }
    }
    CHECK(highest < 0.5f);
    // How far along from the heart, which the shader grows it by: from none, out to the far patch.
    CHECK(nearest < 0.3f);
    CHECK(farthest > 1.7f);
    CHECK(farthest < 3.0f);
}
