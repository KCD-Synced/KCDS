// Runs Lua chunks in the engine's own script system: only the test probe does (KCDS_TEST_HOOKS
// builds); everything the client does for players runs through the game's natives.
//
// CXConsole keeps the ISystem at +0x200; ISystem::GetIScriptSystem is vtable +0x278 and
// IScriptSystem::ExecuteBuffer(buffer, size, description, environment) vtable +0x30
// (WHGame 1.5.5, CXConsole::ExecuteStringInternal reads all three). The console's
// own '#' command takes the same path but only in developer mode, so it is called directly.
#pragma once

#include <string>
#include <vector>

namespace kcds::runtime::hud {

// Engine thread only. console is the CXConsole; false when the script system is missing.
bool run_lua(void* console, const std::vector<std::string>& chunks);

}  // namespace kcds::runtime::hud
