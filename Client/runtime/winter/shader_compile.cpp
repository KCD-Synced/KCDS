#include "shader_compile.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "MinHook.h"

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

int __fastcall compile_with_older_rules(const char* source, std::size_t length,
                                        Arguments* arguments, void** blob, void** errors) {
  if (!arguments || !arguments->values) {
    return original_compile(source, length, arguments, blob, errors);
  }
  std::vector<const wchar_t*> values(arguments->values, arguments->values + arguments->count);
  values.push_back(L"-HV");
  values.push_back(L"2018");
  Arguments with_version{values.data(), std::uint32_t(values.size()), std::uint32_t(values.size())};
  const int result = original_compile(source, length, &with_version, blob, errors);
  if (log_line && !told.exchange(true)) {
    log_line("WINTER shader compiled by the game with HLSL 2018 rules: 0x%08x", unsigned(result));
  }
  // A failed compile's messages (IDxcBlob: buffer at slot 3, size at slot 4), the first few.
  if (log_line && result < 0 && errors && *errors && failures.fetch_add(1) < kLoggedFailures) {
    void* text_blob = *errors;
    using Pointer = const char*(__fastcall*)(void*);
    using Size = std::size_t(__fastcall*)(void*);
    void** table = *static_cast<void***>(text_blob);
    const char* text = reinterpret_cast<Pointer>(table[3])(text_blob);
    const std::size_t size = reinterpret_cast<Size>(table[4])(text_blob);
    log_line("WINTER shader compile failed: %.*s", int(size < 1500 ? size : 1500),
             text ? text : "");
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
