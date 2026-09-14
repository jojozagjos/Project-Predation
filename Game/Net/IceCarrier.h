#pragma once

#include "Engine/Net/IceLink.h"
#include "Engine/Net/Transport.h"

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace pred
{

// The game's datagrams, over connections punched through routers.
//
// A punched hole joins exactly two machines, so one link is one other player. A client has one, to
// the host. A host has one per person who joined, which is the whole reason this holds a list: with
// a single link, a game over the internet was two players and no more however large the lobby said
// it was.
//
// The transport wants somewhere to put datagrams and somewhere to get them from, and that is all
// this is: an adapter, so that the sequence numbers, the acknowledgements and the ordering, which
// have nothing to do with how the bytes travel, are not written a second time. Each link becomes
// one of the transport's peers and goes through exactly the same machinery a real address would.
class IceCarrier final : public DatagramCarrier
{
public:
    IceCarrier() = default;
    explicit IceCarrier(std::shared_ptr<IceLink> link) { Add(std::move(link)); }

    // Adds a far end and returns which one it is. The host calls this once per invitation it hands
    // out, before the other machine has answered: a link that is still being negotiated simply has
    // nothing to receive and nothing that will send, and the transport times it out like any other
    // peer that never replied.
    size_t Add(std::shared_ptr<IceLink> link)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_links.push_back(std::move(link));
        return m_links.size() - 1;
    }

    size_t Links() const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_links.size();
    }

    std::shared_ptr<IceLink> At(size_t link) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return link < m_links.size() ? m_links[link] : nullptr;
    }

    bool Send(size_t link, const uint8_t* data, size_t bytes) override
    {
        const std::shared_ptr<IceLink> target = At(link);
        return target != nullptr && target->Send(data, bytes);
    }

    // Round-robin rather than draining each link in turn.
    //
    // Draining link 0 to exhaustion before looking at link 1 lets one player who is sending hard,
    // or one link with a backlog after a stall, hold the others out of the frame entirely. Starting
    // where the last call left off costs one integer and means a busy connection cannot starve a
    // quiet one.
    bool Receive(size_t& link, std::vector<uint8_t>& out) override
    {
        std::vector<std::shared_ptr<IceLink>> snapshot;
        size_t start = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            snapshot = m_links;
            if (snapshot.empty())
            {
                return false;
            }
            start = m_nextRead % snapshot.size();
        }
        for (size_t step = 0; step < snapshot.size(); ++step)
        {
            const size_t index = (start + step) % snapshot.size();
            if (snapshot[index] != nullptr && snapshot[index]->Receive(out))
            {
                link = index;
                std::lock_guard<std::mutex> lock(m_mutex);
                m_nextRead = index + 1;
                return true;
            }
        }
        return false;
    }

    bool Live(size_t link) const override
    {
        const std::shared_ptr<IceLink> target = At(link);
        return target != nullptr && target->Status() == IceLink::State::Connected;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<IceLink>> m_links;
    size_t m_nextRead = 0;
};

} // namespace pred
