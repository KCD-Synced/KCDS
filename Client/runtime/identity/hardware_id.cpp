#include "hardware_id.hpp"

#include <Windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sha256.hpp"

namespace kcds::runtime::identity {
namespace {

// HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid: made when Windows is installed.
std::string machine_guid() {
  char value[128] = {};
  DWORD size = sizeof(value);
  const LSTATUS read =
      RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid",
                   RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, value, &size);
  return read == ERROR_SUCCESS ? std::string(value) : std::string();
}

// The SMBIOS system UUID (structure type 1, 16 bytes at offset 8), in hex; "" when the firmware
// table has none or it is all zeros or all ones.
std::string system_uuid() {
  constexpr DWORD kRawSmbios = 'RSMB';
  const UINT size = GetSystemFirmwareTable(kRawSmbios, 0, nullptr, 0);
  if (size == 0) {
    return {};
  }
  std::vector<std::uint8_t> table(size);
  if (GetSystemFirmwareTable(kRawSmbios, 0, table.data(), size) != size || size < 8) {
    return {};
  }
  // RawSMBIOSData: 8 bytes of header, then the structures.
  std::size_t at = 8;
  while (at + 4 <= table.size()) {
    const std::uint8_t type = table[at];
    const std::uint8_t length = table[at + 1];
    if (length < 4 || at + length > table.size()) {
      return {};
    }
    if (type == 1 && length >= 24) {
      const std::uint8_t* uuid = &table[at + 8];
      bool zeros = true;
      bool ones = true;
      std::string hex;
      static const char kHex[] = "0123456789abcdef";
      for (int i = 0; i < 16; ++i) {
        zeros = zeros && uuid[i] == 0x00;
        ones = ones && uuid[i] == 0xFF;
        hex.push_back(kHex[uuid[i] >> 4]);
        hex.push_back(kHex[uuid[i] & 15]);
      }
      return zeros || ones ? std::string() : hex;
    }
    // The structure's strings follow it, ended by two zero bytes.
    std::size_t next = at + length;
    while (next + 1 < table.size() && (table[next] != 0 || table[next + 1] != 0)) {
      ++next;
    }
    at = next + 2;
  }
  return {};
}

}  // namespace

std::string hardware_id() {
  static std::once_flag computed;
  static std::string id;
  std::call_once(computed, [] {
    const std::string guid = machine_guid();
    const std::string uuid = system_uuid();
    if (!guid.empty() || !uuid.empty()) {
      id = sha256_hex("kcds-hardware-id:" + guid + "|" + uuid);
    }
  });
  return id;
}

}  // namespace kcds::runtime::identity
