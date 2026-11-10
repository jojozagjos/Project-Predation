// Sitting in the shuttle. Its four seats face its windscreen -- two at the controls and two raised behind them -- and
// everybody going down sits in one: in the hangar the shuttle sets off on its own a few seconds after the last of the crew
// sits down, and they stay in their seats, looking round, as it flies down through the atmosphere to the site (the
// launch and insertion cinematics carry the shuttle; the seat carries the eye). Coming back the same.
//
// Who sits where is the host's, told everybody (WorldEventKind::Seats); a seat is asked for as any interaction is.

#include "Game/PredationGame.h"
#include "Game/PredationGameShared.h"

#include "Engine/Core/Log.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace pred
{
namespace
{

// The seats, by the shuttle's parts: the two at the controls, then the two raised behind them.
constexpr const char* kSeatParts[4] = {"seat_left", "seat_right", "seat_rear_left", "seat_rear_right"};
// A seat's cushion is this high (its part's box): the eye sits this far over the cushion's top, a little back from its middle.
constexpr float kCushion = 0.45f;
constexpr float kEyeOverSeat = 0.78f;
// And the hips a little over its top, back from its middle.
constexpr float kHipsOverSeat = 0.09f;
// How far round and up and down a seated head turns, from looking straight ahead.
constexpr float kLookAround = glm::radians(115.0f);
constexpr float kLookDown = glm::radians(60.0f);
constexpr float kLookUp = glm::radians(55.0f);
// Everybody seated in the hangar: this long before it sets out.
constexpr float kSeatedLaunchSeconds = 4.0f;

} // namespace

VehicleProp& PredationGame::SeatShuttle()
{
    return m_map == MapChoice::Facility ? m_facility.Shuttle() : m_ship.Shuttle();
}

bool PredationGame::SeatPose(int seat, glm::vec3& eye, glm::quat& turn, glm::vec3& stand)
{
    if (seat < 0 || seat >= kSeats)
    {
        return false;
    }
    const Transform* at = m_scene.GetTransform(SeatShuttle().Part(kSeatParts[seat]));
    if (at == nullptr)
    {
        return false;
    }
    // Facing the shuttle's front (its -z), as the seat does; the eye over the cushion, the feet in the aisle behind it.
    turn = at->rotation;
    eye = at->position + at->rotation * glm::vec3(0.0f, kCushion * 0.5f + kEyeOverSeat, 0.1f);
    stand = at->position + at->rotation * glm::vec3(0.0f, -kCushion * 0.5f + 0.02f, 0.75f);
    return true;
}

bool PredationGame::SeatHips(int seat, glm::vec3& hips, glm::quat& turn)
{
    if (seat < 0 || seat >= kSeats)
    {
        return false;
    }
    const Transform* at = m_scene.GetTransform(SeatShuttle().Part(kSeatParts[seat]));
    if (at == nullptr)
    {
        return false;
    }
    turn = at->rotation;
    hips = at->position + at->rotation * glm::vec3(0.0f, kCushion * 0.5f + kHipsOverSeat, 0.07f);
    return true;
}

void PredationGame::RegisterSeats(int shuttle)
{
    // Each seat something to sit in, its payload which seat of which shuttle (the hangar's 0 to 3, the site's 4 to 7).
    const VehicleProp& prop = shuttle == 0 ? m_ship.Shuttle() : m_facility.Shuttle();
    for (int seat = 0; seat < kSeats; ++seat)
    {
        Entity& registered = m_seatParts[static_cast<size_t>(shuttle)][static_cast<size_t>(seat)];
        if (registered.IsValid())
        {
            m_interactions.Unregister(registered);
        }
        registered = prop.Part(kSeatParts[seat]);
        if (!registered.IsValid())
        {
            continue;
        }
        Interactable interactable;
        interactable.entity = registered;
        interactable.kind = InteractionKind::Seat;
        interactable.payload = shuttle * kSeats + seat;
        interactable.verb = "Sit";
        interactable.name = "";
        interactable.focusOffset = {0.0f, kCushion * 0.5f, 0.0f};
        interactable.range = 2.0f;
        m_interactions.Register(interactable);
    }
}

bool PredationGame::Seated(uint8_t player) const
{
    return std::find(m_seats.begin(), m_seats.end(), player) != m_seats.end();
}

bool PredationGame::PerformSeatInteraction(int payload, uint8_t player)
{
    if (!IsAuthority())
    {
        return false;
    }
    const int seat = payload % kSeats;
    if (seat < 0 || seat >= kSeats)
    {
        return false;
    }
    // Getting up from one's own seat: not while the shuttle is flying (a cinematic carrying it).
    if (m_seats[static_cast<size_t>(seat)] == player)
    {
        if (m_cine.Active())
        {
            return false;
        }
        SeatPlayer(player, -1);
        return true;
    }
    if (m_seats[static_cast<size_t>(seat)] != kNoSeat)
    {
        return false; // (somebody else's)
    }
    SeatPlayer(player, seat);
    return true;
}

void PredationGame::SeatPlayer(uint8_t player, int seat)
{
    // Out of any seat first: one each.
    for (uint8_t& occupant : m_seats)
    {
        occupant = occupant == player ? kNoSeat : occupant;
    }
    if (seat >= 0 && seat < kSeats)
    {
        m_seats[static_cast<size_t>(seat)] = player;
    }
    ApplySeats(m_seats);
    BroadcastSeats();
    PRED_LOG_INFO(Gameplay, "Player {} {}", player, seat >= 0 ? "sat down in seat " + std::to_string(seat) : std::string("stood up"));
}

void PredationGame::SeatAboard(uint8_t aboard)
{
    // The host, as the shuttle goes: everybody aboard into the seats still free (those already sitting keep theirs).
    bool changed = false;
    for (uint8_t player = 0; player < kMaxPlayers; ++player)
    {
        if (((aboard >> player) & 1u) == 0u || Seated(player))
        {
            continue;
        }
        for (uint8_t& occupant : m_seats)
        {
            if (occupant == kNoSeat)
            {
                occupant = player;
                changed = true;
                break;
            }
        }
    }
    // Anybody sitting who is not aboard (left behind as it goes) is not in it.
    for (uint8_t& occupant : m_seats)
    {
        if (occupant != kNoSeat && ((aboard >> occupant) & 1u) == 0u)
        {
            occupant = kNoSeat;
            changed = true;
        }
    }
    if (changed)
    {
        ApplySeats(m_seats);
        BroadcastSeats();
    }
}

void PredationGame::BroadcastSeats(int player)
{
    if (m_sessionMode != SessionMode::Host)
    {
        return;
    }
    // Four seats, four bits each: who sits there, fifteen for nobody.
    WorldEventMessage event;
    event.kind = WorldEventKind::Seats;
    uint16_t packed = 0;
    for (int seat = 0; seat < kSeats; ++seat)
    {
        const uint8_t occupant = m_seats[static_cast<size_t>(seat)];
        packed = static_cast<uint16_t>(packed | ((occupant == kNoSeat ? 0xFu : (occupant & 0xFu)) << (seat * 4)));
    }
    event.item = packed;
    event.quiet = player >= 0;
    if (player >= 0)
    {
        m_host.SendTo(static_cast<uint8_t>(player), event);
    }
    else
    {
        m_host.Broadcast(event);
    }
}

void PredationGame::ApplySeatsEvent(const WorldEventMessage& event)
{
    std::array<uint8_t, kSeats> seats{};
    for (int seat = 0; seat < kSeats; ++seat)
    {
        const uint8_t occupant = static_cast<uint8_t>((event.item >> (seat * 4)) & 0xFu);
        seats[static_cast<size_t>(seat)] = occupant == 0xF ? kNoSeat : occupant;
    }
    ApplySeats(seats);
}

void PredationGame::ApplySeats(const std::array<uint8_t, kSeats>& seats)
{
    m_seats = seats;
    const auto mine = std::find(m_seats.begin(), m_seats.end(), LocalPlayerId());
    const int seat = mine == m_seats.end() ? -1 : static_cast<int>(mine - m_seats.begin());
    if (seat == m_mySeat)
    {
        return;
    }
    glm::vec3 eye;
    glm::quat turn;
    glm::vec3 stand;
    if (seat >= 0 && SeatPose(seat, eye, turn, stand))
    {
        // Sitting down: looking ahead, the eye easing down into the seat from where it was.
        if (m_mySeat < 0)
        {
            m_sitFrom = m_player.View().eyePosition;
            m_sitBlend = 0.0f;
        }
        m_seatLookYaw = 0.0f;
        m_seatLookPitch = glm::radians(-6.0f);
        m_player.Attach(stand, m_lookYaw);
        PlayNamed("Player/land", stand, 0.25f, 0.7f);
    }
    else if (m_mySeat >= 0)
    {
        // Getting up, into the aisle behind the seat, facing the way it faced.
        if (SeatPose(m_mySeat, eye, turn, stand))
        {
            const glm::vec3 ahead = turn * glm::vec3(0.0f, 0.0f, -1.0f);
            m_lookYaw = std::atan2(ahead.x, -ahead.z);
            m_lookPitch = 0.0f;
            m_player.Detach(stand + glm::vec3(0.0f, 0.05f, 0.0f));
        }
        else
        {
            m_player.Detach(m_player.State().position);
        }
        m_body.SetVisible(m_scene, m_body.Tuning().visible && !m_cineHidBodies);
    }
    m_mySeat = seat;
}

void PredationGame::UpdateSeats(float dt)
{
    // Seats offered while free (and not to somebody sitting already).
    for (size_t shuttle = 0; shuttle < 2; ++shuttle)
    {
        for (size_t seat = 0; seat < kSeats; ++seat)
        {
            if (Interactable* interactable = m_interactions.Find(m_seatParts[shuttle][seat]))
            {
                const bool free = m_seats[seat] == kNoSeat;
                interactable->enabled = free && m_mySeat < 0;
            }
        }
    }
    // Sitting: held in the seat as the shuttle moves, the body sitting in it -- seen, by everybody and in one's own view, a
    // cinematic carrying the shuttle or not -- and the eye the seat's (SeatedCamera).
    const auto sit = [this](PlayerBody& body, int seat, bool show)
    {
        glm::vec3 hips;
        glm::quat turn;
        if (seat >= 0 && SeatHips(seat, hips, turn))
        {
            body.SetSeat(true, hips, turn, kCushion + kHipsOverSeat);
            body.SetVisible(m_scene, show);
        }
        else
        {
            body.SetSeat(false, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 0.0f);
        }
    };
    if (m_mySeat >= 0)
    {
        glm::vec3 eye;
        glm::quat turn;
        glm::vec3 stand;
        if (SeatPose(m_mySeat, eye, turn, stand))
        {
            m_player.Attach(stand, m_lookYaw);
        }
        m_sitBlend = std::min(m_sitBlend + dt / 0.6f, 1.0f);
        if (!m_player.State().alive)
        {
            m_mySeat = -1;
        }
    }
    sit(m_body, m_mySeat, m_body.Tuning().visible);
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        if (RemoteAvatar* avatar = AvatarFor(remote.id))
        {
            const auto taken = std::find(m_seats.begin(), m_seats.end(), remote.id);
            sit(avatar->body, taken == m_seats.end() ? -1 : static_cast<int>(taken - m_seats.begin()), true);
        }
    }

    // The host: everybody up sitting in the hangar's shuttle with the ship over a site, and it sets out on its own.
    if (!IsAuthority())
    {
        return;
    }
    // (Nobody keeps a seat they cannot be in: gone, or dead.)
    for (uint8_t& occupant : m_seats)
    {
        if (occupant == kNoSeat || occupant == LocalPlayerId())
        {
            continue;
        }
        const bool here = std::any_of(RemotePlayers().begin(), RemotePlayers().end(), [occupant](const RemotePlayerView& r) { return r.id == occupant && r.alive; });
        if (!here)
        {
            occupant = kNoSeat;
            ApplySeats(m_seats);
            BroadcastSeats();
        }
    }
    int everybody = m_player.State().alive ? 1 : 0;
    int seated = m_player.State().alive && Seated(LocalPlayerId()) ? 1 : 0;
    for (const RemotePlayerView& remote : RemotePlayers())
    {
        everybody += remote.alive ? 1 : 0;
        seated += remote.alive && Seated(remote.id) ? 1 : 0;
    }
    const bool ready = m_map == MapChoice::Ship && m_shipReady && !m_cine.Active() && everybody > 0 && seated == everybody &&
                       Travel::SecondsToOver(m_campaign) <= 0.0f;
    if (!ready)
    {
        m_seatLaunchIn = -1.0f;
        return;
    }
    if (m_seatLaunchIn < 0.0f)
    {
        m_seatLaunchIn = kSeatedLaunchSeconds;
        ShareSound("World/terminal_beep", m_ship.Shuttle().Home().position + glm::vec3(0.0f, 1.5f, 0.0f), 0.6f);
        PRED_LOG_INFO(Gameplay, "Everybody is seated: the shuttle sets out in {:.0f} s", kSeatedLaunchSeconds);
    }
    m_seatLaunchIn -= dt;
    if (m_seatLaunchIn <= 0.0f)
    {
        m_seatLaunchIn = -1.0f;
        LaunchFromShip(LocalPlayerId());
    }
}

void PredationGame::SeatLook(const glm::vec2& delta, float sensitivity, float vertical)
{
    // Round from the seat, as a head turns: so far each way, and no further.
    m_seatLookYaw = std::clamp(m_seatLookYaw - delta.x * sensitivity, -kLookAround, kLookAround);
    m_seatLookPitch = std::clamp(m_seatLookPitch + vertical, -kLookDown, kLookUp);
}

bool PredationGame::SeatedCamera(glm::mat4& view, glm::vec3& eye)
{
    glm::vec3 seatEye;
    glm::quat turn;
    glm::vec3 stand;
    if (m_mySeat < 0 || m_cameraMode == CameraMode::Fly || !SeatPose(m_mySeat, seatEye, turn, stand))
    {
        return false;
    }
    // The seat's frame turned by the head -- so the view banks and dips with the shuttle as it flies -- and the eye eased down
    // into it from where it stood.
    const glm::quat look = turn * glm::angleAxis(m_seatLookYaw, glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(m_seatLookPitch, glm::vec3(1.0f, 0.0f, 0.0f));
    const float ease = m_sitBlend * m_sitBlend * (3.0f - 2.0f * m_sitBlend);
    glm::vec3 at = glm::mix(m_sitFrom, seatEye, ease);
    // Shaken with the shuttle, while a cinematic carries it.
    if (m_cine.Active())
    {
        const CinematicSampler sampler = m_cine.Sampler();
        const float shake = sampler.Shake(m_cine.Time());
        if (shake > 0.0f)
        {
            at += look * (Shaking(m_cine.Time(), sampler.ShakeSpeed(m_cine.Time())) * shake * 0.03f);
        }
    }
    view = glm::inverse(glm::translate(glm::mat4(1.0f), at) * glm::mat4_cast(look));
    eye = at;
    return true;
}

} // namespace pred
