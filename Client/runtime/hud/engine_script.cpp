#include "engine_script.hpp"

#include <cstdint>

namespace kcds::runtime::hud {
namespace {

constexpr std::size_t kConsoleSystem = 0x200;
constexpr std::size_t kGetScriptSystem = 0x278;
constexpr std::size_t kExecuteBuffer = 0x30;

template <typename Function>
Function virtual_at(void* object, std::size_t offset) {
  auto table = *reinterpret_cast<std::uint8_t**>(object);
  return *reinterpret_cast<Function*>(table + offset);
}

}  // namespace

bool run_lua(void* console, const std::vector<std::string>& chunks) {
  if (!console) {
    return false;
  }
  void* system = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(console) + kConsoleSystem);
  if (!system) {
    return false;
  }
  using GetScriptSystemFn = void*(__fastcall*)(void*);
  void* scripts = virtual_at<GetScriptSystemFn>(system, kGetScriptSystem)(system);
  if (!scripts) {
    return false;
  }
  using ExecuteBufferFn = bool(__fastcall*)(void*, const char*, std::size_t, const char*, void*);
  auto execute = virtual_at<ExecuteBufferFn>(scripts, kExecuteBuffer);
  for (const std::string& chunk : chunks) {
    execute(scripts, chunk.data(), chunk.size(), "kcds-hud", nullptr);
  }
  return true;
}

}  // namespace kcds::runtime::hud
