add_library(peercore
    log.cpp
    types.cpp
    peer_store.cpp
    routing_table.cpp
    controller.cpp
    swarm.cpp
    node.cpp

    runtime/event_loop.cpp

    transport/tcp_transport.cpp

    protocol/multistream_select.cpp
    protocol/noise/noise.cpp
    protocol/yamux/yamux.cpp

    services/dht_service.cpp
    services/ping_service.cpp
    services/identify_service.cpp
)

target_include_directories(peercore
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(peercore
  PUBLIC
    Libsodium::Libsodium
)

target_compile_options(peercore PRIVATE
  -Wall -Wextra -Wpedantic
)

# Optional: allow overriding compile-time log level via cmake -DPEERCORE_LOG_LEVEL=N
if(DEFINED PEERCORE_LOG_LEVEL)
    target_compile_definitions(peercore
        PUBLIC PEERCORE_LOG_LEVEL=${PEERCORE_LOG_LEVEL}
    )
endif()
