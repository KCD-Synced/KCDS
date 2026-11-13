#pragma once
// Loading the KCDS client DLL into a game process (a remote LoadLibraryW thread).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <string>

namespace kcds::runtime::launch {

// How long the client's DllMain may take before the start counts as failed.
inline constexpr DWORD kInjectTimeoutMs = 15000;
// A game that closes this soon after its start has not run with the client: it has started
// itself again through its store, or failed to start at all.
inline constexpr DWORD kEarlyExitMs = 4000;

// Loads dll into process. true once LoadLibraryW returned a module there.
bool load_client(HANDLE process, const std::wstring& dll);

}  // namespace kcds::runtime::launch
