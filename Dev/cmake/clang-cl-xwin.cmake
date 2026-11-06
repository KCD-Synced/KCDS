# Windows x64 from Linux: clang-cl, lld-link, llvm-rc and llvm-mt against the MSVC
# CRT and the Windows SDK that xwin splats into KCDS_XWIN (Dev/docker/windows/Dockerfile).
#
#   cmake -S . -B build -G "Ninja Multi-Config" \
#     -DCMAKE_TOOLCHAIN_FILE=Dev/cmake/clang-cl-xwin.cmake -DKCDS_XWIN=/xwin
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT KCDS_XWIN)
  set(KCDS_XWIN "$ENV{KCDS_XWIN}")
endif()
if(NOT KCDS_XWIN)
  set(KCDS_XWIN /xwin)
endif()
# Try-compiles see the same toolchain, in Release: xwin splats the release CRT only.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES KCDS_XWIN)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
# MASM (crashpad's .asm files): llvm-ml in its x64 mode, as ml64.
set(CMAKE_ASM_MASM_COMPILER llvm-ml)
set(CMAKE_ASM_MASM_FLAGS_INIT "-m64")

# The MSVC CRT and the Windows SDK as system headers, x64.
set(kcds_xwin_includes
    "/imsvc ${KCDS_XWIN}/crt/include"
    "/imsvc ${KCDS_XWIN}/sdk/include/ucrt"
    "/imsvc ${KCDS_XWIN}/sdk/include/um"
    "/imsvc ${KCDS_XWIN}/sdk/include/shared"
    "/imsvc ${KCDS_XWIN}/sdk/include/winrt")
list(JOIN kcds_xwin_includes " " kcds_xwin_includes)
# clang-cl takes MSVC's /MP (parallel compiles within one cl run, which Ninja does itself) but
# warns that it ignores it; CEF's wrapper builds with warnings as errors (/WX), and clang warns
# where MSVC does not. Warnings stay warnings here, as the MSVC build has them.
set(kcds_clang_cl_flags
    "--target=x86_64-pc-windows-msvc -Wno-unused-command-line-argument -Wno-error")
set(CMAKE_C_FLAGS_INIT "${kcds_clang_cl_flags} ${kcds_xwin_includes}")
set(CMAKE_CXX_FLAGS_INIT "${kcds_clang_cl_flags} ${kcds_xwin_includes}")
# llvm-rc reads the SDK's resource headers.
set(CMAKE_RC_FLAGS_INIT
    "/I ${KCDS_XWIN}/sdk/include/um /I ${KCDS_XWIN}/sdk/include/shared /I ${KCDS_XWIN}/crt/include")

set(kcds_xwin_libs
    "/libpath:${KCDS_XWIN}/crt/lib/x86_64"
    "/libpath:${KCDS_XWIN}/sdk/lib/um/x86_64"
    "/libpath:${KCDS_XWIN}/sdk/lib/ucrt/x86_64")
list(JOIN kcds_xwin_libs " " kcds_xwin_libs)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${kcds_xwin_libs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${kcds_xwin_libs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${kcds_xwin_libs}")

set(CMAKE_FIND_ROOT_PATH "${KCDS_XWIN}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
