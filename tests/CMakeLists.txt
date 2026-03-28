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

add_peercore_test(test_types)
add_peercore_test(test_peer_store)
add_peercore_test(test_routing_table)
add_peercore_test(test_swarm)
add_peercore_test(test_event_loop)
add_peercore_test(test_tcp_transport)
add_peercore_test(test_connection_session)
add_peercore_test(test_multistream_select)
add_peercore_test(test_noise)
