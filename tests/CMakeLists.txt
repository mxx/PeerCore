include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

function(add_peercore_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE peercore GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_library(interop_probe_support
  interop/probe/probe_app.cpp
)
target_include_directories(interop_probe_support
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/interop
)
target_link_libraries(interop_probe_support PUBLIC peercore PRIVATE Libsodium::Libsodium)

add_executable(interop_peercore_probe interop/peercore_probe.cpp)
target_link_libraries(interop_peercore_probe PRIVATE interop_probe_support peercore Libsodium::Libsodium)

add_executable(interop_noise_trace interop/noise_trace.cpp)
target_link_libraries(interop_noise_trace PRIVATE peercore Libsodium::Libsodium)

if(PEERCORE_ENABLE_NETWORK_SMOKE_TESTS)
  add_test(NAME official_adapter_smoke
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/interop/official/smoke.sh"
            "${CMAKE_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/tests/interop_peercore_probe"
  )
  set_tests_properties(official_adapter_smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_peercore_test(test_types)
add_peercore_test(test_peer_store)
add_peercore_test(test_routing_table)
add_peercore_test(test_swarm)
add_peercore_test(test_event_loop)
add_peercore_test(test_tcp_transport)
add_peercore_test(test_connection_session)
add_peercore_test(test_multistream_select)
add_peercore_test(test_noise)
add_peercore_test(test_yamux)
add_peercore_test(test_yamux_replay)
target_compile_definitions(test_yamux_replay
  PRIVATE PEERCORE_YAMUX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/yamux"
)
add_peercore_test(test_probe_app)
target_link_libraries(test_probe_app PRIVATE interop_probe_support)
