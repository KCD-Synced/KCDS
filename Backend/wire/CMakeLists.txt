# Backend/wire/ — KSTP datagram framing (build-order step 3).
add_library(kcd2s_wire STATIC packet.cpp)
add_library(kcd2s::wire ALIAS kcd2s_wire)

target_include_directories(kcd2s_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_wire PUBLIC kcd2s::core kcd2s::flags)

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME wire.packet TARGET test_wire_packet
                 SOURCES tests/packet_test.cpp
                 LIBS kcd2s::wire)
endif()
