#include <Windows.h>
#include <cstdio>
#include <string>
#include <ShlObj.h>
#include <filesystem>
#include <fstream>
#include <regex>

unsigned saved_eula_version() {
  PWSTR saved_games = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved_games)))
    return 0;
  std::filesystem::path attributes = std::filesystem::path(saved_games) / L"kingdomcome2" /
                                     L"profiles" / L"default" / L"attributes.xml";
  CoTaskMemFree(saved_games);
  std::ifstream profile(attributes);
  const std::regex attribute(
      R"eula(<Attr\s+name="eula_confirmed_version"\s+value="([0-9]{1,6})"\s*/>)eula");
  std::string line;
  while (std::getline(profile, line)) {
    std::smatch match;
    if (std::regex_search(line, match, attribute))
      return static_cast<unsigned>(std::stoul(match[1].str()));
  }
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  if (argc != 4) {
    fwprintf(stderr, L"usage: kcd2s-launch-client <game.exe> <client.dll> <engine-port>\n");
    return 2;
  }
  std::wstring game = argv[1], dll = argv[2];
  if (game.find(L'"') != std::wstring::npos || dll.find(L'"') != std::wstring::npos)
    return 2;
  wchar_t* end = nullptr;
  auto port = wcstoul(argv[3], &end, 10);
  if (*end || port < 1024 || port > 65535)
    return 2;
  if (GetFileAttributesW(game.c_str()) == INVALID_FILE_ATTRIBUTES ||
      GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
    return 2;
  auto bin = game.substr(0, game.find_last_of(L"\\/"));
  auto root = bin.substr(0, bin.find_last_of(L"\\/"));
  root = root.substr(0, root.find_last_of(L"\\/"));
  // The audio system's memory pool (s_FmodPrimaryPoolSize, KiB, read once at start; the game
  // ships 140 MiB): a smith's game ran out of it the moment the workpiece was done and crashed
  // (FMOD asked for 64 MiB more), with the other players' sounds playing on top.
  std::wstring cmd =
      L"\"" + game + L"\" +sv_port " + std::to_wstring(port) + L" +s_FmodPrimaryPoolSize 262144";
#ifdef KCDS_DEV_BUILD
  // Developer builds only (CMake KCDS_DEV_BUILD): the local test kit runs several small windowed
  // games side by side with the game's developer mode. A release keeps the player's own display
  // settings (full screen) and no developer mode.
  cmd += L" -devmode +r_Fullscreen 0 +r_Width 960 +r_Height 540";
  // KCDS_TEST_GAME_ARGS: more engine arguments for one test run (e.g. "+mfx_Debug 1").
  wchar_t test_args[1024]{};
  const auto test_args_length = GetEnvironmentVariableW(L"KCDS_TEST_GAME_ARGS", test_args, 1024);
  if (test_args_length > 0 && test_args_length < 1024) {
    cmd += L" " + std::wstring(test_args, test_args_length);
  }
#endif
  // Replay only a confirmation already persisted by the user. Do not upgrade
  // it to the game's current version or auto-accept a new EULA. Native profile
  // loading otherwise races first-time overlays in the early debug startup.
  const auto confirmed = saved_eula_version();
  if (confirmed) {
    cmd += L" +wh_ui_eulaConfirmedVersion " + std::to_wstring(confirmed);
    printf("Restoring saved EULA version=%u from the existing default profile\n", confirmed);
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  // A kit's test game shows its first window without taking the focus
  // (testing/background_mode.hpp).
  if (GetEnvironmentVariableW(L"KCDS_TEST_BACKGROUND", nullptr, 0) != 0) {
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
  }
  PROCESS_INFORMATION pi{};
  // Supply the installed Steam game's app context to the child. SteamAPI still
  // validates the signed-in account and license; Steam must already be running.
  wchar_t prior_app[32768]{}, prior_game[32768]{};
  auto app_length = GetEnvironmentVariableW(L"SteamAppId", prior_app, 32768);
  auto game_length = GetEnvironmentVariableW(L"SteamGameId", prior_game, 32768);
  if (app_length >= 32768 || game_length >= 32768)
    return 2;
  SetEnvironmentVariableW(L"SteamAppId", L"1771300");
  SetEnvironmentVariableW(L"SteamGameId", L"1771300");
#ifdef KCDS_DEV_BUILD
  // A test kit's game stays in the background: no foreground, no controller
  // (testing/background_mode.hpp). KCDS_FOREGROUND=1 gives a game someone plays by hand.
  // Developer builds only: a release's launcher starts every game through this tool, and a
  // player's game in the background mode handed the foreground to another window (OBS) again and
  // again, so the mouse and keys never reached it and its cursor stayed.
  wchar_t foreground[8]{};
  const auto foreground_length = GetEnvironmentVariableW(L"KCDS_FOREGROUND", foreground, 8);
  const bool by_hand = foreground_length == 1 && foreground[0] == L'1';
  if (!by_hand) {
    SetEnvironmentVariableW(L"KCDS_BACKGROUND", L"1");
  }
  printf("%s\n", by_hand ? "Foreground game (KCDS_FOREGROUND=1)"
                         : "Background game: no focus, no controller (KCDS_FOREGROUND=1 to play)");
#else
  // A player's game: never the test kit's background mode, whatever the environment holds.
  SetEnvironmentVariableW(L"KCDS_BACKGROUND", nullptr);
#endif
  auto created = CreateProcessW(game.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                                nullptr, root.c_str(), &si, &pi);
  auto create_error = GetLastError();
  SetEnvironmentVariableW(L"SteamAppId", app_length ? prior_app : nullptr);
  SetEnvironmentVariableW(L"SteamGameId", game_length ? prior_game : nullptr);
  if (!created) {
    printf("CreateProcess error %lu\n", create_error);
    return 1;
  }
  auto bytes = (dll.size() + 1) * sizeof(wchar_t);
  void* mem = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
  bool success = false;
  if (mem && WriteProcessMemory(pi.hProcess, mem, dll.c_str(), bytes, nullptr)) {
    auto thread = CreateRemoteThread(pi.hProcess, nullptr, 0, load, mem, 0, nullptr);
    if (thread) {
      if (WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeThread(thread, &code);
        success = code != 0;
      }
      CloseHandle(thread);
    }
  }
  if (success) {
    VirtualFreeEx(pi.hProcess, mem, 0, MEM_RELEASE);
    ResumeThread(pi.hThread);
    printf("Started own client PID=%lu engine-port=%lu\n", pi.dwProcessId, port);
  } else {
    printf("Injection failed in newly created PID=%lu error=%lu\n", pi.dwProcessId, GetLastError());
    TerminateProcess(pi.hProcess, 1);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return success ? 0 : 1;
}
