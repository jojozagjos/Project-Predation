#pragma once

#include "Engine/Physics/PhysicsWorld.h"
#include "Engine/Scene/Scene.h"
#include "Game/Creature/CreatureBrain.h"

#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

class MeshLibrary;
class NavMesh;

// The creature in the world: a body that walks where its brain sends it.
//
// The brain decides; this carries it out. It follows a route over the navigation mesh, sliding along
// the surface so it can never step off the walkable area, turns towards where it is going or what it
// is looking at, and moves a physics box with it so rounds hit it and loose objects are pushed out of
// its way. What it looks like is a placeholder built from primitives -- a hunched four-legged shape
// with a head held low and two faint points of light for eyes -- because the real anatomy is
// generated from the seed in a later phase, and a body is only worth drawing properly once it is
// decided what the body is.
//
// Only the authority runs one of these. Everybody else is shown where it is.
class Creature
{
public:
    Creature(Scene& scene, MeshLibrary& meshes, PhysicsWorld& physics, const NavMesh* nav,
             const CreatureTraits& traits, const glm::vec3& spawn);
    ~Creature();
    Creature(const Creature&) = delete;
    Creature& operator=(const Creature&) = delete;

    // One tick. `senses` carries the players, the noises and the line-of-sight question; the
    // creature fills in what it knows about itself before its brain sees it.
    void Update(CreatureSenses senses, float time, float dt);
    // Drawn where the body is. Separate from Update so a client showing somebody else's creature can
    // set the state it was sent and draw that.
    void UpdateVisual(float dt);

    // Shot, or otherwise hurt. `byPlayer` is -1 when nobody in particular.
    void TakeDamage(float amount, int byPlayer, const glm::vec3& from, float time);

    // For the game, which applies strikes and runs the inspector.
    const CreatureBrain& Brain() const { return m_brain; }
    CreatureBrain& Brain() { return m_brain; }
    const glm::vec3& Position() const { return m_position; }
    float Yaw() const { return m_yaw; }
    glm::vec3 Forward() const;
    glm::vec3 Eye() const;
    BodyHandle Body() const { return m_body; }
    float Health() const { return m_health; }
    float MaxHealth() const { return m_maxHealth; }
    bool Alive() const { return m_health > 0.0f; }
    float Speed() const { return m_speed; }

    // Set from outside, for a creature this machine only shows.
    void SetShownState(const glm::vec3& position, float yaw, float speed, float windup, bool alive);

    void Destroy();

private:
    void Move(const CreatureIntent& intent, float dt);
    void BuildVisual(MeshLibrary& meshes);
    void SyncBody(float dt);

    Scene& m_scene;
    PhysicsWorld& m_physics;
    const NavMesh* m_nav = nullptr;
    CreatureBrain m_brain;

    glm::vec3 m_position{0.0f};
    float m_yaw = 0.0f;
    float m_speed = 0.0f;
    float m_health = 160.0f;
    float m_maxHealth = 160.0f;
    float m_windup = 0.0f;
    // How far the legs have gone through their cycle, advanced by distance rather than time so a
    // creature that is not moving is not treading water.
    float m_stride = 0.0f;
    float m_deathTime = -1.0f;
    float m_time = 0.0f;

    // The route and when it was worked out. Replanned when the destination moves or on a timer,
    // not every tick: a route across the map is cheap, but not free, and it does not change much
    // between frames.
    std::vector<glm::vec3> m_route;
    glm::vec3 m_routeGoal{0.0f};
    float m_routeAge = 1.0e9f;

    BodyHandle m_body;
    struct Piece
    {
        Entity entity;
        // Where it sits on the body when the body stands still, and the joint it swings about.
        glm::vec3 offset{0.0f};
        int leg = -1; // which leg, for the walk cycle; -1 when it is not one
    };
    std::vector<Piece> m_pieces;
};

} // namespace pred
