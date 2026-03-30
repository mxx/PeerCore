#include "yamux.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace peercore::protocol::yamux {

namespace {

constexpr uint16_t flag_value(YamuxFlag flag) {
    return static_cast<uint16_t>(flag);
}

uint16_t read_be16(ConstBytes bytes) {
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) |
                                 static_cast<uint16_t>(bytes[1]));
}

uint32_t read_be32(ConstBytes bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

void append_be16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

bool has_flag(uint16_t flags, YamuxFlag flag) {
    return (flags & flag_value(flag)) != 0;
}

}  // namespace

YamuxStream::YamuxStream(StreamId id,
                         ConnectionId conn_id,
                         WriteCallback write_cb,
                         WindowUpdateCallback window_update_cb,
                         CloseCallback close_write_cb,
                         CloseCallback reset_cb)
    : id_(id)
    , conn_id_(conn_id)
    , write_cb_(std::move(write_cb))
    , window_update_cb_(std::move(window_update_cb))
    , close_write_cb_(std::move(close_write_cb))
    , reset_cb_(std::move(reset_cb)) {}

StreamId YamuxStream::id() const { return id_; }
ConnectionId YamuxStream::connection_id() const { return conn_id_; }

Result<size_t> YamuxStream::try_read(MutableBytes buf) {
    if (recv_buf_.empty()) {
        return (read_closed_ || reset_) ? Result<size_t>::ok(0)
                                        : Result<size_t>::err("EAGAIN");
    }

    const size_t n = std::min(buf.size(), recv_buf_.size());
    for (size_t i = 0; i < n; ++i) {
        buf[i] = recv_buf_.front();
        recv_buf_.pop_front();
    }
    if (window_update_cb_) {
        (void)window_update_cb_(id_, n);
    }
    return Result<size_t>::ok(n);
}

Result<size_t> YamuxStream::try_write(ConstBytes data) {
    if (write_closed_ || reset_) {
        return Result<size_t>::err("stream is closed for writing");
    }
    return write_cb_(id_, data);
}

Result<void> YamuxStream::close_write() {
    if (write_closed_) return Result<void>::ok();
    write_closed_ = true;
    return close_write_cb_(id_);
}

Result<void> YamuxStream::reset() {
    read_closed_ = true;
    write_closed_ = true;
    reset_ = true;
    recv_buf_.clear();
    return reset_cb_(id_);
}

bool YamuxStream::is_open() const {
    if (reset_) return false;
    return !read_closed_ || !write_closed_ || !recv_buf_.empty();
}

std::optional<ProtocolId> YamuxStream::negotiated_protocol() const {
    return protocol_;
}

void YamuxStream::receive_data(std::vector<uint8_t> data) {
    for (const auto b : data) recv_buf_.push_back(b);
}

void YamuxStream::receive_fin() {
    read_closed_ = true;
}

void YamuxStream::receive_rst() {
    read_closed_ = true;
    write_closed_ = true;
    reset_ = true;
    recv_buf_.clear();
}

void YamuxStream::set_protocol(ProtocolId proto) {
    protocol_ = std::move(proto);
}

void YamuxStream::close_local_write() {
    write_closed_ = true;
}

bool YamuxStream::read_closed() const {
    return read_closed_ || reset_;
}

bool YamuxStream::write_closed() const {
    return write_closed_ || reset_;
}

YamuxSession::YamuxSession(ConnectionId conn_id, bool is_client)
    : conn_id_(conn_id)
    , is_client_(is_client)
    , next_stream_id_(is_client ? 1u : 2u) {}

Result<void> YamuxSession::receive(ConstBytes data) {
    recv_buf_.insert(recv_buf_.end(), data.begin(), data.end());
    process_frames();
    return Result<void>::ok();
}

std::vector<uint8_t> YamuxSession::drain_outgoing() {
    return std::exchange(send_buf_, {});
}

Result<std::shared_ptr<YamuxStream>> YamuxSession::open_stream() {
    if (remote_go_away_) {
        return Result<std::shared_ptr<YamuxStream>>::err("yamux session is shutting down");
    }
    const StreamId sid = next_stream_id_;
    next_stream_id_ += 2;

    auto stream = get_or_create_stream(sid, true);
    write_frame(YamuxHeader{
        .version = kYamuxVersion,
        .type = static_cast<uint8_t>(YamuxType::Data),
        .flags = flag_value(YamuxFlag::SYN),
        .stream_id = sid,
        .length = 0,
    });
    if (outgoing_cb_) {
        auto flushed = outgoing_cb_();
        if (flushed.is_err()) {
            return Result<std::shared_ptr<YamuxStream>>::err(flushed.error_message());
        }
    }
    return Result<std::shared_ptr<YamuxStream>>::ok(stream);
}

void YamuxSession::set_accept_callback(AcceptCallback cb) {
    accept_cb_ = std::move(cb);
}

void YamuxSession::set_outgoing_callback(OutgoingCallback cb) {
    outgoing_cb_ = std::move(cb);
}

void YamuxSession::set_close_callback(CloseCallback cb) {
    close_cb_ = std::move(cb);
}

std::shared_ptr<YamuxStream> YamuxSession::get_or_create_stream(StreamId sid,
                                                                bool open_if_missing) {
    if (const auto it = streams_.find(sid); it != streams_.end()) {
        return it->second;
    }
    if (!open_if_missing) return {};

    auto stream = std::make_shared<YamuxStream>(
        sid,
        conn_id_,
        [this](StreamId stream_id, ConstBytes payload) {
            return write_stream_data(stream_id, payload);
        },
        [this](StreamId stream_id, size_t delta) {
            return update_receive_window(stream_id, delta);
        },
        [this](StreamId stream_id) { return close_stream_write(stream_id); },
        [this](StreamId stream_id) { return reset_stream(stream_id); });
    streams_[sid] = stream;
    send_windows_[sid] = kInitialStreamWindow;
    return stream;
}

Result<size_t> YamuxSession::write_stream_data(StreamId sid, ConstBytes data) {
    auto it = send_windows_.find(sid);
    if (it == send_windows_.end()) {
        return Result<size_t>::err("unknown yamux stream");
    }

    const size_t writable = std::min<size_t>(data.size(), it->second);
    if (writable == 0) {
        return Result<size_t>::err("EAGAIN");
    }

    write_frame(YamuxHeader{
        .version = kYamuxVersion,
        .type = static_cast<uint8_t>(YamuxType::Data),
        .flags = 0,
        .stream_id = sid,
        .length = static_cast<uint32_t>(writable),
    }, data.first(writable));
    it->second -= static_cast<uint32_t>(writable);
    if (outgoing_cb_) {
        auto flushed = outgoing_cb_();
        if (flushed.is_err()) return Result<size_t>::err(flushed.error_message());
    }
    return Result<size_t>::ok(writable);
}

Result<void> YamuxSession::acknowledge_stream(StreamId sid) {
    write_frame(YamuxHeader{
        .version = kYamuxVersion,
        .type = static_cast<uint8_t>(YamuxType::Data),
        .flags = flag_value(YamuxFlag::ACK),
        .stream_id = sid,
        .length = 0,
    });
    if (outgoing_cb_) return outgoing_cb_();
    return Result<void>::ok();
}

Result<void> YamuxSession::close_stream_write(StreamId sid) {
    write_frame(YamuxHeader{
        .version = kYamuxVersion,
        .type = static_cast<uint8_t>(YamuxType::Data),
        .flags = flag_value(YamuxFlag::FIN),
        .stream_id = sid,
        .length = 0,
    });
    if (const auto it = streams_.find(sid); it != streams_.end()) {
        it->second->close_local_write();
    }
    if (outgoing_cb_) return outgoing_cb_();
    return Result<void>::ok();
}

Result<void> YamuxSession::reset_stream(StreamId sid) {
    write_frame(YamuxHeader{
        .version = kYamuxVersion,
        .type = static_cast<uint8_t>(YamuxType::Data),
        .flags = flag_value(YamuxFlag::RST),
        .stream_id = sid,
        .length = 0,
    });
    erase_stream(sid);
    if (outgoing_cb_) return outgoing_cb_();
    return Result<void>::ok();
}

Result<void> YamuxSession::update_receive_window(StreamId sid, size_t delta) {
    if (delta == 0) return Result<void>::ok();

    write_frame(YamuxHeader{
        .version = kYamuxVersion,
        .type = static_cast<uint8_t>(YamuxType::WindowUpdate),
        .flags = 0,
        .stream_id = sid,
        .length = static_cast<uint32_t>(delta),
    });
    if (outgoing_cb_) return outgoing_cb_();
    return Result<void>::ok();
}

Result<void> YamuxSession::send_ping_ack(uint32_t opaque_value) {
    write_frame(YamuxHeader{
        .version = kYamuxVersion,
        .type = static_cast<uint8_t>(YamuxType::Ping),
        .flags = flag_value(YamuxFlag::ACK),
        .stream_id = 0,
        .length = opaque_value,
    });
    if (outgoing_cb_) return outgoing_cb_();
    return Result<void>::ok();
}

Result<void> YamuxSession::send_go_away(YamuxGoAwayCode code) {
    write_frame(YamuxHeader{
        .version = kYamuxVersion,
        .type = static_cast<uint8_t>(YamuxType::GoAway),
        .flags = 0,
        .stream_id = 0,
        .length = static_cast<uint32_t>(code),
    });
    if (outgoing_cb_) return outgoing_cb_();
    return Result<void>::ok();
}

void YamuxSession::erase_stream(StreamId sid) {
    streams_.erase(sid);
    send_windows_.erase(sid);
}

bool YamuxSession::is_remote_stream_id(StreamId sid) const {
    if (sid == 0) return false;
    if (is_client_) return (sid % 2u) == 0u;
    return (sid % 2u) == 1u;
}

void YamuxSession::process_frames() {
    YamuxHeader hdr;
    std::vector<uint8_t> payload;
    while (try_parse_frame(hdr, payload)) {
        handle_frame(hdr, std::move(payload));
        payload.clear();
    }
}

bool YamuxSession::try_parse_frame(YamuxHeader& hdr,
                                   std::vector<uint8_t>& payload) {
    constexpr size_t kHeaderSize = 12;
    if (recv_buf_.size() < kHeaderSize) return false;

    ConstBytes header(recv_buf_.data(), kHeaderSize);
    hdr.version = header[0];
    hdr.type = header[1];
    hdr.flags = read_be16(header.subspan(2, 2));
    hdr.stream_id = read_be32(header.subspan(4, 4));
    hdr.length = read_be32(header.subspan(8, 4));

    const size_t payload_size =
        hdr.type == static_cast<uint8_t>(YamuxType::Data) ? hdr.length : 0u;
    const size_t frame_size = kHeaderSize + payload_size;
    if (recv_buf_.size() < frame_size) return false;

    payload.assign(recv_buf_.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                   recv_buf_.begin() + static_cast<std::ptrdiff_t>(frame_size));
    recv_buf_.erase(recv_buf_.begin(),
                    recv_buf_.begin() + static_cast<std::ptrdiff_t>(frame_size));
    return true;
}

void YamuxSession::handle_frame(const YamuxHeader& hdr,
                                std::vector<uint8_t> payload) {
    if (hdr.version != kYamuxVersion) return;

    if (hdr.type == static_cast<uint8_t>(YamuxType::Ping)) {
        if (!has_flag(hdr.flags, YamuxFlag::ACK)) {
            (void)send_ping_ack(hdr.length);
        }
        return;
    }

    if (hdr.type == static_cast<uint8_t>(YamuxType::GoAway)) {
        remote_go_away_ = true;
        return;
    }

    const bool is_stream_frame = hdr.type == static_cast<uint8_t>(YamuxType::Data) ||
                                 hdr.type == static_cast<uint8_t>(YamuxType::WindowUpdate);
    if (!is_stream_frame || hdr.stream_id == 0) {
        return;
    }

    const bool open_if_missing = has_flag(hdr.flags, YamuxFlag::SYN);
    if (open_if_missing && !is_remote_stream_id(hdr.stream_id)) {
        (void)send_go_away(YamuxGoAwayCode::ProtocolError);
        return;
    }

    const bool existed = streams_.find(hdr.stream_id) != streams_.end();
    auto stream = get_or_create_stream(hdr.stream_id, open_if_missing);
    if (!stream) return;

    if (open_if_missing && !existed && accept_cb_) {
        accept_cb_(stream);
    }
    if (open_if_missing && !existed) {
        (void)acknowledge_stream(hdr.stream_id);
    }

    if (hdr.type == static_cast<uint8_t>(YamuxType::WindowUpdate) && hdr.length > 0) {
        auto it = send_windows_.find(hdr.stream_id);
        if (it != send_windows_.end()) {
            const uint64_t next = static_cast<uint64_t>(it->second) + hdr.length;
            it->second = static_cast<uint32_t>(std::min<uint64_t>(next, UINT32_MAX));
        }
    }

    if (!payload.empty()) {
        stream->receive_data(std::move(payload));
    }
    if (has_flag(hdr.flags, YamuxFlag::FIN)) {
        stream->receive_fin();
        if (close_cb_ && !stream->is_open()) {
            close_cb_(hdr.stream_id, "yamux stream closed");
        }
    }
    if (has_flag(hdr.flags, YamuxFlag::RST)) {
        stream->receive_rst();
        if (close_cb_) {
            close_cb_(hdr.stream_id, "yamux stream reset by peer");
        }
        erase_stream(hdr.stream_id);
    }
}

void YamuxSession::write_frame(const YamuxHeader& hdr, ConstBytes payload) {
    send_buf_.push_back(hdr.version);
    send_buf_.push_back(hdr.type);
    append_be16(send_buf_, hdr.flags);
    append_be32(send_buf_, hdr.stream_id);
    append_be32(send_buf_, hdr.length);
    send_buf_.insert(send_buf_.end(), payload.begin(), payload.end());
}

}  // namespace peercore::protocol::yamux
