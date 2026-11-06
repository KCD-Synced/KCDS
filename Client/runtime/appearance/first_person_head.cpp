#include "first_person_head.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../props/prop_entity.hpp"

namespace kcds::runtime::appearance::first_person_head {
namespace {

// The actor's first-person flag and the byte beside it, as the view switch stores
// them.
constexpr std::size_t kActorFirstPerson = 2616;
constexpr std::size_t kActorViewDetail = 2617;
// IEntity GetCharacter(slot), the body's character in slot 0.
constexpr std::size_t kGetCharacter = 101;
constexpr int kCharacterSlot = 0;
// ICharacterInstance GetIAttachmentManager; IAttachmentManager GetInterfaceByIndex (null past the
// end); IAttachment GetName, HideAttachment, IsAttachmentHidden and HideInShadow (CAttachmentSKIN).
constexpr std::size_t kAttachmentManager = 9;
constexpr std::size_t kAttachmentByIndex = 9;
constexpr std::size_t kAttachmentName = 2;
constexpr std::size_t kHideAttachment = 17;
constexpr std::size_t kIsHidden = 18;
constexpr std::size_t kHideInShadow = 21;
// The joint the first-person camera sits at, and how far from it the camera still counts as the
// body's own eyes (first person: about 0.1 m; the nearest free or shot camera is much
// farther).
constexpr const char* kHeadJoint = "Head";
constexpr float kCameraAtHead = 0.5f;
// A character carries far fewer attachments than this.
constexpr unsigned kMaxAttachments = 512;

// The skins a body's head is drawn from, by their attachment names: a component's type and its
// equipment part (head_face, hair_head; a beard's beard_beard), and the eyes.
constexpr const char* kHeadPrefixes[] = {"head_", "hair_", "beard_"};
constexpr const char* kEyes[] = {"eye_left", "eye_right"};

// The attachments this module hid, so it shows only those again.
std::vector<void*> hidden;
// The view switch, and the actor whose FirstPersonView group this module turned off.
using ViewSwitch = void(__fastcall*)(void* actor, bool first_person, std::uint8_t detail);
ViewSwitch view_switch = nullptr;
void* group_off_for = nullptr;

template <class Fn>
Fn slot_of(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

void* character_of(void* entity) {
  using GetCharacter = void*(__fastcall*)(void*, int);
  return entity ? slot_of<GetCharacter>(entity, kGetCharacter)(entity, kCharacterSlot) : nullptr;
}

bool is_head_part(const char* name) {
  if (!name) {
    return false;
  }
  for (const char* prefix : kHeadPrefixes) {
    if (std::strncmp(name, prefix, std::strlen(prefix)) == 0) {
      return true;
    }
  }
  for (const char* eye : kEyes) {
    if (std::strcmp(name, eye) == 0) {
      return true;
    }
  }
  return false;
}

// The character's head, hair, beard and eye attachments.
std::vector<void*> head_parts(void* character) {
  std::vector<void*> found;
  using GetManager = void*(__fastcall*)(void*);
  using ByIndex = void*(__fastcall*)(void*, unsigned);
  using GetName = const char*(__fastcall*)(void*);
  void* manager = slot_of<GetManager>(character, kAttachmentManager)(character);
  if (!manager) {
    return found;
  }
  for (unsigned index = 0; index < kMaxAttachments; ++index) {
    void* attachment = slot_of<ByIndex>(manager, kAttachmentByIndex)(manager, index);
    if (!attachment) {
      break;
    }
    if (is_head_part(slot_of<GetName>(attachment, kAttachmentName)(attachment))) {
      found.push_back(attachment);
    }
  }
  return found;
}

// Hidden from the camera, still casting its shadow.
void hide(void* attachment, bool hidden_now) {
  using Hide = void(__fastcall*)(void*, std::uint32_t);
  slot_of<Hide>(attachment, kHideAttachment)(attachment, hidden_now ? 1 : 0);
  slot_of<Hide>(attachment, kHideInShadow)(attachment, 0);
}

bool is_hidden(void* attachment) {
  using IsHidden = std::uint32_t(__fastcall*)(void*);
  return slot_of<IsHidden>(attachment, kIsHidden)(attachment) != 0;
}

// Whether the view camera is at the body's head; true when either is unknown, the first-person
// flag alone then decides.
bool camera_at_head(void* entity, const float* camera) {
  float head[3] = {};
  float rotation[4] = {};
  if (!camera || !kcds::runtime::props::entity::joint_pose(entity, kHeadJoint, head, rotation)) {
    return true;
  }
  const float dx = camera[3] - head[0];
  const float dy = camera[7] - head[1];
  const float dz = camera[11] - head[2];
  return dx * dx + dy * dy + dz * dz <= kCameraAtHead * kCameraAtHead;
}

// Turns the actor's FirstPersonView group on or off through the view switch, and gives its flags
// back.
void set_first_person_group(void* actor, bool on) {
  auto* bytes = static_cast<std::uint8_t*>(actor);
  const std::uint8_t first_person = bytes[kActorFirstPerson];
  const std::uint8_t detail = bytes[kActorViewDetail];
  view_switch(actor, on, detail);
  bytes[kActorFirstPerson] = first_person;
  bytes[kActorViewDetail] = detail;
}

}  // namespace

void install(void* found) {
  view_switch = reinterpret_cast<ViewSwitch>(found);
}

void keep(void* actor, void* entity, const float* camera, bool animating, Logger log) {
  if (!actor || !entity) {
    return;
  }
  void* character = character_of(entity);
  if (!character) {
    return;
  }
  const bool first_person = *(static_cast<std::uint8_t*>(actor) + kActorFirstPerson) != 0;
  // An animation that moves the head away from the first-person camera keeps it hidden.
  const bool wanted = first_person && (animating || camera_at_head(entity, camera));
  // Henry's own head: the game's group off while the camera is away from it in first person.
  if (group_off_for && (group_off_for != actor || !first_person)) {
    group_off_for = nullptr;
  }
  if (view_switch && first_person && !wanted && !group_off_for) {
    set_first_person_group(actor, false);
    group_off_for = actor;
    if (log) {
      log("APPEARANCE first person: the head shown to a camera away from it");
    }
  } else if (view_switch && wanted && group_off_for == actor) {
    set_first_person_group(actor, true);
    group_off_for = nullptr;
  }
  const std::vector<void*> parts = head_parts(character);
  // What this module hid and the body no longer has (rebuilt with another look) is forgotten.
  hidden.erase(std::remove_if(hidden.begin(), hidden.end(),
                              [&](void* attachment) {
                                return std::find(parts.begin(), parts.end(), attachment) ==
                                       parts.end();
                              }),
               hidden.end());
  if (wanted) {
    for (void* attachment : parts) {
      if (!is_hidden(attachment)) {
        hide(attachment, true);
        if (std::find(hidden.begin(), hidden.end(), attachment) == hidden.end()) {
          hidden.push_back(attachment);
        }
        if (log) {
          log("APPEARANCE first person: a head part hidden (%p)", attachment);
        }
      }
    }
    return;
  }
  for (void* attachment : hidden) {
    hide(attachment, false);
  }
  if (!hidden.empty() && log) {
    log("APPEARANCE first person: %zu head parts shown again", hidden.size());
  }
  hidden.clear();
}

}  // namespace kcds::runtime::appearance::first_person_head
