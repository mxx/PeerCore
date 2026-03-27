#pragma once
// ── peercore/log.hpp ─────────────────────────────────────────────────────────
// Lightweight, zero-overhead, sink-replaceable logging for embedded library use.
//
// Compile-time gate (set in CMake or before including):
//   0=Trace  1=Debug  2=Info  3=Warn  4=Error  5=Off
// Default: Debug in debug builds, Info in release builds.
//
// Usage (library internals):
//   PEERCORE_LOG_INFO("peercore/swarm", "dialing {}", addr.to_string());
//
// Usage (application / test setup):
//   peercore::log::set_sink([](Level l, std::string_view comp,
//                               std::string_view msg){ ... });
//   peercore::log::set_level(peercore::log::Level::Debug);

#include <functional>
#include <string_view>

#if __has_include(<format>)
#  include <format>
#  define PEERCORE_HAS_FORMAT 1
#endif

namespace peercore::log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Off   = 5,
};

// Sink receives every message that passes the level filter.
// Must be thread-safe if the library is used from multiple threads.
using Sink = std::function<void(Level, std::string_view component, std::string_view msg)>;

// Replace the global sink. Pass nullptr to restore the default (stderr).
// Not thread-safe to call concurrently with log emission — call once at startup.
void set_sink(Sink sink);

// Runtime level gate — independent of PEERCORE_LOG_LEVEL compile-time gate.
void  set_level(Level level);
Level get_level();

// Internal: called by macros only.
void write(Level level, std::string_view component, std::string_view msg);

} // namespace peercore::log

// ── Compile-time level ────────────────────────────────────────────────────────
#ifndef PEERCORE_LOG_LEVEL
#  if defined(NDEBUG)
#    define PEERCORE_LOG_LEVEL 2   // Release → Info+
#  else
#    define PEERCORE_LOG_LEVEL 1   // Debug builds → Debug+
#  endif
#endif

// ── Internal formatting helper ────────────────────────────────────────────────
#ifdef PEERCORE_HAS_FORMAT
#  define PEERCORE_LOG_(lvl, comp, ...) \
     ::peercore::log::write((lvl), (comp), std::format(__VA_ARGS__))
#else
// Fallback for compilers without <format>: emit the literal format string only.
#  define PEERCORE_LOG_(lvl, comp, fmt, ...) \
     ::peercore::log::write((lvl), (comp), (fmt))
#endif

// ── Public macros ─────────────────────────────────────────────────────────────
#if PEERCORE_LOG_LEVEL <= 0
#  define PEERCORE_LOG_TRACE(comp, ...) \
     PEERCORE_LOG_(::peercore::log::Level::Trace, (comp), __VA_ARGS__)
#else
#  define PEERCORE_LOG_TRACE(comp, ...) do {} while(false)
#endif

#if PEERCORE_LOG_LEVEL <= 1
#  define PEERCORE_LOG_DEBUG(comp, ...) \
     PEERCORE_LOG_(::peercore::log::Level::Debug, (comp), __VA_ARGS__)
#else
#  define PEERCORE_LOG_DEBUG(comp, ...) do {} while(false)
#endif

#if PEERCORE_LOG_LEVEL <= 2
#  define PEERCORE_LOG_INFO(comp, ...) \
     PEERCORE_LOG_(::peercore::log::Level::Info, (comp), __VA_ARGS__)
#else
#  define PEERCORE_LOG_INFO(comp, ...) do {} while(false)
#endif

#if PEERCORE_LOG_LEVEL <= 3
#  define PEERCORE_LOG_WARN(comp, ...) \
     PEERCORE_LOG_(::peercore::log::Level::Warn, (comp), __VA_ARGS__)
#else
#  define PEERCORE_LOG_WARN(comp, ...) do {} while(false)
#endif

#if PEERCORE_LOG_LEVEL <= 4
#  define PEERCORE_LOG_ERROR(comp, ...) \
     PEERCORE_LOG_(::peercore::log::Level::Error, (comp), __VA_ARGS__)
#else
#  define PEERCORE_LOG_ERROR(comp, ...) do {} while(false)
#endif
