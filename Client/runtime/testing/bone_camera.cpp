// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "bone_camera.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../core/log.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::game_client {
namespace {

HANDLE bone_camera_event = nullptr;
bool active = false;
bool clear_pending = false;
std::string entity_name;
std::string joint;
float offset[3] = {0.0f, 0.0f, 0.0f};
float fov = 30.0f;

void read_settings() {
  wchar_t folder[MAX_PATH + 1] = {};
  GetTempPathW(MAX_PATH + 1, folder);
  wchar_t file[MAX_PATH + 64];
  swprintf_s(file, L"%skcds-bonecam-%lu.txt", folder, GetCurrentProcessId());
  std::ifstream input(file);
  std::string line;
  std::getline(input, line);
  std::istringstream words(line);
  std::string name;
  std::string wanted_joint;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float wanted_fov = 30.0f;
  if (!(words >> name) || name == "off" || !(words >> wanted_joint >> x >> y >> z)) {
    clear_pending = active;
    active = false;
    kcds::game_client::log("PROBE bone camera off");
    return;
  }
  words >> wanted_fov;
  entity_name = name;
  joint = wanted_joint;
  offset[0] = x;
  offset[1] = y;
  offset[2] = z;
  fov = wanted_fov;
  active = true;
  kcds::game_client::log("PROBE bone camera on %s %s", entity_name.c_str(), joint.c_str());
  // The default pose of the joint (both of the default skeleton's getters), for grip offsets.
  if (void* body = kcds::runtime::props::entity::find(entity_name)) {
    for (const int which : {8, 9}) {
      float p[3] = {};
      float q[4] = {};
      if (kcds::runtime::props::entity::default_joint(body, joint.c_str(), which, p, q)) {
        kcds::game_client::log(
            "PROBE bone camera default%d %s q=%.4f,%.4f,%.4f,%.4f p=%.4f,%.4f,%.4f", which,
            joint.c_str(), q[0], q[1], q[2], q[3], p[0], p[1], p[2]);
      }
    }
  }
}

}  // namespace

void install_bone_camera() {
  wchar_t name[96];
  swprintf_s(name, L"Local\\KCD2S.BoneCamera.%lu", GetCurrentProcessId());
  bone_camera_event = CreateEventW(nullptr, FALSE, FALSE, name);
}

std::optional<std::optional<kcd2s::integration::CameraShot>> bone_camera_order() {
  if (bone_camera_event && WaitForSingleObject(bone_camera_event, 0) == WAIT_OBJECT_0) {
    read_settings();
  }
  if (!active) {
    if (clear_pending) {
      clear_pending = false;
      return std::optional<kcd2s::integration::CameraShot>{};
    }
    return std::nullopt;
  }
  void* body = kcds::runtime::props::entity::find(entity_name);
  float position[3] = {0.0f, 0.0f, 0.0f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  if (!body || !kcds::runtime::props::entity::joint_pose(body, joint.c_str(), position, rotation)) {
    return std::nullopt;
  }
  kcd2s::integration::CameraShot shot;
  shot.aim = kcd2s::integration::CameraAim::LookAt;
  for (int axis = 0; axis < 3; ++axis) {
    shot.position[axis] = position[axis] + offset[axis];
    shot.target[axis] = position[axis];
  }
  shot.fov = fov;
  shot.seconds = 0.0f;
  return shot;
}

}  // namespace kcds::game_client
