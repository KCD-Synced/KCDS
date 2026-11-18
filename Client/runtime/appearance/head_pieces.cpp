#include "head_pieces.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "../core/log.hpp"
#include "../level/model_parts.hpp"
#include "../native_ui/native_ui.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/head_pieces.hpp"
#include "skin_binding.hpp"

namespace kcds::runtime::appearance::head_pieces {
namespace {

namespace integration = kcd2s::integration;
namespace binding = skin_binding;

// Sigismund's crown as a material of our own for his hair's mesh: its parts in the mesh's order,
// the hair's two drawn as nothing, the crown as gold metal as the game's gold cross is drawn (a
// black diffuse, a gold specular colour and the cross's specular map) with the crown's own normal
// map (its own specular map holds the clothing system's masks, not a colour). The game's own
// material colours the crown through the clothing system, from the
// hair's character component; a skin worn on its own has none of that, so the crown drew grey.
constexpr const char* kSigismundMaterial = R"(<Material MtlFlags="524544">
  <SubMaterials>
    <Material Name="haircap" Shader="Nodraw" MtlFlags="525440" Diffuse="1,1,1" Specular="0,0,0" Opacity="1" Shininess="10" />
    <Material Name="haircards" Shader="Nodraw" MtlFlags="525440" Diffuse="1,1,1" Specular="0,0,0" Opacity="1" Shininess="10" />
    <Material Name="crown" Shader="Illum" MtlFlags="524416" StringGenMask="%NORMAL_MAP%SPECULAR_MAP" Diffuse="1,1,1" Specular="1,0.70837581,0.28314877" Opacity="1" Shininess="255">
      <Textures>
        <Texture Map="Diffuse" File="engineassets/textures/black.tif" />
        <Texture Map="Bumpmap" File="objects/characters/humans/male/hair/m_hair_zikmund/crown_zikmund_m01_ddna.tif" />
        <Texture Map="Specular" File="objects/manmade/task_specific_props/religious/cross_gold_a_spec.tif" />
      </Textures>
    </Material>
  </SubMaterials>
</Material>
)";

// The game's mesh of each crown and the material it is drawn with: the game's own (a skin loaded
// on its own draws untextured), or ours (`own`, mounted at objects/kcds_crowns/<name>.mtl).
struct CrownMesh {
  std::string_view name;
  const char* skin;
  const char* material;
  const char* own;
};

const CrownMesh kCrownMeshes[] = {
    {"sigismund", "objects/characters/humans/male/hair/m_hair_zikmund/m_hair_zikmund.skin",
     "objects/kcds_crowns/sigismund", kSigismundMaterial},
    {"wreath", "objects/characters/humans/male/clothing/head/other/wreath/wreath.skin",
     "objects/characters/humans/male/clothing/head/other/wreath/wreath01_m01", nullptr},
    {"twitchWreath",
     "objects/characters/humans/male/clothing/head/other/wreathtwitch/wreathtwitch.skin",
     "objects/characters/humans/male/clothing/head/other/wreathtwitch/wreathtwitch_m01", nullptr},
};

// The part of a hair that is its crown.
const std::vector<std::string> kHairCrownPart{"crown"};

// "hair_": the first-person view hides the attachment with the body's own hair
// (first_person_head.hpp).
constexpr const char* kAttachmentName = "hair_kcds_crown";
// More attachments than any body has.
constexpr std::uint32_t kMostAttachments = 512;
// How often a body is looked at again for a new dressing.
constexpr double kCheckSeconds = 1.0;
// CActor: its entity at + 56.
constexpr std::size_t kActorEntity = 56;

// A level of a hair drawn without its crown: the material it was drawn with before (held) and
// the copy put in its place.
struct Crownless {
  void* object = nullptr;
  int lod = 0;
  void* before = nullptr;
  void* copy = nullptr;
};

struct Body {
  void* actor = nullptr;
  std::string field;
  integration::HeadPiece piece;
  // The crown the attachment wears and the attachment object the game made for it.
  std::string worn;
  void* worn_object = nullptr;
  bool refused = false;
  std::vector<Crownless> crownless;
  double checked_at = -1.0;
};

std::unordered_map<std::uint32_t, Body> bodies;
// The crowns' skins, loaded once (the character manager keeps them).
std::unordered_map<std::string, void*> skins;

const CrownMesh* crown_mesh(const std::string& name) {
  for (const CrownMesh& mesh : kCrownMeshes) {
    if (mesh.name == name) {
      return &mesh;
    }
  }
  return nullptr;
}

void* skin_of(const CrownMesh& mesh) {
  const auto known = skins.find(mesh.skin);
  if (known != skins.end()) {
    return known->second;
  }
  void* skin = binding::load_skin(mesh.skin);
  skins.emplace(mesh.skin, skin);
  game_client::log("HEAD piece %.*s: %s %s", int(mesh.name.size()), mesh.name.data(), mesh.skin,
                   skin ? "loaded" : "not loaded");
  return skin;
}

void* entity_of(void* actor) {
  return actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorEntity)
               : nullptr;
}

// Puts our own material of a crown in the game's file system, once.
bool mount_own(const CrownMesh& mesh) {
  const std::string text(mesh.own);
  const std::string file = "kcds_crowns/" + std::string(mesh.name) + ".mtl";
  return native_ui::mount_files("_kcds/crowns/" + std::string(mesh.name), "objects/",
                                {{file, std::vector<std::uint8_t>(text.begin(), text.end())}});
}

// The material the crown is drawn with. Loaded once and kept for the game's run (a reference), as
// every body wearing it shares it.
void* crown_material(const CrownMesh& mesh) {
  static std::unordered_map<std::string_view, void*> made;
  const auto known = made.find(mesh.name);
  if (known != made.end()) {
    return known->second;
  }
  void* drawn = nullptr;
  if (!mesh.own || mount_own(mesh)) {
    drawn = props::entity::load_material(mesh.material);
  }
  level::parts::hold(drawn);
  made.emplace(mesh.name, drawn);
  game_client::log("HEAD piece %.*s: material %s %s", int(mesh.name.size()), mesh.name.data(),
                   mesh.material, drawn ? "ready" : "not found");
  return drawn;
}

// Draws every level of the attachment object the skin has with the crown's material. The object
// takes a reference to it.
void draw_crown(void* object, const CrownMesh& mesh) {
  void* material = crown_material(mesh);
  if (!material) {
    return;
  }
  for (int lod = 0; lod < binding::kLods; ++lod) {
    if (binding::drawn_material(object, lod)) {
      binding::replace_material(object, lod, material);
    }
  }
}

// Puts on or takes off the crown the body should wear; again when the game dressed it anew.
void keep_crown(Body& body, void* manager) {
  void* attachment = body.piece.crown.empty() ? binding::named_attachment(manager, kAttachmentName)
                                              : binding::own_attachment(manager, kAttachmentName);
  if (!attachment) {
    return;
  }
  void* object = binding::bound_object(attachment);
  if (body.worn == body.piece.crown && (body.worn.empty() || object == body.worn_object)) {
    return;
  }
  if (object) {
    binding::clear(attachment);
  }
  body.worn = body.piece.crown;
  body.worn_object = nullptr;
  const CrownMesh* mesh = crown_mesh(body.piece.crown);
  if (!mesh) {
    return;
  }
  void* skin = skin_of(*mesh);
  if (!skin || !binding::bind(attachment, skin)) {
    if (!body.refused) {
      game_client::log("HEAD piece %s: the game did not put it on", body.piece.crown.c_str());
    }
    body.refused = true;
    return;
  }
  body.refused = false;
  body.worn_object = binding::bound_object(attachment);
  if (body.worn_object) {
    draw_crown(body.worn_object, *mesh);
  }
}

// The skin attachment objects of the body, our crown's left out.
std::vector<void*> skin_objects(void* manager) {
  std::vector<void*> objects;
  void* own = binding::named_attachment(manager, kAttachmentName);
  for (std::uint32_t index = 0; index < kMostAttachments; ++index) {
    void* attachment = binding::attachment_at(manager, index);
    if (!attachment) {
      break;
    }
    if (attachment == own) {
      continue;
    }
    void* object = binding::bound_object(attachment);
    if (binding::skin_object(object)) {
      objects.push_back(object);
    }
  }
  return objects;
}

// Puts back the levels drawn without their crown; `objects` are the ones still on the body (null:
// the body is gone, nothing is touched).
void restore_crowns(Body& body, const std::vector<void*>* objects) {
  for (const Crownless& level : body.crownless) {
    const bool still_on =
        objects && std::find(objects->begin(), objects->end(), level.object) != objects->end();
    if (still_on && binding::replacement(level.object, level.lod) == level.copy) {
      binding::replace_material(level.object, level.lod, level.before);
    }
    level::parts::release(level.before);
  }
  body.crownless.clear();
}

bool crownless_already(const Body& body, void* object, int lod) {
  for (const Crownless& level : body.crownless) {
    if (level.object == object && level.lod == lod &&
        binding::replacement(object, lod) == level.copy) {
      return true;
    }
  }
  return false;
}

// Draws every hair of the body that carries a crown without it, or with it again.
void keep_hair_crown(Body& body, void* manager) {
  const std::vector<void*> objects = skin_objects(manager);
  if (body.piece.hair_crown) {
    restore_crowns(body, &objects);
    return;
  }
  // Levels the game dressed anew since: forgotten, with what they held.
  std::vector<Crownless> kept;
  for (const Crownless& level : body.crownless) {
    const bool still_on =
        std::find(objects.begin(), objects.end(), level.object) != objects.end() &&
        binding::replacement(level.object, level.lod) == level.copy;
    if (still_on) {
      kept.push_back(level);
    } else {
      level::parts::release(level.before);
    }
  }
  body.crownless = std::move(kept);
  for (void* object : objects) {
    for (int lod = 0; lod < binding::kLods; ++lod) {
      if (crownless_already(body, object, lod)) {
        continue;
      }
      void* material = binding::drawn_material(object, lod);
      if (!material || !level::parts::has_part(material, kHairCrownPart.front())) {
        continue;
      }
      std::string names;
      int matched = 0;
      void* copy = level::parts::without(material, kHairCrownPart, names, matched);
      if (!copy || matched == 0) {
        level::parts::release(copy);
        continue;
      }
      void* before = binding::replacement(object, lod);
      level::parts::hold(before);
      binding::replace_material(object, lod, copy);
      body.crownless.push_back({object, lod, before, copy});
      level::parts::release(copy);
    }
  }
}

void look_at(Body& body, double now) {
  body.checked_at = now;
  void* manager = binding::attachment_manager(entity_of(body.actor));
  if (!manager) {
    return;
  }
  keep_crown(body, manager);
  if (!body.piece.hair_crown || !body.crownless.empty()) {
    keep_hair_crown(body, manager);
  }
}

}  // namespace

void show(std::uint32_t player, void* actor, const std::string& field, double now) {
  if (!binding::installed()) {
    return;
  }
  auto found = bodies.find(player);
  if (found == bodies.end()) {
    if (field.empty() || !actor) {
      return;
    }
    found = bodies.emplace(player, Body{}).first;
  }
  Body& body = found->second;
  if (body.actor != actor) {
    // Another body: what was done to the one before went with it.
    restore_crowns(body, nullptr);
    body.actor = actor;
    body.worn.clear();
    body.worn_object = nullptr;
    body.checked_at = -1.0;
  }
  const bool changed = body.field != field;
  if (changed) {
    integration::HeadPiece piece;
    if (!integration::parse_head_piece_field(field, piece)) {
      return;
    }
    body.field = field;
    body.piece = piece;
  }
  if (!actor) {
    return;
  }
  if (changed || now - body.checked_at >= kCheckSeconds) {
    look_at(body, now);
  }
  if (body.field.empty() && body.worn.empty() && body.crownless.empty()) {
    bodies.erase(found);
  }
}

void keep_only(const std::vector<std::uint32_t>& shown) {
  for (auto it = bodies.begin(); it != bodies.end();) {
    if (std::find(shown.begin(), shown.end(), it->first) != shown.end()) {
      ++it;
      continue;
    }
    // The player left: their body goes, so nothing on it is touched.
    restore_crowns(it->second, nullptr);
    it = bodies.erase(it);
  }
}

}  // namespace kcds::runtime::appearance::head_pieces
