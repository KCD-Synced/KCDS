// KCDS branding and startup settings applied at runtime; see runtime_branding.hpp.
//
// WHGame.dll 1.5.5 (sha256 bdf8f9e4...). The engine start sequence in
// CSystem::Init that this file relies on:
//
//  LoadUserConfigs: "Loading user cfg files...", tester.cfg,
//                developer.cfg, user.cfg through CSystem::LoadConfiguration.
//  ApplyBuildInfo: copies wh_sys_version into wh_sys_BuildInfo and
//                wh_sys_MenuBuildInfo, so a user.cfg value of wh_sys_MenuBuildInfo never survives.
//   later        renderer start, where the startup video is decided (sys_intromoviesduringinit
// wh_sys_SkipStartupVideoWhenPossible).
//  OpenModPaks: "Stream Engine Initialization", then for every
//                installed mod the lambda logs "[Mod] Opening paks in %s" and calls
//                ICryPak::OpenPacks(game folder, "mods/<mod>/data/*.pak", 0x10400, nullptr).
//   later        the main menu loads Libs/UI/Textures/KCDLogo.dds through CryPak.
//
// Game cvars such as g_skipIntro are registered by the game module after user.cfg was read; the
// console keeps config values of unregistered cvars and applies them at registration, which is
// why the startup settings go through the same config sink as user.cfg lines.
#include "runtime_branding.hpp"
#include "../characters/skinning_capacity.hpp"
#include "../characters/material_capacity.hpp"
#include "../characters/skinning_metrics.hpp"

#include <Windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "../appearance/female_skeleton.hpp"
#include "../kcd1/kcd1_map.hpp"
#include "../maps/custom_maps.hpp"
#include "../render/release_flush_guard.hpp"
#include "../startup_tables/startup_tables.hpp"
#include "../testing/background_mode.hpp"
#include "MinHook.h"
#include "kcd2s/bridge/sigscan.hpp"
#include "splash.hpp"
#include "window_icon.hpp"

namespace kcds::runtime::branding {
namespace {

namespace bridge = kcd2s::bridge;

constexpr std::uintptr_t kIdaImageBase = 0x180000000ULL;

// --- Our settings --------------------------------------------------------------------------------

struct StartupSetting {
  const char* name;
  const char* value;
};

// The values of the former user.cfg block. wh_sys_StartupVideoFileName is not set: the engine
// rejected the old empty assignment ("no value in console variable assignment"), and with the
// startup video skipped its file name is never needed.
constexpr StartupSetting kStartupSettings[] = {
    {"g_skipIntro", "1"},
    {"sys_intromoviesduringinit", "0"},
    {"wh_sys_SkipStartupVideoWhenPossible", "1"},
};
constexpr const char* kMenuBuildInfoName = "wh_sys_MenuBuildInfo";
constexpr const char* kMenuBuildInfo = "Synced";
constexpr const wchar_t* kPakRelativePath = L"assets\\kcds-branding.pak";
// How long the initialization thread waits for the loader watch before installing the hooks.
constexpr int kLoaderWatchWaitMs = 10000;
constexpr int kLoaderWatchPollMs = 10;

// --- Engine interface offsets, each confirmed in and covered by a signature below ------------

// LoadUserConfigs: the user.cfg sink comes from ISystem slot 2 (+0x10), which returns
// nullptr in 1.5.5; LoadConfiguration then uses CSystem + 8.
constexpr std::size_t kSystemConfigSinkSlot = 0x10;
constexpr std::size_t kSystemOwnSinkOffset = 0x08;
// SystemCFG parser: OnLoadConfigurationEntry(key, value, group) at +0x08 per line,
// OnLoadConfigurationEntry_End at +0x10 after the file.
constexpr std::size_t kSinkEntrySlot = 0x08;
constexpr std::size_t kSinkEndSlot = 0x10;
// LoadConfiguration: IConsole +0x200 sets the config type and returns the previous
// one; user.cfg is loaded with type 1.
constexpr std::size_t kConsoleSetConfigTypeSlot = 0x200;
constexpr int kUserConfigType = 1;
// ApplyBuildInfo: IConsole::GetCVar +0xB8, ICVar::GetString +0x28,
// ICVar::Set(const char*) +0x48.
constexpr std::size_t kConsoleGetCVarSlot = 0xB8;
constexpr std::size_t kCVarGetStringSlot = 0x28;
constexpr std::size_t kCVarSetStringSlot = 0x48;
// Mod pak lambda: ISystem at mod manager + 8, ISystem::GetIPak +0x230,
// ICryPak::OpenPacks(binding root, wildcard, flags, full paths) +0x48, mod pak flags 0x10400.
constexpr std::size_t kModManagerSystemOffset = 0x08;
constexpr std::size_t kSystemGetPakSlot = 0x230;
constexpr std::size_t kPakOpenPacksSlot = 0x48;
constexpr unsigned kModPakFlags = 0x10400;

// --- Signatures (original WHGame 1.5.5 bytes, RIP displacements wildcarded) ----------------------

// The lea at +18 references "Loading user cfg files...".
constexpr const char* kLoadUserConfigsPattern =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 70 48 8B D9 4C 8D 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? "
    "?? BA 8C 18 00 00 E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 10 48 8B 0D ?? ?? ?? ?? 48 8B F8 48 "
    "8B 11 FF 92 98 06 00 00";
constexpr std::size_t kLoadUserConfigsTextLea = 18;
constexpr const char* kLoadUserConfigsText = "Loading user cfg files...";

// CSystem::LoadConfiguration. Only checked: it confirms the config type slot.
constexpr const char* kLoadConfigurationPattern =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 60 48 8B 41 20 48 8B F9 "
    "48 8B EA 45 8B F9 41 8B D1 49 8B F0 48 8B 88 A8 00 00 00 48 8B 01 FF 90 00 02 00 00";

// The mov at +9 loads gEnv->pConsole; the lea at +105 references
// "wh_sys_MenuBuildInfo".
// Loading screen tips: every "@ui_loading_hint_<n>" shows our line instead.
constexpr const char* kLoadingHintPrefix = "@ui_loading_hint_";
constexpr const char* kLoadingHintText = "KCD2-Synced - Developed by KCDS Team";
constexpr const char* kLocalizeLabelPattern =
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 70 "
    "33 FF 45 8A E1 49 8B F0 4C 8B F2 4C 8B E9 48 39 79";
// LocalizeLabel's fallback copies the label into the output: mov rdx,r14; mov rcx,rsi; call
// CryString::assign(const char*) at +0x1DB.
constexpr std::size_t kLocalizeLabelAssignCall = 0x1DB;
// CryString's empty constructor (string*) and its release (string*), as the books use them.
constexpr const char* kStringInitPattern =
    "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 C0 0C 48 89 03 48 8B C3 48 83 C4 20 5B C3 CC "
    "CC 48 89 5C 24 08 48 89 74 24 10 48 89";
constexpr const char* kStringFreePattern = "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ??";
constexpr const char* kApplyBuildInfoPattern =
    "48 89 4C 24 08 48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 90 B8 00 00 "
    "00 48 8B 08 48 8B 51 28 48 8B C8 FF D2 48 8B D0 48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? "
    "?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 90 B8 00 00 00 48 8B 54 24 30 48 8B 08 4C 8B 41 48 48 "
    "8B C8 41 FF D0 48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 90 B8 00 00 00 48 8B 54 "
    "24 30 48 8B 08 4C 8B 41 48 48 8B C8 41 FF D0";
constexpr std::size_t kApplyBuildInfoConsoleMov = 9;
constexpr std::size_t kApplyBuildInfoTextLea = 105;

// the per-mod "open paks" lambda. The lea at +75 references
// "[Mod] Opening paks in %s".
constexpr const char* kModPakLambdaPattern =
    "48 89 5C 24 18 57 48 83 EC 30 48 8B 19 48 8B F9 48 8B 02 48 8B CA FF 10 4C 8B C3 48 8D 4C 24 "
    "48 48 8B D0 E8 ?? ?? ?? ?? 48 8B D0 4C 8D 05 ?? ?? ?? ?? 48 8D 4C 24 40 E8 ?? ?? ?? ?? 48 8D "
    "4C 24 48 E8 ?? ?? ?? ?? 4C 8B 4C 24 40 4C 8D 05 ?? ?? ?? ?? BA 15 01 00 00 48 8D 0D ?? ?? ?? "
    "?? E8 ?? ?? ?? ?? 48 8B 47 08 48 8B 48 08 48 8B 01 FF 90 30 02 00 00 48 8B 17 41 B9 00 04 01 "
    "00 4C 8B 44 24 40 48 C7 44 24 20 00 00 00 00 48 8B 08 48 8B 12 4C 8B 51 48 48 8B C8 41 FF D2";
constexpr std::size_t kModPakLambdaTextLea = 75;
constexpr const char* kModPakLambdaText = "[Mod] Opening paks in %s";

// OpenModPaks: builds a std::function from the lambda's vtable (lea at
// +7) and runs it for every mod. Other wrappers share these bytes, so the match whose vtable
// _Do_call slot (+0x10) is the thunk "add rcx, 8; jmp lambda" is selected.
constexpr const char* kOpenModPaksPattern =
    "4C 8B DC 48 83 EC 68 48 8D 05 ?? ?? ?? ?? 49 89 53 C0 49 89 43 B8 49 8D 53 B8 49 8D 43 B8 49 "
    "89 4B C8 49 89 43 F0 E8 ?? ?? ?? ?? 48 83 C4 68 C3";
constexpr std::size_t kOpenModPaksVtableLea = 7;
constexpr std::size_t kFunctionDoCallSlot = 0x10;
constexpr std::uint8_t kLambdaThunk[] = {0x48, 0x83, 0xC1, 0x08, 0xE9};

// --- Engine function types
// -------------------------------------------------------------------------

using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
using LoadUserConfigsFn = std::intptr_t(__fastcall*)(void* system);
using ApplyBuildInfoFn = std::intptr_t(__fastcall*)(void* unused);
// CLocalizedStringsManager::LocalizeLabel (vtable +0xB8): (this, "@label", string&
// out, english) -> found. LocalizeString translates every "@label" of a text through it, so the
// loading screen's rotating tips (LoadingScreen.gfx setHint: "@ui_loading_hint_" + n) come from
// here.
using LocalizeLabelFn = char(__fastcall*)(void* self, const char* label, void* out, char english);
using StringAssignFn = void*(__fastcall*)(void* string, const char* text);
using StringCallFn = void(__fastcall*)(void* string);
using OpenModPaksFn = std::intptr_t(__fastcall*)(void* mod_manager, const char* const* game_folder);

using GetConfigSinkFn = void*(__fastcall*)(void* system);
using ConfigEntryFn = void(__fastcall*)(void* sink, const char* key, const char* value,
                                        const char* group);
using ConfigEndFn = void(__fastcall*)(void* sink);
using SetConfigTypeFn = int(__fastcall*)(void* console, int type);
using GetCVarFn = void*(__fastcall*)(void* console, const char* name);
using CVarGetStringFn = const char*(__fastcall*)(void* cvar);
using CVarSetStringFn = void(__fastcall*)(void* cvar, const char* value);
using GetPakFn = void*(__fastcall*)(void* system);
using OpenPacksFn = bool(__fastcall*)(void* pak, const char* binding_root, const char* wildcard,
                                      unsigned flags, void* full_paths);

// --- State
// -----------------------------------------------------------------------------------------

Logger log_line = nullptr;
std::wstring pak_path_wide;
std::string pak_path_utf8;
bool pak_available = false;

std::atomic<bool> loader_watch_installed{false};
std::atomic<bool> engine_hooks_attempted{false};
std::atomic<bool> engine_hooks_finished{false};
std::atomic<bool> pak_mount_attempted{false};
std::atomic<bool> pak_mounted{false};
std::uint8_t* image = nullptr;
std::size_t image_size = 0;
void** console_global = nullptr;
bool config_type_confirmed = false;

LoadLibraryAFn original_load_library_a = nullptr;
LoadUserConfigsFn original_load_user_configs = nullptr;
ApplyBuildInfoFn original_apply_build_info = nullptr;
LocalizeLabelFn original_localize_label = nullptr;
StringAssignFn string_assign = nullptr;
StringCallFn string_init = nullptr;
StringCallFn string_free = nullptr;
// The localization manager LocalizeLabel was last called on, for localize().
std::atomic<void*> localization_manager{nullptr};
OpenModPaksFn original_open_mod_paks = nullptr;

template <class... Args>
void note(const char* format, Args... args) {
  if (log_line) {
    log_line(format, args...);
  }
}

template <class Fn>
Fn virtual_slot(void* object, std::size_t byte_offset) {
  auto* table = *reinterpret_cast<std::uint8_t**>(object);
  return *reinterpret_cast<Fn*>(table + byte_offset);
}

unsigned long long ida_address(const void* address) {
  const auto offset = static_cast<const std::uint8_t*>(address) - image;
  return static_cast<unsigned long long>(kIdaImageBase + offset);
}

bool inside_image(const std::uint8_t* address, std::size_t length) {
  if (address < image) {
    return false;
  }
  const auto offset = static_cast<std::size_t>(address - image);
  return offset <= image_size && length <= image_size - offset;
}

// Target of a RIP-relative instruction at `instruction` whose 32-bit displacement sits at
// `displacement_offset` and which is `length` bytes long; nullptr outside the image.
std::uint8_t* rip_target(std::uint8_t* instruction, std::size_t displacement_offset,
                         std::size_t length) {
  std::int32_t displacement = 0;
  std::memcpy(&displacement, instruction + displacement_offset, sizeof(displacement));
  std::uint8_t* target = instruction + length + displacement;
  if (!inside_image(target, 1)) {
    return nullptr;
  }
  return target;
}

// True when the 7-byte lea at `instruction` references exactly the C string `text`.
bool references_text(std::uint8_t* instruction, const char* text) {
  const std::uint8_t* target = rip_target(instruction, 3, 7);
  const std::size_t length = std::strlen(text) + 1;
  if (!target || !inside_image(target, length)) {
    return false;
  }
  return std::memcmp(target, text, length) == 0;
}

std::uint8_t* find_unique(const char* name, const char* pattern) {
  auto signature = bridge::Signature::parse(pattern);
  if (!signature) {
    note("BRANDING invalid signature: %s", name);
    return nullptr;
  }
  const std::size_t first = bridge::scan(image, image_size, *signature);
  if (first == bridge::knpos) {
    note("BRANDING signature not found: %s", name);
    return nullptr;
  }
  const std::size_t rest = image_size - first - 1;
  if (bridge::scan(image + first + 1, rest, *signature) != bridge::knpos) {
    note("BRANDING ambiguous signature: %s", name);
    return nullptr;
  }
  return image + first;
}

std::uint8_t* find_open_mod_paks(std::uint8_t* lambda) {
  auto signature = bridge::Signature::parse(kOpenModPaksPattern);
  if (!signature) {
    note("BRANDING invalid signature: OpenModPaks");
    return nullptr;
  }
  std::uint8_t* found = nullptr;
  std::size_t cursor = 0;
  while (cursor < image_size) {
    const std::size_t offset = bridge::scan(image + cursor, image_size - cursor, *signature);
    if (offset == bridge::knpos) {
      break;
    }
    std::uint8_t* candidate = image + cursor + offset;
    cursor += offset + 1;

    std::uint8_t* vtable = rip_target(candidate + kOpenModPaksVtableLea, 3, 7);
    if (!vtable || !inside_image(vtable, kFunctionDoCallSlot + sizeof(void*))) {
      continue;
    }
    std::uint8_t* do_call = nullptr;
    std::memcpy(&do_call, vtable + kFunctionDoCallSlot, sizeof(do_call));
    if (!inside_image(do_call, sizeof(kLambdaThunk) + 4)) {
      continue;
    }
    if (std::memcmp(do_call, kLambdaThunk, sizeof(kLambdaThunk)) != 0) {
      continue;
    }
    if (rip_target(do_call + 4, 1, 5) != lambda) {
      continue;
    }
    if (found) {
      note("BRANDING ambiguous signature: OpenModPaks");
      return nullptr;
    }
    found = candidate;
  }
  if (!found) {
    note("BRANDING signature not found: OpenModPaks");
  }
  return found;
}

bool install_hook(const char* name, void* target, void* detour, void** original) {
  if (MH_CreateHook(target, detour, original) != MH_OK) {
    note("BRANDING hook %s could not be created", name);
    *original = nullptr;
    return false;
  }
  if (MH_EnableHook(target) != MH_OK) {
    MH_RemoveHook(target);
    note("BRANDING hook %s could not be enabled", name);
    *original = nullptr;
    return false;
  }
  note("BRANDING hook %s installed ida=0x%llx", name, ida_address(target));
  return true;
}

void* console() {
  if (!console_global) {
    return nullptr;
  }
  return *console_global;
}

const char* cvar_text(void* cvar) {
  const char* text = virtual_slot<CVarGetStringFn>(cvar, kCVarGetStringSlot)(cvar);
  if (!text) {
    return "";
  }
  return text;
}

// --- Startup settings
// ------------------------------------------------------------------------------

// Feeds our settings to the sink user.cfg was just loaded through, under the user config type,
// exactly as CSystem::LoadConfiguration does for each user.cfg line.
void apply_startup_settings(void* system) {
  void* engine_console = console();
  if (!system || !engine_console || !config_type_confirmed) {
    note("BRANDING startup settings skipped: engine console not available");
    return;
  }
  void* sink = virtual_slot<GetConfigSinkFn>(system, kSystemConfigSinkSlot)(system);
  if (!sink) {
    sink = static_cast<std::uint8_t*>(system) + kSystemOwnSinkOffset;
  }
  auto set_config_type = virtual_slot<SetConfigTypeFn>(engine_console, kConsoleSetConfigTypeSlot);
  auto add_entry = virtual_slot<ConfigEntryFn>(sink, kSinkEntrySlot);
  auto finish_entries = virtual_slot<ConfigEndFn>(sink, kSinkEndSlot);

  const int previous_type = set_config_type(engine_console, kUserConfigType);
  for (const StartupSetting& setting : kStartupSettings) {
    add_entry(sink, setting.name, setting.value, "");
  }
  finish_entries(sink);
  set_config_type(engine_console, previous_type);

  for (const StartupSetting& setting : kStartupSettings) {
    note("BRANDING startup setting %s = %s", setting.name, setting.value);
  }
}

std::intptr_t __fastcall hooked_load_user_configs(void* system) {
  const std::intptr_t result = original_load_user_configs(system);
  apply_startup_settings(system);
  return result;
}

// --- Menu build text
// -------------------------------------------------------------------------------

void apply_menu_build_info() {
  void* engine_console = console();
  if (!engine_console) {
    note("BRANDING menu build info skipped: engine console not available");
    return;
  }
  void* cvar = virtual_slot<GetCVarFn>(engine_console, kConsoleGetCVarSlot)(engine_console,
                                                                            kMenuBuildInfoName);
  if (!cvar) {
    note("BRANDING menu build info skipped: %s not registered", kMenuBuildInfoName);
    return;
  }
  virtual_slot<CVarSetStringFn>(cvar, kCVarSetStringSlot)(cvar, kMenuBuildInfo);
  note("BRANDING %s = %s", kMenuBuildInfoName, cvar_text(cvar));
}

std::intptr_t __fastcall hooked_apply_build_info(void* unused) {
  const std::intptr_t result = original_apply_build_info(unused);
  apply_menu_build_info();
  return result;
}

// --- Asset pak
// -------------------------------------------------------------------------------------

// Opens our pak the way the engine opens a mod's paks: same binding root, same flags, after the
// installed mods.
void mount_branding_pak(void* mod_manager, const char* const* game_folder) {
  if (pak_mount_attempted.exchange(true)) {
    return;
  }
  if (!mod_manager || !game_folder || !*game_folder) {
    note("BRANDING pak not mounted: no game folder");
    return;
  }
  auto* manager_bytes = static_cast<std::uint8_t*>(mod_manager);
  void* system = *reinterpret_cast<void**>(manager_bytes + kModManagerSystemOffset);
  if (!system) {
    note("BRANDING pak not mounted: no engine system");
    return;
  }
  void* pak = virtual_slot<GetPakFn>(system, kSystemGetPakSlot)(system);
  if (!pak) {
    note("BRANDING pak not mounted: no CryPak");
    return;
  }
  if (pak_available) {
    auto open_packs = virtual_slot<OpenPacksFn>(pak, kPakOpenPacksSlot);
    const bool opened = open_packs(pak, *game_folder, pak_path_utf8.c_str(), kModPakFlags, nullptr);
    pak_mounted.store(opened);
    if (opened) {
      note("BRANDING pak mounted root=%s path=%s", *game_folder, pak_path_utf8.c_str());
    } else {
      note("BRANDING pak could not be opened: %s (vanilla logo)", pak_path_utf8.c_str());
    }
  }
  // The female skeleton lists the male animations too, for a woman's player body.
  kcds::runtime::appearance::female_skeleton::mount(pak, *game_folder, log_line);
  // World Architect's custom maps (maps/custom_maps.hpp): listed as levels, read from the KCDS
  // folder.
  kcds::runtime::maps::custom::mount(pak, *game_folder, log_line);
  // The Kingdom Come: Deliverance map (kcd1/kcd1_map.hpp), from the player's own copy of KCD1.
  kcds::runtime::kcd1::mount(pak, *game_folder, log_line);
  // A server's own perks (startup_tables/startup_tables.hpp): read with the game's tables.
  kcds::runtime::startup_tables::mount(pak, *game_folder, log_line);
}

std::intptr_t __fastcall hooked_open_mod_paks(void* mod_manager, const char* const* game_folder) {
  const std::intptr_t result = original_open_mod_paks(mod_manager, game_folder);
  mount_branding_pak(mod_manager, game_folder);
  return result;
}

// --- Installation
// ----------------------------------------------------------------------------------

void install_startup_settings_hook() {
  std::uint8_t* load_user_configs = find_unique("LoadUserConfigs", kLoadUserConfigsPattern);
  if (!load_user_configs) {
    return;
  }
  if (!references_text(load_user_configs + kLoadUserConfigsTextLea, kLoadUserConfigsText)) {
    note("BRANDING LoadUserConfigs does not reference its log text; not hooked");
    return;
  }
  config_type_confirmed = find_unique("LoadConfiguration", kLoadConfigurationPattern) != nullptr;
  install_hook("LoadUserConfigs", load_user_configs,
               reinterpret_cast<void*>(hooked_load_user_configs),
               reinterpret_cast<void**>(&original_load_user_configs));
}

void install_menu_build_info_hook() {
  std::uint8_t* apply_build_info = find_unique("ApplyBuildInfo", kApplyBuildInfoPattern);
  if (!apply_build_info) {
    return;
  }
  if (!references_text(apply_build_info + kApplyBuildInfoTextLea, kMenuBuildInfoName)) {
    note("BRANDING ApplyBuildInfo does not reference %s; not hooked", kMenuBuildInfoName);
    return;
  }
  auto* global = rip_target(apply_build_info + kApplyBuildInfoConsoleMov, 3, 7);
  if (!global || !inside_image(global, sizeof(void*))) {
    note("BRANDING engine console global not resolved");
    return;
  }
  console_global = reinterpret_cast<void**>(global);
  install_hook("ApplyBuildInfo", apply_build_info, reinterpret_cast<void*>(hooked_apply_build_info),
               reinterpret_cast<void**>(&original_apply_build_info));
}

std::atomic<LabelOverride> label_overrides[kMaxLabelOverrides] = {};

char __fastcall hooked_localize_label(void* self, const char* label, void* out, char english) {
  if (self) {
    localization_manager.store(self, std::memory_order_relaxed);
  }
  if (label && out) {
    for (const auto& slot : label_overrides) {
      const LabelOverride other = slot.load();
      if (other && other(label, out)) {
        return 1;
      }
    }
  }
  if (label && string_assign &&
      std::strncmp(label, kLoadingHintPrefix, std::strlen(kLoadingHintPrefix)) == 0) {
    string_assign(out, kLoadingHintText);
    return 1;
  }
  return original_localize_label(self, label, out, english);
}

}  // namespace

std::string localize(const std::string& key) {
  void* manager = localization_manager.load(std::memory_order_relaxed);
  if (!manager || !original_localize_label || !string_init || !string_free) {
    return {};
  }
  void* text = nullptr;
  string_init(&text);
  const std::string label = "@" + key;
  const bool found = original_localize_label(manager, label.c_str(), &text, 0) != 0;
  std::string result = found && text ? static_cast<const char*>(text) : "";
  string_free(&text);
  return result;
}

bool add_label_override(LabelOverride override_label) {
  for (auto& slot : label_overrides) {
    LabelOverride empty = nullptr;
    if (slot.compare_exchange_strong(empty, override_label)) {
      return true;
    }
  }
  return false;
}

namespace {

void install_loading_hint_hook() {
  std::uint8_t* localize = find_unique("LocalizeLabel", kLocalizeLabelPattern);
  if (!localize) {
    return;
  }
  std::uint8_t* call = localize + kLocalizeLabelAssignCall;
  if (call[0] != 0xE8) {
    note("BRANDING LocalizeLabel string assign not found; loading tips unchanged");
    return;
  }
  std::int32_t relative = 0;
  std::memcpy(&relative, call + 1, sizeof(relative));
  string_assign = reinterpret_cast<StringAssignFn>(call + 5 + relative);
  install_hook("LocalizeLabel", localize, reinterpret_cast<void*>(hooked_localize_label),
               reinterpret_cast<void**>(&original_localize_label));
  string_init =
      reinterpret_cast<StringCallFn>(find_unique("CryStringInitEmpty", kStringInitPattern));
  string_free = reinterpret_cast<StringCallFn>(find_unique("CryStringFree", kStringFreePattern));
}

// Installed with or without our logo pak: the female skeleton's pack is mounted there too.
void install_pak_hook() {
  std::uint8_t* lambda = find_unique("ModPakLambda", kModPakLambdaPattern);
  if (!lambda) {
    return;
  }
  if (!references_text(lambda + kModPakLambdaTextLea, kModPakLambdaText)) {
    note("BRANDING ModPakLambda does not reference its log text; pak not mounted");
    return;
  }
  std::uint8_t* open_mod_paks = find_open_mod_paks(lambda);
  if (!open_mod_paks) {
    return;
  }
  install_hook("OpenModPaks", open_mod_paks, reinterpret_cast<void*>(hooked_open_mod_paks),
               reinterpret_cast<void**>(&original_open_mod_paks));
}

void install_engine_hooks(HMODULE game, const char* trigger) {
  if (engine_hooks_attempted.exchange(true)) {
    return;
  }
  MODULEINFO info{};
  if (!GetModuleInformation(GetCurrentProcess(), game, &info, sizeof(info))) {
    note("BRANDING WHGame.dll module information unavailable");
    engine_hooks_finished.store(true);
    return;
  }
  image = static_cast<std::uint8_t*>(info.lpBaseOfDll);
  image_size = info.SizeOfImage;
  note("BRANDING installing engine hooks (%s)", trigger);
  kcds::runtime::characters::skinning::install(image, image_size, log_line);
  kcds::runtime::characters::materials::install(image, image_size, log_line);
  kcds::runtime::characters::skinning::metrics::install(image, image_size, log_line);
  // ApplyBuildInfo first: it resolves the console that the startup settings need.
  install_menu_build_info_hook();
  install_startup_settings_hook();
  install_pak_hook();
  install_loading_hint_hook();
  // The game's render-resource release lists, so unloading a custom map cannot hang the game
  // (render/release_flush_guard).
  kcds::runtime::render::release_flush_guard::install(image, image_size, log_line);
  // A server's own perks (startup_tables/): mounted at the game's first table patch listing,
  // before the perk table's patches are listed. Taken by its place in WHGame 1.5.5 and checked by
  // its first bytes, the game's CryPak global beside it.
  constexpr std::uint8_t kPatchListPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74,
                                                 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48,
                                                 0x81, 0xEC, 0xF0, 0x01, 0x00, 0x00};
  std::uint8_t* patch_list = image + 0x96D914;
  if (image_size > 0x492D858 &&
      std::memcmp(patch_list, kPatchListPrologue, sizeof(kPatchListPrologue)) == 0) {
    kcds::runtime::startup_tables::hook_table_loads(
        patch_list, reinterpret_cast<void* const*>(image + 0x492D850), log_line);
  } else {
    note("BRANDING table patch listing not where WHGame 1.5.5 has it: server perks unavailable");
  }
  engine_hooks_finished.store(true);
}

HMODULE WINAPI hooked_load_library_a(LPCSTR name) {
  HMODULE result = original_load_library_a(name);
  if (!engine_hooks_attempted.load()) {
    const DWORD error = GetLastError();
    HMODULE game = GetModuleHandleW(L"WHGame.dll");
    if (game) {
      install_engine_hooks(game, "WHGame.dll loaded");
    }
    SetLastError(error);
  }
  return result;
}

std::string to_utf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

}  // namespace

bool start(const std::wstring& install_directory, Logger logger) {
  log_line = logger;
  pak_path_wide = install_directory + kPakRelativePath;
  pak_path_utf8 = to_utf8(pak_path_wide);
  pak_available = !pak_path_utf8.empty() &&
                  GetFileAttributesW(pak_path_wide.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (!pak_available) {
    note("BRANDING pak missing: %s (vanilla logo)", pak_path_utf8.c_str());
  }

  const MH_STATUS status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
    note("BRANDING MinHook unavailable; vanilla visuals");
    return false;
  }
  // The KCDS icon on the game's windows from the first one (no game file changes).
  window_icon::watch(logger);
  // A test game (KCDS_BACKGROUND=1, or the kits' KCDS_TEST_BACKGROUND) takes neither the
  // foreground nor the controller, from its splash on (testing/background_mode.hpp).
  kcds::runtime::background::install(logger);
  // The exe creates its splash before it loads WHGame.dll.
  splash::install(install_directory, logger);
  HMODULE game = GetModuleHandleW(L"WHGame.dll");
  if (game) {
    install_engine_hooks(game, "WHGame.dll already loaded, engine may be past the hook points");
    return true;
  }
  void* target = nullptr;
  if (MH_CreateHookApiEx(L"kernel32", "LoadLibraryA",
                         reinterpret_cast<void*>(hooked_load_library_a),
                         reinterpret_cast<void**>(&original_load_library_a), &target) != MH_OK) {
    note("BRANDING loader watch could not be created; vanilla visuals");
    return false;
  }
  if (MH_EnableHook(target) != MH_OK) {
    MH_RemoveHook(target);
    note("BRANDING loader watch could not be enabled; vanilla visuals");
    return false;
  }
  loader_watch_installed.store(true);
  note("BRANDING loader watch installed");
  return true;
}

void ensure_installed() {
  // WHGame.dll is listed as loaded while its own initialization still runs on the main thread;
  // the loader watch installs the hooks when that LoadLibraryA call returns, before the game
  // calls into the engine. Only when the watch never fires are the hooks installed from here.
  if (loader_watch_installed.load()) {
    for (int waited = 0; waited < kLoaderWatchWaitMs; waited += kLoaderWatchPollMs) {
      if (engine_hooks_finished.load()) {
        return;
      }
      Sleep(kLoaderWatchPollMs);
    }
  }
  if (engine_hooks_attempted.load()) {
    return;
  }
  HMODULE game = GetModuleHandleW(L"WHGame.dll");
  if (game) {
    install_engine_hooks(game,
                         "loader watch missed WHGame.dll, engine may be past the hook points");
  }
}

std::string cvar_value(const char* name) {
  void* engine_console = console();
  if (!engine_console) {
    return {};
  }
  void* cvar = virtual_slot<GetCVarFn>(engine_console, kConsoleGetCVarSlot)(engine_console, name);
  return cvar ? std::string(cvar_text(cvar)) : std::string();
}

void report_applied_settings() {
  void* engine_console = console();
  if (!engine_console) {
    note("BRANDING settings report unavailable: engine console not resolved");
    return;
  }
  auto get_cvar = virtual_slot<GetCVarFn>(engine_console, kConsoleGetCVarSlot);
  for (const StartupSetting& setting : kStartupSettings) {
    void* cvar = get_cvar(engine_console, setting.name);
    if (cvar) {
      note("BRANDING check %s = %s", setting.name, cvar_text(cvar));
    } else {
      note("BRANDING check %s not registered", setting.name);
    }
  }
  void* build_info = get_cvar(engine_console, kMenuBuildInfoName);
  if (build_info) {
    note("BRANDING check %s = %s", kMenuBuildInfoName, cvar_text(build_info));
  }
  note("BRANDING check pak %s", pak_mounted.load() ? "mounted" : "not mounted");
}

}  // namespace kcds::runtime::branding
