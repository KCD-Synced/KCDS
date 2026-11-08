// NativeUI for client resources: the game side of
// Client/resources/native_ui.cjs.
//
// NativeElement drives the game's own UI elements (IUIElement, WHGame 1.5.5) the way the game's
// UIAction script binding does (CScriptBind_UIAction, registered): the element and
// one of its instances come (IFlashUI::GetUIElement, then GetInstance), and the
// element's vtable does the rest:
//   +128 Unload, +136 Reload, +208 RequestHide, +224 SetVisible(bool),
//   +352 GetVariableDesc, +616 SetVariable, +632 GetVariable,
//   +376 GetArrayDesc, +664 SetArray, +680 GetArray,
//   +472 GetFunctionDesc, +544 CallFunction(desc, args, return, 0),
//   +568 GetMovieClip(name, 0), +528 AddEventListener, +536 RemoveEventListener.
// A movie clip (IFlashVariableObject) has +200 SetDisplayInfo, +208 GetDisplayInfo,
// +336 / +344 GotoAndPlay (frame / label), +352 / +360 GotoAndStop (frame / label).
//
// Values cross as TUIData (24 bytes, the type at +16: 0 int, 1 float, 2 EntityId, 3 Vec3,
// 4 string, 5 wide string, 6 bool) inside SUIArguments (appended with
// entries are 32 bytes from +8).
//
// Reads answer at once through the synchronous channel (Client/resources/sync_channel.cjs): the
// runtime sends { op: "syncRequest", id, request } and gets { op: "syncReply", id, value } on the
// next frame. Element events go out as { op: "nativeElementEvent", name, instance, event, args }.
//
// Vanilla menus (C_UIApse): slot 1 Open(page, context), slot 2
// Close(instant), slot 3 the current page, slot 7 MoveTo(page, ...) (the tab switch),
// the open state machine at +104 (its slot 1: 0 or 1 when closed). Pages, from the open_apse_* and
// move_* action handlers: 1 crafting, 2 inventory, 4 player, 6 map, 7 codex, 10 journal. The
// runtime sends { op: "nativeMenus", disabled: [page...] }; Open and MoveTo refuse those pages and
// an open disabled menu closes on the next frame.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>
#include <functional>
#include <string>
#include <utility>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::native_ui {

using RuntimeSender = std::function<bool(const kcd2s::sdk::Value&)>;
using Logger = void (*)(const char* format, ...);
using StringAssign = void*(__fastcall*)(void*, const char*);

struct Natives {
  StringAssign string_assign = nullptr;
  void* string_init = nullptr;
  void* string_free = nullptr;
  void* element = nullptr;
  void* args_init = nullptr;
  void* args_free = nullptr;
  void* args_add = nullptr;
  void* data_free = nullptr;
  void* menu_open = nullptr;
  void* menu_move = nullptr;
  void* function_free = nullptr;
  void** pak = nullptr;          // the ICryPak GFileCryPak opens files with
  void* inflate_init = nullptr;  // inflateInit2_
  void* inflate = nullptr;
  void* inflate_end = nullptr;
  void* file_open = nullptr;        // CryGFxFileOpener::OpenFile
  void* file_create = nullptr;      // GFileCryPak(path, flags)
  void** file_allocator = nullptr;  // the allocator OpenFile takes the GFileCryPak from
  // A file mounted for a resource's replace/ folder, by its game path: a recipe table's patch is
  // loaded into the game's table (crafting/recipe_tables.hpp).
  bool (*table_patch)(const std::string& path, const std::string& text) = nullptr;
};

bool install(const Natives& natives, Logger logger);

// For local::Client::set_local_runtime_handler: true when the message was NativeUI's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// A blip's own map icon is mounted and readable under its name. Engine thread.
bool map_icon_ready(const std::string& name);

// A resource's native screen has the focus: game actions stop and the pointer shows.
bool has_focus();

// A picture of the world without the resources' native screens (Render.view's ui: false,
// render/render_view.hpp): the screens shown now are hidden while its frames are filmed, behind
// the kept frame the player sees, and shown again after. Engine thread.
void hide_screens_for_picture();
void show_screens_after_picture();

// Keeps the vanilla menu on one page: Open and MoveTo refuse every other page until the hold is
// released with -1 (the CharacterCreator holds the player page while its stage is up).
void hold_page(int page);

// The element instance (IUIElement) of a game UI element, or null while the game has none.
void* element_instance(const char* name, int instance);

// Engine thread: answers a request in the runtime's own form ({ op: "elementCall", name,
// instance, fn, args }, "elementSetClip", "elementGetClip" and the others) at once, for the
// client's own callers (the resource Hud); null when NativeUI is not installed.
kcd2s::sdk::Value request(const kcd2s::sdk::Value& body);

// Engine thread, every frame: answers the queued requests and applies the queued changes.
void frame(bool in_world);

// Engine thread: mounts the resources' files that wait to be mounted (their stream/ and replace/
// packs, pictures and icons) now. The world start calls it right before the level loads: the
// level then reads a resource's replacing files (a texture, a model) instead of the game's
// frame() would mount them only after the load.
void mount_pending();

// Engine thread: a file a resource ships, which the runtime sent ({ op: "nativeFile" }, kept in
// memory as <resource>/<path>), put in the game's file system at <root><entry> in a
// stored pack mounted with ICryPak OpenPack, as the book pictures are, for the game's loaders that
// read the archives only (an animation clip). The entry is a file name without folders.
// True when the game reads the file there, also when it was mounted before; false when the file
// was not sent yet or the pack could not be mounted.
bool mount_shipped(const std::string& resource, const std::string& path, const std::string& root,
                   const std::string& entry);

// Engine thread: files of this PC (World Architect's own assets folder) put in the game's file
// system at their game paths ("objects/kcds/<name>/stall.cgf"), as a resource's streamed files
// are: a stored pack per top folder, mounted with ICryPak OpenPack. `key` names the packs (a new
// key for files that changed). Answers the paths the game reads.
std::vector<std::string> mount_local_files(
    const std::string& key,
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& files);

// Engine thread: a recipe table patch the server sent, put in the game's file
// system at its game path (Libs/Tables/minigame/<file>) in a stored pack of its own, then loaded
// into the game's table as a resource's replace/ file is (table_patch). True when the game reads
// it there, also when it was mounted before.
bool mount_table_patch(const std::string& path, const std::string& text);

// Engine thread: files put in the game's file system below a top folder ("objects/"), each
// entry a path under it and its bytes, in a stored pack of their own (from memory when KCDS's
// memory paks are in: assets/memory_paks.hpp). `key` names the pack; a new key for every new set,
// as the game keeps a mounted pack open. True when the pack was mounted.
bool mount_files(const std::string& key, const std::string& folder,
                 const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries);

// Engine thread: an item icon the server sent (a recipe's own), a .dds the game
// shows as the icon of that name (Libs/UI/Textures/Icons/Items/<name>_icon.dds), fitted onto the
// item icon square. True when the game reads it, also when it was mounted before.
bool mount_item_icon(const std::string& name, const std::vector<std::uint8_t>& dds);

// Engine thread: one of an alchemy recipe's pictures in the recipe book, mounted as
// Libs/UI/Textures/Books/{Products,Recipes}/<recipe id>_ui.dds: its own .dds, or (dds empty) a
// copy of the game's own picture of recipe `from`. True when the game reads it, also when it
// was mounted before.
bool mount_recipe_picture(bool product, std::uint32_t recipe_id, std::uint32_t from,
                          const std::vector<std::uint8_t>& dds);

// Engine thread: one of the client's own files (key "_kcds/<name>"), kept in memory, which a movie
// loads as "/%USER%/kcds_ui/<key>" (nothing is written to disk). False for a key outside _kcds/.
bool write_own_file(const std::string& key, const std::vector<std::uint8_t>& bytes);

// A file of the game's file system (in a pack or on disk, up to 64 MiB) read whole; false when it
// is not there. Any thread.
bool read_game_data(const std::string& path, std::vector<std::uint8_t>& out);

// Network thread: sends the answers and the element events to the runtime of session.
void pump(const void* session, const RuntimeSender& send);

// Any thread: whether the Esc menu or one of the game's own screens (the inventory, the map, the
// journal and the like) shows over the world now, as the engine thread last looked.
bool world_covered_by_screen();

}  // namespace kcds::runtime::native_ui
