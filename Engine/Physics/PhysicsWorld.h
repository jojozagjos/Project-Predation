#pragma once

#include "Engine/Core/Math.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace pred
{

class DebugDraw;
struct MeshData;

enum class BodyMotion : uint8_t
{
    Static,    // never moves; level geometry
    Kinematic, // moved by code, pushes dynamic bodies
    Dynamic    // simulated
};

// Which broad category a body belongs to. Kept deliberately small; gameplay filtering
// (bullets ignoring the shooter, creature-only volumes) layers on top later.
enum class PhysicsLayer : uint8_t
{
    Static = 0,
    Moving = 1
};

struct BodyHandle
{
    static constexpr uint32_t kInvalid = 0xffffffffu;
    uint32_t id = kInvalid;

    bool IsValid() const { return id != kInvalid; }
    bool operator==(const BodyHandle& other) const = default;
};

struct RayHit
{
    bool hit = false;
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    BodyHandle body;

    explicit operator bool() const { return hit; }
};

// Rigid body physics, wrapping Jolt.
//
// Jolt's headers are heavy and its types would otherwise spread through the codebase, so the whole
// library lives behind a pimpl and this interface speaks glm and engine types only. Swapping the
// backend, or stubbing physics out in a headless test, stays a change to one .cpp file.
class PhysicsWorld
{
public:
    struct Settings
    {
        uint32_t maxBodies = 16384;
        uint32_t maxBodyPairs = 32768;
        uint32_t maxContactConstraints = 16384;
        glm::vec3 gravity{0.0f, -9.81f, 0.0f};
        // More collision steps means more accurate stacking at a proportional cost.
        int collisionSteps = 1;
        int workerThreads = 0; // 0 = pick from hardware concurrency
    };

    struct Stats
    {
        size_t bodyCount = 0;
        size_t activeBodyCount = 0;
        double lastStepMs = 0.0;
    };

    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool Init(const Settings& settings);
    void Shutdown();
    bool IsInitialized() const;

    void Step(float deltaSeconds);

    // --- Body creation. Density is ignored for static bodies. -----------------------------------
    BodyHandle CreateBox(const glm::vec3& halfExtents, const Transform& transform, BodyMotion motion,
                         float density = 1000.0f);
    BodyHandle CreateSphere(float radius, const Transform& transform, BodyMotion motion,
                            float density = 1000.0f);
    // Capsule total height is 2 * (halfHeight + radius). This is the player/creature body shape.
    BodyHandle CreateCapsule(float halfHeight, float radius, const Transform& transform, BodyMotion motion,
                             float density = 1000.0f);
    // Triangle mesh, static only. Used for level geometry that boxes cannot represent.
    BodyHandle CreateMeshBody(const MeshData& mesh, const Transform& transform);

    void DestroyBody(BodyHandle body);
    void DestroyAllBodies();

    // Rebuilds the broad-phase acceleration structure. Worth calling once after bulk-adding static
    // level geometry; without it the tree stays in the order bodies happened to be created.
    void OptimizeBroadPhase();

    // --- Body state -----------------------------------------------------------------------------
    bool IsValid(BodyHandle body) const;
    Transform GetTransform(BodyHandle body) const;
    void SetTransform(BodyHandle body, const Transform& transform);
    // Drives a kinematic body towards a transform over one step, giving it a real velocity so it
    // pushes what it meets instead of teleporting through it. This is how doors should move.
    void MoveKinematic(BodyHandle body, const Transform& target, float deltaSeconds);
    glm::vec3 GetLinearVelocity(BodyHandle body) const;
    void SetLinearVelocity(BodyHandle body, const glm::vec3& velocity);
    void AddImpulse(BodyHandle body, const glm::vec3& impulse);
    bool IsActive(BodyHandle body) const;

    // --- Queries --------------------------------------------------------------------------------
    // `direction` need not be normalized; `maxDistance` is measured along the normalized direction.
    RayHit RayCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const;

    // --- Debug ----------------------------------------------------------------------------------
    void DebugDraw(class DebugDraw& draw) const;
    const Stats& GetStats() const;
    void SetGravity(const glm::vec3& gravity);
    glm::vec3 GetGravity() const;

    // --- Internal ---------------------------------------------------------------------------------
    // Handles to the underlying Jolt objects, for other physics translation units such as
    // CharacterController. Typed as void* so Jolt's headers stay out of this header; nothing outside
    // Engine/Physics should touch these.
    void* NativePhysicsSystem() const;
    void* NativeTempAllocator() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace pred
