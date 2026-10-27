#include "Game/World/Lifts.h"

#include "Engine/Render/Material.h"
#include "Engine/Render/Mesh.h"
#include "Engine/Render/Primitives.h"
#include "Game/Interaction/InteractionSystem.h"

#include <glm/common.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pred
{

namespace
{

constexpr float kHalfShaft = FacilityLayout::kCell * 0.5f - FacilitySpec::kHalfWall; // the shaft inside its walls, half across
constexpr float kCarHalf = FacilitySpec::kLiftCarWidth * 0.5f;
// The car's front: up to a few centimetres short of the shaft's wall, so stepping in is stepping over a hand's width.
constexpr float kCarFront = kHalfShaft - 0.03f;
constexpr float kCarTop = FacilitySpec::kLiftCarHeight;
constexpr float kWall = 0.05f;
constexpr float kHalfDoor = FacilitySpec::kLiftDoorWidth * 0.5f;
// Each stop's doors: two leaves in the middle of the wall, sliding apart into it.
constexpr float kLeaf = kHalfDoor + 0.02f;
constexpr float kLeafThick = 0.05f;
constexpr float kDoorLine = -FacilityLayout::kCell * 0.5f;

const Material kCarWallMaterial = Material::Metal({0.5f, 0.51f, 0.52f}, 0.75f); // brushed: no mirror for its lamp
const Material kCarFloorMaterial = Material::Metal({0.2f, 0.21f, 0.22f}, 0.55f);
const Material kLeafMaterial = Material::Metal({0.55f, 0.57f, 0.58f}, 0.6f);
const Material kRailMaterial = Material::Metal({0.75f, 0.76f, 0.77f}, 0.4f);
const Material kPanelMaterial = Material::Diffuse({0.08f, 0.08f, 0.09f}, 0.5f);
const Material kLightMaterial = Material::Emissive({1.0f, 0.97f, 0.9f}, 2.5f);
const Material kButtonMaterial = Material::Emissive({1.0f, 0.75f, 0.35f}, 1.2f);

std::string BoxName(const glm::vec3& size)
{
    char name[64];
    std::snprintf(name, sizeof(name), "lift_box_%.3f_%.3f_%.3f", size.x, size.y, size.z);
    return name;
}

} // namespace

namespace LiftRules
{

Pressed Press(LiftState& state, int stop, bool powered)
{
    const bool asked = std::find(state.queue.begin(), state.queue.end(), stop) != state.queue.end();
    if (state.phase == LiftState::Phase::Moving)
    {
        if (!powered || stop == state.to || asked)
        {
            return Pressed::Nothing;
        }
        state.queue.push_back(stop);
        return Pressed::Queued;
    }
    // Where it is: its doors open, power or none, so that nobody is ever shut in it (or out of it).
    if (stop == state.at)
    {
        if (state.phase == LiftState::Phase::Shut || state.phase == LiftState::Phase::Closing)
        {
            state.phase = LiftState::Phase::Opening;
        }
        state.hold = kHoldSeconds;
        return Pressed::Opened;
    }
    if (!powered || asked)
    {
        return Pressed::Nothing;
    }
    state.queue.push_back(stop);
    // Stood open: it shuts soon, and goes.
    if (state.phase == LiftState::Phase::Open)
    {
        state.hold = std::min(state.hold, 1.0f);
    }
    return Pressed::Queued;
}

float TripSeconds(const FacilityMap::Lift& lift, int from, int to)
{
    const float rise = std::abs(lift.stops[static_cast<size_t>(to)] - lift.stops[static_cast<size_t>(from)]);
    // Eased in and out (CarHeight): half again as long as at its fastest the whole way, and a moment to start and stop.
    return 0.6f + 1.5f * rise / kSpeed;
}

float CarHeight(const FacilityMap::Lift& lift, const LiftState& state)
{
    const float from = lift.stops[static_cast<size_t>(state.at)];
    if (state.phase != LiftState::Phase::Moving)
    {
        return from;
    }
    const float to = lift.stops[static_cast<size_t>(state.to)];
    const float s = std::clamp(state.travelled / TripSeconds(lift, state.at, state.to), 0.0f, 1.0f);
    return from + (to - from) * s * s * (3.0f - 2.0f * s);
}

Ticked Tick(LiftState& state, const FacilityMap::Lift& lift, float dt, bool authority, bool powered, bool blocked)
{
    Ticked ticked;
    switch (state.phase)
    {
    case LiftState::Phase::Opening:
        state.doors = std::min(state.doors + dt / kDoorSeconds, 1.0f);
        if (state.doors >= 1.0f)
        {
            state.phase = LiftState::Phase::Open;
            state.hold = std::max(state.hold, kHoldSeconds * (state.queue.empty() ? 1.0f : 0.4f));
        }
        break;
    case LiftState::Phase::Open:
        state.hold -= dt;
        if (state.hold <= 0.0f && !blocked)
        {
            state.phase = LiftState::Phase::Closing;
        }
        break;
    case LiftState::Phase::Closing:
        if (blocked)
        {
            state.phase = LiftState::Phase::Opening;
            state.hold = kHoldSeconds * 0.5f;
            break;
        }
        state.doors = std::max(state.doors - dt / kDoorSeconds, 0.0f);
        if (state.doors <= 0.0f)
        {
            state.phase = LiftState::Phase::Shut;
        }
        break;
    case LiftState::Phase::Shut:
        // Off to the next stop asked for -- by the host alone, which tells everybody (Depart).
        while (authority && powered && !state.queue.empty())
        {
            const int next = state.queue.front();
            state.queue.erase(state.queue.begin());
            if (next == state.at)
            {
                OpenAt(state, next);
                break;
            }
            if (next >= 0 && next < static_cast<int>(lift.stops.size()))
            {
                Depart(state, state.at, next);
                ticked.departed = true;
                break;
            }
        }
        break;
    case LiftState::Phase::Moving:
        state.travelled += dt;
        if (state.travelled >= TripSeconds(lift, state.at, state.to))
        {
            state.at = state.to;
            state.travelled = 0.0f;
            state.phase = LiftState::Phase::Opening;
            state.hold = kHoldSeconds;
            ticked.arrived = true;
        }
        break;
    }
    return ticked;
}

void Depart(LiftState& state, int from, int to)
{
    state.at = from;
    state.to = to;
    state.phase = LiftState::Phase::Moving;
    state.travelled = 0.0f;
    state.doors = 0.0f;
}

void OpenAt(LiftState& state, int stop)
{
    state.at = stop;
    if (state.phase != LiftState::Phase::Open)
    {
        state.phase = LiftState::Phase::Opening;
    }
    state.hold = kHoldSeconds;
}

void RestAt(LiftState& state, int stop)
{
    state = LiftState{};
    state.at = stop;
}

} // namespace LiftRules

glm::vec3 Lifts::At(const Built& lift, float height, const glm::vec3& local) const
{
    return glm::vec3(lift.def.centre.x, height, lift.def.centre.z) + lift.turn * local;
}

void Lifts::Build(const std::vector<FacilityMap::Lift>& lifts, Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics,
                  InteractionSystem& interactions)
{
    Clear(scene, physics, interactions);
    m_group = physics.NewOverlapGroup();
    for (size_t l = 0; l < lifts.size(); ++l)
    {
        const FacilityMap::Lift& def = lifts[l];
        if (def.stops.size() < 2)
        {
            continue;
        }
        Built lift;
        lift.def = def;
        lift.turn = glm::angleAxis(std::atan2(-def.out.x, -def.out.z), glm::vec3(0.0f, 1.0f, 0.0f));
        const float bottom = def.stops.front();
        const auto box = [&](const glm::vec3& lo, const glm::vec3& hi, const Material& material, const char* name, float height)
        {
            const glm::vec3 size = hi - lo;
            Transform transform;
            transform.position = At(lift, height, (lo + hi) * 0.5f);
            transform.rotation = lift.turn;
            const Entity entity = scene.CreateMeshEntity(name, transform, meshes.Upload(Primitives::Box(size), BoxName(size)), material);
            return Part{entity, (lo + hi) * 0.5f};
        };
        const auto solid = [&](const glm::vec3& lo, const glm::vec3& hi)
        {
            Transform transform;
            transform.position = At(lift, bottom, (lo + hi) * 0.5f);
            transform.rotation = lift.turn;
            const BodyHandle body = physics.CreateBox((hi - lo) * 0.5f, transform, BodyMotion::Kinematic);
            physics.SetOverlapGroup(body, m_group);
            lift.carBodies.push_back(body);
            lift.carBodyLocal.push_back((lo + hi) * 0.5f);
        };

        // The car: a chequer-plate floor, steel walls with a handrail round the back, a lit panel in its ceiling, and its front
        // either side of the doorway and over it.
        const glm::vec3 floorLo{-kCarHalf, -FacilitySpec::kLiftCarFloor, -kCarFront};
        const glm::vec3 floorHi{kCarHalf, 0.0f, kCarHalf};
        lift.car.push_back(box(floorLo, floorHi, kCarFloorMaterial, "lift_car_floor", bottom));
        solid(floorLo, floorHi);
        const glm::vec3 backLo{-kCarHalf, 0.0f, kCarHalf - kWall};
        const glm::vec3 backHi{kCarHalf, kCarTop, kCarHalf};
        lift.car.push_back(box(backLo, backHi, kCarWallMaterial, "lift_car_wall", bottom));
        solid(backLo, backHi);
        for (const float side : {-1.0f, 1.0f})
        {
            const glm::vec3 lo{side < 0.0f ? -kCarHalf : kCarHalf - kWall, 0.0f, -kCarFront};
            const glm::vec3 hi{side < 0.0f ? -kCarHalf + kWall : kCarHalf, kCarTop, kCarHalf - kWall};
            lift.car.push_back(box(lo, hi, kCarWallMaterial, "lift_car_wall", bottom));
            solid(lo, hi);
            const glm::vec3 frontLo{side < 0.0f ? -kCarHalf + kWall : kHalfDoor, 0.0f, -kCarFront};
            const glm::vec3 frontHi{side < 0.0f ? -kHalfDoor : kCarHalf - kWall, kCarTop, -kCarFront + kWall};
            lift.car.push_back(box(frontLo, frontHi, kCarWallMaterial, "lift_car_front", bottom));
            solid(frontLo, frontHi);
            // A handrail along it, a couple of centimetres off it.
            const float railIn = side * (kCarHalf - kWall - 0.02f);
            lift.car.push_back(box({std::min(railIn, railIn - side * 0.04f), 0.9f, -0.9f}, {std::max(railIn, railIn - side * 0.04f), 0.95f, 0.9f},
                                   kRailMaterial, "lift_car_rail", bottom));
        }
        lift.car.push_back(box({-kHalfDoor, FacilitySpec::kLiftDoorHeight, -kCarFront}, {kHalfDoor, kCarTop, -kCarFront + kWall}, kCarWallMaterial,
                               "lift_car_front", bottom));
        lift.car.push_back(box({-kCarHalf, kCarTop, -kCarFront}, {kCarHalf, kCarTop + 0.06f, kCarHalf}, kCarWallMaterial, "lift_car_roof", bottom));
        lift.car.push_back(box({-0.55f, kCarTop - 0.02f, -0.6f}, {0.55f, kCarTop, 0.4f}, kLightMaterial, "lift_car_light", bottom));
        lift.car.push_back(box({-0.9f, 0.9f, kCarHalf - kWall - 0.06f}, {0.9f, 0.95f, kCarHalf - kWall - 0.02f}, kRailMaterial, "lift_car_rail", bottom));
        // Its buttons, on a panel on the wall by the doorway: one for each stop, the lowest at the bottom, and under them the one
        // that opens its doors.
        const float panelX = kCarHalf - kWall;
        const float panelZ = -kCarFront + kWall + 0.2f;
        const size_t stops = def.stops.size();
        const float panelTop = 1.0f + 0.13f * static_cast<float>(stops + 1);
        lift.car.push_back(box({panelX - 0.015f, 0.95f, panelZ - 0.11f}, {panelX, panelTop, panelZ + 0.11f}, kPanelMaterial, "lift_car_panel", bottom));
        for (size_t s = 0; s <= stops; ++s)
        {
            const bool open = s == stops;
            const float y = open ? 1.05f : 1.05f + 0.13f * static_cast<float>(s + 1);
            Part button = box({panelX - 0.03f, y - 0.035f, panelZ - 0.035f}, {panelX - 0.015f, y + 0.035f, panelZ + 0.035f},
                              open ? kRailMaterial : kButtonMaterial, "lift_button", bottom);
            Interactable interactable;
            interactable.entity = button.entity;
            interactable.kind = InteractionKind::Lift;
            interactable.verb = open ? "Open" : "Go to";
            interactable.name = open ? "the doors" : StopNameOf(def, static_cast<int>(s));
            interactable.range = 1.8f;
            interactable.payload = Payload(m_lifts.size(), open ? kOpenButton : static_cast<int>(s));
            interactions.Register(interactable);
            lift.buttons.push_back(button);
        }

        // At every stop, its doors, solid while shut, and the button that calls the car there.
        for (size_t s = 0; s < stops; ++s)
        {
            const float floor = def.stops[s];
            Part left = box({-kLeaf, 0.005f, kDoorLine - kLeafThick * 0.5f}, {0.0f, FacilitySpec::kLiftDoorHeight - 0.005f, kDoorLine + kLeafThick * 0.5f},
                            kLeafMaterial, "lift_door", floor);
            Part right = box({0.0f, 0.005f, kDoorLine - kLeafThick * 0.5f}, {kLeaf, FacilitySpec::kLiftDoorHeight - 0.005f, kDoorLine + kLeafThick * 0.5f},
                             kLeafMaterial, "lift_door", floor);
            lift.doors.emplace_back(left, right);
            lift.shut.push_back(BodyHandle{});
            Transform call;
            call.position = At(lift, floor, {kHalfDoor + 0.31f, 1.18f, kDoorLine - FacilitySpec::kHalfWall - 0.035f});
            call.rotation = lift.turn;
            const glm::vec3 buttonSize{0.07f, 0.07f, 0.02f};
            const Entity entity = scene.CreateMeshEntity("lift_call", call, meshes.Upload(Primitives::Box(buttonSize), BoxName(buttonSize)), kButtonMaterial);
            Interactable interactable;
            interactable.entity = entity;
            interactable.kind = InteractionKind::Lift;
            interactable.verb = "Call";
            interactable.name = "the lift";
            interactable.range = 2.0f;
            interactable.payload = Payload(m_lifts.size(), static_cast<int>(s));
            interactions.Register(interactable);
            lift.calls.push_back(entity);
        }
        m_lifts.push_back(std::move(lift));
    }
    Show(scene, physics);
}

void Lifts::Clear(Scene& scene, PhysicsWorld& physics, InteractionSystem& interactions)
{
    for (Built& lift : m_lifts)
    {
        for (const Part& part : lift.car)
        {
            scene.Destroy(part.entity);
        }
        for (const Part& part : lift.buttons)
        {
            interactions.Unregister(part.entity);
            scene.Destroy(part.entity);
        }
        for (const auto& [left, right] : lift.doors)
        {
            scene.Destroy(left.entity);
            scene.Destroy(right.entity);
        }
        for (const Entity call : lift.calls)
        {
            interactions.Unregister(call);
            scene.Destroy(call);
        }
        for (const BodyHandle body : lift.carBodies)
        {
            physics.DestroyBody(body);
        }
        for (const BodyHandle body : lift.shut)
        {
            if (body.IsValid())
            {
                physics.DestroyBody(body);
            }
        }
    }
    m_lifts.clear();
}

void Lifts::Show(Scene& scene, PhysicsWorld& physics)
{
    for (Built& lift : m_lifts)
    {
        const float height = LiftRules::CarHeight(lift.def, lift.state);
        const auto place = [&](const Part& part, float at, const glm::vec3& shift)
        {
            if (Transform* transform = scene.GetTransform(part.entity))
            {
                transform->position = At(lift, at, part.local + shift);
                transform->rotation = lift.turn;
            }
        };
        for (const Part& part : lift.car)
        {
            place(part, height, glm::vec3(0.0f));
        }
        for (const Part& part : lift.buttons)
        {
            place(part, height, glm::vec3(0.0f));
        }
        for (size_t b = 0; b < lift.carBodies.size(); ++b)
        {
            Transform transform;
            transform.position = At(lift, height, lift.carBodyLocal[b]);
            transform.rotation = lift.turn;
            physics.SetTransform(lift.carBodies[b], transform);
        }
        for (size_t s = 0; s < lift.doors.size(); ++s)
        {
            const bool here = static_cast<int>(s) == lift.state.at && lift.state.phase != LiftState::Phase::Moving;
            const float open = here ? lift.state.doors : 0.0f;
            // Eased, into the wall either side.
            const float slide = (kLeaf - 0.04f) * open * open * (3.0f - 2.0f * open);
            const float floor = lift.def.stops[s];
            place(lift.doors[s].first, floor, {-slide, 0.0f, 0.0f});
            place(lift.doors[s].second, floor, {slide, 0.0f, 0.0f});
            // Solid only shut: open or opening, the doorway is the way into the car.
            const bool solid = open <= 0.0f;
            BodyHandle& body = lift.shut[s];
            if (solid && !body.IsValid())
            {
                Transform transform;
                transform.position = At(lift, floor, {0.0f, FacilitySpec::kLiftDoorHeight * 0.5f, kDoorLine});
                transform.rotation = lift.turn;
                body = physics.CreateBox({kHalfDoor - 0.005f, FacilitySpec::kLiftDoorHeight * 0.5f - 0.005f, kLeafThick * 0.5f}, transform, BodyMotion::Static);
                physics.SetOverlapGroup(body, m_group);
            }
            else if (!solid && body.IsValid())
            {
                physics.DestroyBody(body);
                body = BodyHandle{};
            }
        }
    }
}

glm::vec3 Lifts::CarFloor(size_t lift) const
{
    const Built& built = m_lifts[lift];
    return At(built, LiftRules::CarHeight(built.def, built.state), glm::vec3(0.0f));
}

bool Lifts::InCar(size_t lift, const glm::vec3& feet) const
{
    const Built& built = m_lifts[lift];
    const glm::vec3 floor = CarFloor(lift);
    const glm::vec3 local = glm::inverse(built.turn) * (feet - floor);
    return std::abs(local.x) < kCarHalf && local.z > -kCarFront - 0.05f && local.z < kCarHalf && local.y > -0.4f && local.y < 1.2f;
}

bool Lifts::InDoorway(size_t lift, int stop, const glm::vec3& feet) const
{
    const Built& built = m_lifts[lift];
    const glm::vec3 local = glm::inverse(built.turn) * (feet - At(built, built.def.stops[static_cast<size_t>(stop)], glm::vec3(0.0f)));
    // Across the doorway: its body over the line its doors shut on, from the landing or from the car.
    return std::abs(local.x) < kHalfDoor + 0.2f && local.z > kDoorLine - 0.5f && local.z < kDoorLine + 0.35f && std::abs(local.y) < 1.0f;
}

glm::vec3 Lifts::Doorway(size_t lift, int stop) const
{
    const Built& built = m_lifts[lift];
    return At(built, built.def.stops[static_cast<size_t>(stop)], {0.0f, 0.0f, kDoorLine});
}

std::string Lifts::StopNameOf(const FacilityMap::Lift& def, int stop)
{
    const int floor = def.lowest + stop - def.ground;
    return floor == 0 ? "the ground floor" : floor < 0 ? "basement " + std::to_string(-floor) : "floor " + std::to_string(floor);
}

std::string Lifts::StopName(size_t lift, int stop) const
{
    return StopNameOf(m_lifts[lift].def, stop);
}

} // namespace pred
