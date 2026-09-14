#pragma once

#include "Engine/Net/IceLink.h"
#include "Engine/Net/Transport.h"

#include <memory>

namespace pred
{

// The game's datagrams, over a connection punched through two routers.
//
// The link does the punching and then behaves like a pipe with one far end. The transport wants
// somewhere to put datagrams and somewhere to get them from, and that is all this is: forty lines
// of adapter so that the sequence numbers, the acknowledgements and the ordering, which have
// nothing to do with how the bytes travel, are not written a second time.
class IceCarrier final : public DatagramCarrier
{
public:
    explicit IceCarrier(std::shared_ptr<IceLink> link) : m_link(std::move(link)) {}

    bool Send(const uint8_t* data, size_t bytes) override
    {
        return m_link != nullptr && m_link->Send(data, bytes);
    }

    bool Receive(std::vector<uint8_t>& out) override
    {
        return m_link != nullptr && m_link->Receive(out);
    }

    bool Live() const override
    {
        return m_link != nullptr && m_link->Status() == IceLink::State::Connected;
    }

private:
    std::shared_ptr<IceLink> m_link;
};

} // namespace pred
