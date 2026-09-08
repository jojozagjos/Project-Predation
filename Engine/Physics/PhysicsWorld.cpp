#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Core/CVar.h"
#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Render/DebugDraw.h"
#include "Engine/Render/Mesh.h"

// Jolt.h must come before every other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <unordered_map>

namespace pred
{
namespace
{

// --- Collision layers -----------------------------------------------------------------------
// Two narrow-phase layers and two broad-phase layers is the minimum useful setup: static geometry
// never tests against other static geometry, which is most of the win.
namespace Layers
{
constexpr JPH::ObjectLayer kNonMoving = PhysicsLayers::kNonMoving;
constexpr JPH::ObjectLayer kMoving = PhysicsLayers::kMoving;
constexpr JPH::ObjectLayer kCount = PhysicsLayers::kCount;
} // namespace Layers

namespace BroadPhaseLayers
{
constexpr JPH::BroadPhaseLayer kNonMoving(PhysicsLayers::kBroadPhaseNonMoving);
constexpr JPH::BroadPhaseLayer kMoving(PhysicsLayers::kBroadPhaseMoving);
constexpr JPH::uint kCount(PhysicsLayers::kBroadPhaseCount);
} // namespace BroadPhaseLayers

class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    BroadPhaseLayerInterfaceImpl()
    {
        m_objectToBroadPhase[Layers::kNonMoving] = BroadPhaseLayers::kNonMoving;
        m_objectToBroadPhase[Layers::kMoving] = BroadPhaseLayers::kMoving;
    }

    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kCount; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        JPH_ASSERT(layer < Layers::kCount);
        return m_objectToBroadPhase[layer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        switch (static_cast<JPH::BroadPhaseLayer::Type>(layer))
        {
        case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::kNonMoving):
            return "NON_MOVING";
        case static_cast<JPH::BroadPhaseLayer::Type>(BroadPhaseLayers::kMoving):
            return "MOVING";
        default:
            return "INVALID";
        }
    }
#endif

private:
    JPH::BroadPhaseLayer m_objectToBroadPhase[Layers::kCount];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer1, JPH::BroadPhaseLayer layer2) const override
    {
        // Static things only need to be tested against moving things.
        return layer1 != Layers::kNonMoving || layer2 == BroadPhaseLayers::kMoving;
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer1, JPH::ObjectLayer layer2) const override
    {
        return layer1 != Layers::kNonMoving || layer2 == Layers::kMoving;
    }
};

// --- Conversions ------------------------------------------------------------------------------
JPH::Vec3 ToJolt(const glm::vec3& v)
{
    return JPH::Vec3(v.x, v.y, v.z);
}

JPH::RVec3 ToJoltR(const glm::vec3& v)
{
    return JPH::RVec3(v.x, v.y, v.z);
}

glm::vec3 FromJolt(JPH::Vec3Arg v)
{
    return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
}

glm::vec3 FromJoltR(JPH::RVec3Arg v)
{
    return glm::vec3(static_cast<float>(v.GetX()), static_cast<float>(v.GetY()), static_cast<float>(v.GetZ()));
}

JPH::Quat ToJolt(const glm::quat& q)
{
    return JPH::Quat(q.x, q.y, q.z, q.w);
}

glm::quat FromJolt(JPH::QuatArg q)
{
    return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); // glm takes (w, x, y, z)
}

JPH::EMotionType ToJoltMotion(BodyMotion motion)
{
    switch (motion)
    {
    case BodyMotion::Static:
        return JPH::EMotionType::Static;
    case BodyMotion::Kinematic:
        return JPH::EMotionType::Kinematic;
    case BodyMotion::Dynamic:
    default:
        return JPH::EMotionType::Dynamic;
    }
}

JPH::ObjectLayer ToJoltLayer(BodyMotion motion)
{
    return motion == BodyMotion::Static ? Layers::kNonMoving : Layers::kMoving;
}

// --- Jolt diagnostics into our log --------------------------------------------------------------
void JoltTrace(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    PRED_LOG_DEBUG(Physics, "jolt: {}", buffer);
}

CVar<bool> cv_breakOnPhysicsAssert{"physics.break_on_assert", false,
                                   "Break into the debugger when Jolt reports a failed assertion"};

#ifdef JPH_ENABLE_ASSERTS
bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
{
    PRED_LOG_ERROR(Physics, "jolt assert: {}:{}: ({}) {}", file != nullptr ? file : "?", line, expression,
                   message != nullptr ? message : "");
    // Returning true triggers a breakpoint, which with no debugger attached terminates the process.
    // A failed physics assertion is a bug worth shouting about, but it must never be able to close
    // the game from a keypress, so the default is to log loudly and carry on.
    return cv_breakOnPhysicsAssert.Get();
}
#endif

// Jolt's factory and type registry are process-global, so guard them with a reference count.
int g_joltRefCount = 0;

void AcquireJoltGlobals()
{
    if (g_joltRefCount++ > 0)
    {
        return;
    }
    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void ReleaseJoltGlobals()
{
    if (--g_joltRefCount > 0)
    {
        return;
    }
    g_joltRefCount = 0;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

} // namespace

// -----------------------------------------------------------------------------------------------

struct PhysicsWorld::Impl
{
    // Shape metadata kept alongside Jolt's body, so debug drawing does not have to interrogate
    // Jolt's shape hierarchy every frame.
    enum class ShapeKind : uint8_t
    {
        Box,
        Sphere,
        Capsule,
        Mesh
    };

    struct BodyRecord
    {
        ShapeKind kind = ShapeKind::Box;
        BodyMotion motion = BodyMotion::Static;
        glm::vec3 halfExtents{0.5f};
        float radius = 0.5f;
        float halfHeight = 0.5f;
        AABB meshBounds;
    };

    bool initialized = false;
    Settings settings;

    std::unique_ptr<JPH::PhysicsSystem> system;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;

    BroadPhaseLayerInterfaceImpl broadPhaseLayers;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseFilter;
    ObjectLayerPairFilterImpl objectLayerPairFilter;

    std::unordered_map<uint32_t, BodyRecord> records;
    Stats stats;

    JPH::BodyInterface& Bodies() { return system->GetBodyInterface(); }
    const JPH::BodyInterface& Bodies() const { return system->GetBodyInterface(); }

    BodyHandle AddBody(const JPH::ShapeRefC& shape, const Transform& transform, BodyMotion motion,
                       const BodyRecord& record);
};

PhysicsWorld::PhysicsWorld() : m_impl(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld()
{
    Shutdown();
}

bool PhysicsWorld::Init(const Settings& settings)
{
    Shutdown();
    Impl& impl = *m_impl;
    impl.settings = settings;

    AcquireJoltGlobals();

    // 32 MB scratch space; Jolt allocates its per-step working set from here.
    impl.tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(32 * 1024 * 1024);

    int workers = settings.workerThreads;
    if (workers <= 0)
    {
        const unsigned hardware = std::thread::hardware_concurrency();
        // Leave one core for the main thread; Jolt counts workers in addition to the calling thread.
        workers = static_cast<int>(hardware > 2 ? hardware - 1 : 1);
    }
    workers = std::clamp(workers, 1, 16);
    impl.jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                                               workers);

    impl.system = std::make_unique<JPH::PhysicsSystem>();
    impl.system->Init(settings.maxBodies, 0, settings.maxBodyPairs, settings.maxContactConstraints,
                      impl.broadPhaseLayers, impl.objectVsBroadPhaseFilter, impl.objectLayerPairFilter);
    impl.system->SetGravity(ToJolt(settings.gravity));

    impl.initialized = true;
    PRED_LOG_INFO(Physics, "Physics initialized: {} worker threads, max {} bodies, gravity {:.2f} m/s^2",
                  workers, settings.maxBodies, settings.gravity.y);
    return true;
}

void PhysicsWorld::Shutdown()
{
    Impl& impl = *m_impl;
    if (!impl.initialized)
    {
        return;
    }
    DestroyAllBodies();
    impl.system.reset();
    impl.jobSystem.reset();
    impl.tempAllocator.reset();
    impl.initialized = false;
    ReleaseJoltGlobals();
    PRED_LOG_INFO(Physics, "Physics shut down");
}

bool PhysicsWorld::IsInitialized() const
{
    return m_impl->initialized;
}

void PhysicsWorld::Step(float deltaSeconds)
{
    Impl& impl = *m_impl;
    if (!impl.initialized || deltaSeconds <= 0.0f)
    {
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    const JPH::EPhysicsUpdateError error = impl.system->Update(
        deltaSeconds, impl.settings.collisionSteps, impl.tempAllocator.get(), impl.jobSystem.get());
    const auto end = std::chrono::steady_clock::now();

    if (error != JPH::EPhysicsUpdateError::None)
    {
        // These mean a buffer was too small, so the step was only partially simulated.
        PRED_LOG_WARN(Physics, "Physics update reported error flags {}", static_cast<int>(error));
    }

    impl.stats.lastStepMs = std::chrono::duration<double, std::milli>(end - start).count();
    impl.stats.bodyCount = impl.records.size();
    impl.stats.activeBodyCount = impl.system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
}

BodyHandle PhysicsWorld::Impl::AddBody(const JPH::ShapeRefC& shape, const Transform& transform,
                                       BodyMotion motion, const BodyRecord& record)
{
    JPH::BodyCreationSettings creation(shape, ToJoltR(transform.position), ToJolt(transform.rotation),
                                       ToJoltMotion(motion), ToJoltLayer(motion));
    // Sensible defaults for level props; specific bodies can be tuned later.
    creation.mFriction = 0.6f;
    creation.mRestitution = 0.05f;
    creation.mLinearDamping = 0.05f;
    creation.mAngularDamping = 0.1f;

    const JPH::BodyID id = Bodies().CreateAndAddBody(
        creation, motion == BodyMotion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid())
    {
        PRED_LOG_ERROR(Physics, "Failed to create physics body (limit {} reached?)", settings.maxBodies);
        return BodyHandle{};
    }

    records[id.GetIndexAndSequenceNumber()] = record;
    stats.bodyCount = records.size();
    return BodyHandle{id.GetIndexAndSequenceNumber()};
}

BodyHandle PhysicsWorld::CreateBox(const glm::vec3& halfExtents, const Transform& transform, BodyMotion motion,
                                   float density)
{
    Impl& impl = *m_impl;
    if (!impl.initialized)
    {
        return BodyHandle{};
    }

    const glm::vec3 safeExtents = glm::max(halfExtents, glm::vec3(0.001f));
    JPH::BoxShapeSettings shapeSettings(ToJolt(safeExtents));
    shapeSettings.SetEmbedded();
    // The convex radius rounds the box corners and must fit inside it, or Create() fails on thin shapes.
    const float smallestExtent = std::min({safeExtents.x, safeExtents.y, safeExtents.z});
    shapeSettings.mConvexRadius = std::min(0.05f, smallestExtent * 0.5f);
    shapeSettings.SetDensity(density);

    const JPH::ShapeSettings::ShapeResult result = shapeSettings.Create();
    if (result.HasError())
    {
        PRED_LOG_ERROR(Physics, "Box shape creation failed: {}", result.GetError().c_str());
        return BodyHandle{};
    }

    Impl::BodyRecord record;
    record.kind = Impl::ShapeKind::Box;
    record.motion = motion;
    record.halfExtents = safeExtents;
    return impl.AddBody(result.Get(), transform, motion, record);
}

BodyHandle PhysicsWorld::CreateSphere(float radius, const Transform& transform, BodyMotion motion,
                                      float density)
{
    Impl& impl = *m_impl;
    if (!impl.initialized)
    {
        return BodyHandle{};
    }

    const float safeRadius = std::max(radius, 0.001f);
    JPH::SphereShapeSettings shapeSettings(safeRadius);
    shapeSettings.SetEmbedded();
    shapeSettings.SetDensity(density);

    const JPH::ShapeSettings::ShapeResult result = shapeSettings.Create();
    if (result.HasError())
    {
        PRED_LOG_ERROR(Physics, "Sphere shape creation failed: {}", result.GetError().c_str());
        return BodyHandle{};
    }

    Impl::BodyRecord record;
    record.kind = Impl::ShapeKind::Sphere;
    record.motion = motion;
    record.radius = safeRadius;
    return impl.AddBody(result.Get(), transform, motion, record);
}

BodyHandle PhysicsWorld::CreateCapsule(float halfHeight, float radius, const Transform& transform,
                                       BodyMotion motion, float density)
{
    Impl& impl = *m_impl;
    if (!impl.initialized)
    {
        return BodyHandle{};
    }

    const float safeRadius = std::max(radius, 0.001f);
    const float safeHalfHeight = std::max(halfHeight, 0.001f);
    JPH::CapsuleShapeSettings shapeSettings(safeHalfHeight, safeRadius);
    shapeSettings.SetEmbedded();
    shapeSettings.SetDensity(density);

    const JPH::ShapeSettings::ShapeResult result = shapeSettings.Create();
    if (result.HasError())
    {
        PRED_LOG_ERROR(Physics, "Capsule shape creation failed: {}", result.GetError().c_str());
        return BodyHandle{};
    }

    Impl::BodyRecord record;
    record.kind = Impl::ShapeKind::Capsule;
    record.motion = motion;
    record.radius = safeRadius;
    record.halfHeight = safeHalfHeight;
    return impl.AddBody(result.Get(), transform, motion, record);
}

BodyHandle PhysicsWorld::CreateMeshBody(const MeshData& mesh, const Transform& transform)
{
    Impl& impl = *m_impl;
    if (!impl.initialized)
    {
        return BodyHandle{};
    }
    if (mesh.vertices.empty() || mesh.indices.size() < 3)
    {
        PRED_LOG_ERROR(Physics, "Cannot build a mesh body from empty geometry");
        return BodyHandle{};
    }

    JPH::VertexList vertices;
    vertices.reserve(mesh.vertices.size());
    for (const MeshVertex& vertex : mesh.vertices)
    {
        vertices.push_back(JPH::Float3(vertex.position.x, vertex.position.y, vertex.position.z));
    }

    JPH::IndexedTriangleList triangles;
    triangles.reserve(mesh.indices.size() / 3);
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        triangles.push_back(JPH::IndexedTriangle(mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2], 0));
    }

    JPH::MeshShapeSettings shapeSettings(std::move(vertices), std::move(triangles));
    shapeSettings.SetEmbedded();

    const JPH::ShapeSettings::ShapeResult result = shapeSettings.Create();
    if (result.HasError())
    {
        PRED_LOG_ERROR(Physics, "Mesh shape creation failed: {}", result.GetError().c_str());
        return BodyHandle{};
    }

    Impl::BodyRecord record;
    record.kind = Impl::ShapeKind::Mesh;
    record.motion = BodyMotion::Static;
    record.meshBounds = mesh.ComputeBounds();
    // Triangle meshes have no volume, so they can only ever be static.
    return impl.AddBody(result.Get(), transform, BodyMotion::Static, record);
}

void PhysicsWorld::DestroyBody(BodyHandle body)
{
    Impl& impl = *m_impl;
    if (!impl.initialized || !body.IsValid())
    {
        return;
    }
    const JPH::BodyID id(body.id);
    impl.Bodies().RemoveBody(id);
    impl.Bodies().DestroyBody(id);
    impl.records.erase(body.id);
    impl.stats.bodyCount = impl.records.size();
}

void PhysicsWorld::DestroyAllBodies()
{
    Impl& impl = *m_impl;
    if (!impl.initialized)
    {
        return;
    }
    for (const auto& [rawId, record] : impl.records)
    {
        const JPH::BodyID id(rawId);
        impl.Bodies().RemoveBody(id);
        impl.Bodies().DestroyBody(id);
    }
    impl.records.clear();
    impl.stats.bodyCount = 0;
}

void PhysicsWorld::OptimizeBroadPhase()
{
    if (m_impl->initialized)
    {
        m_impl->system->OptimizeBroadPhase();
    }
}

bool PhysicsWorld::IsValid(BodyHandle body) const
{
    return m_impl->initialized && body.IsValid() && m_impl->records.contains(body.id);
}

Transform PhysicsWorld::GetTransform(BodyHandle body) const
{
    Transform transform;
    if (!IsValid(body))
    {
        return transform;
    }
    const JPH::BodyID id(body.id);
    JPH::RVec3 position;
    JPH::Quat rotation;
    m_impl->Bodies().GetPositionAndRotation(id, position, rotation);
    transform.position = FromJoltR(position);
    transform.rotation = FromJolt(rotation);
    return transform;
}

void PhysicsWorld::SetTransform(BodyHandle body, const Transform& transform)
{
    if (!IsValid(body))
    {
        return;
    }
    const JPH::BodyID id(body.id);
    m_impl->Bodies().SetPositionAndRotation(id, ToJoltR(transform.position), ToJolt(transform.rotation),
                                            JPH::EActivation::Activate);
}

void PhysicsWorld::MoveKinematic(BodyHandle body, const Transform& target, float deltaSeconds)
{
    if (!IsValid(body) || deltaSeconds <= 0.0f)
    {
        return;
    }
    m_impl->Bodies().MoveKinematic(JPH::BodyID(body.id), ToJoltR(target.position),
                                   ToJolt(target.rotation), deltaSeconds);
}

glm::vec3 PhysicsWorld::GetLinearVelocity(BodyHandle body) const
{
    if (!IsValid(body))
    {
        return glm::vec3(0.0f);
    }
    return FromJolt(m_impl->Bodies().GetLinearVelocity(JPH::BodyID(body.id)));
}

void PhysicsWorld::SetLinearVelocity(BodyHandle body, const glm::vec3& velocity)
{
    if (IsValid(body))
    {
        m_impl->Bodies().SetLinearVelocity(JPH::BodyID(body.id), ToJolt(velocity));
    }
}

void PhysicsWorld::AddImpulse(BodyHandle body, const glm::vec3& impulse)
{
    if (IsValid(body))
    {
        m_impl->Bodies().AddImpulse(JPH::BodyID(body.id), ToJolt(impulse));
    }
}

bool PhysicsWorld::IsActive(BodyHandle body) const
{
    return IsValid(body) && m_impl->Bodies().IsActive(JPH::BodyID(body.id));
}

RayHit PhysicsWorld::RayCast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const
{
    RayHit hit;
    const Impl& impl = *m_impl;
    if (!impl.initialized || maxDistance <= 0.0f)
    {
        return hit;
    }

    const float length = glm::length(direction);
    if (length < 1e-6f)
    {
        return hit;
    }
    const glm::vec3 normalized = direction / length;

    const JPH::RRayCast ray(ToJoltR(origin), ToJolt(normalized * maxDistance));
    JPH::RayCastResult result;
    if (!impl.system->GetNarrowPhaseQuery().CastRay(ray, result))
    {
        return hit;
    }

    hit.hit = true;
    hit.distance = result.mFraction * maxDistance;
    hit.position = origin + normalized * hit.distance;
    hit.body = BodyHandle{result.mBodyID.GetIndexAndSequenceNumber()};

    // The surface normal needs the body itself, which requires a read lock.
    const JPH::BodyLockInterfaceLocking& lockInterface = impl.system->GetBodyLockInterface();
    const JPH::BodyLockRead lock(lockInterface, result.mBodyID);
    if (lock.Succeeded())
    {
        hit.normal = FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ToJoltR(hit.position)));
    }
    return hit;
}

void PhysicsWorld::DebugDraw(class DebugDraw& draw) const
{
    const Impl& impl = *m_impl;
    if (!impl.initialized)
    {
        return;
    }

    for (const auto& [rawId, record] : impl.records)
    {
        const BodyHandle handle{rawId};
        const Transform transform = GetTransform(handle);
        const bool active = IsActive(handle);
        const uint32_t color = record.motion == BodyMotion::Static ? Color::kGreen
                               : active                            ? Color::kYellow
                                                                   : Color::kGrey;

        switch (record.kind)
        {
        case Impl::ShapeKind::Box:
            draw.BoxOriented(transform.Matrix(), record.halfExtents, color);
            break;
        case Impl::ShapeKind::Sphere:
            draw.Sphere(transform.position, record.radius, color);
            break;
        case Impl::ShapeKind::Capsule:
        {
            const glm::vec3 up = glm::vec3(transform.Matrix()[1]) * record.halfHeight;
            draw.Sphere(transform.position + up, record.radius, color, 12);
            draw.Sphere(transform.position - up, record.radius, color, 12);
            draw.Line(transform.position + up, transform.position - up, color);
            break;
        }
        case Impl::ShapeKind::Mesh:
            if (record.meshBounds.IsValid())
            {
                draw.BoxOriented(transform.Matrix(), record.meshBounds.HalfExtents(), color);
            }
            break;
        }
    }
}

const PhysicsWorld::Stats& PhysicsWorld::GetStats() const
{
    return m_impl->stats;
}

void PhysicsWorld::SetGravity(const glm::vec3& gravity)
{
    m_impl->settings.gravity = gravity;
    if (m_impl->initialized)
    {
        m_impl->system->SetGravity(ToJolt(gravity));
    }
}

glm::vec3 PhysicsWorld::GetGravity() const
{
    return m_impl->initialized ? FromJolt(m_impl->system->GetGravity()) : m_impl->settings.gravity;
}

void* PhysicsWorld::NativePhysicsSystem() const
{
    return m_impl->initialized ? m_impl->system.get() : nullptr;
}

void* PhysicsWorld::NativeTempAllocator() const
{
    return m_impl->initialized ? m_impl->tempAllocator.get() : nullptr;
}

} // namespace pred
