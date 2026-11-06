// The female skeleton for a player's body.
//
// A woman's player body is built on the game's female skeleton
// (Objects/Characters/humans/female/skeleton/female.cdf): the female body parts are skinned for it,
// and on Henry's male skeleton their fingers stretched into long dark ribbons. A player's body
// keeps the player's animation database, whose clips are the male ones, while female.chrparams
// lists the female animations only (Animations\humans\female): on its own the body stood in its
// bind pose.
//
// mount() gives a player woman a skeleton of her own, the game's women keeping theirs: at startup
// it reads through CryPak the game's female.chr, female.cdf, female.chrparams and male.chrparams
// and writes, next to the female skeleton, kcds_player_female.chr (the female skeleton as it is),
// kcds_player_female.chrparams (the female one's IK and limbs, the male one's animation list: the
// clips both sets name, as the drawn sword's idle, play the male way; the female twin holds
// nothing, which left her palms open and the sword's slot unturned; after it the female clips, for
// the fragments only the game's women have) and kcds_player_female.cdf (the
// female definition on that skeleton), as a stored zip at
// %TEMP%\kcds-characters\female-skeleton.pak it mounts as a mod pack. Nothing of the game is
// shipped or written into its installation.
#pragma once

namespace kcds::runtime::appearance::female_skeleton {

// The player woman's character definition (mounted by mount()).
inline constexpr char kPlayerModel[] =
    "Objects/Characters/humans/female/skeleton/kcds_player_female.cdf";

using Logger = void (*)(const char* format, ...);

// The game's CryPak and the binding root the mod packs are opened with (OpenModPaks' game folder).
// False when the file could not be read, patched, written or mounted: a woman's body then has no
// male clips.
bool mount(void* pak, const char* binding_root, Logger logger);

}  // namespace kcds::runtime::appearance::female_skeleton
