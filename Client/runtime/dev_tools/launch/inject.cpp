#include "inject.hpp"

namespace kcds::runtime::launch {

bool load_client(HANDLE process, const std::wstring& dll) {
  const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
  void* memory = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!memory) {
    return false;
  }
  const auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
  bool loaded = false;
  bool started = false;
  bool finished = false;
  if (load && WriteProcessMemory(process, memory, dll.c_str(), bytes, nullptr)) {
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, load, memory, 0, nullptr);
    if (thread) {
      started = true;
      if (WaitForSingleObject(thread, kInjectTimeoutMs) == WAIT_OBJECT_0) {
        finished = true;
        DWORD code = 0;
        GetExitCodeThread(thread, &code);
        loaded = code != 0;
      }
      CloseHandle(thread);
    }
  }
  // A thread still inside LoadLibraryW reads the path: its memory stays until the process ends.
  if (!started || finished) {
    VirtualFreeEx(process, memory, 0, MEM_RELEASE);
  }
  return loaded;
}

}  // namespace kcds::runtime::launch
