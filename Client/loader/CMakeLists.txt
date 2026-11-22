# The KCDS loader (kcds_loader.dll): injected before the payload, owns the game's hooks so a payload
# (the game client of one KCDS release) can be swapped. The hook bookkeeping builds everywhere for
# its tests; the DLL only with the Windows game client.
add_library(kcds_loader_core STATIC hook_registry.cpp)
target_include_directories(kcds_loader_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kcds_loader_core PUBLIC kcd2s::flags)
add_library(kcds::loader_core ALIAS kcds_loader_core)

# The loader/payload interface, header only (the payload's MinHook bridge includes it).
add_library(kcds_loader_abi INTERFACE)
target_include_directories(kcds_loader_abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(kcds::loader_abi ALIAS kcds_loader_abi)

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME loader.hook_registry TARGET test_loader_hook_registry
                 SOURCES tests/hook_registry_test.cpp LIBS kcds::loader_core)
endif()

if(WIN32 AND KCD2S_BUILD_CLIENT AND KCD2S_BUILD_GAME_CLIENT)
  set(KCD2S_MINHOOK_DIR "" CACHE PATH "MinHook source root")
  if(NOT EXISTS "${KCD2S_MINHOOK_DIR}/include/MinHook.h")
    message(FATAL_ERROR "KCD2S_MINHOOK_DIR must contain MinHook source")
  endif()
  # The one real MinHook of the game: the loader's. The payload's MH_* calls come here.
  add_library(kcds_loader_minhook STATIC
    ${KCD2S_MINHOOK_DIR}/src/buffer.c ${KCD2S_MINHOOK_DIR}/src/hook.c
    ${KCD2S_MINHOOK_DIR}/src/trampoline.c ${KCD2S_MINHOOK_DIR}/src/hde/hde64.c)
  target_include_directories(kcds_loader_minhook PUBLIC ${KCD2S_MINHOOK_DIR}/include)
  if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(kcds_loader_minhook PRIVATE /FIintrin.h)
  endif()
  add_library(kcds-loader SHARED loader_main.cpp minhook_backend.cpp)
  set_target_properties(kcds-loader PROPERTIES OUTPUT_NAME kcds_loader)
  target_link_libraries(kcds-loader PRIVATE kcds::loader_core kcds_loader_minhook)
endif()
