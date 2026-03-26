#pragma once

#include "types.hpp"

#include <memory>
#include <optional>

namespace peercore {

class MuxedStream {
public:
    virtual ~MuxedStream() = default;

    virtual StreamId     id()            const = 0;
    virtual ConnectionId connection_id() const = 0;

    virtual Result<size_t> try_read(MutableBytes buf)  = 0;
    virtual Result<size_t> try_write(ConstBytes data)  = 0;

    virtual Result<void> close_write() = 0;
    virtual Result<void> reset()       = 0;

    virtual bool is_open() const = 0;

    virtual std::optional<ProtocolId> negotiated_protocol() const = 0;
};

using StreamHandle = std::shared_ptr<MuxedStream>;

}  // namespace peercore
