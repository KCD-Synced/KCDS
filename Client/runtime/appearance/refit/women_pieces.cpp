#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "women_pieces.hpp"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../files/stored_zip.hpp"
#include "body_fit.hpp"
#include "kcd2s/bridge/sigscan.hpp"
#include "skin_file.hpp"

namespace kcds::runtime::appearance::refit::pieces {
namespace {

constexpr char kClothingType[] = ".?AVC_Clothing@clothing@entitymodule@wh@@";
constexpr char kSkinElementType[] = ".?AVC_SkinElement@clothing@entitymodule@wh@@";
constexpr char kSkinCountType[] =
    ".?AV?$_Ref_count@VC_SkinElement@clothing@entitymodule@wh@@@std@@";

// C_Component / C_Clothing.
constexpr std::size_t kGender = 24;
constexpr std::size_t kGenderSet = 25;
constexpr std::uint8_t kMale = 1;
constexpr std::size_t kFilePath = 32;
constexpr std::size_t kElementsBegin = 80;
constexpr std::size_t kElementsEnd = 88;
constexpr std::size_t kCloneSlot = 1;
// C_SkinElement.
constexpr std::size_t kSkinElementSize = 56;
constexpr std::size_t kPart = 8;
constexpr std::size_t kModel = 24;
constexpr std::size_t kModelSet = 32;
constexpr std::size_t kMaterial = 40;
constexpr std::size_t kMaterialSet = 48;
// std::_Ref_count<C_SkinElement>: its vtable, uses, weak uses, the element.
constexpr std::size_t kCountSize = 24;

// The parts refitted: torso, arms, hands, legs, feet, waist, belt (equipment_part.xml).
constexpr std::uint32_t kBodyParts[] = {2, 3, 4, 5, 6, 11, 12};

constexpr char kCharacters[] = "Objects/characters/";
constexpr char kFittedPrefix[] = "kcds_female_";
constexpr char kMaleBody[] = "Objects/characters/humans/male/body/male_body.skin";
constexpr char kFemaleBody[] = "Objects/characters/humans/female/body/female_body_naked.skin";
constexpr char kFemaleSkeleton[] = "Objects/characters/humans/female/skeleton/female.chr";
// A new fitting gives the cached files a new name.
constexpr char kFitVersion[] = "2";

// ICryPak by byte offset in its vtable: FOpen, FReadRaw, FGetSize, FClose, OpenPacks.
constexpr std::size_t kPakOpen = 288;
constexpr std::size_t kPakRead = 304;
constexpr std::size_t kPakSize = 368;
constexpr std::size_t kPakClose = 440;
constexpr std::size_t kPakOpenPacks = 0x48;
constexpr unsigned kModPakFlags = 0x10400;
constexpr std::size_t kMaxFileBytes = 64 * 1024 * 1024;

Natives natives;
Logger log_line = nullptr;
const void* clothing_vtable = nullptr;
const void* skin_element_vtable = nullptr;
const void* skin_count_vtable = nullptr;
void* pak = nullptr;
std::string binding_root;

struct Fitted {
  void* shared[2] = {nullptr, nullptr};
};
// Each man's piece a woman wore: its fitted copy, or none when it has nothing to fit.
std::unordered_map<const void*, Fitted> fitted;
// The copies themselves, which are put on as they are.
std::unordered_set<const void*> copies;

std::unique_ptr<BodyField> body_field;
std::vector<Joint> her_joints;
bool body_tried = false;

template <class T>
T read(const void* object, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[offset / sizeof(void*)]);
}

const char* text_of(const void* object, std::size_t offset) {
  const char* text = read<const char*>(object, offset);
  return text ? text : "";
}

const void* vtable_of(const char* type) {
  const std::size_t at = kcd2s::bridge::rtti_vtable(
      natives.image, natives.image_size, reinterpret_cast<std::uintptr_t>(natives.image), type);
  return at == kcd2s::bridge::knpos ? nullptr : natives.image + at;
}

bool read_game_file(const std::string& path, std::vector<std::uint8_t>& out) {
  using Open = void*(__fastcall*)(void*, const char*, const char*, unsigned);
  using Read = std::size_t(__fastcall*)(void*, void*, std::size_t, std::size_t, void*);
  using Size = std::size_t(__fastcall*)(void*, void*);
  using Close = int(__fastcall*)(void*, void*);
  void* file = slot<Open>(pak, kPakOpen)(pak, path.c_str(), "rb", 0);
  if (!file) {
    return false;
  }
  const std::size_t size = slot<Size>(pak, kPakSize)(pak, file);
  bool done = false;
  if (size > 0 && size <= kMaxFileBytes) {
    out.resize(size);
    done = slot<Read>(pak, kPakRead)(pak, out.data(), 1, size, file) == size;
  }
  slot<Close>(pak, kPakClose)(pak, file);
  return done;
}

// The male and female bodies' difference, read and measured once.
const BodyField* field() {
  if (body_tried) {
    return body_field.get();
  }
  body_tried = true;
  std::vector<std::uint8_t> male_bytes;
  std::vector<std::uint8_t> female_bytes;
  std::vector<std::uint8_t> skeleton_bytes;
  SkinFile male;
  SkinFile female;
  SkinFile skeleton;
  if (!read_game_file(kMaleBody, male_bytes) || !read_game_file(kFemaleBody, female_bytes) ||
      !read_game_file(kFemaleSkeleton, skeleton_bytes) || !read_skin(male_bytes, male) ||
      !read_skin(female_bytes, female) || !read_skin(skeleton_bytes, skeleton)) {
    log_line("REFIT the game's bodies could not be read: women wear men's pieces as made");
    return nullptr;
  }
  auto built = std::make_unique<BodyField>();
  if (!built->build(male, skeleton.joints, female.positions)) {
    log_line("REFIT the bodies' difference could not be measured");
    return nullptr;
  }
  her_joints = skeleton.joints;
  body_field = std::move(built);
  log_line("REFIT body field of %zu points ready", body_field->size());
  return body_field.get();
}

std::wstring refit_folder() {
  wchar_t temp[MAX_PATH];
  const DWORD length = GetTempPathW(MAX_PATH, temp);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  std::wstring folder = std::wstring(temp) + L"kcds-characters";
  CreateDirectoryW(folder.c_str(), nullptr);
  folder += L"\\refit";
  CreateDirectoryW(folder.c_str(), nullptr);
  return folder;
}

std::uint64_t fnv1a(const std::string& text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char c : text) {
    hash = (hash ^ std::uint8_t(c)) * 1099511628211ULL;
  }
  return hash;
}

std::string utf8_of(const std::wstring& path) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string text(std::size_t(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, text.data(), size, nullptr, nullptr);
  return text;
}

// The fitted file `fitted_path` of the game's skin `original_path`, as a mounted pack; written
// first when it is not kept from an earlier start.
bool provide_fitted(const std::string& original_path, const std::string& fitted_path) {
  const std::wstring folder = refit_folder();
  if (folder.empty() || !pak || binding_root.empty()) {
    return false;
  }
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.pak",
                static_cast<unsigned long long>(fnv1a(fitted_path + "|" + kFitVersion)));
  const std::wstring pack = folder + L"\\" + std::wstring(name, name + std::strlen(name));
  if (GetFileAttributesW(pack.c_str()) == INVALID_FILE_ATTRIBUTES) {
    const BodyField* body = field();
    std::vector<std::uint8_t> bytes;
    SkinFile skin;
    if (!body || !read_game_file(original_path, bytes) || !read_skin(bytes, skin)) {
      return false;
    }
    const std::vector<float> moved = fit(skin, carry(skin.joints, her_joints), *body);
    std::vector<std::uint8_t> fitted_bytes = with_positions(bytes, skin, moved);
    if (fitted_bytes.empty()) {
      return false;
    }
    const std::vector<std::uint8_t> zip =
        files::stored_zip_entries({{fitted_path, std::move(fitted_bytes)}});
    std::ofstream out(pack, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(zip.data()), std::streamsize(zip.size()));
    if (!out) {
      return false;
    }
  }
  using OpenPacks = bool(__fastcall*)(void*, const char*, const char*, unsigned, void*);
  const std::string path = utf8_of(pack);
  return !path.empty() && slot<OpenPacks>(pak, kPakOpenPacks)(pak, binding_root.c_str(),
                                                              path.c_str(), kModPakFlags, nullptr);
}

bool body_part(std::uint32_t part) {
  for (const std::uint32_t wanted : kBodyParts) {
    if (part == wanted) {
      return true;
    }
  }
  return false;
}

// A copy of a skin element with `model` as its model, held by a shared_ptr of its own (the two
// pointers in `shared`). Its strings are its own: nothing of the original is released.
bool copy_skin_element(const std::uint8_t* original, const std::string& model, void* shared[2]) {
  auto* element = static_cast<std::uint8_t*>(natives.allocate(kSkinElementSize));
  auto* count = static_cast<std::uint8_t*>(natives.allocate(kCountSize));
  if (!element || !count) {
    return false;
  }
  std::memcpy(element, original, kSkinElementSize);
  natives.string_init(element + kModel);
  natives.string_assign(element + kModel, model.c_str());
  element[kModelSet] = 1;
  if (original[kMaterialSet]) {
    natives.string_init(element + kMaterial);
    natives.string_assign(element + kMaterial, text_of(original, kMaterial));
  }
  const void* vtable = skin_count_vtable;
  std::memcpy(count, &vtable, sizeof(vtable));
  const std::int32_t one = 1;
  std::memcpy(count + 8, &one, sizeof(one));
  std::memcpy(count + 12, &one, sizeof(one));
  std::memcpy(count + 16, &element, sizeof(element));
  shared[0] = element;
  shared[1] = count;
  return true;
}

// The fitted copy of `component`; an empty one when nothing of it is refitted.
Fitted make_fitted(void* component) {
  Fitted result;
  const std::string file_path = text_of(component, kFilePath);
  struct Swap {
    std::size_t index;
    void* shared[2];
  };
  std::vector<Swap> swaps;
  auto* begin = read<std::uint8_t*>(component, kElementsBegin);
  auto* end = read<std::uint8_t*>(component, kElementsEnd);
  std::size_t index = 0;
  for (auto* at = begin; at && at < end; at += 16, ++index) {
    auto* element = read<std::uint8_t*>(at, 0);
    if (!element || read<const void*>(element, 0) != skin_element_vtable ||
        !element[kModelSet] || !body_part(read<std::uint32_t>(element, kPart))) {
      continue;
    }
    const std::string model = text_of(element, kModel);
    if (model.empty()) {
      continue;
    }
    const std::size_t slash = model.find_last_of("/\\");
    const std::string folder = slash == std::string::npos ? "" : model.substr(0, slash + 1);
    const std::string name = slash == std::string::npos ? model : model.substr(slash + 1);
    const std::string fitted_model = folder + kFittedPrefix + name;
    const std::string original_path = kCharacters + file_path + model;
    const std::string fitted_path = kCharacters + file_path + fitted_model;
    if (!provide_fitted(original_path, fitted_path)) {
      log_line("REFIT %s not fitted (kept as made)", original_path.c_str());
      continue;
    }
    Swap swap{index, {nullptr, nullptr}};
    if (!copy_skin_element(element, fitted_model, swap.shared)) {
      continue;
    }
    swaps.push_back(swap);
    log_line("REFIT %s fitted to a woman's body", original_path.c_str());
  }
  if (swaps.empty()) {
    return result;
  }
  using Clone = void*(__fastcall*)(void*, void**);
  void* clone[2] = {nullptr, nullptr};
  slot<Clone>(component, kCloneSlot * sizeof(void*))(component, clone);
  if (!clone[0]) {
    return result;
  }
  auto* clone_begin = read<std::uint8_t*>(clone[0], kElementsBegin);
  auto* clone_end = read<std::uint8_t*>(clone[0], kElementsEnd);
  for (const Swap& swap : swaps) {
    std::uint8_t* entry = clone_begin + swap.index * 16;
    if (entry + 16 > clone_end) {
      continue;
    }
    // The copy's entry held the original element; that reference is left to the original.
    std::memcpy(entry, swap.shared, sizeof(swap.shared));
  }
  result.shared[0] = clone[0];
  result.shared[1] = clone[1];
  copies.insert(clone[0]);
  return result;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.image || !natives.allocate || !natives.string_init || !natives.string_assign) {
    return false;
  }
  clothing_vtable = vtable_of(kClothingType);
  skin_element_vtable = vtable_of(kSkinElementType);
  skin_count_vtable = vtable_of(kSkinCountType);
  return clothing_vtable && skin_element_vtable && skin_count_vtable;
}

void set_pak(void* game_pak, const char* root) {
  pak = game_pak;
  binding_root = root ? root : "";
}

void* const* fitted_for_woman(void* const* component) {
  if (!clothing_vtable || !pak || !component || !component[0]) {
    return nullptr;
  }
  void* object = component[0];
  const auto* bytes = static_cast<const std::uint8_t*>(object);
  if (read<const void*>(object, 0) != clothing_vtable || copies.count(object) != 0 ||
      !bytes[kGenderSet] || bytes[kGender] != kMale) {
    return nullptr;
  }
  auto found = fitted.find(object);
  if (found == fitted.end()) {
    found = fitted.emplace(object, make_fitted(object)).first;
  }
  return found->second.shared[0] ? found->second.shared : nullptr;
}

}  // namespace kcds::runtime::appearance::refit::pieces
