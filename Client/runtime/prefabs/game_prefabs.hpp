// The game's own prefabs placed at run time (World Architect): a house, a wall, a
// well, any template of IPL_GameData.pak's Prefabs/**.xml, built where the server puts it.
//
// WHGame 1.5.5 builds a prefab at run time as the RuntimePrefab entity class does: the script
// binding Game.SpawnPrefab reads the template's GUID and calls the prefab
// manager's spawner (on the game global 's + 336) for the entity, which builds
// the template once and lays its brushes as render nodes at the entity's place.
// Game.DeletePrefab takes them away again.
//
// The game's own run-time path leaves three things out that a placed building needs, and this
// module puts them in:
//   - A brush whose RndFlags value is wider than 32 bits (a house's main mesh,
//     RndFlags="180060000408") is read as ffffffff: hidden and without physics.
//     Such brushes get kBrushFlags in the built template before it is laid.
//   - Brushes laid at run time are left to the on-demand physics, which never comes
//     for them: each brush is physicalized at once (CBrush slot 33).
//   - The builder refuses a template that holds another prefab (stops at a nested
//     "Prefab" object: a house's doors). While such a template is first built, its nested
//     prefabs' Type reads "" (the read-only XML node's getAttr is hooked for that call), so the
//     builder passes over them; each is then built from its own template in the holder's frame.
//
// Engine thread.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace kcds::runtime::prefabs {

using Logger = void (*)(const char* format, ...);

// Finds the game's prefab functions; false when one is missing (prefabs are then not built).
bool install(Logger logger);

// The template GUID of a prop model naming a game prefab ("prefab:<GUID>"), empty for any other
// model.
std::string template_of(const std::string& model);

// Builds the prefab whose template has that GUID at the place and turn (x, y, z, w) as an entity
// named `name`; false when the template is unknown, refused or the game's functions are missing.
bool spawn(const std::string& name, const std::string& guid, const float position[3],
           const float rotation[4]);

// Takes away the prefab built as `name` and its entity; nothing when there is none.
void remove(const std::string& name);

// A brush of a template, for a preview: its mesh (the object's "Prefab" attribute, as the brush
// reader takes it) and its place and turn (x, y, z, w) in the template's frame.
struct Piece {
  std::string mesh;
  float position[3] = {};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

// The brushes and entity models (a door's) of the template whose GUID is `guid` and of the
// prefabs it holds (one level in), in the template's order, at most `max`; none when the template
// is unknown.
std::vector<Piece> pieces(const std::string& guid, std::size_t max);

}  // namespace kcds::runtime::prefabs
