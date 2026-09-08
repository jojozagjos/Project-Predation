#include "Engine/Physics/CharacterController.h"

#include "Engine/Core/Log.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsWorld.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

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
    return glm::vec3(static_cast<float>(v.GetX()), static_cast<float>(v.GetY()),
                     static_cast<float>(v.GetZ()));
}

// Builds a capsule whose bottom sits at the origin, so the character's position is its feet rather
// than its middle. Everything downstream (eye height, ground checks, spawn points) is easier to
// reason about in those terms.
JPH::RefConst<JPH::Shape> MakeStandingShape(float height, float radius)
{
    const float clampedRadius = std::max(0.01f, radius);
    // A capsule's cylindrical section cannot be negative, so very short shapes become spheres.
    const float halfCylinder = std::max(0.0f, height * 0.5f - clampedRadius);

    const JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(halfCylinder, clampedRadius);
    const JPH::ShapeSettings::ShapeResult result =
        JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.0f, height * 0.5f, 0.0f), JPH::Quat::sIdentity(),
                                            capsule)
            .Create();
    if (result.HasError())
    {
        PRED_LOG_ERROR(Physics, "Character shape creation failed: {}", result.GetError().c_str());
        return {};
    }
    return result.Get();
}

} // namespace

struct CharacterController::Impl
{
    Settings settings;
    float height = 1.8f;
    float radius = 0.32f;

    JPH::PhysicsSystem* system = nullptr;
    JPH::TempAllocator* tempAllocator = nullptr;
    JPH::Ref<JPH::CharacterVirtual> character;
};

CharacterController::CharacterController() : m_impl(std::make_unique<Impl>()) {}

CharacterController::~CharacterController()
{
    Destroy();
}

bool CharacterController::Create(PhysicsWorld& world, const Settings& settings,
                                 const glm::vec3& footPosition)
{
    Destroy();
    Impl& impl = *m_impl;

    impl.system = static_cast<JPH::PhysicsSystem*>(world.NativePhysicsSystem());
    impl.tempAllocator = static_cast<JPH::TempAllocator*>(world.NativeTempAllocator());
    if (impl.system == nullptr || impl.tempAllocator == nullptr)
    {
        PRED_LOG_ERROR(Physics, "Cannot create a character before the physics world is initialized");
        return false;
    }

    impl.settings = settings;
    impl.height = settings.height;
    impl.radius = settings.radius;

    const JPH::RefConst<JPH::Shape> shape = MakeStandingShape(settings.height, settings.radius);
    if (shape == nullptr)
    {
        return false;
    }

    JPH::CharacterVirtualSettings characterSettings;
    characterSettings.mShape = shape;
    characterSettings.mMass = settings.mass;
    characterSettings.mMaxSlopeAngle = glm::radians(settings.maxSlopeAngleDegrees);
    characterSettings.mCharacterPadding = settings.padding;
    characterSettings.mPenetrationRecoverySpeed = settings.penetrationRecoverySpeed;
    characterSettings.mPredictiveContactDistance = settings.predictiveContactDistance;
    // Contacts below this plane can hold the character up. Restricting it to the bottom of the
    // capsule stops the character treating a wall brushed by its side as walkable ground.
    characterSettings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -settings.radius);

    impl.character = new JPH::CharacterVirtual(&characterSettings, ToJoltR(footPosition),
                                               JPH::Quat::sIdentity(), 0, impl.system);
    if (impl.character == nullptr)
    {
        PRED_LOG_ERROR(Physics, "CharacterVirtual creation failed");
        return false;
    }

    PRED_LOG_INFO(Physics, "Character created: {:.2f} m tall, {:.2f} m radius, max slope {:.0f} deg",
                  settings.height, settings.radius, settings.maxSlopeAngleDegrees);
    return true;
}

void CharacterController::Destroy()
{
    Impl& impl = *m_impl;
    impl.character = nullptr;
    impl.system = nullptr;
    impl.tempAllocator = nullptr;
}

bool CharacterController::IsValid() const
{
    return m_impl->character != nullptr;
}

void CharacterController::Update(float deltaSeconds, const glm::vec3& gravity)
{
    Impl& impl = *m_impl;
    if (impl.character == nullptr || deltaSeconds <= 0.0f)
    {
        return;
    }

    JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
    // Searching downwards keeps the character glued to the floor when walking down slopes and
    // stairs instead of launching off every edge.
    updateSettings.mStickToFloorStepDown = JPH::Vec3(0.0f, -impl.settings.stickToFloorDistance, 0.0f);
    updateSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, impl.settings.stepHeight, 0.0f);

    const JPH::ObjectLayer movingLayer = PhysicsLayers::kMoving;
    impl.character->ExtendedUpdate(deltaSeconds, ToJolt(gravity), updateSettings,
                                   impl.system->GetDefaultBroadPhaseLayerFilter(movingLayer),
                                   impl.system->GetDefaultLayerFilter(movingLayer), {}, {},
                                   *impl.tempAllocator);
}

glm::vec3 CharacterController::GetPosition() const
{
    return m_impl->character != nullptr ? FromJoltR(m_impl->character->GetPosition()) : glm::vec3(0.0f);
}

void CharacterController::SetPosition(const glm::vec3& footPosition)
{
    if (m_impl->character != nullptr)
    {
        m_impl->character->SetPosition(ToJoltR(footPosition));
    }
}

glm::vec3 CharacterController::GetLinearVelocity() const
{
    return m_impl->character != nullptr ? FromJolt(m_impl->character->GetLinearVelocity()) : glm::vec3(0.0f);
}

void CharacterController::SetLinearVelocity(const glm::vec3& velocity)
{
    if (m_impl->character != nullptr)
    {
        m_impl->character->SetLinearVelocity(ToJolt(velocity));
    }
}

GroundState CharacterController::GetGroundState() const
{
    if (m_impl->character == nullptr)
    {
        return GroundState::InAir;
    }
    switch (m_impl->character->GetGroundState())
    {
    case JPH::CharacterBase::EGroundState::OnGround:
        return GroundState::Grounded;
    case JPH::CharacterBase::EGroundState::OnSteepGround:
        return GroundState::SteepGround;
    case JPH::CharacterBase::EGroundState::NotSupported:
    case JPH::CharacterBase::EGroundState::InAir:
    default:
        return GroundState::InAir;
    }
}

glm::vec3 CharacterController::GetGroundNormal() const
{
    return m_impl->character != nullptr ? FromJolt(m_impl->character->GetGroundNormal())
                                        : glm::vec3(0.0f, 1.0f, 0.0f);
}

glm::vec3 CharacterController::GetGroundVelocity() const
{
    return m_impl->character != nullptr ? FromJolt(m_impl->character->GetGroundVelocity()) : glm::vec3(0.0f);
}

bool CharacterController::TryResize(float height, float radius)
{
    Impl& impl = *m_impl;
    if (impl.character == nullptr)
    {
        return false;
    }
    if (std::abs(height - impl.height) < 1e-4f && std::abs(radius - impl.radius) < 1e-4f)
    {
        return true; // already this size
    }

    const JPH::RefConst<JPH::Shape> shape = MakeStandingShape(height, radius);
    if (shape == nullptr)
    {
        return false;
    }

    const JPH::ObjectLayer movingLayer = PhysicsLayers::kMoving;
    // A zero penetration allowance means the resize is rejected outright if the new shape would
    // overlap anything, which is exactly the "cannot stand up under this ceiling" test.
    const bool resized = impl.character->SetShape(shape, 0.0f,
                                                  impl.system->GetDefaultBroadPhaseLayerFilter(movingLayer),
                                                  impl.system->GetDefaultLayerFilter(movingLayer), {}, {},
                                                  *impl.tempAllocator);
    if (resized)
    {
        impl.height = height;
        impl.radius = radius;
    }
    return resized;
}

float CharacterController::GetHeight() const
{
    return m_impl->height;
}

float CharacterController::GetRadius() const
{
    return m_impl->radius;
}

const CharacterController::Settings& CharacterController::GetSettings() const
{
    return m_impl->settings;
}

} // namespace pred
