// Your own textures in the studio: a picture (.png, .jpg, or a .dds as it is) from
// %LOCALAPPDATA%\KCDS\studio\textures\ is read with Windows' own image decoder (WIC), made a .dds
// the game loads (texture_dds.hpp, at most 2048 pixels a side), put in the game's file system with
// a material that shows it (objects/kcds_studio/tex_<name>_<n>.dds and .mtl, a pack from memory:
// native_ui mount_files) and drawn on your mesh in place of its own material
// (props/prop_entity draw_with_material). Nothing is written to the game's folders.
//
//   { op: 'studioTextureList' }        answered { op: 'studioTextures', names: [...] }
//   { op: 'studioTexture', name, tiling }
//                                      draws your mesh with that texture, repeated `tiling` times
//                                      (1 at first); name '' gives the mesh its own material
//                                      back; answered { op: 'studioTextured', name, ok, width,
//                                      height, error }
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::textures {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

void tick(const std::function<bool(const kcd2s::sdk::Value&)>& send);

// The .dds of one of your textures by its name (a .png or .jpg made one, a .dds as it is), with
// its size; false and why not (the export, uses it too).
bool dds_of_texture(const std::string& name, std::vector<std::uint8_t>& dds, int& width,
                    int& height, std::string& error);

// The material that shows a texture (its game path) on every sub-material, repeated `tiling`
// times.
std::string material_text(const std::string& texture_path, double tiling);

// The same with a normal map (its game path; empty for none).
std::string material_text(const std::string& texture_path, double tiling,
                          const std::string& normal_path);

// The same with a normal map and a specular map (game paths; empty for none).
std::string material_text(const std::string& texture_path, double tiling,
                          const std::string& normal_path, const std::string& specular_path);

}  // namespace kcds::runtime::studio::textures
