# Backend/transport/ — KSTP substrate: links + loss/reorder sim, sequence buffer, ack system (step 3).
add_library(kcd2s_transport STATIC datagram.cpp ack_system.cpp connection.cpp fragment.cpp
            handshake.cpp endpoint.cpp disconnect.cpp)
add_library(kcd2s::transport ALIAS kcd2s_transport)

target_include_directories(kcd2s_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_transport PUBLIC kcd2s::core kcd2s::wire kcd2s::flags)

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME transport.datagram TARGET test_transport_datagram
                 SOURCES tests/datagram_test.cpp
                 LIBS kcd2s::transport kcd2s::wire)
  kcd2s_add_test(NAME transport.sequence_buffer TARGET test_transport_seqbuf
                 SOURCES tests/sequence_buffer_test.cpp
                 LIBS kcd2s::transport)
  kcd2s_add_test(NAME transport.ack_system TARGET test_transport_ack
                 SOURCES tests/ack_system_test.cpp
                 LIBS kcd2s::transport)
  kcd2s_add_test(NAME transport.connection TARGET test_transport_connection
                 SOURCES tests/connection_test.cpp
                 LIBS kcd2s::transport kcd2s::wire)
  kcd2s_add_test(NAME transport.fragment TARGET test_transport_fragment
                 SOURCES tests/fragment_test.cpp
                 LIBS kcd2s::transport kcd2s::wire)
  kcd2s_add_test(NAME transport.handshake TARGET test_transport_handshake
                 SOURCES tests/handshake_test.cpp
                 LIBS kcd2s::transport kcd2s::wire)
  kcd2s_add_test(NAME transport.endpoint TARGET test_transport_endpoint
                 SOURCES tests/endpoint_test.cpp
                 LIBS kcd2s::transport kcd2s::wire)
  kcd2s_add_test(NAME transport.disconnect TARGET test_transport_disconnect
                 SOURCES tests/disconnect_test.cpp
                 LIBS kcd2s::transport kcd2s::wire)
endif()
