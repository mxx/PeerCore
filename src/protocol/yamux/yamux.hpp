#pragma once

#include "../../../include/peercore/muxed_stream.hpp"
#include "../../../include/peercore/types.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace peercore::protocol::yamux {

// Yamux multiplexer frame header (12 bytes, big-endian)
struct YamuxHeader {
    uint8_t  version{0};
    uint8_t  type{0};    // Data=0, WindowUpdate=1, Ping=2, GoAway=3
    uint16_t flags{0};   // SYN=1, ACK=2, FIN=4, RST=8
    uint32_t stream_id{0};
    uint32_t length{0};
};

constexpr uint8_t kYamuxVersion = 0;

enum class YamuxType : uint8_t { Data = 0, WindowUpdate = 1, Ping = 2, GoAway = 3 };
enum class YamuxFlag : uint16_t { SYN = 1, ACK = 2, FIN = 4, RST = 8 };

// Single yamux-managed stream
class YamuxStream : public MuxedStream {
public:
    using WriteCallback = std::function<Result<void>(StreamId, ConstBytes)>;
    using CloseCallback = std::function<Result<void>(StreamId)>;

    YamuxStream(StreamId id,
                ConnectionId conn_id,
                WriteCallback write_cb,
                CloseCallback close_write_cb,
                CloseCallback reset_cb);

    StreamId     id()            const override;
    ConnectionId connection_id() const override;

    Result<size_t> try_read(MutableBytes buf)  override;
    Result<size_t> try_write(ConstBytes data)  override;
    Result<void>   close_write()               override;
    Result<void>   reset()                     override;
    bool           is_open()           const   override;

    std::optional<ProtocolId> negotiated_protocol() const override;

    // Called by YamuxSession when data arrives for this stream
    void receive_data(std::vector<uint8_t> data);
    void receive_fin();
    void receive_rst();

    void set_protocol(ProtocolId proto);
    void close_local_write();
    bool read_closed() const;
    bool write_closed() const;

private:
    StreamId     id_;
    ConnectionId conn_id_;
    bool         read_closed_{false};
    bool         write_closed_{false};
    bool         reset_{false};
    WriteCallback write_cb_;
    CloseCallback close_write_cb_;
    CloseCallback reset_cb_;

    std::deque<uint8_t>     recv_buf_;
    std::optional<ProtocolId> protocol_;
};

// Per-connection yamux session
class YamuxSession {
public:
    using AcceptCallback = std::function<void(std::shared_ptr<YamuxStream>)>;
    using OutgoingCallback = std::function<Result<void>()>;
    using CloseCallback = std::function<void(StreamId, std::string)>;

    explicit YamuxSession(ConnectionId conn_id, bool is_client);

    // Feed incoming raw bytes from the socket
    Result<void> receive(ConstBytes data);

    // Drain outgoing bytes to be written to the socket
    std::vector<uint8_t> drain_outgoing();

    // Open a new outbound stream
    Result<std::shared_ptr<YamuxStream>> open_stream();

    // Set callback for inbound streams (server side)
    void set_accept_callback(AcceptCallback cb);
    void set_outgoing_callback(OutgoingCallback cb);
    void set_close_callback(CloseCallback cb);

private:
    ConnectionId conn_id_;
    uint32_t     next_stream_id_;

    std::unordered_map<StreamId, std::shared_ptr<YamuxStream>> streams_;
    std::vector<uint8_t>  recv_buf_;   // unparsed incoming bytes
    std::vector<uint8_t>  send_buf_;   // pending outgoing bytes
    AcceptCallback        accept_cb_;
    OutgoingCallback      outgoing_cb_;
    CloseCallback         close_cb_;

    std::shared_ptr<YamuxStream> get_or_create_stream(StreamId sid, bool open_if_missing);
    Result<void> write_stream_data(StreamId sid, ConstBytes data);
    Result<void> close_stream_write(StreamId sid);
    Result<void> reset_stream(StreamId sid);

    void        process_frames();
    bool        try_parse_frame(YamuxHeader& hdr, std::vector<uint8_t>& payload);
    void        handle_frame(const YamuxHeader& hdr, std::vector<uint8_t> payload);
    void        write_frame(const YamuxHeader& hdr, ConstBytes payload = {});
};

}  // namespace peercore::protocol::yamux
