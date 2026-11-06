// The local body's own head out of the first-person view, WHGame 1.5.5.
//
// The game hides the player's head in first person through the clothing hiding group
// FirstPersonView (Libs/Tables/Character/ClothingHidingGroup.xml, index 0, system only): the view
// switch stores the first-person flag at actor + 2616 and turns the group on, and only
// the player's own head and hair meshes (Henry's) carry it. Any other head does not, a female
// body's or a male body given another face, so its eyes, mouth and hair stayed in front of the
// camera; carrying an item brings them into view (hair and an ear fill the screen).
//
// While the view is first person, whatever the body, its head's, hair's and beard's skin
// attachments and its eyes are hidden from the camera and kept in the shadows (on Henry's own head
// this repeats what the group does): the character's attachment
// manager (ICharacterInstance slot 9) lists them (slot 9 GetInterfaceByIndex, null past the end),
// each attachment's name is slot 2, and slot 17 HideAttachment sets the hidden flags (main pass
// 0x10000, shadow 0x20000, recursion 0x40000) and slot 21 clears the shadow's again, as the
// script's Entity.HideAttachment does.
//
// They are hidden only while the view camera is at the head: the actor's first-person flag stays
// set under a free camera, a camera shot or the photo mode, whose camera is elsewhere and shows
// the whole body, head included. A script animation (Player.playAnimation) can move the head
// itself away from the first-person camera (bowing, drinking, looking down): the player then saw
// the inside of their own face, so while one runs and no other camera films (the
// caller knows NoClip and the server's camera shots; the photo mode is not told apart) the head
// stays hidden wherever the camera is. Third person clears the flag, and every part hidden is shown
// again. Henry's own head is hidden by the game's group, which stays on under such a camera: the
// view switch is called with the first person off meanwhile, and its two flags (actor + 2616 and
// + 2617) are given back at once, so it only turns the group off; and on again when the camera is
// back at the head.
//
// Engine thread.
#pragma once

namespace kcds::runtime::appearance::first_person_head {

using Logger = void (*)(const char* format, ...);

// The view switch; without it Henry's own head stays hidden under a free camera.
void install(void* view_switch);

// Every frame in a world: the local actor, its entity, the view camera (a 3x4 world matrix, null
// when unknown) and whether a script animation runs under the player's own camera. What it hid
// and the body no longer has (rebuilt with another look, another world) is forgotten.
void keep(void* actor, void* entity, const float* camera, bool animating, Logger log);

}  // namespace kcds::runtime::appearance::first_person_head
