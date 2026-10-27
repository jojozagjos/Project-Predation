#include "Game/World/FacilityOutside.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pred
{

namespace FacilityOutside
{

namespace
{

using Kind = FacilityMap::Piece::Kind;
using Form = FacilityMap::Dressing::Form;
using Windows = FacilityLayout::Outside::Windows;
constexpr float kCell = FacilityLayout::kCell;
constexpr float kStorey = FacilityLayout::kStorey;

// How far each kind of thing stands out from the cladding, all different, so where two cross no two faces are in the same
// place (which flickers); and how far each reaches back into it, so none stands off it with a line of nothing behind.
constexpr float kInto = 0.012f;
constexpr float kGlassOut = 0.01f;
constexpr float kStripeProud = 0.035f;
constexpr float kBandProud = 0.05f;
constexpr float kFrameProud = 0.07f;
constexpr float kJambProud = 0.09f;
constexpr float kRibProud = 0.1f;
constexpr float kCornerProud = 0.14f;
// A canopy over a way out: its underside, how thick it is, how far out and how wide.
constexpr float kCanopyUnder = 2.95f;
constexpr float kCanopyThick = 0.12f;
constexpr float kCanopyOut = 1.4f;
constexpr float kCanopyWidth = 2.6f;
// The frame round a way out: its sides' width, and its head over the doorway.
constexpr float kJamb = 0.15f;

// SplitMix64, as the buildings are planned with: the same everywhere from the same seed.
class Random
{
public:
    explicit Random(uint64_t seed) : m_state(seed) {}
    uint64_t Next()
    {
        uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Unit() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
    float Range(float low, float high) { return low + (high - low) * Unit(); }
    int Int(int low, int high) { return low + static_cast<int>(Next() % static_cast<uint64_t>(high - low + 1)); }

private:
    uint64_t m_state;
};

// One straight run of the building's outside wall (FacilityLayout::OuterWall): which way it faces, where its cladding's face
// is, how far it runs, and the storeys it is the outside of.
struct Face
{
    int side = 0;
    bool alongX = false; // the sides facing -z and +z run along x
    float skin = 0.0f;   // the cladding's outer face, on the axis it faces along
    float outward = 1.0f;
    float from = 0.0f;   // along it, from end to end
    float to = 0.0f;
    float cellsFrom = 0.0f; // where cell 0 begins, along it
    int line = 0;
    int first = 0;          // the cells along it it runs past, [first, last)
    int last = 0;
    int low = 0;
    int high = 0;
};

struct Builder
{
    const FacilityLayout& layout;
    FacilityMap::Blueprint& out;

    float Base(int floor) const { return layout.origin.y + static_cast<float>(floor) * kStorey; }
    float X(float cells) const { return layout.origin.x + cells * kCell; }
    float Z(float cells) const { return layout.origin.z + cells * kCell; }

    void Add(Form form, Kind kind, glm::vec3 centre, glm::vec3 size, glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
    {
        FacilityMap::Dressing dressing;
        dressing.form = form;
        dressing.kind = kind;
        dressing.centre = centre;
        dressing.size = size;
        dressing.rotation = rotation;
        out.dressing.push_back(dressing);
    }

    Face FaceOf(int side, int line, int first, int last, int low, int high) const
    {
        const float past = FacilitySpec::kShellThickness + FacilitySpec::kCladding - FacilitySpec::kHalfWall;
        Face face;
        face.side = side;
        face.alongX = side >= 2;
        face.outward = side == 0 || side == 2 ? -1.0f : 1.0f;
        face.skin = (face.alongX ? Z(static_cast<float>(line)) : X(static_cast<float>(line))) + face.outward * past;
        face.cellsFrom = face.alongX ? X(0.0f) : Z(0.0f);
        face.from = face.cellsFrom + static_cast<float>(first) * kCell;
        face.to = face.cellsFrom + static_cast<float>(last) * kCell;
        face.line = line;
        face.first = first;
        face.last = last;
        face.low = low;
        face.high = high;
        return face;
    }

    // A box on a face: along it from a to b, up it from low to high, out of it from `back` to `front` metres past the cladding.
    void OnFace(const Face& face, Kind kind, float a, float b, float low, float high, float back, float front)
    {
        if (b - a < 0.005f || high - low < 0.005f)
        {
            return;
        }
        // (Along z a few millimetres shorter top and bottom: where a band along x and one along z cross round an inside
        // corner, their tops would be one face drawn twice.)
        if (!face.alongX)
        {
            low += 0.005f;
            high -= 0.005f;
        }
        const float n0 = face.skin + face.outward * back;
        const float n1 = face.skin + face.outward * front;
        const float lo = std::min(n0, n1);
        const float hi = std::max(n0, n1);
        const glm::vec3 min = face.alongX ? glm::vec3(a, low, lo) : glm::vec3(lo, low, a);
        const glm::vec3 max = face.alongX ? glm::vec3(b, high, hi) : glm::vec3(hi, high, b);
        Add(Form::Box, kind, (min + max) * 0.5f, max - min);
    }

    // The ways out through a face, by where their middles are along it.
    std::vector<float> ExitsOn(const Face& face) const
    {
        std::vector<float> middles;
        if (face.low > layout.ground || face.high <= layout.ground)
        {
            return middles;
        }
        for (const FacilityLayout::Exit& exit : layout.exits)
        {
            const int line = exit.side == 0 ? exit.cell.x : exit.side == 1 ? exit.cell.x + 1 : exit.side == 2 ? exit.cell.y : exit.cell.y + 1;
            const int along = face.alongX ? exit.cell.x : exit.cell.y;
            if (exit.side == face.side && line == face.line && along >= face.first && along < face.last)
            {
                middles.push_back(face.cellsFrom + (static_cast<float>(along) + 0.5f) * kCell);
            }
        }
        return middles;
    }

    // As OnFace, but leaving out every way out's frame: what runs along the foot of a face goes round them.
    void OnFaceRound(const Face& face, Kind kind, float a, float b, float low, float high, float back, float front)
    {
        std::vector<std::pair<float, float>> holes;
        for (const float mid : ExitsOn(face))
        {
            // (Into the frame's sides, not to their edge: the two ends would be one face, drawn twice.)
            const float half = FacilitySpec::kDoorWidth * 0.5f + kJamb * 0.5f;
            holes.push_back({mid - half, mid + half});
        }
        std::sort(holes.begin(), holes.end());
        float at = a;
        for (const auto& [h0, h1] : holes)
        {
            if (h1 <= at || h0 >= b)
            {
                continue;
            }
            OnFace(face, kind, at, std::min(h0, b), low, high, back, front);
            at = std::max(at, h1);
        }
        OnFace(face, kind, at, b, low, high, back, front);
    }

    // Whether a bay of a floor has a window: in a steady rhythm along the face, one bay in so many, each floor's over the last -- not
    // where a way out is. (The windows are the outside's alone for now: the rooms inside keep a cell clear of the wall.)
    bool Windowed(const Face& face, int floor, int i, const std::vector<float>& exits) const
    {
        const float mid = face.cellsFrom + (static_cast<float>(i) + 0.5f) * kCell;
        const bool door = floor == layout.ground && std::any_of(exits.begin(), exits.end(), [&](float at) { return std::abs(at - mid) < 0.1f; });
        const int every = std::max(layout.outside.windowEvery, 1);
        return !door && i % every == 0;
    }

    // Where a face's ribs stand: on every so many lines between its bays (counted from the building's corner, so they line up
    // round it), not across a strip of windows (which has its own bars between the bays), nor by a way out, where its frame
    // and canopy are.
    std::vector<int> Ribs(const Face& face) const
    {
        std::vector<int> ribs;
        const FacilityLayout::Outside& look = layout.outside;
        if (look.ribEvery <= 0 || look.windows == Windows::Strip)
        {
            return ribs;
        }
        const std::vector<float> exits = ExitsOn(face);
        for (int k = face.first + 1; k < face.last; ++k)
        {
            const float at = face.cellsFrom + static_cast<float>(k) * kCell;
            if (k % look.ribEvery == 0 && std::none_of(exits.begin(), exits.end(), [&](float mid) { return std::abs(at - mid) < kCanopyWidth * 0.5f + 0.3f; }))
            {
                ribs.push_back(k);
            }
        }
        return ribs;
    }

    // Where a face's punched or tall windows go along it: in each panel between its ribs (the whole face, without them) as
    // a building's are -- centred in it, and the same either side of its middle: one to a panel a bay wide, one to each bay
    // of a panel two wide; in a wider one, the middle bay's (or the two either side of the middle's) and every so many out
    // from there. Always in the middle of a bay, so the room behind it looks out of it. Each floor's over the last.
    std::vector<float> WindowSpots(const Face& face) const
    {
        std::vector<float> spots;
        std::vector<int> edges{face.first};
        for (const int k : Ribs(face))
        {
            edges.push_back(k);
        }
        edges.push_back(face.last);
        const int every = std::max(layout.outside.windowEvery, 1);
        for (size_t p = 0; p + 1 < edges.size(); ++p)
        {
            const int bays = edges[p + 1] - edges[p];
            const float from = face.cellsFrom + static_cast<float>(edges[p]) * kCell;
            // (A panel two bays wide has a window to each, either side of its middle: a window is always in one bay, so it is one
            // room's -- or the corridor's -- to look out of from inside.)
            if (bays == 2)
            {
                spots.push_back(from + kCell * 0.5f);
                spots.push_back(from + kCell * 1.5f);
                continue;
            }
            for (int j = 0; j < bays; ++j)
            {
                // How many bays out from the middle: from the middle bay (odd), or from the two either side of the middle (even).
                const int away = bays % 2 == 1 ? std::abs(j - bays / 2) : (j < bays / 2 ? bays / 2 - 1 - j : j - bays / 2);
                if (away % every == 0)
                {
                    spots.push_back(from + (static_cast<float>(j) + 0.5f) * kCell);
                }
            }
        }
        return spots;
    }

    // A window's glass and the frame round it, `a` to `b` along a face and `low` to `high` up it; a sill under it, and bars
    // across it at the given fractions of its height.
    void Window(const Face& face, float a, float b, float low, float high, const std::vector<float>& bars = {})
    {
        constexpr float kFrame = FacilityOutside::kWindowFrame;
        // The glass (FacilityOutside::Pane), a centimetre out from the wall's face inside the frame, facing out.
        const float n = face.skin + face.outward * kGlassOut;
        const glm::vec3 facing = face.alongX ? glm::vec3(0.0f, 0.0f, face.outward) : glm::vec3(face.outward, 0.0f, 0.0f);
        const glm::vec3 middle = face.alongX ? glm::vec3((a + b) * 0.5f, (low + high) * 0.5f, n) : glm::vec3(n, (low + high) * 0.5f, (a + b) * 0.5f);
        Add(Form::Pane, Kind::Glazing, middle, {b - a - kFrame, high - low - kFrame, 0.0f},
            glm::angleAxis(std::atan2(-facing.x, -facing.z), glm::vec3(0.0f, 1.0f, 0.0f)));
        OnFace(face, Kind::Trim, a, b, high - kFrame, high, -kInto, kFrameProud);
        OnFace(face, Kind::Trim, a, a + kFrame, low, high - kFrame, -kInto, kFrameProud);
        OnFace(face, Kind::Trim, b - kFrame, b, low, high - kFrame, -kInto, kFrameProud);
        // The sill: wider and further out than the frame.
        OnFace(face, Kind::Trim, a - 0.06f, b + 0.06f, low - 0.06f, low + 0.05f, -kInto, kFrameProud + 0.06f);
        for (const float bar : bars)
        {
            const float y = low + (high - low) * bar;
            OnFace(face, Kind::Trim, a + kFrame, b - kFrame, y - 0.03f, y + 0.03f, -kInto, kFrameProud - 0.01f);
        }
    }

    // Every window of the building, wall by wall and floor by floor (FacilityOutside::Windows).
    std::vector<FacilityOutside::Window> Plan() const
    {
        std::vector<FacilityOutside::Window> windows;
        const Windows style = layout.outside.windows;
        if (style == Windows::None)
        {
            return windows;
        }
        const float size = layout.outside.windowSize;
        for (const FacilityLayout::OuterWall& wall : layout.OuterWalls())
        {
            const Face face = FaceOf(wall.side, wall.line, wall.from, wall.to, wall.low, wall.high);
            const std::vector<float> exits = ExitsOn(face);
            for (int f = std::max(face.low, layout.ground); f < face.high; ++f)
            {
                const float base = Base(f);
                const auto add = [&](int bay, float a, float b, float low, float high, bool tall)
                {
                    if (ShaftBehind(face, f, bay))
                    {
                        return;
                    }
                    FacilityOutside::Window window;
                    window.side = face.side;
                    window.line = face.line;
                    window.bay = bay;
                    window.floor = f;
                    window.a = a;
                    window.b = b;
                    window.low = low;
                    window.high = high;
                    window.tall = tall;
                    // An opening through the wall where there is a room, a corridor or a stairwell behind it to look out of; elsewhere
                    // its glass is on the outside of the wall, and looks no different from out there (Pane).
                    const FacilityLayout::Cell behind = CellAt(face, f, bay);
                    window.open = behind == FacilityLayout::Cell::Room || behind == FacilityLayout::Cell::Corridor || behind == FacilityLayout::Cell::Stair;
                    windows.push_back(window);
                };
                if (style == Windows::Strip)
                {
                    // A band of glass along the floor, a pane to a bay, a bar between each.
                    for (int i = face.first; i < face.last; ++i)
                    {
                        if (Windowed(face, f, i, exits))
                        {
                            const float from = face.cellsFrom + static_cast<float>(i) * kCell;
                            add(i, from + 0.22f, from + kCell - 0.22f, base + 1.0f, base + 2.35f, false);
                        }
                    }
                    continue;
                }
                for (const float mid : WindowSpots(face))
                {
                    // (On the ground floor, none where a way out is, or so near one its frame would crowd it.)
                    if (f == layout.ground && std::any_of(exits.begin(), exits.end(), [&](float door) { return std::abs(door - mid) < 1.7f; }))
                    {
                        continue;
                    }
                    const int bay = static_cast<int>(std::floor((mid - face.cellsFrom) / kCell));
                    if (style == Windows::Punched)
                    {
                        // Its sill where it always is, a smaller one shorter from the top down.
                        add(bay, mid - 0.65f * size, mid + 0.65f * size, base + 1.05f, base + 1.05f + 1.25f * size, false);
                    }
                    else
                    {
                        add(bay, mid - 0.5f * size, mid + 0.5f * size, base + 0.5f, base + 0.5f + 2.25f * size, true);
                    }
                }
            }
        }
        return windows;
    }

    // The cell behind a face at `bay` along it, on a floor: the one against the wall.
    FacilityLayout::Cell CellAt(const Face& face, int floor, int bay) const
    {
        const int across = face.side == 0 || face.side == 2 ? face.line : face.line - 1;
        return face.alongX ? layout.At(floor, bay, across) : layout.At(floor, across, bay);
    }

    // Whether the cell behind a bay of a face is a ladder's or a lift's: the ladder goes up one of its walls, the lift's car up
    // the shaft, and neither has a window.
    bool ShaftBehind(const Face& face, int floor, int bay) const
    {
        const int across = face.side == 0 || face.side == 2 ? face.line : face.line - 1;
        const glm::ivec2 cell = face.alongX ? glm::ivec2(bay, across) : glm::ivec2(across, bay);
        const int well = layout.WellAt(floor, cell.x, cell.y);
        return well >= 0 && layout.stairwells[static_cast<size_t>(well)].way != FacilityLayout::Stairwell::Way::Flight;
    }

    // Each window drawn on its wall: its frame and sill round the opening through the wall, and its glass.
    void Glaze()
    {
        for (const FacilityOutside::Window& window : Plan())
        {
            const Face face = FaceOf(window.side, window.line, window.bay, window.bay + 1, window.floor, window.floor + 1);
            Window(face, window.a, window.b, window.low, window.high, window.tall ? std::vector<float>{0.68f} : std::vector<float>{});
        }
    }

    // What runs along a face: the plinth along its foot, a band at every floor, the stripe under its top; ribs between its
    // bays.
    void Lines(const Face& face)
    {
        const FacilityLayout::Outside& look = layout.outside;
        const float ground = Base(layout.ground);
        const float foot = face.low <= layout.ground ? ground : Base(face.low);
        const float top = Base(face.high) + FacilitySpec::kParapet;
        if (look.bands)
        {
            if (face.low <= layout.ground)
            {
                OnFaceRound(face, Kind::Trim, face.from, face.to, ground - 0.3f, ground + 0.4f, -kInto, kBandProud);
            }
            for (int f = std::max(face.low, layout.ground) + 1; f < face.high; ++f)
            {
                OnFace(face, Kind::Trim, face.from, face.to, Base(f) - 0.32f, Base(f) + 0.04f, -kInto, kBandProud);
            }
        }
        if (look.stripe)
        {
            OnFace(face, Kind::Accent, face.from, face.to, top - 0.6f, top - 0.28f, -kInto, kStripeProud);
        }
        for (const int k : Ribs(face))
        {
            const float at = face.cellsFrom + static_cast<float>(k) * kCell;
            // (Into the ground at its foot -- but standing on a lower roof, on it, not into the part under it.)
            OnFace(face, Kind::Trim, at - 0.15f, at + 0.15f, face.low <= layout.ground ? foot - 0.25f : foot, top, -kInto, kRibProud);
        }
    }

    // A pier wrapping each outside corner, in the trim's colour, as tall as the corner is.
    void Corners()
    {
        const FacilityLayout::Outside& look = layout.outside;
        if (!look.bands && !look.stripe && look.ribEvery == 0 && look.windows == Windows::None)
        {
            return;
        }
        const float past = FacilitySpec::kShellThickness + FacilitySpec::kCladding - FacilitySpec::kHalfWall;
        for (const FacilityLayout::OuterCorner& corner : layout.OuterCorners())
        {
            const glm::vec2 at{X(static_cast<float>(corner.at.x)), Z(static_cast<float>(corner.at.y))};
            const glm::vec2 o = glm::vec2(corner.out);
            const float top = Base(corner.high) + FacilitySpec::kParapet + FacilitySpec::kCapHeight + 0.04f;
            // The part standing highest here (its cell off the corner away from `out`), and round the corner from it: the cell
            // beside it along x, beside it along z, and across from it.
            const glm::ivec2 mine = corner.at + glm::ivec2(corner.out.x < 0 ? 0 : -1, corner.out.y < 0 ? 0 : -1);
            const int besideX = layout.Height(mine.x + corner.out.x, mine.y);
            const int besideZ = layout.Height(mine.x, mine.y + corner.out.y);
            const int across = layout.Height(mine.x + corner.out.x, mine.y + corner.out.y);
            // Along each of the two faces from the corner, back along it and out past the cladding -- down as far as that face
            // goes: where a lower part stands flush with this one along it, the face runs on down past that part's roof as one
            // wall, and the pier with it, to wherever there is building out in front of it.
            for (int k = 0; k < 2; ++k)
            {
                const int low = std::max(across, k == 0 ? besideZ : besideX);
                const float foot = low <= layout.ground ? Base(layout.ground) - 0.25f : Base(low);
                const glm::vec2 a = k == 0 ? glm::vec2(at.x - o.x * 0.3f, at.y + o.y * (past - kInto)) : glm::vec2(at.x + o.x * (past - kInto), at.y - o.y * 0.3f);
                const glm::vec2 b = at + o * (past + kCornerProud);
                const glm::vec3 min{std::min(a.x, b.x), foot, std::min(a.y, b.y)};
                const glm::vec3 max{std::max(a.x, b.x), top - (k == 0 ? 0.0f : 0.01f), std::max(a.y, b.y)};
                Add(Form::Box, Kind::Trim, (min + max) * 0.5f, max - min);
            }
        }
    }

    // A frame round every way out, and a canopy over it.
    void Entrances()
    {
        const float ground = Base(layout.ground);
        for (const FacilityLayout::Exit& exit : layout.exits)
        {
            const int line = exit.side == 0 ? exit.cell.x : exit.side == 1 ? exit.cell.x + 1 : exit.side == 2 ? exit.cell.y : exit.cell.y + 1;
            const int along = exit.side >= 2 ? exit.cell.x : exit.cell.y;
            const Face face = FaceOf(exit.side, line, along, along + 1, 0, layout.ground + 1);
            const float mid = face.cellsFrom + (static_cast<float>(along) + 0.5f) * kCell;
            const float half = FacilitySpec::kDoorWidth * 0.5f;
            const float head = ground + FacilitySpec::kDoorHeight;
            OnFace(face, Kind::Accent, mid - half - kJamb, mid - half, ground - 0.05f, head + kJamb, -kInto, kJambProud);
            OnFace(face, Kind::Accent, mid + half, mid + half + kJamb, ground - 0.05f, head + kJamb, -kInto, kJambProud);
            OnFace(face, Kind::Accent, mid - half, mid + half, head, head + kJamb, -kInto, kJambProud);
            if (!layout.outside.canopies)
            {
                continue;
            }
            const float under = ground + kCanopyUnder;
            OnFace(face, Kind::Trim, mid - kCanopyWidth * 0.5f, mid + kCanopyWidth * 0.5f, under, under + kCanopyThick, -kInto, kCanopyOut);
            // Its fascia, in the accent colour, along its front edge.
            OnFace(face, Kind::Accent, mid - kCanopyWidth * 0.5f - 0.01f, mid + kCanopyWidth * 0.5f + 0.01f, under - 0.04f, under + kCanopyThick + 0.03f,
                   kCanopyOut - 0.04f, kCanopyOut + 0.01f);
            // Two rods up from its front corners to the wall over it.
            for (const float s : {-1.0f, 1.0f})
            {
                const float at = mid + s * (kCanopyWidth * 0.5f - 0.15f);
                const float n0 = face.skin + face.outward * (kCanopyOut - 0.12f);
                const float n1 = face.skin;
                const glm::vec3 lowEnd = face.alongX ? glm::vec3(at, under + kCanopyThick, n0) : glm::vec3(n0, under + kCanopyThick, at);
                const glm::vec3 highEnd = face.alongX ? glm::vec3(at, under + 1.1f, n1) : glm::vec3(n1, under + 1.1f, at);
                const glm::vec3 run = highEnd - lowEnd;
                Add(Form::Cylinder, Kind::Trim, (lowEnd + highEnd) * 0.5f, {0.05f, glm::length(run), 0.05f},
                    glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(run)));
            }
        }
        // Its name over its main way out: on the front of its canopy, or over the door's frame where it has none -- lit, where that
        // is how the way in is marked.
        if (!layout.outside.label.empty() && !layout.exits.empty())
        {
            const FacilityLayout::Exit& exit = layout.exits.front();
            const int line = exit.side == 0 ? exit.cell.x : exit.side == 1 ? exit.cell.x + 1 : exit.side == 2 ? exit.cell.y : exit.cell.y + 1;
            const int along = exit.side >= 2 ? exit.cell.x : exit.cell.y;
            const Face face = FaceOf(exit.side, line, along, along + 1, 0, layout.ground + 1);
            const float mid = face.cellsFrom + (static_cast<float>(along) + 0.5f) * kCell;
            const bool canopy = layout.outside.canopies;
            const float height = canopy ? 0.12f : 0.26f;
            const float y = canopy ? ground + kCanopyUnder + kCanopyThick * 0.5f : ground + FacilitySpec::kDoorHeight + kJamb + 0.08f + height * 0.5f;
            const float front = face.skin + face.outward * ((canopy ? kCanopyOut + 0.01f : kJambProud) + 0.003f);
            FacilityMap::Dressing words;
            words.form = Form::Text;
            words.kind = layout.outside.labelLit ? Kind::Screen : Kind::Lettering;
            words.text = layout.outside.label;
            words.centre = face.alongX ? glm::vec3(mid, y, front) : glm::vec3(front, y, mid);
            words.size = {0.0f, height, 0.0f};
            const glm::vec3 facing = face.alongX ? glm::vec3(0.0f, 0.0f, face.outward) : glm::vec3(face.outward, 0.0f, 0.0f);
            words.rotation = glm::angleAxis(std::atan2(-facing.x, -facing.z), glm::vec3(0.0f, 1.0f, 0.0f));
            out.dressing.push_back(words);
        }
        // And round each doorway a floor down into a passage to another building, a plain frame on the passage's side.
        for (const FacilityLayout::Exit& tunnel : layout.tunnels)
        {
            const int line = tunnel.side == 0 ? tunnel.cell.x : tunnel.side == 1 ? tunnel.cell.x + 1 : tunnel.side == 2 ? tunnel.cell.y : tunnel.cell.y + 1;
            const int along = tunnel.side >= 2 ? tunnel.cell.x : tunnel.cell.y;
            const Face face = FaceOf(tunnel.side, line, along, along + 1, 0, layout.ground);
            const float mid = face.cellsFrom + (static_cast<float>(along) + 0.5f) * kCell;
            const float half = FacilitySpec::kDoorWidth * 0.5f;
            const float floor = Base(layout.ground - 1);
            const float head = floor + FacilitySpec::kDoorHeight;
            OnFace(face, Kind::Trim, mid - half - kJamb, mid - half, floor, head + kJamb, -kInto, kJambProud);
            OnFace(face, Kind::Trim, mid + half, mid + half + kJamb, floor, head + kJamb, -kInto, kJambProud);
            OnFace(face, Kind::Trim, mid - half, mid + half, head, head + kJamb, -kInto, kJambProud);
        }
    }

    // On the roofs -- each part's own, at its own height: what stands there.
    void Roof(Random& random)
    {
        const FacilityLayout::Outside& look = layout.outside;
        const int w = layout.width;
        const int d = layout.depth;
        // Which cells are free to put something on: in the middle of a roof -- every cell round them part of the same roof --
        // and not under or beside what is put there already.
        std::vector<bool> free(static_cast<size_t>(w * d), false);
        for (int z = 0; z < d; ++z)
        {
            for (int x = 0; x < w; ++x)
            {
                const int h = layout.Height(x, z);
                bool middle = h > 0;
                for (int dz = -1; dz <= 1 && middle; ++dz)
                {
                    for (int dx = -1; dx <= 1 && middle; ++dx)
                    {
                        middle = layout.Height(x + dx, z + dz) == h;
                    }
                }
                free[static_cast<size_t>(z * w + x)] = middle;
            }
        }
        const auto taken = [&](int x0, int z0, int x1, int z1)
        {
            for (int z = std::max(z0, 0); z <= std::min(z1, d - 1); ++z)
            {
                for (int x = std::max(x0, 0); x <= std::min(x1, w - 1); ++x)
                {
                    free[static_cast<size_t>(z * w + x)] = false;
                }
            }
        };
        // Free, and all one roof.
        const auto isFree = [&](int x0, int z0, int x1, int z1)
        {
            if (x0 < 0 || z0 < 0 || x1 >= w || z1 >= d)
            {
                return false;
            }
            const int h = layout.Height(x0, z0);
            for (int z = z0; z <= z1; ++z)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    if (!free[static_cast<size_t>(z * w + x)] || layout.Height(x, z) != h)
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        // A free spot `cw` by `cd` cells, somewhere on a roof (false when there is none).
        const auto find = [&](int cw, int cd, glm::ivec2& at)
        {
            for (int attempt = 0; attempt < 80; ++attempt)
            {
                const glm::ivec2 tried{random.Int(1, std::max(1, w - 1 - cw)), random.Int(1, std::max(1, d - 1 - cd))};
                if (isFree(tried.x, tried.y, tried.x + cw - 1, tried.y + cd - 1))
                {
                    at = tried;
                    return true;
                }
            }
            return false;
        };
        const auto middleOf = [&](glm::ivec2 at, int cw, int cd)
        {
            return glm::vec3(X(static_cast<float>(at.x) + static_cast<float>(cw) * 0.5f), Base(layout.Height(at.x, at.y)),
                             Z(static_cast<float>(at.y) + static_cast<float>(cd) * 0.5f));
        };

        // Over each stair that climbs to the top floor of its part, the housing a stair to the roof would come up into.
        for (const FacilityLayout::Stairwell& well : layout.stairwells)
        {
            if (well.way != FacilityLayout::Stairwell::Way::Flight || well.floor + 2 != layout.Height(well.min.x, well.min.y))
            {
                continue;
            }
            const float roof = Base(well.floor + 2);
            const glm::vec3 lo{X(static_cast<float>(well.min.x)) + 0.25f, roof, Z(static_cast<float>(well.min.y)) + 0.25f};
            const glm::vec3 hi{X(static_cast<float>(well.max.x + 1)) - 0.25f, roof + 2.6f, Z(static_cast<float>(well.max.y + 1)) - 0.25f};
            Add(Form::Box, Kind::Cladding, (lo + hi) * 0.5f, hi - lo);
            Add(Form::Box, Kind::Trim, {(lo.x + hi.x) * 0.5f, hi.y + 0.05f, (lo.z + hi.z) * 0.5f}, {hi.x - lo.x + 0.16f, 0.1f, hi.z - lo.z + 0.16f});
            taken(well.min.x - 1, well.min.y - 1, well.max.x + 1, well.max.y + 1);
        }
        // An observatory's dome, on a drum: on the highest roof with room for it, as near the middle as it can be.
        if (look.dome)
        {
            const float radius = std::clamp(static_cast<float>(std::min(w, d)) * kCell * 0.18f, 4.0f, 8.0f);
            const int span = static_cast<int>(std::ceil(radius * 2.0f / kCell)) + 1;
            glm::ivec2 best{-1};
            float bestScore = -1.0e9f;
            for (int z = 0; z + span <= d; ++z)
            {
                for (int x = 0; x + span <= w; ++x)
                {
                    if (!isFree(x, z, x + span - 1, z + span - 1))
                    {
                        continue;
                    }
                    const float off = glm::length(glm::vec2(static_cast<float>(x) + static_cast<float>(span) * 0.5f - static_cast<float>(w) * 0.5f,
                                                            static_cast<float>(z) + static_cast<float>(span) * 0.5f - static_cast<float>(d) * 0.5f));
                    const float score = static_cast<float>(layout.Height(x, z)) * 100.0f - off;
                    if (score > bestScore)
                    {
                        bestScore = score;
                        best = {x, z};
                    }
                }
            }
            if (best.x >= 0)
            {
                const glm::vec3 middle = middleOf(best, span, span);
                Add(Form::Cylinder, Kind::Cladding, middle + glm::vec3(0.0f, 0.7f, 0.0f), {radius * 2.0f, 1.4f, radius * 2.0f});
                Add(Form::Cylinder, Kind::Trim, middle + glm::vec3(0.0f, 1.45f, 0.0f), {radius * 2.0f + 0.2f, 0.1f, radius * 2.0f + 0.2f});
                Add(Form::Dome, Kind::Kit, middle + glm::vec3(0.0f, 1.5f, 0.0f), {radius, radius * 0.95f, radius});
                taken(best.x - 1, best.y - 1, best.x + span, best.y + span);
            }
        }
        // Plant: a box on rails, its fans on top.
        for (int i = 0; i < look.units; ++i)
        {
            const bool wide = random.Unit() < 0.4f;
            glm::ivec2 at;
            if (!find(wide ? 2 : 1, 1, at))
            {
                break;
            }
            const glm::vec3 middle = middleOf(at, wide ? 2 : 1, 1);
            const float length = wide ? random.Range(4.0f, 5.2f) : random.Range(1.8f, 2.5f);
            const float deep = random.Range(1.2f, 1.7f);
            const float high = random.Range(1.0f, 1.5f);
            for (const float s : {-1.0f, 1.0f})
            {
                Add(Form::Box, Kind::Trim, middle + glm::vec3(0.0f, 0.06f, s * (deep * 0.5f - 0.15f)), {length + 0.2f, 0.12f, 0.12f});
            }
            Add(Form::Box, Kind::Kit, middle + glm::vec3(0.0f, 0.12f + high * 0.5f, 0.0f), {length, high, deep});
            const int fans = wide ? 2 : 1;
            for (int k = 0; k < fans; ++k)
            {
                const float along = (static_cast<float>(k) + 0.5f) / static_cast<float>(fans) * length - length * 0.5f;
                const glm::vec3 fan = middle + glm::vec3(along, 0.12f + high, 0.0f);
                Add(Form::Cylinder, Kind::Kit, fan + glm::vec3(0.0f, 0.12f, 0.0f), {deep * 0.75f, 0.24f, deep * 0.75f});
                Add(Form::Cylinder, Kind::Glazing, fan + glm::vec3(0.0f, 0.25f, 0.0f), {deep * 0.62f, 0.03f, deep * 0.62f});
            }
            taken(at.x - 1, at.y - 1, at.x + (wide ? 2 : 1), at.y + 1);
        }
        // Vent pipes, capped.
        for (int i = 0; i < look.vents; ++i)
        {
            glm::ivec2 at;
            if (!find(1, 1, at))
            {
                break;
            }
            const glm::vec3 foot = middleOf(at, 1, 1) + glm::vec3(random.Range(-0.6f, 0.6f), 0.0f, random.Range(-0.6f, 0.6f));
            const float across = random.Range(0.25f, 0.45f);
            const float high = random.Range(0.8f, 1.7f);
            Add(Form::Cylinder, Kind::Kit, foot + glm::vec3(0.0f, high * 0.5f, 0.0f), {across, high, across});
            Add(Form::Cone, Kind::Kit, foot + glm::vec3(0.0f, high + 0.12f, 0.0f), {across * 1.9f, 0.24f, 0.0f});
            taken(at.x, at.y, at.x, at.y);
        }
        // Stacks: tall, with a band of the accent colour near the top.
        for (int i = 0; i < look.stacks; ++i)
        {
            glm::ivec2 at;
            if (!find(1, 1, at))
            {
                break;
            }
            const glm::vec3 foot = middleOf(at, 1, 1);
            const float across = random.Range(0.8f, 1.3f);
            const float high = random.Range(6.0f, 11.0f);
            Add(Form::Cylinder, Kind::Kit, foot + glm::vec3(0.0f, high * 0.5f, 0.0f), {across, high, across});
            Add(Form::Cylinder, Kind::Accent, foot + glm::vec3(0.0f, high - 1.2f, 0.0f), {across + 0.06f, 0.9f, across + 0.06f});
            Add(Form::Cylinder, Kind::Trim, foot + glm::vec3(0.0f, high + 0.05f, 0.0f), {across + 0.12f, 0.1f, across + 0.12f});
            Add(Form::Cylinder, Kind::Trim, foot + glm::vec3(0.0f, 0.2f, 0.0f), {across + 0.3f, 0.4f, across + 0.3f});
            taken(at.x - 1, at.y - 1, at.x + 1, at.y + 1);
        }
        // Rows of panels on low frames, tilted up the same way.
        if (look.solar)
        {
            const int rows = std::clamp(d / 6, 1, 4);
            const int run = std::clamp(w / 4, 2, 7);
            const float tilt = glm::radians(24.0f);
            const glm::quat turned = glm::angleAxis(tilt, glm::vec3(1.0f, 0.0f, 0.0f));
            for (int r = 0; r < rows; ++r)
            {
                glm::ivec2 at;
                if (!find(run, 1, at))
                {
                    break;
                }
                for (int k = 0; k < run; ++k)
                {
                    const glm::vec3 middle = middleOf(at + glm::ivec2(k, 0), 1, 1);
                    Add(Form::Box, Kind::Glazing, middle + glm::vec3(0.0f, 0.95f, 0.0f), {2.7f, 0.05f, 1.8f}, turned);
                    for (const float s : {-1.0f, 1.0f})
                    {
                        // Its legs: the back ones taller, under the raised edge.
                        const float z = s * 0.65f;
                        const float high = 0.95f - std::sin(tilt) * z - 0.03f;
                        Add(Form::Box, Kind::Trim, middle + glm::vec3(0.0f, high * 0.5f, z), {2.2f, high, 0.06f});
                    }
                }
                taken(at.x - 1, at.y - 1, at.x + run, at.y + 1);
            }
        }
        // A mast, with its crossbars.
        if (look.antenna)
        {
            glm::ivec2 at;
            if (find(1, 1, at))
            {
                const glm::vec3 foot = middleOf(at, 1, 1);
                const float high = random.Range(6.0f, 10.0f);
                Add(Form::Box, Kind::Trim, foot + glm::vec3(0.0f, 0.15f, 0.0f), {0.9f, 0.3f, 0.9f});
                Add(Form::Cylinder, Kind::Kit, foot + glm::vec3(0.0f, high * 0.5f, 0.0f), {0.14f, high, 0.14f});
                for (int k = 0; k < 3; ++k)
                {
                    const float y = high - 0.5f - static_cast<float>(k) * 1.1f;
                    const float wide = 1.6f - static_cast<float>(k) * 0.35f;
                    Add(Form::Box, Kind::Kit, foot + glm::vec3(0.0f, y, 0.0f), {wide, 0.06f, 0.06f},
                        glm::angleAxis(static_cast<float>(k) * 0.9f, glm::vec3(0.0f, 1.0f, 0.0f)));
                }
                taken(at.x - 1, at.y - 1, at.x + 1, at.y + 1);
            }
        }
        // Dishes, on posts, turned up to the sky -- each its own way.
        for (int i = 0; i < look.dishes; ++i)
        {
            glm::ivec2 at;
            if (!find(2, 2, at))
            {
                break;
            }
            const glm::vec3 middle = middleOf(at, 2, 2);
            const float roof = middle.y;
            const float radius = random.Range(1.1f, 2.0f);
            const float depth = radius * 0.32f;
            const float rise = glm::radians(random.Range(30.0f, 60.0f));
            const float turn = random.Range(0.0f, glm::two_pi<float>());
            const glm::vec3 axis{std::cos(rise) * std::sin(turn), std::sin(rise), -std::cos(rise) * std::cos(turn)};
            // High enough that its low edge clears the roof, its back on the post.
            const glm::vec3 rim = middle + glm::vec3(0.0f, radius * std::cos(rise) + 1.1f, 0.0f);
            const glm::vec3 back = rim - axis * depth;
            Add(Form::Dish, Kind::Kit, rim, {radius, depth, radius}, glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), axis));
            Add(Form::Cylinder, Kind::Trim, {back.x, (roof + back.y) * 0.5f, back.z}, {0.3f, back.y - roof, 0.3f});
            // The feed, out in front of the bowl on its arm.
            const glm::vec3 feed = rim + axis * radius * 0.6f;
            const glm::vec3 arm = feed - back;
            Add(Form::Cylinder, Kind::Trim, (feed + back) * 0.5f, {0.05f, glm::length(arm), 0.05f}, glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), glm::normalize(arm)));
            Add(Form::Box, Kind::Kit, feed, {0.18f, 0.18f, 0.18f}, glm::rotation(glm::vec3(0.0f, 1.0f, 0.0f), axis));
            Add(Form::Box, Kind::Trim, {back.x, roof + 0.1f, back.z}, {1.0f, 0.2f, 1.0f});
            taken(at.x - 1, at.y - 1, at.x + 2, at.y + 2);
        }
    }
};

} // namespace

void Dress(const FacilityLayout& layout, FacilityMap::Blueprint& out)
{
    Builder builder{layout, out};
    for (const FacilityLayout::OuterWall& wall : layout.OuterWalls())
    {
        const Face face = builder.FaceOf(wall.side, wall.line, wall.from, wall.to, wall.low, wall.high);
        builder.Lines(face);
    }
    builder.Glaze();
    builder.Corners();
    builder.Entrances();
    Random random((static_cast<uint64_t>(layout.seed) << 20) ^ 0x0D7051DEull);
    builder.Roof(random);
}

std::vector<Window> Windows(const FacilityLayout& layout)
{
    FacilityMap::Blueprint unused;
    return Builder{layout, unused}.Plan();
}

MeshData Dome(float radius, float height, int segments)
{
    // As a sphere's top half, from its pole down to its rim: rings round, the shape stretched to `height`.
    const int rings = std::max(4, segments / 4);
    MeshData mesh;
    for (int ring = 0; ring <= rings; ++ring)
    {
        const float phi = static_cast<float>(ring) / static_cast<float>(rings) * glm::half_pi<float>();
        const float s = ring == 0 ? 0.0f : std::sin(phi);
        const float c = ring == rings ? 0.0f : std::cos(phi);
        for (int segment = 0; segment <= segments; ++segment)
        {
            const float u = static_cast<float>(segment) / static_cast<float>(segments);
            const float theta = u * glm::two_pi<float>();
            const glm::vec3 position{radius * s * std::sin(theta), height * c, radius * s * std::cos(theta)};
            const glm::vec3 normal = glm::normalize(glm::vec3(s * std::sin(theta) / radius, c / height, s * std::cos(theta) / radius));
            mesh.vertices.push_back(MeshVertex{position, normal, {u * radius * 2.0f, static_cast<float>(ring) / static_cast<float>(rings) * height * 1.6f}});
        }
    }
    const int stride = segments + 1;
    for (int ring = 0; ring < rings; ++ring)
    {
        for (int segment = 0; segment < segments; ++segment)
        {
            const auto i0 = static_cast<uint32_t>(ring * stride + segment);
            const auto i1 = static_cast<uint32_t>(i0 + stride);
            if (ring != 0)
            {
                mesh.indices.insert(mesh.indices.end(), {i0, i1, i0 + 1});
            }
            mesh.indices.insert(mesh.indices.end(), {i0 + 1, i1, i1 + 1});
        }
    }
    return mesh;
}

// A pipe's bend: a quarter of a ring, `radius` round its middle, the pipe `thick` across. Laid out with the corner it turns at
// at the origin: in along +x from -x, round, and out along +z -- or along -z, `turn` below nought, the same mirrored.
MeshData PipeBend(float radius, float thick, float turn)
{
    constexpr int kAlong = 12;
    constexpr int kAround = 16;
    const float side = turn < 0.0f ? -1.0f : 1.0f;
    MeshData mesh;
    for (int i = 0; i <= kAlong; ++i)
    {
        const float a = static_cast<float>(i) / static_cast<float>(kAlong) * glm::half_pi<float>();
        // The middle of the pipe round the bend, and across it, flat: from (-radius, 0) heading +x to (0, radius) heading +z.
        const glm::vec3 middle{-radius + radius * std::sin(a), 0.0f, radius - radius * std::cos(a)};
        const glm::vec3 across{-std::sin(a), 0.0f, std::cos(a)};
        for (int k = 0; k <= kAround; ++k)
        {
            const float b = static_cast<float>(k) / static_cast<float>(kAround) * glm::two_pi<float>();
            const glm::vec3 out = across * std::cos(b) + glm::vec3(0.0f, 1.0f, 0.0f) * std::sin(b);
            MeshVertex vertex;
            vertex.position = middle + out * (thick * 0.5f);
            vertex.normal = out;
            vertex.position.z *= side;
            vertex.normal.z *= side;
            vertex.uv = {static_cast<float>(k) / static_cast<float>(kAround) * thick * 3.1416f, a * radius};
            mesh.vertices.push_back(vertex);
        }
    }
    const int stride = kAround + 1;
    for (int i = 0; i < kAlong; ++i)
    {
        for (int k = 0; k < kAround; ++k)
        {
            const auto a = static_cast<uint32_t>(i * stride + k);
            const auto c = a + static_cast<uint32_t>(stride);
            // (Mirrored, the other way round, so it still faces out.)
            if (side < 0.0f)
            {
                mesh.indices.insert(mesh.indices.end(), {a, a + 1, c, a + 1, c + 1, c});
            }
            else
            {
                mesh.indices.insert(mesh.indices.end(), {a, c, a + 1, a + 1, c, c + 1});
            }
        }
    }
    return mesh;
}
MeshData Dish(float radius, float depth, int segments)
{
    // A bowl, deepest in its middle: rings out from there to its rim, its inside facing up and its outside down -- the same
    // rings twice, turned the other way and facing out, so it is solid seen from either side.
    const int rings = std::max(4, segments / 3);
    MeshData mesh;
    for (int face = 0; face < 2; ++face)
    {
        const auto first = static_cast<uint32_t>(mesh.vertices.size());
        const float facing = face == 0 ? 1.0f : -1.0f;
        for (int ring = 0; ring <= rings; ++ring)
        {
            const float r = static_cast<float>(ring) / static_cast<float>(rings);
            const float y = depth * (r * r - 1.0f);
            const float slope = 2.0f * depth * r / radius; // up the bowl, a metre out
            for (int segment = 0; segment <= segments; ++segment)
            {
                const float u = static_cast<float>(segment) / static_cast<float>(segments);
                const float theta = u * glm::two_pi<float>();
                const glm::vec3 out{std::sin(theta), 0.0f, std::cos(theta)};
                const glm::vec3 normal = glm::normalize(glm::vec3(0.0f, 1.0f, 0.0f) - out * slope) * facing;
                mesh.vertices.push_back(MeshVertex{out * (radius * r) + glm::vec3(0.0f, y, 0.0f), normal, {u * radius * 2.0f, r * radius}});
            }
        }
        const int stride = segments + 1;
        for (int ring = 0; ring < rings; ++ring)
        {
            for (int segment = 0; segment < segments; ++segment)
            {
                const auto i0 = first + static_cast<uint32_t>(ring * stride + segment);
                const auto i1 = i0 + static_cast<uint32_t>(stride);
                if (face == 0)
                {
                    if (ring != 0)
                    {
                        mesh.indices.insert(mesh.indices.end(), {i0, i1, i0 + 1});
                    }
                    mesh.indices.insert(mesh.indices.end(), {i0 + 1, i1, i1 + 1});
                }
                else
                {
                    if (ring != 0)
                    {
                        mesh.indices.insert(mesh.indices.end(), {i0, i0 + 1, i1});
                    }
                    mesh.indices.insert(mesh.indices.end(), {i0 + 1, i1 + 1, i1});
                }
            }
        }
    }
    return mesh;
}

MeshData Pane(float wide, float high)
{
    MeshData mesh;
    const float x = wide * 0.5f;
    const float y = high * 0.5f;
    const glm::vec3 normal{0.0f, 0.0f, -1.0f};
    // Wound to face -z, as Lettering's quads are.
    mesh.vertices.push_back(MeshVertex{{x, -y, 0.0f}, normal, {0.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{-x, -y, 0.0f}, normal, {1.0f, 0.0f}});
    mesh.vertices.push_back(MeshVertex{{-x, y, 0.0f}, normal, {1.0f, 1.0f}});
    mesh.vertices.push_back(MeshVertex{{x, y, 0.0f}, normal, {0.0f, 1.0f}});
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

} // namespace FacilityOutside

} // namespace pred
