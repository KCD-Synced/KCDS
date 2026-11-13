#pragma once
// Finding the game's process when a store starts it (the Epic launcher, a Microsoft Store
// package): the KingdomCome.exe of exactly this install that was not running before.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <set>
#include <string>

namespace kcds::runtime::launch {

// The ids of the processes named image_name (KingdomCome.exe) running now.
std::set<DWORD> running_processes(const wchar_t* image_name);

// A process running exactly the exe at image_path that is not in before, or 0 after timeout_ms.
DWORD wait_for_new_process(const std::set<DWORD>& before, const std::wstring& image_path,
                           DWORD timeout_ms);

// The full path of path (GetFullPathName), or path itself when it can't be made one.
std::wstring full_path(const std::wstring& path);

}  // namespace kcds::runtime::launch
