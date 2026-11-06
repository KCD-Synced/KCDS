#include "studio_mesh.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../core/log.hpp"
#include "../files/base64.hpp"
#include "../native_ui/native_ui.hpp"
#include "../physical_props/cloth.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "../props/prop_physics.hpp"
#include "cgf_mesh.hpp"
#include "cgf_patch.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_hold.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::mesh {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace fs = std::filesystem;

constexpr const char* kEntityName = "kcds_studio_mesh";
constexpr std::size_t kLongestName = 48;
constexpr std::uintmax_t kLargestFile = 64u * 1024u * 1024u;

struct Request {
  bool list = false;
  bool show = false;
  // Only a new place, turn and size for the mesh shown (studioMeshPlace).
  bool place = false;
  // The mesh shown made cloth or solid again (studioMeshCloth).
  bool cloth = false;
  bool cloth_on = false;
  float mass = 1.0f;
  // The mesh shown given a body (studioMeshBody): none, solid or falling.
  bool body = false;
  props::entity::Physics body_kind = props::entity::Physics::None;
  // A mesh of yours written (studioMeshWrite): its .cgf bytes and .mtl text.
  bool write = false;
  std::vector<std::uint8_t> model;
  std::string material;
  std::string name;
  // One of the game's own objects instead of a mesh of yours (Objects/....cgf in its packs).
  std::string game_path;
  float at[3] = {0, 0, 0};
  float turn[3] = {0, 0, 0};
  float scale = 1.0f;
  // The character's joint the mesh is held in, '' for free in the scene; held, at
  // and turn are its place (metres) and turn (degrees) in the joint.
  std::string joint;
};

constexpr std::size_t kLongestJoint = 64;
// The longest material text of a shape the studio writes.
constexpr std::size_t kLargestMaterial = 16 * 1024;

constexpr float kLightestCloth = 0.05f;
constexpr float kHeaviestCloth = 10.0f;
// The cloth's pinned vertices: those within this of its highest vertex in the world.
constexpr float kPinnedBand = 0.01f;

std::mutex requests_mutex;
std::vector<Request> requests;
// Each mesh shown gets packs of its own (the game keeps a mounted pack and a loaded model).
unsigned shown_count = 0;
bool showing = false;
// The entity of the mesh shown (kEntityName and its number).
std::string entity_name = kEntityName;
// The name of the mesh shown (its model names its material objects/kcds_studio/<name>).
std::string shown_name;
// The material the mesh shown is drawn with: its .mtl under a name of its own each time
// (objects/kcds_studio/<name>_<number>), empty when it has none.
std::string shown_material;
// Whether the mesh shown is one of the game's objects (no files of yours, its own material).
bool shown_game = false;
// The last mesh asked for with its place (shown again to make it solid or to move its cloth),
// and whether it is cloth.
Request last_shown;
bool cloth_on = false;
float cloth_mass = 1.0f;
// Whether the mesh shown now is cloth, and when to make it cloth again after it was moved.
using Clock = std::chrono::steady_clock;
constexpr auto kClothAfterMove = std::chrono::milliseconds(500);
bool cloth_live = false;
std::optional<Clock::time_point> cloth_due;
// The body the mesh shown has: none (it stands as placed, nothing hits it), static
// (it stands and things hit it) or rigid (it falls from where it is placed and rests), of
// body_mass kg. Kept for each mesh shown after it, as cloth is.
props::entity::Physics body_kind = props::entity::Physics::None;
float body_mass = 5.0f;
constexpr float kLightestBody = 0.1f;
constexpr float kHeaviestBody = 200.0f;
// Frames after a falling body is made until its mass is checked (as placed props check theirs:
// a mesh whose collision carries no mass does not fall).
constexpr int kBodyCheckFrames = 5;
int body_check_frames = 0;

// Puts the mesh shown at its place, turn (degrees about x, y, z) and size.
void place_mesh(void* entity, const Request& request) {
  const float size = std::clamp(request.scale, 0.01f, 100.0f);
  const float scale[3] = {size, size, size};
  props::entity::set_scale(entity, scale);
  if (!request.joint.empty()) {
    // Held in a joint: it follows the joint every frame (follow_joint).
    return;
  }
  float turn[4];
  hold::turn_of_degrees(request.turn, turn);
  props::entity::place_in_world(entity, request.at, turn);
}

// A joint's name as the game's skeletons have them: letters, digits and '_', at most 64.
bool joint_name(const std::string& name) {
  return name.size() <= kLongestJoint && std::all_of(name.begin(), name.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_';
         });
}

bool mesh_name(const std::string& name) {
  return !name.empty() && name.size() <= kLongestName &&
         std::all_of(name.begin(), name.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
         });
}

// A path of the game's own object: Objects/....cgf, no way out of the packs.
bool game_object_path(const std::string& path) {
  return path.size() > 12 && path.size() < 260 && _strnicmp(path.c_str(), "Objects/", 8) == 0 &&
         _stricmp(path.c_str() + path.size() - 4, ".cgf") == 0 &&
         path.find("..") == std::string::npos && path.find(':') == std::string::npos;
}

// The game's object shown: the game loads it from its packs with its own material.
void show_game_object(const Request& request) {
  ++shown_count;
  entity_name = std::string(kEntityName) + "_" + std::to_string(shown_count);
  // With a body: spawned with the game's physics on, as a mesh of yours (show).
  const bool with_body =
      body_kind != props::entity::Physics::None && !cloth_on && request.joint.empty();
  void* entity = nullptr;
  if (with_body) {
    entity = props::entity::spawn_solid_mesh(entity_name, request.at, request.game_path);
  } else {
    entity = props::entity::spawn_mesh(entity_name, request.at, request.game_path, "");
  }
  showing = entity != nullptr;
  if (showing) {
    shown_name = request.name;
    shown_game = true;
    place_mesh(entity, request);
  }
  game_client::log("STUDIO mesh %s: %s", request.game_path.c_str(),
                   showing ? "the game's object shown" : "the game did not load it");
}

std::optional<fs::path> meshes_folder() {
  const char* local = std::getenv("LOCALAPPDATA");
  if (!local || !*local) {
    return std::nullopt;
  }
  return fs::path(local) / "KCDS" / "studio" / "meshes";
}

// Writes a mesh of yours made in the studio (its prop menu's shapes) as <name>.cgf
// and <name>.mtl into the meshes folder; the error, or empty when written.
std::string write_mesh(const Request& request) {
  const auto folder = meshes_folder();
  if (!folder) {
    return "no LOCALAPPDATA folder";
  }
  std::error_code error;
  fs::create_directories(*folder, error);
  const fs::path model_path = *folder / (request.name + ".cgf");
  const fs::path material_path = *folder / (request.name + ".mtl");
  std::ofstream model(model_path, std::ios::binary | std::ios::trunc);
  model.write(reinterpret_cast<const char*>(request.model.data()),
              static_cast<std::streamsize>(request.model.size()));
  model.close();
  std::ofstream material(material_path, std::ios::binary | std::ios::trunc);
  material.write(request.material.data(), static_cast<std::streamsize>(request.material.size()));
  material.close();
  if (!model || !material) {
    return "the files could not be written";
  }
  return std::string();
}

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path) {
  std::error_code error;
  const auto size = fs::file_size(path, error);
  if (error || size == 0 || size > kLargestFile) {
    return std::nullopt;
  }
  std::ifstream file(path, std::ios::binary);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  return file ? std::optional(bytes) : std::nullopt;
}

std::vector<std::string> mesh_names() {
  std::vector<std::string> names;
  const auto folder = meshes_folder();
  std::error_code error;
  if (!folder || !fs::is_directory(*folder, error)) {
    return names;
  }
  for (const auto& entry : fs::directory_iterator(*folder, error)) {
    if (entry.path().extension() == ".cgf") {
      const std::string name = entry.path().stem().string();
      if (mesh_name(name)) {
        names.push_back(name);
      }
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace

std::vector<std::pair<std::string, std::vector<std::uint8_t>>> mesh_textures(
    const std::string& name) {
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> found;
  const auto folder = mesh_name(name) ? meshes_folder() : std::nullopt;
  std::error_code error;
  if (!folder || !fs::is_directory(*folder, error)) {
    return found;
  }
  const std::string prefix = name + "_";
  for (const auto& entry : fs::directory_iterator(*folder, error)) {
    const std::string file = entry.path().filename().string();
    const bool ours = file.size() > prefix.size() + 4 &&
                      file.compare(0, prefix.size(), prefix) == 0 &&
                      entry.path().extension() == ".dds";
    if (!ours || found.size() >= 16) {
      continue;
    }
    if (auto bytes = read_file(entry.path())) {
      found.emplace_back(file, std::move(*bytes));
    }
  }
  return found;
}

namespace {

void show(const Request& request) {
  physical_props::cloth::forget(nullptr);
  cloth_live = false;
  if (showing) {
    props::entity::remove(entity_name);
    showing = false;
  }
  shown_game = false;
  if (!request.game_path.empty()) {
    show_game_object(request);
    return;
  }
  if (request.name.empty()) {
    return;
  }
  const auto folder = meshes_folder();
  const auto model = folder ? read_file(*folder / (request.name + ".cgf")) : std::nullopt;
  if (!model) {
    game_client::log("STUDIO mesh %s: no such mesh file", request.name.c_str());
    return;
  }
  ++shown_count;
  // The model under a name of its own each time, so the game loads the new one; its material
  // keeps the name the converter gave it (objects/kcds_studio/<name>).
  const std::string model_path =
      "kcds_studio/" + request.name + "_" + std::to_string(shown_count) + ".cgf";
  // The material under a name of its own each time as well, the model's copy naming it: the game
  // keeps a material it has loaded by its name, so a mesh made anew in another colour drew in the
  // old one (studio_make_mesh). Drawn with a material set on the entity instead,
  // a texture put on it later did not show.
  std::vector<std::uint8_t> renamed = *model;
  std::string material_path;
  const auto material = read_file(*folder / (request.name + ".mtl"));
  if (material) {
    material_path = "kcds_studio/" + request.name + "_" + std::to_string(shown_count);
    if (!cgf::rename_material(renamed, "objects/" + material_path)) {
      material_path = "kcds_studio/" + request.name;
    }
  }
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
  entries.emplace_back(model_path, renamed);
  if (material) {
    entries.emplace_back(material_path + ".mtl", *material);
  }
  // Its textures from Blender (kcds_export.py: <name>_<material>.dds), which its material names.
  for (const auto& [file, bytes] : mesh_textures(request.name)) {
    entries.emplace_back("kcds_studio/" + file, bytes);
  }
  const std::string key = "_kcds/studio/mesh-" + std::to_string(shown_count);
  if (!native_ui::mount_files(key, "objects/", entries)) {
    game_client::log("STUDIO mesh %s: its pack could not be mounted", request.name.c_str());
    return;
  }
  // A name of its own each time: the mesh before goes at the frame's end, and a lookup by a shared
  // name found it instead of this one (a texture then went on the old mesh).
  entity_name = std::string(kEntityName) + "_" + std::to_string(shown_count);
  // A mesh given a body is spawned with the game's physics on and then given its kind: a body once
  // given cannot change its kind (props/placed_props.cpp), and one spawned with its physics off
  // stood still as a rigid body.
  const bool with_body =
      body_kind != props::entity::Physics::None && !cloth_on && request.joint.empty();
  const std::string model_file = "objects/" + model_path;
  void* entity = with_body ? props::entity::spawn_solid_mesh(entity_name, request.at, model_file)
                           : props::entity::spawn_mesh(entity_name, request.at, model_file, "");
  showing = entity != nullptr;
  if (showing) {
    shown_name = request.name;
    shown_material = material_path.empty() ? std::string() : "objects/" + material_path;
    place_mesh(entity, request);
  }
  game_client::log("STUDIO mesh %s: %s (%zu bytes)", request.name.c_str(),
                   showing ? "shown" : "the game did not load it", model->size());
}

// The mesh shown again at its last place (to make a cloth solid, or before it is cloth anew),
// drawn with the material it had: a texture put on it (studio/studio_texture.hpp) stays.
void show_again() {
  void* before = showing ? props::entity::find(entity_name) : nullptr;
  const std::string material = before ? props::entity::material_name(before, 256) : std::string();
  show(last_shown);
  void* entity = showing ? props::entity::find(entity_name) : nullptr;
  if (entity && !material.empty()) {
    props::entity::draw_with_material(entity, material);
    props::entity::redraw(entity);
  }
}

// A point's height in the world once turned by `turn` (x, y, z, w) and sized by `scale`.
float height_of(const float* point, const float turn[4], float scale) {
  const float x = turn[0];
  const float y = turn[1];
  const float z = turn[2];
  const float w = turn[3];
  // v' = v + w t + q x t, t = 2 q x v (q the turn's x y z).
  const float tx = 2.0f * (y * point[2] - z * point[1]);
  const float ty = 2.0f * (z * point[0] - x * point[2]);
  const float tz = 2.0f * (x * point[1] - y * point[0]);
  return scale * (point[2] + w * tz + (x * ty - y * tx));
}

// Makes the mesh shown cloth (physical_props/cloth.hpp) of its own vertices from its .cgf, its
// highest vertices in the world pinned where they are: a plane stood up hangs from its top edge.
void make_cloth() {
  if (!last_shown.joint.empty()) {
    // A mesh held in a joint goes with the joint, not with the cloth's physics.
    game_client::log("STUDIO mesh %s: held in %s, not made cloth", shown_name.c_str(),
                     last_shown.joint.c_str());
    return;
  }
  void* entity = showing ? props::entity::find(entity_name) : nullptr;
  std::vector<std::uint8_t> model;
  std::vector<std::uint8_t> material;
  cgf::MeshShape shape;
  if (!entity || !mesh_files(shown_name, model, material) || !cgf::read_shape(model, shape)) {
    game_client::log("STUDIO mesh %s: no vertices for cloth", shown_name.c_str());
    return;
  }
  float turn[4];
  hold::turn_of_degrees(last_shown.turn, turn);
  const float scale = std::clamp(last_shown.scale, 0.01f, 100.0f);
  const std::size_t vertices = shape.positions.size() / 3;
  float highest = -1e30f;
  for (std::size_t vertex = 0; vertex < vertices; ++vertex) {
    highest = std::max(highest, height_of(&shape.positions[vertex * 3], turn, scale));
  }
  std::vector<int> pinned;
  for (std::size_t vertex = 0; vertex < vertices; ++vertex) {
    if (height_of(&shape.positions[vertex * 3], turn, scale) >= highest - kPinnedBand) {
      pinned.push_back(int(vertex));
    }
  }
  cloth_live =
      physical_props::cloth::make(entity, shape.positions, shape.indices, cloth_mass, pinned);
}

// The mesh shown given its body (body_kind), unless it is cloth or held in a joint (no body then).
// A falling one falls from where it was placed: placed anew, it starts there again.
void apply_body() {
  void* entity = showing ? props::entity::find(entity_name) : nullptr;
  if (!entity || cloth_on || cloth_live || !last_shown.joint.empty()) {
    return;
  }
  if (body_kind != props::entity::Physics::None && !shown_game) {
    // A mesh of yours has no collision in its file: the game gets one of its own triangles first
    // (the game's objects bring theirs).
    std::vector<std::uint8_t> model;
    std::vector<std::uint8_t> material;
    cgf::MeshShape shape;
    if (mesh_files(shown_name, model, material) && cgf::read_shape(model, shape)) {
      physical_props::cloth::give_collision(entity, shape.positions, shape.indices);
    }
  }
  const bool done = props::entity::physicalize(entity, body_kind, body_mass);
  if (done && body_kind == props::entity::Physics::Rigid) {
    props::physics::set_awake(props::physics::body_of(entity), true);
    body_check_frames = kBodyCheckFrames;
  }
  std::string kind = "none";
  if (body_kind == props::entity::Physics::Rigid) {
    kind = "falls";
  } else if (body_kind == props::entity::Physics::Static) {
    kind = "solid";
  }
  game_client::log("STUDIO mesh %s: body %s%s", shown_name.c_str(), kind.c_str(),
                   done ? "" : " (the game did not take it)");
}

// A few frames after a falling body is made: its mass, and whether it moves. A mesh whose
// collision carries no mass cannot fall: its body is taken away where it stands and the editor told
// (studioMeshBodied { ok: false, error }).
void check_body(const std::function<bool(const sdk::Value&)>& send) {
  if (body_check_frames == 0 || --body_check_frames > 0) {
    return;
  }
  void* entity = showing ? props::entity::find(entity_name) : nullptr;
  void* body = entity ? props::physics::body_of(entity) : nullptr;
  const float mass = body ? props::physics::mass(body) : 0.0f;
  const bool moving = body && props::physics::awake(body);
  game_client::log("STUDIO mesh %s: falling body of %.2f kg, %s", shown_name.c_str(), mass,
                   moving ? "moving" : "still");
  if (mass > 0.0f || !entity) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioMeshBodied")},
                                    {"ok", sdk::Value(true)},
                                    {"error", sdk::Value(std::string())}}));
    return;
  }
  // No body, for it and the meshes shown after it, as the editor now has it.
  body_kind = props::entity::Physics::None;
  props::entity::physicalize(entity, props::entity::Physics::None, body_mass);
  const std::string error = "it has no collision of its own, so it cannot fall";
  send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioMeshBodied")},
                                  {"ok", sdk::Value(false)},
                                  {"error", sdk::Value(error)}}));
}

// A mesh held in a joint of the character goes with it every frame: its at and turn are its place
// and turn in the joint. The hands hold it at the game's grip joints in their palms, as the
// studio's prop (studio_hold.hpp).
void follow_joint(void* local_entity) {
  if (!showing || cloth_live || last_shown.joint.empty() || !local_entity) {
    return;
  }
  void* entity = props::entity::find(entity_name);
  if (!entity) {
    return;
  }
  const std::string& wanted = last_shown.joint;
  const bool hand = wanted == "RightHand" || wanted == "LeftHand";
  const std::string joint = hand ? wanted + "Slot" : wanted;
  float joint_place[3];
  float joint_turn[4];
  if (!props::entity::joint_pose(local_entity, joint.c_str(), joint_place, joint_turn)) {
    return;
  }
  float offset_turn[4];
  hold::turn_of_degrees(last_shown.turn, offset_turn);
  float place[3];
  float turn[4];
  pose_math::compose(joint_place, joint_turn, last_shown.at, offset_turn, place, turn);
  props::entity::place_in_world(entity, place, turn);
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  Request request;
  if (operation == "studioMeshList") {
    request.list = true;
  } else if (operation == "studioMesh" || operation == "studioMeshPlace") {
    request.show = operation == "studioMesh";
    request.place = !request.show;
    const sdk::Value* name = integration::script_field(message, "name");
    request.name = name && name->tag() == sdk::Tag::Str ? name->as_str() : std::string();
    const sdk::Value* path = integration::script_field(message, "path");
    if (request.show && path && path->tag() == sdk::Tag::Str) {
      if (!game_object_path(path->as_str())) {
        return true;
      }
      request.game_path = path->as_str();
      request.name.clear();
    } else if (request.show && !request.name.empty() && !mesh_name(request.name)) {
      return true;
    }
    double numbers[3];
    if (view::numbers_of(message, "at", numbers, 3)) {
      for (int axis = 0; axis < 3; ++axis) {
        request.at[axis] = float(numbers[axis]);
      }
    }
    if (view::numbers_of(message, "turn", numbers, 3)) {
      for (int axis = 0; axis < 3; ++axis) {
        request.turn[axis] = float(numbers[axis]);
      }
    }
    const sdk::Value* scale = integration::script_field(message, "scale");
    double size = 1.0;
    if (scale && view::number_of(*scale, size)) {
      request.scale = float(size);
    }
    const sdk::Value* joint = integration::script_field(message, "joint");
    if (joint && joint->tag() == sdk::Tag::Str) {
      if (!joint_name(joint->as_str())) {
        return true;
      }
      request.joint = joint->as_str();
    }
  } else if (operation == "studioMeshWrite") {
    // A shape the studio made (studio/shapes.cjs): a .cgf of the game's own kind (signature CrCh)
    // in base64 and its material's text, under a mesh name.
    request.write = true;
    const sdk::Value* name = integration::script_field(message, "name");
    const sdk::Value* model = integration::script_field(message, "model");
    const sdk::Value* material = integration::script_field(message, "material");
    const auto text = [](const sdk::Value* value) {
      return value && value->tag() == sdk::Tag::Str;
    };
    if (!text(name) || !text(model) || !text(material)) {
      return true;
    }
    if (!mesh_name(name->as_str()) || material->as_str().size() > kLargestMaterial ||
        material->as_str().rfind("<Material", 0) != 0) {
      return true;
    }
    request.name = name->as_str();
    request.material = material->as_str();
    if (!files::decode_base64(model->as_str(), request.model) || request.model.size() < 16 ||
        std::memcmp(request.model.data(), "CrCh", 4) != 0) {
      return true;
    }
  } else if (operation == "studioMeshBody") {
    request.body = true;
    const sdk::Value* kind = integration::script_field(message, "kind");
    const std::string given = kind && kind->tag() == sdk::Tag::Str ? kind->as_str() : "none";
    if (given == "solid") {
      request.body_kind = props::entity::Physics::Static;
    } else if (given == "falls") {
      request.body_kind = props::entity::Physics::Rigid;
    } else if (given != "none") {
      return true;
    }
    const sdk::Value* mass = integration::script_field(message, "mass");
    double kilograms = 5.0;
    if (mass && view::number_of(*mass, kilograms)) {
      request.mass = std::clamp(float(kilograms), kLightestBody, kHeaviestBody);
    } else {
      request.mass = 5.0f;
    }
  } else if (operation == "studioMeshCloth") {
    request.cloth = true;
    const sdk::Value* on = integration::script_field(message, "on");
    request.cloth_on = on && on->tag() == sdk::Tag::Bool && on->as_bool();
    const sdk::Value* mass = integration::script_field(message, "mass");
    double kilograms = 1.0;
    if (mass && view::number_of(*mass, kilograms)) {
      request.mass = std::clamp(float(kilograms), kLightestCloth, kHeaviestCloth);
    }
  } else {
    return false;
  }
  std::lock_guard<std::mutex> lock(requests_mutex);
  requests.push_back(request);
  return true;
}

void tick(void* local_entity, const std::function<bool(const sdk::Value&)>& send) {
  std::vector<Request> now;
  {
    std::lock_guard<std::mutex> lock(requests_mutex);
    now.swap(requests);
  }
  for (const Request& request : now) {
    if (request.list) {
      sdk::Value::Array names;
      for (const std::string& name : mesh_names()) {
        names.push_back(sdk::Value(name));
      }
      send(sdk::Value(
          sdk::Value::Map{{"op", sdk::Value("studioMeshes")}, {"names", sdk::Value(names)}}));
    } else if (request.show) {
      last_shown = request;
      show(request);
      if (cloth_on && showing) {
        make_cloth();
      } else if (body_kind != props::entity::Physics::None) {
        apply_body();
      }
    } else if (request.place && showing) {
      std::copy(std::begin(request.at), std::end(request.at), std::begin(last_shown.at));
      std::copy(std::begin(request.turn), std::end(request.turn), std::begin(last_shown.turn));
      last_shown.scale = request.scale;
      const bool hold_changed = last_shown.joint != request.joint;
      last_shown.joint = request.joint;
      if (hold_changed && body_kind != props::entity::Physics::None) {
        // Into a joint or out of one: built anew, with its body only while free.
        show_again();
        apply_body();
      } else if (cloth_live) {
        // A cloth hangs from where it was pinned: moved, it is solid until the moving stops,
        // then made again at its new place (taken into a joint, it stays solid: make_cloth).
        show_again();
      } else if (void* entity = props::entity::find(entity_name)) {
        place_mesh(entity, request);
        if (body_kind != props::entity::Physics::None) {
          // Placed anew, a falling mesh falls from its new place.
          apply_body();
        }
      }
      if (cloth_on) {
        cloth_due = Clock::now() + kClothAfterMove;
      }
    } else if (request.cloth) {
      cloth_on = request.cloth_on;
      cloth_mass = request.mass;
      cloth_due.reset();
      if (cloth_live) {
        // Solid again (or cloth of another weight): the mesh shown anew, as the game loads it.
        show_again();
      }
      if (cloth_on && showing) {
        make_cloth();
      } else if (showing) {
        apply_body();
      }
    } else if (request.write) {
      const std::string error = write_mesh(request);
      game_client::log("STUDIO mesh %s: written (%zu bytes)%s%s", request.name.c_str(),
                       request.model.size(), error.empty() ? "" : ": ", error.c_str());
      send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioMeshWritten")},
                                      {"name", sdk::Value(request.name)},
                                      {"ok", sdk::Value(error.empty())},
                                      {"error", sdk::Value(error)}}));
    } else if (request.body) {
      body_kind = request.body_kind;
      body_mass = request.mass;
      if (showing) {
        // Built anew where it was placed (a mesh that fell lies elsewhere), with its new body.
        show_again();
        apply_body();
      }
    }
  }
  if (cloth_due && Clock::now() >= *cloth_due) {
    cloth_due.reset();
    if (cloth_on && showing && !cloth_live) {
      make_cloth();
    }
  }
  follow_joint(local_entity);
  check_body(send);
  physical_props::cloth::tick();
}

bool mesh_files(const std::string& name, std::vector<std::uint8_t>& model,
                std::vector<std::uint8_t>& material) {
  const auto folder = mesh_name(name) ? meshes_folder() : std::nullopt;
  const auto read = folder ? read_file(*folder / (name + ".cgf")) : std::nullopt;
  if (!read) {
    return false;
  }
  model = *read;
  material = read_file(*folder / (name + ".mtl")).value_or(std::vector<std::uint8_t>());
  return true;
}

void* shown_entity() {
  return showing ? props::entity::find(entity_name) : nullptr;
}

std::string own_material() {
  if (!showing || shown_game) {
    return std::string();
  }
  return shown_material.empty() ? "objects/kcds_studio/" + shown_name : shown_material;
}

bool where(float place[3], float turn[4], float min[3], float max[3]) {
  void* entity = showing ? props::entity::find(entity_name) : nullptr;
  if (!entity) {
    return false;
  }
  props::entity::world_pose(entity, place, turn);
  if (!props::entity::world_bounds(entity, min, max)) {
    for (int axis = 0; axis < 3; ++axis) {
      min[axis] = place[axis] - 0.1f;
      max[axis] = place[axis] + 0.1f;
    }
  }
  return true;
}

}  // namespace kcds::runtime::studio::mesh
