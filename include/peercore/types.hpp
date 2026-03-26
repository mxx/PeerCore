#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace peercore {

// ── Identity ──────────────────────────────────────────────────────────────────

// 32-byte Ed25519 public key used as peer identity
struct PeerId {
    std::array<uint8_t, 32> bytes{};

    bool operator==(const PeerId&) const = default;
    std::string to_string() const;
    static PeerId from_bytes(std::span<const uint8_t, 32> b);
};

struct Identity {
    PeerId peer_id;
    std::array<uint8_t, 64> secret_key{};  // Ed25519 secret key (seed || pubkey)
};

// ── Addresses ─────────────────────────────────────────────────────────────────

// Minimal Multiaddr (stored as serialised bytes, decoded on demand)
struct Multiaddr {
    std::vector<uint8_t> bytes;

    explicit Multiaddr(std::string_view text);
    std::string to_string() const;
};

// ── Handles & IDs ─────────────────────────────────────────────────────────────

using ConnectionId = uint64_t;
using StreamId     = uint32_t;
using ProtocolId   = std::string;  // e.g. "/ipfs/ping/1.0.0"

// ── Byte spans ────────────────────────────────────────────────────────────────

using ConstBytes   = std::span<const uint8_t>;
using MutableBytes = std::span<uint8_t>;

// ── Result<T> ─────────────────────────────────────────────────────────────────

struct Error {
    std::string message;
    explicit Error(std::string msg) : message(std::move(msg)) {}
};

template <typename T>
class Result {
public:
    static Result ok(T value) { return Result(std::move(value)); }
    static Result err(std::string msg) { return Result(Error(std::move(msg))); }

    bool is_ok()  const { return std::holds_alternative<T>(data_); }
    bool is_err() const { return std::holds_alternative<Error>(data_); }

    T&       value()       { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }
    const Error& error() const { return std::get<Error>(data_); }

private:
    explicit Result(T v)     : data_(std::move(v)) {}
    explicit Result(Error e) : data_(std::move(e)) {}
    std::variant<T, Error> data_;
};

template <>
class Result<void> {
public:
    static Result ok()                  { return Result(true); }
    static Result err(std::string msg)  { return Result(std::move(msg)); }

    bool is_ok()  const { return ok_; }
    bool is_err() const { return !ok_; }
    const std::string& error_message() const { return err_msg_; }

private:
    explicit Result(bool)        : ok_(true) {}
    explicit Result(std::string msg) : ok_(false), err_msg_(std::move(msg)) {}
    bool ok_{false};
    std::string err_msg_;
};

// ── Debug ─────────────────────────────────────────────────────────────────────

struct DebugSnapshot {
    uint32_t    connection_count{0};
    uint32_t    stream_count{0};
    std::string extra;
};

}  // namespace peercore
