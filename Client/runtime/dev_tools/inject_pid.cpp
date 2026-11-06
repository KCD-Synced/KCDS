#include <Windows.h>
#include <cstdio>
#include <string>
int wmain(int argc, wchar_t** argv) {
  if (argc != 3)
    return 2;
  wchar_t* end = nullptr;
  auto pid = wcstoul(argv[1], &end, 10);
  if (*end || !pid)
    return 2;
  std::wstring dll = argv[2];
  HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
  if (!process)
    return 1;
  auto bytes = (dll.size() + 1) * sizeof(wchar_t);
  auto mem = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  bool ok = false;
  if (mem && WriteProcessMemory(process, mem, dll.c_str(), bytes, nullptr)) {
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    auto t = CreateRemoteThread(process, nullptr, 0, load, mem, 0, nullptr);
    if (t) {
      if (WaitForSingleObject(t, 15000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeThread(t, &code);
        ok = code != 0;
        VirtualFreeEx(process, mem, 0, MEM_RELEASE);
      }
      CloseHandle(t);
    }
  }
  CloseHandle(process);
  printf("PID %lu load %s\n", pid, ok ? "OK" : "FAILED");
  return ok ? 0 : 1;
}
