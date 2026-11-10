// Knights' Domain — "launched from Steam with an owned copy" check.
//
// Run once, after the co-op Session initialised and before hosting/joining is allowed:
//   1. steam_api64.dll is loaded in this process and carries a valid Authenticode signature (WinVerifyTrust,
//      WINTRUST_ACTION_GENERIC_VERIFY_V2, no UI, no revocation check) whose signer is Valve.
//   2. SteamAPI_SteamUser_v023 / SteamApps_v008 / SteamUtils_v010 return interfaces (resolved from the loaded DLL).
//   3. ISteamUtils::GetAppID() == 1799840.
//   4. ISteamUser::BLoggedOn().
//   5. ISteamApps::BIsSubscribedApp(1799840).
// Steps 2-5 run synchronously on the calling (game) thread; step 1 can touch the network for certificate-chain
// retrieval, so it runs on a short-lived worker thread and the state stays Pending until it finishes.
// The result is cached for the whole process. Dev flavour: skipped (one log line), state Passed.
#pragma once
#include <cstdint>
#include <string>

namespace qd::core {

enum class Ownership : uint8_t { NotChecked, Pending, Passed, Failed };

// Game thread. Starts the check the first time it is called; later calls do nothing.
void OwnershipCheck();
// Game thread. Marks the check failed without running it (e.g. the Steam API never came up within 90 s, i.e. the game
// was not launched from Steam). No effect once the check has passed or failed.
void OwnershipFail(const std::string& why);
// Thread-safe.
Ownership OwnershipState();
std::string OwnershipFailReason();   // English, for the log; "" unless Failed

// Steam's game language ("english", "schinese", ...), or "" when Steam is not available yet. Game thread.
std::string SteamGameLanguage();

}  // namespace qd::core
