// QD Coop — per-save-slot display names.
// File: paths::ModSaved()\profiles.json  =  { "<SteamID64 | dev-profile-N>": { "slot<i>": "名字" } }
// No SDK / UObject access in here: thread-safe, usable from any thread.
#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace qd::profiles {

constexpr size_t kMaxNameChars = 24;   // code points (bytes are additionally capped at net::kMaxNameBytes)

// Trims ASCII whitespace, drops control characters and invalid UTF-8, caps at 24 code points / 72 bytes.
std::string Sanitize(std::string_view utf8);
size_t CountCodepoints(std::string_view utf8);

// Thread-safe. Loads the file lazily on first use.
std::optional<std::string> GetName(const std::string& profileKey, int saveSlot);
// Empty name removes the entry. Persists immediately (atomic replace). Returns false on I/O failure.
bool SetName(const std::string& profileKey, int saveSlot, const std::string& name);

// Steam persona name via steam_api64.dll flat exports (SteamAPI_SteamFriends_v018 + SteamAPI_ISteamFriends_GetPersonaName).
// Empty when Steam is not loaded / not initialized.
std::string SteamPersonaName();

}  // namespace qd::profiles
