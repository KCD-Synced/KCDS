// Voice chat in the game's own Settings > Sound page (the voice settings belong
// in the game's real settings, not a page of ours).
//
// WHGame.dll 1.5.5, resolved by unique signature
// (voice_settings_page_install):
//  sound page builder (C_UISettings): ClearAll and PreparePage of page 14, the
//                Master, Music and Sound volume sliders (AddValueButton), then reads
//                the three volumes, their SetValue, the Reset / Apply / Back buttons (container 1)
//                and ShowPage.
//  reads the three volumes; also called from four other places. While the sound page
//                is being built, the voice rows are added right after it: below the game's own
//                sliders and before its bottom buttons.
//  the page's OnInteractiveValue(settings, id by value, float value in xmm2)
//  the page's OnInteractiveChoice(settings, id by value, unsigned choice)
//                Both look the id up in the game's button names and ignore an id that
//                is not one of the game's; they also free the id. The hooks apply the voice rows'
//                changes first and always run the game's handler after.
//  applies three volumes (settings, float[3], refresh): leaving with No or with
//                nothing changed passes the ones in effect (+230h), Reset the defaults (+118h),
//                Confirm the sliders'. The voice rows apply at once, so that they are heard; No
//                puts the voice settings back as they were when the page opened, Reset sets the
//                defaults.
// The rows are the Menu movie's own (Libs/UI/UIElements/Menu.xml): AddValueButton (a slider) and
// AddChoicesButton with AddChoiceOption (a choice), in the list container 0, through the element's
// CallFunction (native_ui), as the game's own rows are.
#pragma once

namespace kcds::runtime::voice::settings_page {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* sound_page = nullptr;
  void* read_volumes = nullptr;
  void* value_event = nullptr;
  void* choice_event = nullptr;
  void* apply_volumes = nullptr;
  Logger log = nullptr;
};

// Hooks the five through the process's MinHook instance (already initialized). False, with nothing
// enabled, when one is missing or a hook fails.
bool install(const Natives& natives);

// Engine thread, every frame (in the menus too): while the sound page is open, its "Microphone
// test" slider shows the microphone's level.
void frame();

}  // namespace kcds::runtime::voice::settings_page
