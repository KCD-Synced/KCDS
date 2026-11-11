#include "shader_compile.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>

#include "MinHook.h"
#include "snow_cover.hpp"

namespace kcds::runtime::winter::shader_compile {
namespace {

// The compiler's command line as the game hands it over: its arguments (wide strings) and their
// count, read by IDxcCompiler3::Compile.
struct Arguments {
  const wchar_t** values;
  std::uint32_t count;
  std::uint32_t capacity;
};

// The game's compile: the HLSL text and its length, the arguments, the compiled blob and the
// errors (both out); an HRESULT.
using Compile = int(__fastcall*)(const char* source, std::size_t length, Arguments* arguments,
                                 void** blob, void** errors);

// Failed compiles whose messages go to the log.
constexpr int kLoggedFailures = 5;

Logger log_line = nullptr;
Compile original_compile = nullptr;
std::atomic<bool> told{false};
std::atomic<int> failures{0};

// A failed compile's messages (IDxcBlob: buffer at slot 3, size at slot 4), the first few.
void log_failure(void** errors) {
  if (!log_line || !errors || !*errors || failures.fetch_add(1) >= kLoggedFailures) {
    return;
  }
  void* text_blob = *errors;
  using Pointer = const char*(__fastcall*)(void*);
  using Size = std::size_t(__fastcall*)(void*);
  void** table = *static_cast<void***>(text_blob);
  const char* text = reinterpret_cast<Pointer>(table[3])(text_blob);
  const std::size_t size = reinterpret_cast<Size>(table[4])(text_blob);
  log_line("WINTER shader compile failed: %.*s", int(size < 1500 ? size : 1500), text ? text : "");
}

// The text of a snow shader that did not compile, once, beside the logs: to see what the game's
// own text calls things when a change of KCDS's does not fit it.
std::atomic<bool> source_written{false};

void write_failed_source(std::string_view text) {
  const char* base = std::getenv("LOCALAPPDATA");
  if (!base || source_written.exchange(true)) {
    return;
  }
  const std::string folder = std::string(base) + "\\KCDS\\logs";
  std::error_code made;
  std::filesystem::create_directories(folder, made);
  const std::string path = folder + "\\snow-shader-failed.hlsl";
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), std::streamsize(text.size()));
  if (log_line) {
    log_line("WINTER the snow shader that did not compile is in %s", path.c_str());
  }
}

int __fastcall compile_with_older_rules(const char* source, std::size_t length,
                                        Arguments* arguments, void** blob, void** errors) {
  if (!arguments || !arguments->values) {
    return original_compile(source, length, arguments, blob, errors);
  }
  std::vector<const wchar_t*> values(arguments->values, arguments->values + arguments->count);
  values.push_back(L"-HV");
  values.push_back(L"2018");
  Arguments with_version{values.data(), std::uint32_t(values.size()), std::uint32_t(values.size())};
  const std::string_view text = source ? std::string_view(source, length) : std::string_view();
  // The snow shader with KCDS's cover (snow_cover.hpp), and should that not compile, with the
  // plain cover, then as the game wrote it: the players always get snow. Every other shader as
  // the game wrote it.
  const std::string covers[] = {snow_cover::with_full_cover(text, true),
                                snow_cover::with_full_cover(text, false)};
  for (const std::string& covered : covers) {
    if (covered.empty()) {
      continue;
    }
    const int result =
        original_compile(covered.c_str(), covered.size(), &with_version, blob, errors);
    if (log_line) {
      log_line("WINTER snow shader compiled with the %s cover: 0x%08x",
               &covered == &covers[0] ? "full" : "plain", unsigned(result));
    }
    if (result >= 0) {
      return result;
    }
    log_failure(errors);
    write_failed_source(covered);
    // The next try gives its own blobs: these are let go (IUnknown::Release, slot 2).
    for (void** out : {blob, errors}) {
      if (out && *out) {
        using Release = unsigned long(__fastcall*)(void*);
        reinterpret_cast<Release>((*static_cast<void***>(*out))[2])(*out);
        *out = nullptr;
      }
    }
  }
  if (log_line && source && covers[1].empty()) {
    // The snow shader written otherwise than the cover expects: what stands there, to adapt it.
    const std::size_t at = text.find("Blending.a");
    if (text.find("fIceTopZ") != std::string_view::npos && at != std::string_view::npos) {
      log_line("WINTER snow shader cover not found: %.*s",
               int(std::min<std::size_t>(200, length - at)), source + at);
    }
  }
  const int result = original_compile(source, length, &with_version, blob, errors);
  if (log_line && !told.exchange(true)) {
    log_line("WINTER shader compiled by the game with HLSL 2018 rules: 0x%08x", unsigned(result));
  }
  if (result < 0) {
    log_failure(errors);
  }
  return result;
}

}  // namespace

bool install(void* compile, Logger logger) {
  log_line = logger;
  if (original_compile) {
    return true;
  }
  if (!compile) {
    return false;
  }
  const bool hooked = MH_CreateHook(compile, reinterpret_cast<void*>(&compile_with_older_rules),
                                    reinterpret_cast<void**>(&original_compile)) == MH_OK &&
                      MH_EnableHook(compile) == MH_OK;
  if (!hooked) {
    original_compile = nullptr;
  }
  return hooked;
}

}  // namespace kcds::runtime::winter::shader_compile
