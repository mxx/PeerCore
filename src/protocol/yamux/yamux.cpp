#include "yamux.hpp"

// TODO: implement Yamux framing per spec:
// https://github.com/hashicorp/yamux/blob/master/spec.md

namespace peercore::protocol::yamux {

// ── YamuxStream ───────────────────────────────────────────────────────────────

YamuxStream::YamuxStream(StreamId id, ConnectionId conn_id)
    : id_(id), conn_id_(conn_id) {}

StreamId     YamuxStream::id()            const { return id_; }
ConnectionId YamuxStream::connection_id() const { return conn_id_; }

Result<size_t> YamuxStream::try_read(MutableBytes buf) {
    if (recv_buf_.empty()) return Result<size_t>::err("EAGAIN");
    size_t n = std::min(buf.size(), recv_buf_.size());
    for (size_t i = 0; i < n; ++i) {
        buf[i] = recv_buf_.front();
        recv_buf_.pop_front();
    }
    return Result<size_t>::ok(n);
}

Result<size_t> YamuxStream::try_write(ConstBytes data) {
    // TODO: enqueue data frame into YamuxSession send buffer
    (void)data;
    return Result<size_t>::err("YamuxStream::try_write not implemented");
}

Result<void> YamuxStream::close_write() {
    write_closed_ = true;
    // TODO: send FIN frame
    return Result<void>::ok();
}

Result<void> YamuxStream::reset() {
    open_ = false;
    // TODO: send RST frame
    return Result<void>::ok();
}

bool YamuxStream::is_open() const { return open_; }

std::optional<ProtocolId> YamuxStream::negotiated_protocol() const {
    return protocol_;
}

void YamuxStream::receive_data(std::vector<uint8_t> data) {
    for (auto b : data) recv_buf_.push_back(b);
}

void YamuxStream::receive_fin() { open_ = false; }
void YamuxStream::receive_rst() { open_ = false; }
void YamuxStream::set_protocol(ProtocolId proto) { protocol_ = std::move(proto); }

// ── YamuxSession ──────────────────────────────────────────────────────────────

YamuxSession::YamuxSession(ConnectionId conn_id, bool is_client)
    : conn_id_(conn_id)
    , is_client_(is_client)
    , next_stream_id_(is_client ? 1 : 2) {}

Result<void> YamuxSession::receive(ConstBytes data) {
    recv_buf_.insert(recv_buf_.end(), data.begin(), data.end());
    process_frames();
    return Result<void>::ok();
}

std::vector<uint8_t> YamuxSession::drain_outgoing() {
    return std::exchange(send_buf_, {});
}

Result<std::shared_ptr<YamuxStream>> YamuxSession::open_stream() {
    StreamId sid = next_stream_id_;
    next_stream_id_ += 2;

    auto stream = std::make_shared<YamuxStream>(sid, conn_id_);
    streams_[sid] = stream;

    // TODO: send SYN frame
    return Result<std::shared_ptr<YamuxStream>>::ok(stream);
}

void YamuxSession::set_accept_callback(AcceptCallback cb) {
    accept_cb_ = std::move(cb);
}

void YamuxSession::process_frames() {
    YamuxHeader hdr;
    std::vector<uint8_t> payload;
    while (try_parse_frame(hdr, payload)) {
        handle_frame(hdr, std::move(payload));
    }
}

bool YamuxSession::try_parse_frame(YamuxHeader& /*hdr*/,
                                    std::vector<uint8_t>& /*payload*/) {
    // TODO: parse 12-byte header + payload from recv_buf_
    return false;
}

void YamuxSession::handle_frame(const YamuxHeader& /*hdr*/,
                                 std::vector<uint8_t> /*payload*/) {
    // TODO: dispatch to stream based on stream_id and frame type
}

void YamuxSession::write_frame(const YamuxHeader& /*hdr*/, ConstBytes /*payload*/) {
    // TODO: serialise header + payload into send_buf_
}

}  // namespace peercore::protocol::yamux
