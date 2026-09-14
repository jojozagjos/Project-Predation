#include "Engine/Net/IceLink.h"

#include "Engine/Core/Log.h"

#include <juice/juice.h>

#include <algorithm>
#include <cstring>

namespace pred
{
namespace
{

// The most datagrams that may wait to be read. A peer sending faster than the game reads is either
// broken or hostile, and either way the answer is to drop rather than to grow.
constexpr size_t kMaxQueued = 256;

// A code is one line, because it is going to be pasted into a chat window and a chat window is not
// a text editor: anything with newlines in it arrives split, quoted, or half missing. The
// description libjuice produces is several lines of SDP, so it is turned into one line of base64
// and back again.
constexpr char kPrefix[] = "PRED1:";
const char* const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int ValueOf(char c)
{
    const char* found = std::strchr(kAlphabet, c);
    return (found == nullptr || c == '\0') ? -1 : static_cast<int>(found - kAlphabet);
}

} // namespace

std::string EncodeIceCode(const std::string& description)
{
    std::string out = kPrefix;
    size_t i = 0;
    while (i + 2 < description.size())
    {
        const uint32_t block = (static_cast<uint8_t>(description[i]) << 16) |
                               (static_cast<uint8_t>(description[i + 1]) << 8) |
                               static_cast<uint8_t>(description[i + 2]);
        out += kAlphabet[(block >> 18) & 0x3F];
        out += kAlphabet[(block >> 12) & 0x3F];
        out += kAlphabet[(block >> 6) & 0x3F];
        out += kAlphabet[block & 0x3F];
        i += 3;
    }
    if (i < description.size())
    {
        const size_t left = description.size() - i;
        uint32_t block = static_cast<uint32_t>(static_cast<uint8_t>(description[i])) << 16;
        if (left > 1)
        {
            block |= static_cast<uint32_t>(static_cast<uint8_t>(description[i + 1])) << 8;
        }
        out += kAlphabet[(block >> 18) & 0x3F];
        out += kAlphabet[(block >> 12) & 0x3F];
        out += left > 1 ? kAlphabet[(block >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

bool DecodeIceCode(const std::string& code, std::string& outDescription)
{
    // Whatever a chat window did to it on the way. Spaces, tabs and newlines are stripped, and the
    // prefix is allowed to have anything in front of it, because somebody will paste a sentence
    // with the code on the end of it.
    std::string cleaned;
    cleaned.reserve(code.size());
    for (const char c : code)
    {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            cleaned += c;
        }
    }
    const size_t start = cleaned.find(kPrefix);
    if (start == std::string::npos)
    {
        return false;
    }
    cleaned = cleaned.substr(start + std::strlen(kPrefix));
    if (cleaned.empty() || cleaned.size() % 4 != 0)
    {
        return false;
    }

    outDescription.clear();
    for (size_t i = 0; i + 3 < cleaned.size(); i += 4)
    {
        const int a = ValueOf(cleaned[i]);
        const int b = ValueOf(cleaned[i + 1]);
        const int c = cleaned[i + 2] == '=' ? 0 : ValueOf(cleaned[i + 2]);
        const int d = cleaned[i + 3] == '=' ? 0 : ValueOf(cleaned[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0)
        {
            return false;
        }
        const uint32_t block = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12) |
                               (static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(d);
        outDescription += static_cast<char>((block >> 16) & 0xFF);
        if (cleaned[i + 2] != '=')
        {
            outDescription += static_cast<char>((block >> 8) & 0xFF);
        }
        if (cleaned[i + 3] != '=')
        {
            outDescription += static_cast<char>(block & 0xFF);
        }
    }
    return !outDescription.empty();
}

IceLink::~IceLink()
{
    Stop();
}

void IceLink::OnState(juice_agent_t*, int state, void* user)
{
    auto* link = static_cast<IceLink*>(user);
    std::lock_guard lock(link->m_mutex);
    switch (state)
    {
    case JUICE_STATE_GATHERING:
        link->m_state = State::Gathering;
        break;
    case JUICE_STATE_CONNECTING:
        link->m_state = State::Connecting;
        break;
    case JUICE_STATE_CONNECTED:
    case JUICE_STATE_COMPLETED:
        if (link->m_state != State::Connected)
        {
            PRED_LOG_INFO(Network, "Punched through to the other player");
        }
        link->m_state = State::Connected;
        link->m_message.clear();
        break;
    case JUICE_STATE_FAILED:
        link->m_state = State::Failed;
        link->m_message = "Could not get through to them. One of the two networks is handing out a "
                          "different hole for every destination, and nothing either end can do "
                          "opens that. Playing on one network, or a virtual network tool, is the "
                          "way round it.";
        PRED_LOG_WARN(Network, "Hole punching failed");
        break;
    default:
        break;
    }
}

void IceLink::OnGatheringDone(juice_agent_t* agent, void* user)
{
    auto* link = static_cast<IceLink*>(user);
    // Asked for once the gathering is done rather than as each candidate arrives, so the code is
    // one complete thing to paste rather than a trickle of them.
    char description[JUICE_MAX_SDP_STRING_LEN] = {};
    if (juice_get_local_description(agent, description, sizeof(description)) < 0)
    {
        std::lock_guard lock(link->m_mutex);
        link->m_state = State::Failed;
        link->m_message = "Could not work out how this machine looks from outside.";
        return;
    }

    std::lock_guard lock(link->m_mutex);
    link->m_localCode = EncodeIceCode(description);
    if (link->m_state != State::Connecting && link->m_state != State::Connected)
    {
        link->m_state = State::Ready;
    }
}

void IceLink::OnReceive(juice_agent_t*, const char* data, size_t size, void* user)
{
    auto* link = static_cast<IceLink*>(user);
    std::lock_guard lock(link->m_mutex);
    if (link->m_incoming.size() >= kMaxQueued)
    {
        // The game is not reading as fast as this is arriving, which means it is not a game.
        return;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    link->m_incoming.emplace_back(bytes, bytes + size);
}

bool IceLink::Start(const Settings& settings)
{
    Stop();

    juice_config_t config{};
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    config.stun_server_host = settings.stunHost.c_str();
    config.stun_server_port = settings.stunPort;
    config.cb_state_changed = [](juice_agent_t* agent, juice_state_t state, void* user)
    { OnState(agent, static_cast<int>(state), user); };
    config.cb_gathering_done = &IceLink::OnGatheringDone;
    config.cb_recv = &IceLink::OnReceive;
    config.user_ptr = this;

    m_agent = juice_create(&config);
    if (m_agent == nullptr)
    {
        std::lock_guard lock(m_mutex);
        m_state = State::Failed;
        m_message = "Could not start looking for a way through.";
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_state = State::Gathering;
        m_message = "Working out how this connection looks from outside...";
        m_localCode.clear();
        m_hasRemote = false;
        m_incoming.clear();
    }

    if (juice_gather_candidates(m_agent) < 0)
    {
        std::lock_guard lock(m_mutex);
        m_state = State::Failed;
        m_message = "Could not reach a server to ask how this connection looks from outside.";
        return false;
    }
    return true;
}

void IceLink::Stop()
{
    if (m_agent != nullptr)
    {
        // Destroying the agent stops the callbacks, so nothing is writing to the queue by the time
        // it is cleared.
        juice_destroy(m_agent);
        m_agent = nullptr;
    }
    std::lock_guard lock(m_mutex);
    m_state = State::Idle;
    m_localCode.clear();
    m_message.clear();
    m_hasRemote = false;
    m_incoming.clear();
}

IceLink::State IceLink::Status() const
{
    std::lock_guard lock(m_mutex);
    return m_state;
}

std::string IceLink::LocalCode() const
{
    std::lock_guard lock(m_mutex);
    return m_localCode;
}

bool IceLink::HasRemote() const
{
    std::lock_guard lock(m_mutex);
    return m_hasRemote;
}

std::string IceLink::Message() const
{
    std::lock_guard lock(m_mutex);
    return m_message;
}

bool IceLink::SetRemoteCode(const std::string& code)
{
    if (m_agent == nullptr)
    {
        return false;
    }
    std::string description;
    if (!DecodeIceCode(code, description))
    {
        std::lock_guard lock(m_mutex);
        m_message = "That does not look like a code. It should start with PRED1: and be one long "
                    "line; a paste that has been broken across lines usually loses some of itself.";
        return false;
    }
    if (juice_set_remote_description(m_agent, description.c_str()) < 0)
    {
        std::lock_guard lock(m_mutex);
        m_message = "That code was not one this version understands.";
        return false;
    }
    std::lock_guard lock(m_mutex);
    m_hasRemote = true;
    m_message = "Dialling...";
    return true;
}

bool IceLink::Send(const uint8_t* data, size_t bytes)
{
    if (m_agent == nullptr || data == nullptr || bytes == 0)
    {
        return false;
    }
    return juice_send(m_agent, reinterpret_cast<const char*>(data), bytes) >= 0;
}

bool IceLink::Receive(std::vector<uint8_t>& out)
{
    std::lock_guard lock(m_mutex);
    if (m_incoming.empty())
    {
        return false;
    }
    out = std::move(m_incoming.front());
    m_incoming.pop_front();
    return true;
}

} // namespace pred
