#pragma once

#include "Game/Campaign/Universe.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace pred
{

// The system map's picture of a system: where each body is drawn and how big, and the camera it is seen through.
//
// Not to scale -- at scale the planets are specks and the inner ones are lost in the star -- but true to direction:
// distances from the star are drawn by their square root, so the inner planets have room and the outer ones are
// still further out, and a body's size is drawn larger than life but in proportion. Moons are drawn round their
// planets well clear of them. Anything placed in the system (the ship, a course) goes through Place, so it is drawn
// where it is among them.
class SystemMapView
{
public:
    // A body as it is drawn: its place and size on the map.
    struct Drawn
    {
        int index = -1;
        glm::vec3 at{0.0f};
        float radius = 0.1f;
    };

    static constexpr float kStarRadius = 1.4f;

    // Where a point of the system (astronomical units from the star) is drawn.
    static glm::vec3 Place(const glm::vec3& au);
    static float DrawnRadius(const Body& body);
    // Every body of a system where it is at `clock`.
    static std::vector<Drawn> Layout(const StarSystem& system, double clock);
    // Where the ship is drawn: on its body, if it is at one (just outside it), or where it is.
    static glm::vec3 ShipAt(const std::vector<Drawn>& drawn, int body, const glm::vec3& au);
    // The drawn orbit of a body as a loop of points, for drawing as lines.
    static std::vector<glm::vec3> Orbit(const StarSystem& system, int body, const std::vector<Drawn>& drawn, double clock, int points);

    // --- The camera: turned about a point it looks at, from a distance; eased to where it is asked to be. ---
    void Focus(const glm::vec3& at, float distance);
    void Turn(float yaw, float pitch);
    void Zoom(float factor);
    void Update(float dt);
    glm::vec3 Eye() const;
    glm::mat4 View() const;
    glm::mat4 Projection(float aspect, bool homogeneousDepth) const;
    glm::vec3 Right() const;
    glm::vec3 Up() const;
    float Distance() const { return m_distance; }
    // Which drawn body is under a point on the picture (0 to 1 across and down), or -1.
    int Pick(const glm::vec2& point, const std::vector<Drawn>& drawn, float aspect, bool homogeneousDepth) const;
    // Where a point of the map comes on the picture (0 to 1 across and down); false when it is behind the camera.
    bool OnScreen(const glm::vec3& at, float aspect, bool homogeneousDepth, glm::vec2& out) const;

private:
    glm::vec3 m_focus{0.0f};
    glm::vec3 m_focusWanted{0.0f};
    float m_distance = 60.0f;
    float m_distanceWanted = 60.0f;
    float m_yaw = 0.6f;
    float m_pitch = 0.55f;
};

} // namespace pred
