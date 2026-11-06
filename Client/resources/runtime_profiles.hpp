#pragma once

#include <cstdint>
#include <functional>
#include <string>

// The resource runtime's WebView2 profiles: each running runtime keeps its own
// folder, %LOCALAPPDATA%\KCDS\ResourceRuntime\<pid>, so several clients on one PC stay apart.
// A runtime removes its folder when it ends, and a starting one removes the folders of runtimes
// that no longer run (a crash, a kill), so the folders never pile up on a player's disk. A removed
// profile's WebView2 crash reports are kept first in %LOCALAPPDATA%\KCDS\crash\webview (the
// newest five), so a failed browser process can be read afterwards.
namespace kcds::resources::profiles {

// Whether the runtime with this process id still runs.
using Alive = std::function<bool(std::uint32_t pid)>;

// Removes the profile folders under `base` named after a runtime that no longer runs (and keeps
// `own`); the number removed.
std::size_t sweep(const std::wstring& base, std::uint32_t own, const Alive& alive);

// This process's profile folder under %LOCALAPPDATA% (`local_app_data`), after a sweep of the
// folders of runtimes that no longer run.
std::wstring open(const std::wstring& local_app_data);

// Removes this process's profile folder; false when WebView2 still holds it (the next runtime's
// sweep removes it then).
bool close(const std::wstring& folder);

// Whether a kcd2s-resource-runtime with this process id runs.
bool runtime_alive(std::uint32_t pid);

}  // namespace kcds::resources::profiles
