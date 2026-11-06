// The player's hardware id for the server (player.deviceId): a SHA-256, in lowercase hex, of
// the machine's Windows installation id (MachineGuid) and its firmware's system UUID (SMBIOS).
// One-way: the server never learns the ids themselves. Reported by the client, so a changed
// client can send another; servers use it to recognise a machine, not to prove anything.
#pragma once

#include <string>

namespace kcds::runtime::identity {

// 64 lowercase hex characters, or "" when neither id could be read. Computed once.
std::string hardware_id();

}  // namespace kcds::runtime::identity
