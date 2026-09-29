#include "Game/Cinematic/Cinematic.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <cmath>

using namespace pred;

// Cinematics: keys on a timeline, measured from anchors the game supplies, sampled at any moment. What is tested here is
// the sampling -- that the picture, the actors and the words are where the file says, whatever site it is played at.

namespace
{

bool Near(const glm::vec3& a, const glm::vec3& b, float within = 0.01f)
{
    return glm::distance(a, b) < within;
}

Ease Linear()
{
    Ease ease;
    ease.kind = Ease::Kind::Linear;
    return ease;
}

} // namespace

TEST_CASE("Easing starts at the key before and ends at the key after, however it goes between", "[cinematic]")
{
    for (const Ease::Kind kind : {Ease::Kind::Linear, Ease::Kind::In, Ease::Kind::Out, Ease::Kind::InOut, Ease::Kind::Curve})
    {
        Ease ease;
        ease.kind = kind;
        CHECK(std::abs(ease.Apply(0.0f)) < 1.0e-4f);
        CHECK(std::abs(ease.Apply(1.0f) - 1.0f) < 1.0e-4f);
        // Never back, and never past.
        float was = 0.0f;
        for (float t = 0.05f; t <= 1.0f; t += 0.05f)
        {
            const float now = ease.Apply(t);
            CHECK(now >= was - 1.0e-4f);
            CHECK(now <= 1.0f + 1.0e-4f);
            was = now;
        }
    }
    Ease step;
    step.kind = Ease::Kind::Step;
    CHECK(step.Apply(0.99f) == 0.0f);
    CHECK(step.Apply(1.0f) == 1.0f);
    // A curve with its handles on the diagonal is a straight line.
    Ease straight;
    straight.kind = Ease::Kind::Curve;
    straight.a = {0.25f, 0.25f};
    straight.b = {0.75f, 0.75f};
    CHECK(std::abs(straight.Apply(0.3f) - 0.3f) < 0.01f);
    CHECK(std::abs(Linear().Apply(0.5f) - 0.5f) < 1.0e-5f);
}

TEST_CASE("A turn in degrees is yaw to the right, pitch up and roll, and comes back out as it went in", "[cinematic]")
{
    CHECK(Near(TurnFromDegrees({0.0f, 0.0f, 0.0f}) * glm::vec3(0.0f, 0.0f, -1.0f), {0.0f, 0.0f, -1.0f}));
    CHECK(Near(TurnFromDegrees({0.0f, 90.0f, 0.0f}) * glm::vec3(0.0f, 0.0f, -1.0f), {1.0f, 0.0f, 0.0f}));
    CHECK(Near(TurnFromDegrees({30.0f, 0.0f, 0.0f}) * glm::vec3(0.0f, 0.0f, -1.0f), {0.0f, 0.5f, -0.866f}));
    for (const glm::vec3 degrees : {glm::vec3(10.0f, 45.0f, 0.0f), glm::vec3(-35.0f, -120.0f, 12.0f), glm::vec3(60.0f, 170.0f, -30.0f)})
    {
        INFO(degrees.x << " " << degrees.y << " " << degrees.z);
        const glm::vec3 back = DegreesFromTurn(TurnFromDegrees(degrees));
        CHECK(Near(back, degrees, 0.05f));
    }
}

TEST_CASE("A camera measured from an anchor is where the anchor puts it, and a shot blends from the one before", "[cinematic]")
{
    Cinematic cinematic;
    CameraTrack a;
    a.name = "a";
    a.anchor = "pad";
    a.keys.push_back({0.0f, {0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 0.0f}, 40.0f, 0.0f, Linear()});
    CameraTrack b;
    b.name = "b";
    b.keys.push_back({0.0f, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 80.0f, 0.0f, Linear()});
    cinematic.cameras = {a, b};
    cinematic.shots.push_back({0.0f, "a", 0.0f, Linear()});
    cinematic.shots.push_back({5.0f, "b", 2.0f, Linear()});
    CinematicBindings bindings;
    // The pad ten metres along x, turned a quarter to the right: five metres ahead of it is fifteen along x.
    bindings.anchors["pad"] = {{10.0f, 0.0f, 0.0f}, TurnFromDegrees({0.0f, 90.0f, 0.0f})};
    const CinematicSampler sampler(cinematic, bindings);

    std::string shot;
    float blend = 0.0f;
    const CameraState first = sampler.Picture(1.0f, &shot, &blend);
    CHECK(shot == "a");
    CHECK(Near(first.position, {15.0f, 0.0f, 0.0f}));
    CHECK(first.fov == 40.0f);
    // Halfway through the blend to b.
    const CameraState between = sampler.Picture(6.0f, &shot, &blend);
    CHECK(shot == "b");
    CHECK(std::abs(blend - 0.5f) < 1.0e-4f);
    CHECK(Near(between.position, {7.5f, 0.0f, 0.0f}));
    CHECK(std::abs(between.fov - 60.0f) < 1.0e-3f);
    const CameraState after = sampler.Picture(9.0f, &shot, &blend);
    CHECK(Near(after.position, {0.0f, 0.0f, 0.0f}));
    CHECK(blend == 1.0f);
}

TEST_CASE("An actor goes along the path the game supplies, facing where it goes, and waits at its ends", "[cinematic]")
{
    Cinematic cinematic;
    cinematic.actors.push_back({"crawler", "snow_crawler", ""});
    PathFollow follow;
    follow.actor = "crawler";
    follow.path = "route";
    follow.start = 2.0f;
    follow.end = 6.0f;
    follow.ease = Linear();
    cinematic.paths.push_back(follow);
    CinematicBindings bindings;
    bindings.paths["route"] = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 10.0f}};
    const CinematicSampler sampler(cinematic, bindings);

    CHECK(Near(sampler.Actor("crawler", 0.0f).position, {0.0f, 0.0f, 0.0f}));
    // Halfway along twenty metres is the corner.
    CHECK(Near(sampler.Actor("crawler", 4.0f).position, {10.0f, 0.0f, 0.0f}));
    const CinePose going = sampler.Actor("crawler", 3.0f);
    CHECK(Near(going.position, {5.0f, 0.0f, 0.0f}));
    CHECK(Near(going.rotation * glm::vec3(0.0f, 0.0f, -1.0f), {1.0f, 0.0f, 0.0f}));
    CHECK(Near(sampler.Actor("crawler", 9.0f).position, {10.0f, 0.0f, 10.0f}));

    // And a camera riding on it, and one watching it.
    CameraTrack riding;
    riding.name = "riding";
    riding.anchor = "crawler";
    riding.keys.push_back({0.0f, {0.0f, 2.0f, 1.0f}, {0.0f, 0.0f, 0.0f}, 60.0f, 0.0f, Linear()});
    CameraTrack watching;
    watching.name = "watching";
    watching.lookAt = "crawler";
    watching.keys.push_back({0.0f, {5.0f, 0.0f, -10.0f}, {0.0f, 0.0f, 0.0f}, 60.0f, 0.0f, Linear()});
    cinematic.cameras = {riding, watching};
    const CinematicSampler again(cinematic, bindings);
    // Going along +x, a metre behind it is a metre towards -x.
    CHECK(Near(again.Camera(cinematic.cameras[0], 3.0f).position, {4.0f, 2.0f, 0.0f}));
    const CameraState watched = again.Camera(cinematic.cameras[1], 3.0f);
    CHECK(Near(watched.rotation * glm::vec3(0.0f, 0.0f, -1.0f), {0.0f, 0.0f, 1.0f}));
}

TEST_CASE("A title card's words are filled in by the game and typed out a letter at a time", "[cinematic]")
{
    Cinematic cinematic;
    TextItem card;
    card.time = 1.0f;
    card.typeSpeed = 10.0f;
    card.lines = {"{site}", "{planet} -- {nothing}"};
    cinematic.texts.push_back(card);
    CinematicBindings bindings;
    bindings.words["site"] = "ALPHA";
    bindings.words["planet"] = "K-9";
    const CinematicSampler sampler(cinematic, bindings);
    CHECK(sampler.Text(card, 0, 0.5f).empty());
    CHECK(sampler.Text(card, 0, 1.3f) == "ALP");
    CHECK(sampler.Text(card, 0, 5.0f) == "ALPHA");
    // The second line starts once the first is done and a pause has passed: five letters and six of pause.
    CHECK(sampler.Text(card, 1, 1.0f + 1.0f).empty());
    CHECK(sampler.Text(card, 1, 1.0f + 1.4f) == "K-9");
    // A word the game does not know stays as it is written.
    CHECK(sampler.Text(card, 1, 30.0f) == "K-9 -- {nothing}");
}

TEST_CASE("What happens as time passes is found once, in order", "[cinematic]")
{
    Cinematic cinematic;
    cinematic.markers = {{3.0f, "c", ""}, {1.0f, "a", ""}, {2.0f, "b", ""}};
    cinematic.sounds = {{2.5f, "World/breaker", 1.0f, "", false}};
    cinematic.Tidy();
    const std::vector<CinematicHappening> passed = HappeningsBetween(cinematic, 1.0f, 3.0f);
    REQUIRE(passed.size() == 3);
    CHECK(passed[0].time == 2.0f);
    CHECK(passed[1].kind == CinematicHappening::Kind::Sound);
    CHECK(passed[2].time == 3.0f);
    // From before the start, what is at the start.
    CHECK(HappeningsBetween(cinematic, -1.0f, 1.0f).size() == 1);
    CHECK(cinematic.duration >= 3.0f);
}

TEST_CASE("A cinematic written to its file and read back is the same cinematic", "[cinematic]")
{
    Cinematic cinematic;
    cinematic.name = "test";
    cinematic.duration = 12.0f;
    cinematic.blendOut = 1.5f;
    cinematic.actors.push_back({"shuttle", "shuttle", ""});
    cinematic.actors.push_back({"crawler", "", "site_crawler"});
    CameraTrack camera;
    camera.name = "wide";
    camera.anchor = "pad";
    camera.lookAt = "shuttle";
    camera.lookOffset = {0.0f, 1.0f, 0.0f};
    Ease curve;
    curve.kind = Ease::Kind::Curve;
    curve.a = {0.1f, 0.6f};
    curve.b = {0.3f, 1.0f};
    camera.keys.push_back({0.0f, {1.0f, 2.0f, 3.0f}, {5.0f, 10.0f, 0.0f}, 45.0f, 0.0f, curve});
    camera.keys.push_back({4.0f, {4.0f, 5.0f, 6.0f}, {0.0f, 20.0f, 3.0f}, 55.0f, 12.0f, Linear()});
    cinematic.cameras.push_back(camera);
    cinematic.shots.push_back({0.0f, "wide", 0.0f, Linear()});
    cinematic.actorTracks.push_back({"shuttle", "pad", {{0.0f, {0.0f, 50.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, Linear()}}});
    cinematic.paths.push_back({"crawler", "route", 2.0f, 9.0f, curve, 0.1f, true});
    cinematic.clips.push_back({8.0f, "crawler", "doors_open", 1.0f});
    cinematic.sounds.push_back({1.0f, "World/shuttle_launch", 0.8f, "shuttle", false});
    cinematic.markers.push_back({11.0f, "control", ""});
    cinematic.texts.push_back({6.0f, 5.0f, "title_card", {"{site}", "{planet}"}, 20.0f});
    cinematic.lights.push_back({"shuttle_lamp", {{0.0f, 0.0f, Linear()}, {2.0f, 1.0f, Linear()}}});
    cinematic.particles.push_back({3.0f, "exhaust", "shuttle", {0.0f, -1.0f, 0.0f}, 2.0f});
    cinematic.shake = {{0.0f, 0.0f, Linear()}, {1.0f, 2.0f, Linear()}};
    cinematic.fade = {{0.0f, 1.0f, Linear()}, {1.0f, 0.0f, Linear()}};
    cinematic.letterbox = {{0.0f, 1.0f, Linear()}};

    Cinematic back;
    std::string error;
    REQUIRE(back.FromJsonText(cinematic.ToJsonText(), &error));
    CHECK(back.name == "test");
    CHECK(back.blendOut == 1.5f);
    REQUIRE(back.actors.size() == 2);
    CHECK(back.actors[1].bind == "site_crawler");
    REQUIRE(back.cameras.size() == 1);
    CHECK(back.cameras[0].lookAt == "shuttle");
    REQUIRE(back.cameras[0].keys.size() == 2);
    CHECK(back.cameras[0].keys[0].ease.kind == Ease::Kind::Curve);
    CHECK(back.cameras[0].keys[0].ease.a.y == 0.6f);
    CHECK(back.cameras[0].keys[1].focus == 12.0f);
    CHECK(back.paths[0].lift == 0.1f);
    CHECK(back.clips[0].clip == "doors_open");
    CHECK(back.sounds[0].at == "shuttle");
    CHECK(back.markers[0].name == "control");
    CHECK(back.texts[0].lines.size() == 2);
    CHECK(back.lights[0].intensity.size() == 2);
    CHECK(back.particles[0].effect == "exhaust");
    CHECK(back.shake.size() == 2);
    CHECK(back.fade.size() == 2);
    CHECK(back.letterbox.size() == 1);
    // And it samples the same.
    CinematicBindings bindings;
    bindings.anchors["pad"] = {{3.0f, 0.0f, 1.0f}, TurnFromDegrees({0.0f, 30.0f, 0.0f})};
    const CinematicSampler a(cinematic, bindings);
    const CinematicSampler b(back, bindings);
    for (float t = 0.0f; t < 12.0f; t += 0.7f)
    {
        CHECK(Near(a.Picture(t).position, b.Picture(t).position));
        CHECK(std::abs(a.Fade(t) - b.Fade(t)) < 1.0e-4f);
    }
}

TEST_CASE("An actor that drives a route and then turns on the spot waits at the start, drives, and turns", "[cinematic]")
{
    Cinematic cinematic;
    cinematic.actors.push_back({"crawler", "snow_crawler", ""});
    PathFollow follow;
    follow.actor = "crawler";
    follow.path = "route";
    follow.start = 2.0f;
    follow.end = 6.0f;
    follow.ease = Linear();
    cinematic.paths.push_back(follow);
    // At the end of the route, facing along it (+x), it turns round on the spot to face -x.
    ActorTrack turn;
    turn.actor = "crawler";
    turn.anchor = "park";
    turn.keys.push_back({6.0f, {0.0f, 0.0f, 0.0f}, {0.0f, 180.0f, 0.0f}, Linear()});
    turn.keys.push_back({8.0f, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, Linear()});
    cinematic.actorTracks.push_back(turn);
    CinematicBindings bindings;
    bindings.paths["route"] = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}};
    // The park faces -x: turned a quarter to the left.
    bindings.anchors["park"] = {{10.0f, 0.0f, 0.0f}, TurnFromDegrees({0.0f, -90.0f, 0.0f})};
    const CinematicSampler sampler(cinematic, bindings);
    CHECK(Near(sampler.Actor("crawler", 0.0f).position, {0.0f, 0.0f, 0.0f}));
    CHECK(Near(sampler.Actor("crawler", 4.0f).position, {5.0f, 0.0f, 0.0f}));
    const glm::vec3 arriving = sampler.Actor("crawler", 5.99f).rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 turning = sampler.Actor("crawler", 6.01f).rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    CHECK(Near(arriving, {1.0f, 0.0f, 0.0f}, 0.02f));
    CHECK(Near(turning, {1.0f, 0.0f, 0.0f}, 0.05f));
    CHECK(Near(sampler.Actor("crawler", 9.0f).position, {10.0f, 0.0f, 0.0f}));
    CHECK(Near(sampler.Actor("crawler", 9.0f).rotation * glm::vec3(0.0f, 0.0f, -1.0f), {-1.0f, 0.0f, 0.0f}, 0.02f));
}

TEST_CASE("A saved cinematic reads as it was laid out, with its numbers as short as they can be", "[cinematic]")
{
    // The editor saves over files people have laid out and edited by hand: saving must not reorder them or turn every
    // number into a float's own digits, or every edit would be a diff of the whole file.
    Cinematic cinematic;
    cinematic.name = "layout";
    cinematic.duration = 12.0f;
    CameraTrack camera;
    camera.name = "wide";
    camera.keys.push_back({0.8f, {0.1f, 6.0f, -2.35f}, {0.0f, 0.0f, 0.0f}, 60.0f, 0.0f, Linear()});
    cinematic.cameras.push_back(camera);
    const std::string text = cinematic.ToJsonText();
    CHECK(text.find("\"name\"") < text.find("\"duration\""));
    CHECK(text.find("\"duration\"") < text.find("\"cameras\""));
    CHECK(text.find("\"t\": 0.8,") != std::string::npos);
    CHECK(text.find("-2.35") != std::string::npos);
    CHECK(text.find("\"duration\": 12,") != std::string::npos);
    CHECK(text.find("0000000") == std::string::npos);
    // And it still reads back as it was.
    Cinematic back;
    REQUIRE(back.FromJsonText(text));
    REQUIRE(back.cameras.size() == 1);
    CHECK(std::abs(back.cameras[0].keys[0].time - 0.8f) < 1.0e-6f);
    CHECK(std::abs(back.cameras[0].keys[0].position.z + 2.35f) < 1.0e-6f);
}

TEST_CASE("Something backing along a path faces the way it came from", "[cinematic]")
{
    Cinematic cinematic;
    cinematic.duration = 10.0f;
    cinematic.actors.push_back({"crawler", "", ""});
    PathFollow follow;
    follow.actor = "crawler";
    follow.path = "in";
    follow.start = 0.0f;
    follow.end = 10.0f;
    follow.ease.kind = Ease::Kind::Linear;
    follow.reverse = true;
    cinematic.paths.push_back(follow);
    CinematicBindings bindings;
    bindings.paths["in"] = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -20.0f}}; // moving towards -z
    const CinematicSampler sampler(cinematic, bindings);
    const CinePose pose = sampler.Actor("crawler", 5.0f);
    CHECK(Near(pose.position, {0.0f, 0.0f, -10.0f}));
    // Its front (-z of it) is towards +z: where it came from.
    CHECK((pose.rotation * glm::vec3(0.0f, 0.0f, -1.0f)).z > 0.99f);
    // And the flag is kept in the file.
    Cinematic back;
    REQUIRE(back.FromJsonText(cinematic.ToJsonText()));
    CHECK(back.paths[0].reverse);
}
