// The client's log (client-<pid>.log next to the DLL) and where the DLL lies (shared
// by the features split out of game_client.cpp).
#pragma once

// Winsock before Windows.h (the network headers need its newer socket types), and no min/max
// macros (the shared headers use std::min and std::max).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <Windows.h>

#include <cstdio>
#include <string>

namespace kcds::game_client {

// The client DLL's module, set by DllMain.
extern HMODULE module;
extern FILE* log_file;

// One line in the log, with the system tick in front.
void log(const char* format, ...);

// Opens the log (from DllMain, before the game's main thread runs).
void open_log();

// The folder the client DLL lies in, with a trailing backslash.
std::wstring module_directory();

}  // namespace kcds::game_client
