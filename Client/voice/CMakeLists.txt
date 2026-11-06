# Client/voice/: microphone to network and network to speakers. Knows nothing of the
# game, so tests and the quality bench run it without one.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_DISABLE_INTRINSICS OFF CACHE BOOL "" FORCE)
# clang-cl (the release build's compiler) counts as MSVC, so Opus gives its SSE4.1 and AVX files no
# target flags of their own and clang refuses their intrinsics: there they are left out, and Opus
# keeps its SSE and SSE2 code.
if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
  set(OPUS_X86_MAY_HAVE_SSE4_1 OFF CACHE BOOL "" FORCE)
  set(OPUS_X86_MAY_HAVE_AVX OFF CACHE BOOL "" FORCE)
  set(OPUS_X86_MAY_HAVE_AVX2 OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)

add_library(kcds_rnnoise STATIC
  third_party/rnnoise/src/celt_lpc.c
  third_party/rnnoise/src/denoise.c
  third_party/rnnoise/src/kiss_fft.c
  third_party/rnnoise/src/pitch.c
  third_party/rnnoise/src/rnn.c
  third_party/rnnoise/src/rnn_data.c)
target_include_directories(kcds_rnnoise PUBLIC third_party/rnnoise/include)
# FLOAT_APPROX: its NaN check reads the float's bits, which /fp:fast keeps (clang-cl, the release
# build's compiler, refuses the plain check under fast math).
target_compile_definitions(kcds_rnnoise PRIVATE _USE_MATH_DEFINES FLOAT_APPROX)
# Its filters are float loops: strict float order keeps them from being vectorised.
if(MSVC)
  target_compile_options(kcds_rnnoise PRIVATE /fp:fast /O2)
else()
  # Its stack buffers use the MSVC name of alloca (the Linux build of the tests compiles it too).
  target_compile_definitions(kcds_rnnoise PRIVATE _alloca=__builtin_alloca)
endif()

# SpeexDSP's echo canceller only (third_party/speexdsp/README.txt).
add_library(kcds_speexdsp_echo STATIC
  third_party/speexdsp/mdf.c
  third_party/speexdsp/fftwrap.c
  third_party/speexdsp/smallft.c)
target_include_directories(kcds_speexdsp_echo PUBLIC third_party/speexdsp/include
                           PRIVATE third_party/speexdsp)
target_compile_definitions(kcds_speexdsp_echo PRIVATE FLOATING_POINT USE_SMALLFT EXPORT=
                           _USE_MATH_DEFINES)
if(MSVC)
  target_compile_options(kcds_speexdsp_echo PRIVATE /fp:fast /O2 /wd4244 /wd4305)
endif()

add_library(kcds_voice_engine STATIC voice_engine.cpp spatial.cpp echo_canceller.cpp)
add_library(kcds::voice_engine ALIAS kcds_voice_engine)
target_include_directories(kcds_voice_engine
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
          third_party/miniaudio
          third_party/steamaudio/include)
target_link_libraries(kcds_voice_engine PUBLIC kcd2s::voice PRIVATE opus kcds_rnnoise kcds_speexdsp_echo)

if(KCD2S_BUILD_TESTS)
  kcd2s_add_test(NAME voice.engine TARGET test_voice_engine
                 SOURCES tests/voice_engine_test.cpp
                 LIBS kcds::voice_engine)
  target_include_directories(test_voice_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  # Explicit bench, never a pass or fail threshold: the numbers go into the issues.
  add_executable(voice_quality_bench bench/voice_quality_bench.cpp)
  target_link_libraries(voice_quality_bench PRIVATE kcds::voice_engine)
endif()
