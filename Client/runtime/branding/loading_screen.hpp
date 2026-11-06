// The KCDS loading screen: from the game's first frame until its main menu is ready,
// the window shows KCDS's own animated screen instead of the game's black one (the game loads its
// paks, shaders and the menu's UI then). It is the owner's design (the design canvas's "Loading"
// and "Loading-Spec" pages): the gradient background, the breathing glow, the turning ring, the
// KCDS mark with pulsing corner brackets, the wordmark, the subtitle, the line with the gold streak
// and the diamond it lights, and "Loading the game...", all on a 2200 ms beat. It covers the whole
// frame and fades out as soon as the main menu is built or a first-start page of the game (its
// EULA and the like) opens, so it never hides one; a safety limit ends it in any case.
//
// The game presents no frame at all until its menu is ready (measured: its first Present comes
// after the main menu is built), so the screen is a window of its own: borderless, never active,
// owned by the game window and laid exactly over its client area (it follows a move, a resize and
// full screen), drawn about 60 times a second with Direct2D on its own thread. Everything scales
// with the window's height and stays centred, so every resolution and aspect ratio fits. Only the
// client draws it, in a game the KCDS launcher started; the game's files are never changed.
#pragma once

namespace kcds::runtime::branding::loading_screen {

// True once the game has something of its own to show (the main menu or a first-start page).
using ReadyCheck = bool (*)();
using Logger = void (*)(const char* format, ...);

// Starts the screen's thread from the client's start; `ready` is asked on every frame until it
// says yes.
void install(ReadyCheck ready, Logger logger);

// The screen again, over a game already running, until `ready` says yes (the main menu's tools,
// until their editor is drawn), with `status` as its status line ("Loading the editor"); nothing
// while it still shows. install must have run. `status` must live for the process.
void show_again(ReadyCheck ready, const wchar_t* status);

// The status line from now on (a game started in a tool: "Loading the editor"); `status` must live
// for the process.
void set_status(const wchar_t* status);

// Renders the screen into a PNG file, width x height pixels at `ms` into its animation, for the
// preview tool (dev_tools/loading_preview.cpp). False when it could not.
bool write_preview(const wchar_t* path, unsigned width, unsigned height, double ms);

}  // namespace kcds::runtime::branding::loading_screen
