#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace pred
{

// How a creature is built. Made from its seed, like its temperament, but from a random stream of its
// own: a seed keeps the temperament it already had and gains a body.
//
// Assembled from parametric parts -- a segmented body, a neck, a head with a jaw, eyes, pairs of
// legs, a tail, plates and spines along the back -- rather than synthesised as a mesh, which is what
// the plan asks for first: every one of these is something its capabilities can be worked out from,
// and something the animation in the next phase can move.
//
// Every part stands on the ground or hangs from the body. Nothing sprouts from the back and nothing
// spreads to fly: there are no wings, by design, in any seed.
enum class BodyPlan : uint8_t
{
    Quadruped, // four legs under a horizontal body
    Hexapod,   // six legs splayed out wide, knees high, low to the ground
    Biped,     // two legs under the middle, the body balanced over them by a long tail
    Crawler,   // a gaunt, human-like thing on all fours: long arms in front, legs behind, no tail
    Count
};

const char* BodyPlanName(BodyPlan plan);

// Within a plan, how the body is built: what makes two four-legged things different animals rather
// than one animal at two sizes.
enum class BodyBuild : uint8_t
{
    Ordinary,
    Gaunt,   // starved thin, every bone showing, limbs like sticks
    Heavy,   // deep-chested and thick-limbed, low and broad
    Stilted, // tall on long thin legs, a small body carried high
    Low,     // long and flat, belly near the floor, short bowed legs
    Hunched, // shoulders far above the hips, like something that walks on its knuckles
    Long,    // a long body, serpentine, on legs that seem too few for it
    Count
};

// And its head.
enum class HeadShape : uint8_t
{
    Ordinary,
    Skull,  // a person's skull, flat-faced
    Snout,  // a long narrow muzzle full of teeth
    Dome,   // a smooth swollen dome, eyeless or nearly
    Hammer, // wide, with the eyes out at the ends
    Maw,    // mostly mouth: a deep jaw that hangs open
    Count
};

const char* BodyBuildName(BodyBuild build);
const char* HeadShapeName(HeadShape shape);

// Markings on the skin.
enum class SkinPattern : uint8_t
{
    Mottled,
    Spotted,
    Striped,
    Count
};

// One pair of legs, left and right alike.
struct LegPair
{
    // Where along the body the hips are, from 0 at the shoulders to 1 at the rump.
    float along = 0.0f;
    // How far out from the middle of the body each hip is, and each foot rests.
    float hipSpread = 0.2f;
    float footSpread = 0.25f;
    // Hip to knee, knee to ankle, ankle to toe, in metres.
    float upper = 0.4f;
    float lower = 0.45f;
    float foot = 0.12f;
    float thickness = 0.07f;
    // Knee bending backwards, as a dog's hind leg does; otherwise forwards, as a person's does.
    bool backwardKnee = false;
    // How far ahead of the hip the foot rests: a crawler reaches its hands out in front of it.
    float footForward = 0.0f;
    // Arms rather than legs: a crawler's front pair, which bend at the elbow and end in long fingers.
    bool arm = false;
};

struct CreatureAnatomy
{
    uint32_t seed = 0;
    BodyPlan plan = BodyPlan::Quadruped;

    // The body, from the shoulders back to the rump, not counting neck, head or tail.
    float length = 1.4f;
    float width = 0.5f;
    float depth = 0.45f;
    // How high the underside of the body is off the ground: what the legs have to reach.
    float hipHeight = 0.7f;
    // How much higher the shoulders sit than the hips.
    float hunch = 0.1f;
    // How many segments the body is drawn in, and how much it narrows towards the rump (0 to 0.5).
    int segments = 4;
    float taper = 0.2f;

    float neckLength = 0.3f;
    float neckThickness = 0.14f;
    float headLength = 0.45f;
    float headWidth = 0.3f;
    float headDepth = 0.26f;
    float jawLength = 0.35f;

    // Eyes: how many (0, 2, 4 or 6), how big, and the colour they glow. None at all is a creature
    // that hunts by sound.
    int eyes = 2;
    float eyeSize = 0.035f;
    glm::vec3 eyeGlow{2.4f, 0.5f, 0.35f};
    // Frills either side of the head that catch sound, 0 for none to 1 for large.
    float frills = 0.0f;

    float tailLength = 0.8f;
    int tailSegments = 4;
    float tailThickness = 0.08f;

    // Armour plates along the back, and spines along the ridge. Plates stop some of every round.
    int plates = 0;
    int spines = 0;
    float spineLength = 0.15f;

    std::vector<LegPair> legs;

    glm::vec3 skin{0.40f, 0.42f, 0.38f};
    glm::vec3 underside{0.26f, 0.27f, 0.25f};
    float roughness = 0.72f;

    // The face and the details that make each one itself. Every kind of body draws these, so two of the
    // same kind can still look nothing alike.
    float snout = 1.0f;       // how far the face juts: 0.6 flat, like a person's; 1.8 a long muzzle
    float gape = 0.4f;        // how far the jaw hangs open at rest: 0 shut, 1 unhinged
    int teeth = 10;           // along each jaw
    float toothLength = 1.0f; // on how long teeth are for a head that size
    float ribs = 0.5f;        // how plainly the ribs and spine show: 0 well fed, 1 starved
    int fingers = 4;          // on each hand or foot
    float clawLength = 1.0f;
    float brow = 0.5f;        // how heavy the ridge over the eyes is
    float cranium = 1.0f;     // how swollen the back of the skull is

    BodyBuild build = BodyBuild::Ordinary;
    HeadShape headShape = HeadShape::Ordinary;
    SkinPattern pattern = SkinPattern::Mottled;
    // Growths on the skin: lumps, tumours, blisters -- 0 none, 1 covered.
    float growths = 0.0f;

    static CreatureAnatomy FromSeed(uint32_t seed);
    std::string Describe() const;

    // Where everything is when it stands still, in its own frame: forward is -Z, up is +Y, and the
    // origin is the ground under the middle of the body. Worked out once, here, so the drawing, the box
    // rounds hit and where it sees from cannot disagree about where its head is.
    struct Leg
    {
        glm::vec3 hip{0.0f};
        glm::vec3 foot{0.0f}; // where the foot rests on the ground
        const LegPair* pair = nullptr;
        float side = 1.0f; // +1 its right, -1 its left
    };
    struct RestPose
    {
        glm::vec3 shoulders{0.0f}; // front of the body, on its middle line
        glm::vec3 rump{0.0f};      // back of it
        std::vector<glm::vec3> spine; // the middle of each body segment, front to back
        std::vector<float> spineRadius; // half the width of each segment
        glm::vec3 neckBase{0.0f};
        glm::vec3 head{0.0f};
        glm::vec3 jaw{0.0f};
        std::vector<glm::vec3> eyes;
        std::vector<Leg> legs;
        glm::vec3 tailBase{0.0f};
        glm::vec3 tailDirection{0.0f, 0.0f, 1.0f};
    };
    RestPose Rest() const;

    // How high a hip is, at a place `along` the body (0 shoulders, 1 rump). One formula for the legs
    // and the pose, which once each had their own and front legs came out too short to stand.
    float HipY(float along) const;

    // Height of the highest point of the body at rest, and the full length from nose to tail tip.
    float TopHeight() const;
    float OverallLength() const;
};

// A size of body, for what it can fit through. The navigation mesh is built once for each.
enum class SizeClass : uint8_t
{
    Small,  // under a metre tall and narrow: fits vents and crawlspaces
    Medium, // about the size of a large dog or a crouching person
    Large,  // too big for a narrow gap
    Count
};

const char* SizeClassName(SizeClass size);

// What a body can do, worked out from the body rather than drawn separately: speed from the length of
// its legs and its weight, health and armour from its bulk and its plates, what it sees from its eyes,
// what it hears from its frills -- and an eyeless one hears the best of all. The same seed is the same
// animal on every machine and in every test.
struct CreatureCapabilities
{
    SizeClass size = SizeClass::Medium;
    float mass = 150.0f;          // kilograms
    float health = 160.0f;
    float armour = 0.0f;          // fraction of each round the plates stop, 0 to 0.3
    float runSpeed = 5.0f;        // metres a second
    float walkSpeed = 1.6f;
    float sight = 1.0f;           // on its sight range; 0 when it has no eyes
    float hearing = 1.0f;         // on how far it hears
    float strikeDamage = 28.0f;
    float strikeReach = 2.3f;     // from its feet to where a blow lands
    bool fitsVents = false;       // for the vents of Milestone 11
    bool climbs = false;          // likewise, for walls and ceilings
    // How high it can get up onto something, jumping or climbing, and how high above its feet it can
    // strike, rearing up. Somebody on a crate is safe from one that can do neither, for a while.
    float jump = 1.0f;
    float verticalReach = 1.5f;

    // The box rounds hit, in the creature's own frame (forward -Z), from the ground to its back.
    glm::vec3 bodyHalfExtents{0.3f, 0.5f, 0.85f};
    glm::vec3 bodyCentre{0.0f, 0.62f, -0.1f};
    // Where it sees from, at rest.
    glm::vec3 eye{0.0f, 1.0f, -1.08f};

    static CreatureCapabilities From(const CreatureAnatomy& anatomy);
    std::string Describe() const;
};

} // namespace pred
