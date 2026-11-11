// The client's log (client-<pid>.log in %LOCALAPPDATA%\KCDS\logs) and where the DLL lies (shared
// by the features split out of game_client.cpp).
#pragma once

// Winsock before Windows.h (the network headers need its newer socket types), and no min/max
// macros (the shared headers use std::min and std::max).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace kcds::game_client {

// The client DLL's module, set by DllMain.
extern HMODULE module;
extern FILE* log_file;

// One line in the log, with the system tick in front.
void log(const char* format, ...);

// A line that repeats for every body of a crowd (hundreds of players): one budget per place in
// the code, at most `per_second` of its lines a second; the rest are only counted.
struct LogBudget {
  std::atomic<unsigned long long> second{0};
  std::atomic<unsigned> used{0};
  std::atomic<unsigned> left_out{0};
};

// True when the caller may write its line now; `left_out` then holds how many of its lines were
// left out since the last one written, for the caller to add to its line.
bool log_budget(LogBudget& budget, unsigned per_second, unsigned& left_out);

// Opens the log (from DllMain, before the game's main thread runs).
void open_log();

// The folder the client DLL lies in, with a trailing backslash.
std::wstring module_directory();

// The folder every log of a player lies in (%LOCALAPPDATA%\KCDS\logs\, the launcher's too), with
// a trailing backslash; empty when it cannot be made.
std::wstring logs_directory();

// The path of this game's log (client-<pid>.log in the logs folder, or beside the DLL).
std::wstring client_log_path();

}  // namespace kcds::game_client
