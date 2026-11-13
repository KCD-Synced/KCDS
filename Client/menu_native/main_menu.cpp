// Native main-menu hooks; see include/kcds/menu_native/main_menu.hpp.
#include "kcds/menu_native/main_menu.hpp"

#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <new>
#include <utility>

#include "MinHook.h"

namespace kcds::menu_native {

namespace {

using BuildRootMenu = std::int64_t(__fastcall*)(std::int64_t menu, std::int64_t state);
using AddButton = std::int64_t(__fastcall*)(void* menu, std::uint8_t id, void* callback,
                                            char enabled, void* tooltip);
using Cleanup = std::int64_t(__fastcall*)(void*);
using FirstTimeOverlay = char(__fastcall*)(void* menu);
using MenuStep = void(__fastcall*)(void* menu);
// The game's string type is one pointer; these are its constructor and destructor.
using StringInit = void*(__fastcall*)(void* string, const char* text);
using StringFree = void(__fastcall*)(void* string);
using InvokeText = char(__fastcall*)(void* movie_owner, void* name, const char* const* value);
// IFlashVariableObject (CFlashVariableObject): slot 0 Release, slot 8
// SetMember(name, const SFlashVarValue&), slot 9 GetMember(name, IFlashVariableObject*&), slot 14
// Invoke(method, args, count, result). The UI element calls its movie functions through Invoke on
// the cached "_root" object.
using FlashInvoke = char(__fastcall*)(void* object, const char* method, const void* args,
                                      unsigned count, void* result);
using FlashRelease = void(__fastcall*)(void* object);
using FlashSetMember = char(__fastcall*)(void* object, const char* name, const void* value);
using FlashGetMember = char(__fastcall*)(void* object, const char* name, void** member);
constexpr std::size_t kFlashReleaseSlot = 0;
constexpr std::size_t kFlashSetMemberSlot = 8;
constexpr std::size_t kFlashGetMemberSlot = 9;
// SFlashVarValue: type at 0 (2 = bool), value at +8.
struct FlashValue {
  std::int32_t type;
  std::int32_t padding;
  std::uint64_t value;
};
constexpr std::int32_t kFlashBool = 2;

// cmp byte [rcx+0A0h],1 (menu state 1) then the PROS service at +1E8h.
constexpr const char* kProsBannerSignature =
    "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8B EC 48 81 EC 80 00 00 00 80 B9 A0 00 00 00 01 "
    "48 8B F1 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 98 E8 01 00 00";
constexpr std::size_t kProsStringInitCall = 0x1C3;
constexpr std::size_t kProsStringFreeCall = 0x1DC;
// menu open; mov dl,9 into the sound interface, then the "Menu" movie lookup.
constexpr const char* kMenuOpenSignature =
    "40 55 53 56 57 48 8B EC 48 83 EC 38 0F 29 74 24 20 48 8B F1 E8 ?? ?? ?? ?? B2 09 48 8B 88 D8 "
    "00 00 00 48 8B 01 FF 10 48 8B 46 08 48 8D 15";
constexpr std::size_t kMenuOpenSetVersionCall = 0x1D2;
// CFlashVariableObject::Invoke; takes the object lock at +48h, then
// mov edi,r9d for the argument count.
constexpr const char* kFlashInvokeSignature =
    "40 55 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 48 8D 6C 24 40 48 89 5D 70 48 89 75 78 48 "
    "89 BD 80 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 45 30 4C 8B 69 48 48 8B D9 49 8B CD 41 "
    "8B F9";
// Menu.gfx (Libs/UI/Menu.gfx in IPL_GameData.pak): fc_setVersion writes
// _root.version.tField.htmlText = "@ui_version_menu " + value, and fc_preparePage sets
// _root.version._visible = true on every page.
constexpr const char* kSetVersionFunction = "fc_setVersion";
constexpr const char* kVersionClip = "version";
// _root.userMessage: the account status line ("Deep Silver Account services are unavailable or you
// are not connected to the internet.") shown at the bottom left once the PROS banners are off.
constexpr const char* kUserMessageClip = "userMessage";
// _root.titleBox.logo: the game's logo (a shape filled with Textures/KCDLogo.dds, our branding
// pak's picture).
constexpr const char* kTitleBoxClip = "titleBox";
constexpr const char* kLogoClip = "logo";
// Menu state byte of C_UIMenu: 1 is the main menu (2 the pause menu).
constexpr std::size_t kMenuStateOffset = 0xA0;

// B2 01 = menu state 1, then cmp byte [rsi+0A0h], 4.
constexpr const char* kBuildRootMenuSignature =
    "40 55 53 56 57 41 56 48 8B EC 48 83 EC 70 B2 01 48 8B F1 E8 ?? ?? ?? ?? 80 BE A0 00 00 00 04 "
    "75";
// reads wh_ui_showFirstTimeOverlays first (lea rdx, name; console vtable +0B8h).
constexpr const char* kFirstTimeOverlaySignature =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 4C 8B F1 48 8D 15 "
    "?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 B8 00 00 00 33 ED";
constexpr const char* kAddButtonSignature =
    "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 48 8B 75 60 45 "
    "8A E9 4D 8B F8";
constexpr const char* kCallbackCleanupSignature =
    "40 53 48 83 EC 20 48 8B D9 48 8B 49 38 48 85 C9 74 14 48 8B 01 48 3B CB 0F 95 C2 FF 50 20 48 "
    "C7 43 38 00 00 00 00";
constexpr const char* kStringCleanupSignature =
    "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ?? 85 C0 78 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? "
    "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B "
    "C4 48";

// Pause menu (state 2) in a server's world: "Save and exit to main menu" (E_ButtonId 20) is shown
// before "Exit game" (18), and its confirmation's Yes leaves the world instead of saving.
// (pause builder): lea rcx,[rbp-48h]; mov r8,rax; call MakeExitToMenuCallback
// (std::function opening confirmation 4); ...; mov dl,14h; mov rcx,rsi; call
// AddButton.
constexpr const char* kPauseExitCallbackSignature =
    "48 8D 4D B8 4C 8B C0 E8 ?? ?? ?? ?? 4C 89 44 24 20 44 8A CB 4C 8B C0 B2 14 48 8B CE E8";
constexpr std::size_t kPauseExitCallbackCall = 7;
// the Yes of the game's own confirmation 4 (opened by the button through):
// mov byte [menu+73h],4, ClearAll, then the exit save (type 5). In a server's world it leaves the
// world instead, without saving.
constexpr const char* kExitToMenuYesSignature =
    "48 89 5C 24 10 57 48 83 EC 30 48 8B 01 48 8D 15 ?? ?? ?? ?? 48 8B F9 C6 40 73 04 48 8B 19";
// AddButton calls its "AddBasicButton" invoke (menu, name, id, flags, label, tooltip,
// disabled) at +0x120; the label is the button database's text for the id.
constexpr std::size_t kAddBasicButtonCall = 0x120;
constexpr const char* kLeaveButtonLabel = "Exit to main menu";
// opens confirmation <id>: reads its texts from the confirmation database and calls
// its "AddConfirmation" invoke (menu, name, id, text, yes, no, ...) at +0x12C.
constexpr const char* kOpenConfirmationSignature =
    "40 55 53 56 57 41 56 41 57 48 8D 6C 24 D1 48 81 EC 88 00 00 00 49 8B F1 4D 8B F0 8A DA 4C 8B "
    "F9 84 D2 0F 84";
constexpr std::size_t kAddConfirmationCall = 0x12C;
// Confirmation 4 asks "save and return to main menu"; in a server's world nothing is saved.
constexpr std::uint8_t kExitToMenuConfirmation = 4;
constexpr const char* kLeaveConfirmationText =
    "Are you sure you want to leave the server and return to the main menu?";
constexpr std::uint8_t kExitGameButton = 18;
// The main menu's Settings (E_ButtonId 14): its callback opens the settings page (menu state 10:
// game, controls, video, audio and the other pages, then Back). Back and Esc return to the main
// menu through the root-menu builder, which builds the main menu again.
constexpr std::uint8_t kSettingsButton = 14;
constexpr std::uint8_t kExitToMenuButton = 20;
// "Report a bug": the game's SaveExit (E_ButtonId 19), which the multiplayer filter keeps out of a
// server's world, so the id is free there; its label and its callback are ours.
constexpr std::uint8_t kBugReportButton = 19;
constexpr const char* kBugReportButtonLabel = "Report a bug";
constexpr std::uint8_t kPauseMenuState = 2;
// The settings list (page 10): its categories' E_ButtonIds (GameSettings 21 to PhotoMode 27), then
// Back (4). KCDS's category goes before Back, in the place of LangSubtitles (29), an id of the
// language page that is never on the list; its label and callback are ours.
constexpr std::uint8_t kFirstSettingsCategory = 21;
constexpr std::uint8_t kLastSettingsCategory = 27;
constexpr std::uint8_t kBackButton = 4;
constexpr std::uint8_t kKcdsCategoryButton = 29;
// The menu's settings screen (C_UISettings) at +0x330; the page laid out as the Sound page (14).
constexpr std::size_t kMenuSettings = 0x330;
constexpr char kSettingsPageLayout = 14;
// The settings screen's page preparation (ClearAll, then PreparePage of a page number it keeps).
constexpr char kPreparePageSignature[] =
    "40 55 53 57 48 8B EC 48 83 EC 50 48 8B F9 88 91 A1 00 00 00 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 "
    "0F 84 ?? ?? ?? ?? 48 8D 55";
// The settings list's builder (menu, focus): mov dl,0Ah (page 10) into the preparation; it ends in
// a jump to the screen's ShowPage (E9 rel32 at +0x25D).
constexpr char kSettingsListSignature[] =
    "48 89 5C 24 10 55 56 57 41 56 41 57 48 8B EC 48 83 EC 70 44 8A F2 48 8B F9 48 8B 89 30 03 00 "
    "00 B2 0A E8";
constexpr std::size_t kSettingsListShowJump = 0x25D;
using PreparePage = void(__fastcall*)(void* settings, char page);
using SettingsList = std::int64_t(__fastcall*)(void* menu, char focus);
using ShowPage = std::int64_t(__fastcall*)(void* settings);
// A std::function<void()> is 64 bytes: the callable in place, its own address at +0x38.
constexpr std::size_t kCallbackBytes = 64;
static_assert(sizeof(std::function<void()>) == kCallbackBytes,
              "the game's button callback is an MSVC std::function<void()>");
using MakeCallback = void*(__fastcall*)(void* storage, void* const* menu);
using ConfirmYes = void(__fastcall*)(void* capture);
using AddBasicButton = char(__fastcall*)(void* menu, void* name, void* id, void* flags, void* label,
                                         void* tooltip, void* disabled);
using OpenConfirmation = std::int64_t(__fastcall*)(void* menu, std::uint8_t id, void* yes,
                                                   void* no);
using AddConfirmation = char(__fastcall*)(void* menu, void* name, void* id, void* text, void* yes,
                                          void* no, void* first, void* second);

Logger log_line = nullptr;
MakeCallback make_exit_to_menu_callback = nullptr;
ConfirmYes original_exit_to_menu_yes = nullptr;
AddBasicButton original_add_basic_button = nullptr;
OpenConfirmation original_open_confirmation = nullptr;
AddConfirmation original_add_confirmation = nullptr;
// Raised on the game thread while our leave button is added: its label is replaced.
thread_local bool adding_leave_button = false;
// Raised on the game thread while the leave confirmation opens: its question is replaced.
thread_local bool opening_leave_confirmation = false;
std::atomic<PauseLeave> pause_leave{PauseLeave{}};
// Raised on the game thread while our bug report button is added: its label is replaced.
thread_local bool adding_bug_button = false;
std::atomic<PauseBugReport> pause_bug_report{PauseBugReport{}};
// The resources' own pause-menu buttons and their E_ButtonId places: the game's
// SaveGame (11) and LoadGame (12), which the multiplayer filter keeps out of a server's world.
std::mutex menu_buttons_mutex;
std::vector<MenuButton> menu_buttons;
std::atomic<void (*)(const std::string&)> menu_button_pressed{nullptr};
constexpr std::uint8_t kMenuButtonIds[kMaxMenuButtons] = {11, 12};
BuildRootMenu original_build_root_menu = nullptr;
AddButton original_add_button = nullptr;
FirstTimeOverlay original_first_time_overlay = nullptr;
MenuStep original_pros_banner = nullptr;
MenuStep original_menu_open = nullptr;
StringInit string_init = nullptr;
StringFree string_free = nullptr;
InvokeText invoke_text = nullptr;
FlashInvoke original_flash_invoke = nullptr;
Cleanup destroy_callback = nullptr;
Cleanup destroy_string = nullptr;

std::atomic<bool> hide_main_menu_buttons{false};
std::atomic<bool> built{false};
std::atomic<bool> overlay_open{false};
// install() could not hook the menu's building: main_menu_built() never turns true.
std::atomic<bool> hooks_failed{false};
std::atomic<ButtonFilter> button_filter{nullptr};
// The Menu movie's "_root" object (seen on fc_setVersion) and whether the menu that last opened is
// the main menu under our page.
std::atomic<void*> menu_root{nullptr};
std::atomic<bool> version_hidden{false};
// Raised on the game thread only while the main-menu builder runs.
thread_local int building_main_menu = 0;

// A copy of the Settings button's callback from the last main-menu build (the button itself is
// dropped while our page covers the menu), so our page can open the game's own settings.
std::mutex settings_mutex;
std::function<void()> settings_callback;
// Asked by our page (any thread); served on the game thread by frame().
std::atomic<bool> settings_requested{false};
// The game's settings page is open over the main menu, from our request until the game builds its
// main menu again.
std::atomic<bool> settings_shown{false};

void keep_settings_callback(void* callback) {
  const auto* game_callback = static_cast<const std::function<void()>*>(callback);
  std::lock_guard<std::mutex> lock(settings_mutex);
  settings_callback = *game_callback;
}

void hide_version_clip(void* root);

std::int64_t __fastcall hooked_build_root_menu(std::int64_t menu, std::int64_t state) {
  ++building_main_menu;
  const std::int64_t result = original_build_root_menu(menu, state);
  --building_main_menu;
  // Back or Esc on the settings page lands here: our page comes back over the menu.
  if (settings_shown.exchange(false) && log_line) {
    log_line("MENU native settings closed; main menu built again");
  }
  // The menu was built while Settings still counted as open: its logo goes again under our page.
  void* root = menu_root.load();
  if (root && version_hidden.load()) {
    hide_version_clip(root);
  }
  // The game builds its main menu only when no first-time page is pending.
  if (overlay_open.exchange(false) && log_line) {
    log_line("MENU native first-time overlay finished; main menu built");
  }
  if (!built.exchange(true) && log_line) {
    log_line("MENU native main menu built; buttons hidden=%u",
             unsigned(hide_main_menu_buttons.load()));
  }
  return result;
}

// Adds "exit to main menu" with the game's own callback; AddButton takes over the callback and
// the tooltip string, like for every button the builder adds.
void add_leave_button(void* menu) {
  const PauseLeave leave = pause_leave.load();
  const bool pause_menu =
      *(static_cast<const std::uint8_t*>(menu) + kMenuStateOffset) == kPauseMenuState;
  if (!pause_menu || !leave.available || !leave.available() || !make_exit_to_menu_callback ||
      !string_init) {
    return;
  }
  alignas(16) std::uint8_t callback[kCallbackBytes] = {};
  void* const capture = menu;
  make_exit_to_menu_callback(callback, &capture);
  std::uintptr_t tooltip = 0;
  string_init(&tooltip, "");
  adding_leave_button = true;
  original_add_button(menu, kExitToMenuButton, callback, 1, &tooltip);
  adding_leave_button = false;
}

// Adds "Report a bug" with a callback of ours (AddButton moves it in and later destroys it, as it
// does with the game's own callbacks).
void add_bug_report_button(void* menu) {
  const PauseBugReport report = pause_bug_report.load();
  const bool pause_menu =
      *(static_cast<const std::uint8_t*>(menu) + kMenuStateOffset) == kPauseMenuState;
  // The label is replaced through the AddBasicButton hook: without it the button would carry the
  // game's own "save and exit" text, so it is not added.
  if (!pause_menu || !report.available || !report.open || !report.available() || !string_init ||
      !original_add_basic_button) {
    return;
  }
  alignas(16) std::uint8_t callback[kCallbackBytes] = {};
  void (*const open)() = report.open;
  new (callback) std::function<void()>([open] { open(); });
  std::uintptr_t tooltip = 0;
  string_init(&tooltip, "");
  adding_bug_button = true;
  original_add_button(menu, kBugReportButton, callback, 1, &tooltip);
  adding_bug_button = false;
}

// The label of a button of ours being added (add_menu_buttons), for the AddBasicButton hook.
thread_local const char* adding_menu_label = nullptr;

// The pause menu's Continue (its button 1, callback): the menu's + 88 object, its
// vtable slot 2. A resource's button closes the menu the same way.
constexpr std::size_t kMenuResume = 88;
constexpr std::size_t kResumeSlot = 2;

void resume_game(void* menu) {
  auto* resume = static_cast<std::uint8_t*>(menu) + kMenuResume;
  using Resume = void(__fastcall*)(void* self);
  Resume* table = *reinterpret_cast<Resume**>(resume);
  if (table && table[kResumeSlot]) {
    table[kResumeSlot](resume);
  }
}

// Adds the resources' own buttons, each with a callback of ours that hands its key
// to menu_button_pressed and closes the menu as Continue does.
void add_menu_buttons(void* menu) {
  const bool pause_menu =
      *(static_cast<const std::uint8_t*>(menu) + kMenuStateOffset) == kPauseMenuState;
  const PauseBugReport report = pause_bug_report.load();
  if (!pause_menu || !report.available || !report.available() || !string_init ||
      !original_add_basic_button) {
    return;
  }
  std::vector<MenuButton> buttons;
  {
    std::lock_guard<std::mutex> lock(menu_buttons_mutex);
    buttons = menu_buttons;
  }
  for (std::size_t index = 0; index < buttons.size() && index < kMaxMenuButtons; ++index) {
    alignas(16) std::uint8_t callback[kCallbackBytes] = {};
    const std::string key = buttons[index].key;
    new (callback) std::function<void()>([key, menu] {
      if (auto pressed = menu_button_pressed.load()) {
        pressed(key);
      }
      resume_game(menu);
    });
    std::uintptr_t tooltip = 0;
    string_init(&tooltip, "");
    adding_menu_label = buttons[index].label.c_str();
    original_add_button(menu, kMenuButtonIds[index], callback, 1, &tooltip);
    adding_menu_label = nullptr;
  }
}

char __fastcall hooked_add_basic_button(void* menu, void* name, void* id, void* flags, void* label,
                                        void* tooltip, void* disabled) {
  if ((!adding_leave_button && !adding_bug_button && !adding_menu_label) || !string_init ||
      !string_free) {
    return original_add_basic_button(menu, name, id, flags, label, tooltip, disabled);
  }
  std::uintptr_t text = 0;
  const char* const relabel = adding_menu_label   ? adding_menu_label
                              : adding_bug_button ? kBugReportButtonLabel
                                                  : kLeaveButtonLabel;
  string_init(&text, relabel);
  const char added = original_add_basic_button(menu, name, id, flags, &text, tooltip, disabled);
  string_free(&text);
  return added;
}

// KCDS's category in the settings list (set_settings_page) and the game's page functions it uses.
std::mutex settings_page_mutex;
SettingsPage kcds_settings_page;
PreparePage prepare_page = nullptr;
SettingsList settings_list = nullptr;
ShowPage show_page = nullptr;
// Raised on the game thread while the settings list's categories are being added.
thread_local bool listing_settings = false;

SettingsPage settings_page_now() {
  std::lock_guard<std::mutex> lock(settings_page_mutex);
  return kcds_settings_page;
}

// Adds a button of ours: `id` in a place free on this page, `label` shown, `run` on a press.
void add_own_button(void* menu, std::uint8_t id, const char* label, std::function<void()> run) {
  alignas(16) std::uint8_t callback[kCallbackBytes] = {};
  new (callback) std::function<void()>(std::move(run));
  std::uintptr_t tooltip = 0;
  string_init(&tooltip, "");
  adding_menu_label = label;
  original_add_button(menu, id, callback, 1, &tooltip);
  adding_menu_label = nullptr;
}

// Set while KCDS's page is shown: Back, or Esc (which builds the settings list again), closes it.
std::atomic<bool> kcds_page_shown{false};

// KCDS's page: the Sound page's layout with our rows, and a Back to the settings list.
void open_kcds_page(void* menu) {
  const SettingsPage page = settings_page_now();
  void* settings = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(menu) + kMenuSettings);
  if (!settings || !page.add_rows) {
    return;
  }
  if (page.opening) {
    page.opening();
  }
  prepare_page(settings, kSettingsPageLayout);
  page.add_rows();
  kcds_page_shown.store(true);
  add_own_button(menu, kBackButton, nullptr, [menu, page] {
    if (kcds_page_shown.exchange(false) && page.closed) {
      page.closed();
    }
    settings_list(menu, 0);
  });
  show_page(settings);
  if (log_line) {
    log_line("MENU KCDS settings page shown");
  }
}

// The menu whose settings list was last built, and a test kit's request to open the KCDS page on it
// (request_kcds_page).
std::atomic<void*> last_settings_menu{nullptr};
std::atomic<bool> kcds_page_requested{false};

void add_kcds_category(void* menu) {
  const SettingsPage page = settings_page_now();
  if (!page.label || !page.add_rows || !prepare_page || !settings_list || !show_page ||
      !string_init) {
    return;
  }
  last_settings_menu.store(menu);
  // The list again after the page without its Back (Esc): the page is closed all the same.
  if (kcds_page_shown.exchange(false) && page.closed) {
    page.closed();
  }
  add_own_button(menu, kKcdsCategoryButton, page.label, [menu] { open_kcds_page(menu); });
}

std::int64_t __fastcall hooked_add_button(void* menu, std::uint8_t id, void* callback, char enabled,
                                          void* tooltip) {
  // KCDS's category comes last in the settings list, before its Back.
  if (id >= kFirstSettingsCategory && id <= kLastSettingsCategory) {
    listing_settings = true;
  } else if (id == kBackButton && listing_settings) {
    listing_settings = false;
    add_kcds_category(menu);
  } else {
    listing_settings = false;
  }
  if (building_main_menu > 0 && id == kSettingsButton && callback) {
    keep_settings_callback(callback);
  }
  bool drop = building_main_menu > 0 && hide_main_menu_buttons.load();
  if (!drop) {
    const ButtonFilter filter = button_filter.load();
    drop = filter && filter(id);
  }
  if (drop) {
    destroy_callback(callback);
    return destroy_string(tooltip);
  }
  if (id == kExitGameButton) {
    add_menu_buttons(menu);
    add_bug_report_button(menu);
    add_leave_button(menu);
  }
  return original_add_button(menu, id, callback, enabled, tooltip);
}

// Target of the E8 rel32 call at function + offset, or null when no call is there.
void* call_target(void* function, std::size_t offset) {
  const auto* code = static_cast<const std::uint8_t*>(function) + offset;
  if (code[0] != 0xE8) {
    return nullptr;
  }
  std::int32_t relative = 0;
  std::memcpy(&relative, code + 1, sizeof(relative));
  return const_cast<std::uint8_t*>(code) + 5 + relative;
}

bool in_hidden_main_menu(void* menu) {
  return hide_main_menu_buttons.load() &&
         *(static_cast<const std::uint8_t*>(menu) + kMenuStateOffset) == 1;
}

void __fastcall hooked_pros_banner(void* menu) {
  if (!in_hidden_main_menu(menu)) {
    original_pros_banner(menu);
    return;
  }
  // Nothing, as the original does for an account that is already linked: no tile and no line.
  // SetPros(false), the original's branch for a service that is not online, hides the tile but
  // shows the line "Deep Silver Account servers are not available..." (ui_pros_banner_error)
  // under our page.
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true) && log_line) {
    log_line("MENU native PROS banner hidden");
  }
}

template <typename Function>
Function flash_slot(void* object, std::size_t slot) {
  return reinterpret_cast<Function>((*static_cast<void***>(object))[slot]);
}

void hide_clip(void* root, const char* name) {
  void* clip = nullptr;
  if (!flash_slot<FlashGetMember>(root, kFlashGetMemberSlot)(root, name, &clip) || !clip) {
    return;
  }
  const FlashValue hidden{kFlashBool, 0, 0};
  flash_slot<FlashSetMember>(clip, kFlashSetMemberSlot)(clip, "_visible", &hidden);
  flash_slot<FlashRelease>(clip, kFlashReleaseSlot)(clip);
}

// Shows or hides the movie's logo (_root.titleBox.logo).
void show_logo(void* root, bool shown) {
  void* box = nullptr;
  if (!flash_slot<FlashGetMember>(root, kFlashGetMemberSlot)(root, kTitleBoxClip, &box) || !box) {
    return;
  }
  void* logo = nullptr;
  if (flash_slot<FlashGetMember>(box, kFlashGetMemberSlot)(box, kLogoClip, &logo) && logo) {
    const FlashValue visible{kFlashBool, 0, shown ? 1u : 0u};
    flash_slot<FlashSetMember>(logo, kFlashSetMemberSlot)(logo, "_visible", &visible);
    flash_slot<FlashRelease>(logo, kFlashReleaseSlot)(logo);
  }
  flash_slot<FlashRelease>(box, kFlashReleaseSlot)(box);
}

// Hides the movie's "version" clip (its "version:" label stays even when the text is empty) and
// the account status line under our page. The game's logo goes too while our page covers the menu
// (the server browser carries its own name, and the logo showed through its glass); it shows on
// the game's own pages our page steps aside for, Settings and the first-time pages.
void hide_version_clip(void* root) {
  hide_clip(root, kVersionClip);
  hide_clip(root, kUserMessageClip);
  const bool native_page =
      settings_requested.load() || settings_shown.load() || overlay_open.load();
  show_logo(root, native_page);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true) && log_line) {
    log_line("MENU native version label hidden");
  }
}

char __fastcall hooked_flash_invoke(void* object, const char* method, const void* args,
                                    unsigned count, void* result) {
  const char invoked = original_flash_invoke(object, method, args, count, result);
  if (!method) {
    return invoked;
  }
  if (std::strcmp(method, kSetVersionFunction) == 0) {
    menu_root.store(object);
  }
  // Any of the movie's own functions may show the clips again (fc_preparePage on every page, the
  // account status whenever it changes): hide them after each call on the menu root.
  if (object == menu_root.load() && version_hidden.load()) {
    hide_version_clip(object);
  }
  return invoked;
}

void __fastcall hooked_menu_open(void* menu) {
  original_menu_open(menu);
  // The pause menu keeps its label: its own fc_preparePage shows the clip again.
  version_hidden.store(in_hidden_main_menu(menu));
  if (!version_hidden.load()) {
    return;
  }
  // Our page shows the version; the movie's version text stays empty in the main menu.
  std::uintptr_t name = 0;
  string_init(&name, "SetVersion");
  const char* empty = "";
  invoke_text(menu, &name, &empty);
  string_free(&name);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true) && log_line) {
    log_line("MENU native version text hidden");
  }
}

std::int64_t __fastcall hooked_open_confirmation(void* menu, std::uint8_t id, void* yes, void* no) {
  const PauseLeave leave = pause_leave.load();
  opening_leave_confirmation =
      id == kExitToMenuConfirmation && leave.available && leave.available();
  const std::int64_t opened = original_open_confirmation(menu, id, yes, no);
  opening_leave_confirmation = false;
  return opened;
}

char __fastcall hooked_add_confirmation(void* menu, void* name, void* id, void* text, void* yes,
                                        void* no, void* first, void* second) {
  if (!opening_leave_confirmation || !string_init || !string_free) {
    return original_add_confirmation(menu, name, id, text, yes, no, first, second);
  }
  std::uintptr_t question = 0;
  string_init(&question, kLeaveConfirmationText);
  const char added = original_add_confirmation(menu, name, id, &question, yes, no, first, second);
  string_free(&question);
  return added;
}

// In a server's world the confirmed exit leaves the world without saving.
void __fastcall hooked_exit_to_menu_yes(void* capture) {
  const PauseLeave leave = pause_leave.load();
  if (leave.available && leave.available() && leave.leave) {
    if (log_line) {
      log_line("MENU pause menu: exit to main menu confirmed (no save)");
    }
    leave.leave();
    return;
  }
  original_exit_to_menu_yes(capture);
}

char __fastcall hooked_first_time_overlay(void* menu) {
  const char opened = original_first_time_overlay(menu);
  if (opened) {
    overlay_open.store(true);
    if (log_line) {
      log_line("MENU native first-time overlay opened (EULA or telemetry); our menu waits");
    }
  }
  return opened;
}

}  // namespace

bool install(Resolver resolve, Logger log) {
  log_line = log;
  void* build_root_menu = resolve("MainMenuBuild", kBuildRootMenuSignature);
  void* add_button = resolve("AddMenuButton", kAddButtonSignature);
  destroy_callback =
      reinterpret_cast<Cleanup>(resolve("MenuCallbackCleanup", kCallbackCleanupSignature));
  destroy_string = reinterpret_cast<Cleanup>(resolve("MenuStringCleanup", kStringCleanupSignature));
  if (!build_root_menu || !add_button || !destroy_callback || !destroy_string) {
    log("MENU native hooks unavailable: build=%p add=%p cleanup=%p/%p", build_root_menu, add_button,
        reinterpret_cast<void*>(destroy_callback), reinterpret_cast<void*>(destroy_string));
    hooks_failed.store(true);
    return false;
  }
  if (MH_CreateHook(add_button, reinterpret_cast<void*>(hooked_add_button),
                    reinterpret_cast<void**>(&original_add_button)) != MH_OK) {
    hooks_failed.store(true);
    return false;
  }
  // KCDS's settings category: the page preparation, the list builder and its ShowPage (the jump
  // it ends in). Without them the category is left out.
  prepare_page =
      reinterpret_cast<PreparePage>(resolve("SettingsPreparePage", kPreparePageSignature));
  settings_list = reinterpret_cast<SettingsList>(resolve("SettingsList", kSettingsListSignature));
  if (settings_list) {
    const auto* jump = reinterpret_cast<const std::uint8_t*>(settings_list) + kSettingsListShowJump;
    if (jump[0] == 0xE9) {
      std::int32_t relative = 0;
      std::memcpy(&relative, jump + 1, sizeof(relative));
      show_page = reinterpret_cast<ShowPage>(const_cast<std::uint8_t*>(jump) + 5 + relative);
    }
  }
  log("MENU KCDS settings category %s",
      prepare_page && settings_list && show_page ? "ready" : "unavailable");
  if (MH_CreateHook(build_root_menu, reinterpret_cast<void*>(hooked_build_root_menu),
                    reinterpret_cast<void**>(&original_build_root_menu)) != MH_OK) {
    MH_RemoveHook(add_button);
    hooks_failed.store(true);
    return false;
  }
  if (MH_EnableHook(add_button) != MH_OK || MH_EnableHook(build_root_menu) != MH_OK) {
    MH_DisableHook(add_button);
    MH_DisableHook(build_root_menu);
    MH_RemoveHook(add_button);
    MH_RemoveHook(build_root_menu);
    hooks_failed.store(true);
    return false;
  }
  log("MENU native hooks installed build=%p add=%p", build_root_menu, add_button);
  // Optional: without it the menu still waits for the first main-menu build, which the game skips
  // while a first-time page is pending at startup.
  void* first_time = resolve("FirstTimeOverlay", kFirstTimeOverlaySignature);
  const bool watched =
      first_time &&
      MH_CreateHook(first_time, reinterpret_cast<void*>(hooked_first_time_overlay),
                    reinterpret_cast<void**>(&original_first_time_overlay)) == MH_OK &&
      MH_EnableHook(first_time) == MH_OK;
  log("MENU native first-time overlay watch=%u at %p", unsigned(watched), first_time);

  // Optional: without them the PROS banner and the version text stay behind our page.
  void* pros = resolve("MainMenuProsBanner", kProsBannerSignature);
  void* menu_open = resolve("MenuOpen", kMenuOpenSignature);
  if (pros && menu_open) {
    string_init = reinterpret_cast<StringInit>(call_target(pros, kProsStringInitCall));
    string_free = reinterpret_cast<StringFree>(call_target(pros, kProsStringFreeCall));
    invoke_text = reinterpret_cast<InvokeText>(call_target(menu_open, kMenuOpenSetVersionCall));
  }
  const bool helpers = string_init && string_free && invoke_text;
  bool pros_hooked = false;
  bool open_hooked = false;
  if (helpers) {
    pros_hooked = MH_CreateHook(pros, reinterpret_cast<void*>(hooked_pros_banner),
                                reinterpret_cast<void**>(&original_pros_banner)) == MH_OK &&
                  MH_EnableHook(pros) == MH_OK;
    open_hooked = MH_CreateHook(menu_open, reinterpret_cast<void*>(hooked_menu_open),
                                reinterpret_cast<void**>(&original_menu_open)) == MH_OK &&
                  MH_EnableHook(menu_open) == MH_OK;
  }
  log("MENU native PROS banner hook=%u version hook=%u (pros=%p open=%p)", unsigned(pros_hooked),
      unsigned(open_hooked), pros, menu_open);

  // Optional: without it the movie's "version:" label stays in the corner.
  void* flash_invoke = open_hooked ? resolve("FlashInvoke", kFlashInvokeSignature) : nullptr;
  const bool label_hooked =
      flash_invoke &&
      MH_CreateHook(flash_invoke, reinterpret_cast<void*>(hooked_flash_invoke),
                    reinterpret_cast<void**>(&original_flash_invoke)) == MH_OK &&
      MH_EnableHook(flash_invoke) == MH_OK;
  log("MENU native version label hook=%u at %p", unsigned(label_hooked), flash_invoke);

  // Optional: without them the pause menu keeps its buttons and a world is left by quitting.
  void* pause_exit = resolve("PauseExitToMenuCallback", kPauseExitCallbackSignature);
  void* exit_yes = resolve("ExitToMenuConfirm", kExitToMenuYesSignature);
  void* add_basic = call_target(add_button, kAddBasicButtonCall);
  if (pause_exit) {
    make_exit_to_menu_callback =
        reinterpret_cast<MakeCallback>(call_target(pause_exit, kPauseExitCallbackCall));
  }
  const bool exit_hooked =
      make_exit_to_menu_callback && exit_yes && add_basic && string_init && string_free &&
      MH_CreateHook(exit_yes, reinterpret_cast<void*>(hooked_exit_to_menu_yes),
                    reinterpret_cast<void**>(&original_exit_to_menu_yes)) == MH_OK &&
      MH_EnableHook(exit_yes) == MH_OK &&
      MH_CreateHook(add_basic, reinterpret_cast<void*>(hooked_add_basic_button),
                    reinterpret_cast<void**>(&original_add_basic_button)) == MH_OK &&
      MH_EnableHook(add_basic) == MH_OK;
  if (!exit_hooked) {
    make_exit_to_menu_callback = nullptr;
  }
  log("MENU native pause leave button=%u (callback=%p confirm=%p label=%p)", unsigned(exit_hooked),
      reinterpret_cast<void*>(make_exit_to_menu_callback), exit_yes, add_basic);
  // Optional: without them the confirmation keeps the game's "save and return" question.
  void* open_confirmation =
      exit_hooked ? resolve("OpenConfirmation", kOpenConfirmationSignature) : nullptr;
  void* add_confirmation =
      open_confirmation ? call_target(open_confirmation, kAddConfirmationCall) : nullptr;
  const bool question_hooked =
      add_confirmation &&
      MH_CreateHook(open_confirmation, reinterpret_cast<void*>(hooked_open_confirmation),
                    reinterpret_cast<void**>(&original_open_confirmation)) == MH_OK &&
      MH_EnableHook(open_confirmation) == MH_OK &&
      MH_CreateHook(add_confirmation, reinterpret_cast<void*>(hooked_add_confirmation),
                    reinterpret_cast<void**>(&original_add_confirmation)) == MH_OK &&
      MH_EnableHook(add_confirmation) == MH_OK;
  log("MENU native leave question=%u (open=%p add=%p)", unsigned(question_hooked),
      open_confirmation, add_confirmation);
  return true;
}

void set_pause_leave(PauseLeave leave) {
  pause_leave.store(leave);
}

void set_pause_bug_report(PauseBugReport report) {
  pause_bug_report.store(report);
}

void set_hide_main_menu_buttons(bool hide) {
  hide_main_menu_buttons.store(hide);
}

void reset_main_menu_built() {
  built.store(false);
}

bool main_menu_built() {
  return built.load();
}

bool hooks_unavailable() {
  return hooks_failed.load();
}

bool first_time_overlay_open() {
  return overlay_open.load();
}

bool open_settings() {
  {
    std::lock_guard<std::mutex> lock(settings_mutex);
    if (!settings_callback) {
      return false;
    }
  }
  settings_requested.store(true);
  return true;
}

bool settings_open() {
  return settings_requested.load() || settings_shown.load();
}

void frame() {
  // A test kit's request for the KCDS page, once the settings list was built.
  if (kcds_page_requested.load()) {
    if (void* menu = last_settings_menu.load()) {
      kcds_page_requested.store(false);
      open_kcds_page(menu);
    }
  }
  if (!settings_requested.load()) {
    return;
  }
  // Shown before the request is cleared, so settings_open() never reads false in between and our
  // page does not flash back over the menu.
  settings_shown.store(true);
  settings_requested.store(false);
  // The callback runs on a copy and without the lock: the game may build its main menu again
  // from inside it, which replaces the kept callback.
  std::function<void()> open;
  {
    std::lock_guard<std::mutex> lock(settings_mutex);
    open = settings_callback;
  }
  if (!open) {
    settings_shown.store(false);
    return;
  }
  if (log_line) {
    log_line("MENU native settings opened from our page");
  }
  open();
}

void set_menu_buttons(std::vector<MenuButton> buttons) {
  if (buttons.size() > kMaxMenuButtons) {
    buttons.resize(kMaxMenuButtons);
  }
  std::lock_guard<std::mutex> lock(menu_buttons_mutex);
  menu_buttons = std::move(buttons);
}

void set_menu_button_pressed(void (*pressed)(const std::string& key)) {
  menu_button_pressed.store(pressed);
}

void set_settings_page(SettingsPage page) {
  std::lock_guard<std::mutex> lock(settings_page_mutex);
  kcds_settings_page = page;
}

void request_kcds_page() {
  kcds_page_requested.store(true);
}

void set_button_filter(ButtonFilter filter) {
  button_filter.store(filter);
}

}  // namespace kcds::menu_native
