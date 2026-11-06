// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "stack_sampler.hpp"

#include <mmsystem.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../core/log.hpp"

#pragma comment(lib, "winmm.lib")

namespace kcds::game_client {

namespace {

constexpr int kSampleMilliseconds = 2;
constexpr int kMaxFrames = 96;
constexpr double kDefaultSeconds = 10.0;

HANDLE sampler_event = nullptr;
HANDLE sampler_thread = nullptr;
DWORD engine_thread_id = 0;

struct Module {
  std::uintptr_t base = 0;
  std::uintptr_t size = 0;
  std::string name;
};

struct Stack {
  int count = 0;
  std::uintptr_t frames[kMaxFrames] = {};
};

// Walks the stopped thread's stack. It must not allocate: the thread may hold the heap's lock.
void walk(CONTEXT context, Stack& stack) {
  stack.count = 0;
  while (context.Rip != 0 && stack.count < kMaxFrames) {
    stack.frames[stack.count++] = context.Rip;
    DWORD64 image = 0;
    const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
    if (!function) {
      // A leaf function: its return address is on the top of the stack.
      context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void* handler = nullptr;
    DWORD64 frame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler, &frame,
                     nullptr);
  }
}

std::vector<Module> loaded_modules() {
  HMODULE handles[1024];
  DWORD needed = 0;
  std::vector<Module> modules;
  if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) {
    return modules;
  }
  const DWORD count = (std::min)(needed / DWORD(sizeof(HMODULE)), DWORD(1024));
  for (DWORD i = 0; i < count; ++i) {
    MODULEINFO info{};
    char name[MAX_PATH] = {};
    if (GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)) &&
        GetModuleBaseNameA(GetCurrentProcess(), handles[i], name, MAX_PATH)) {
      modules.push_back(
          {reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll), info.SizeOfImage, name});
    }
  }
  return modules;
}

void write_frame(FILE* file, std::uintptr_t address, const std::vector<Module>& modules) {
  for (const Module& module : modules) {
    if (address >= module.base && address < module.base + module.size) {
      fprintf(file, "%s+%llx ", module.name.c_str(),
              static_cast<unsigned long long>(address - module.base));
      return;
    }
  }
  fprintf(file, "?+%llx ", static_cast<unsigned long long>(address));
}

std::wstring temp_file(const wchar_t* extension) {
  wchar_t folder[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, folder);
  wchar_t name[64];
  swprintf_s(name, L"kcds-stacks-%lu.%s", GetCurrentProcessId(), extension);
  return std::wstring(folder) + name;
}

struct Request {
  double seconds = kDefaultSeconds;
  // The thread to watch, 0 for the engine's.
  unsigned long thread = 0;
};

Request read_request() {
  FILE* file = _wfopen(temp_file(L"seconds").c_str(), L"r");
  Request request;
  if (file) {
    double seconds = 0.0;
    unsigned long thread = 0;
    const int read = fscanf_s(file, "%lf %lu", &seconds, &thread);
    if (read >= 1 && seconds > 0.0 && seconds <= 600.0) {
      request.seconds = seconds;
    }
    if (read == 2) {
      request.thread = thread;
    }
    fclose(file);
  }
  return request;
}

DWORD WINAPI sample(void* parameter) {
  const Request request = *static_cast<Request*>(parameter);
  delete static_cast<Request*>(parameter);
  const double seconds = request.seconds;
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                             FALSE, request.thread ? request.thread : engine_thread_id);
  if (!thread) {
    log("STACKS the engine thread cannot be opened");
    return 0;
  }
  const std::size_t room = std::size_t(seconds * 1000.0 / kSampleMilliseconds) + 16;
  std::vector<Stack> stacks(room);
  std::size_t taken = 0;
  const ULONGLONG end = GetTickCount64() + ULONGLONG(seconds * 1000.0);
  timeBeginPeriod(1);
  while (GetTickCount64() < end && taken < room) {
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT context{};
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(thread, &context)) {
        walk(context, stacks[taken]);
        ++taken;
      }
      ResumeThread(thread);
    }
    Sleep(kSampleMilliseconds);
  }
  timeEndPeriod(1);
  CloseHandle(thread);
  const std::vector<Module> modules = loaded_modules();
  FILE* file = _wfopen(temp_file(L"txt").c_str(), L"w");
  if (file) {
    for (std::size_t i = 0; i < taken; ++i) {
      for (int frame = 0; frame < stacks[i].count; ++frame) {
        write_frame(file, stacks[i].frames[frame], modules);
      }
      fputc('\n', file);
    }
    fclose(file);
  }
  log("STACKS %zu samples written", taken);
  return 0;
}

}  // namespace

void install_stack_sampler() {
  wchar_t name[96];
  swprintf_s(name, L"Local\\KCD2S.StackSampler.%lu", GetCurrentProcessId());
  sampler_event = CreateEventW(nullptr, FALSE, FALSE, name);
}

void tick_stack_sampler() {
  if (!sampler_event) {
    return;
  }
  engine_thread_id = GetCurrentThreadId();
  if (WaitForSingleObject(sampler_event, 0) != WAIT_OBJECT_0) {
    return;
  }
  if (sampler_thread) {
    if (WaitForSingleObject(sampler_thread, 0) != WAIT_OBJECT_0) {
      return;
    }
    CloseHandle(sampler_thread);
  }
  sampler_thread = CreateThread(nullptr, 0, sample, new Request(read_request()), 0, nullptr);
}

}  // namespace kcds::game_client
