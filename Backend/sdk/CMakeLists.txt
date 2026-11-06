# Backend/sdk/ — cross-language SDK: value model + marshaling, event bus, IDL + generators (step 11).
# Host-agnostic parts build headless here; the V8/LuaJIT/CoreCLR runtime adapters live under
# Backend/scripting/runtimes and are built only where those toolchains exist.
add_library(kcd2s_sdk STATIC marshal.cpp idl.cpp gen.cpp shared_state.cpp nui.cpp)
add_library(kcd2s::sdk ALIAS kcd2s_sdk)

target_include_directories(kcd2s_sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcd2s_sdk PUBLIC kcd2s::core kcd2s::flags)

# kcd2s-gen CLI: regenerate the per-language bindings from api.idl.
add_executable(kcd2s-gen tools/gen_main.cpp)
target_link_libraries(kcd2s-gen PRIVATE kcd2s::sdk)

# Every build writes the bindings and the website's API reference (docs/api.json) to
# <build>/sdk. A cross build can't
# run the generator it just built, so it skips this (the native CI build makes the docs).
if(NOT CMAKE_CROSSCOMPILING)
  set(KCD2S_SDK_OUT ${CMAKE_BINARY_DIR}/sdk)
  add_custom_command(
    OUTPUT ${KCD2S_SDK_OUT}/docs/api.json ${KCD2S_SDK_OUT}/ts/kcd2s.d.ts
    COMMAND kcd2s-gen ${CMAKE_CURRENT_SOURCE_DIR}/api.idl ${KCD2S_SDK_OUT}
    DEPENDS kcd2s-gen ${CMAKE_CURRENT_SOURCE_DIR}/api.idl
    COMMENT "kcd2s-gen: SDK bindings and API docs"
    VERBATIM)
  add_custom_target(kcd2s_sdk_docs ALL DEPENDS ${KCD2S_SDK_OUT}/docs/api.json)
endif()

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME sdk.marshal TARGET test_sdk_marshal
                 SOURCES tests/marshal_test.cpp
                 LIBS kcd2s::sdk)
  kcd2s_add_test(NAME sdk.idl_gen TARGET test_sdk_idl_gen
                 SOURCES tests/idl_gen_test.cpp
                 LIBS kcd2s::sdk)
  kcd2s_add_test(NAME sdk.shared_state TARGET test_sdk_shared_state
                 SOURCES tests/shared_state_test.cpp
                 LIBS kcd2s::sdk)
  kcd2s_add_test(NAME sdk.nui TARGET test_sdk_nui
                 SOURCES tests/nui_test.cpp
                 LIBS kcd2s::sdk)
  target_compile_definitions(test_sdk_idl_gen PRIVATE
    KCD2S_API_IDL="${CMAKE_CURRENT_SOURCE_DIR}/api.idl")
endif()
