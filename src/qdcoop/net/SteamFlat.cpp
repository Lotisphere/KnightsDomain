// QD Coop — runtime resolution of the Steamworks flat API from the game's steam_api64.dll.
#include "qdcoop/net/SteamFlat.h"

#include "qdcoop/core/Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

namespace qd::net::steam {
namespace {

constexpr wchar_t kSteamDll[] = L"steam_api64.dll";

std::mutex        g_mutex;
Api               g_api;
std::atomic<bool> g_loaded{false};
bool              g_unavailable = false;  // a required export is missing: never retry
bool              g_loggedNoModule = false;
bool              g_loggedNoIfaces = false;

}  // namespace

bool Load() {
    if (g_loaded.load(std::memory_order_acquire)) return true;
    std::lock_guard lock(g_mutex);
    if (g_loaded.load(std::memory_order_relaxed)) return true;
    if (g_unavailable) return false;

    HMODULE mod = GetModuleHandleW(kSteamDll);
    if (!mod) {
        if (!g_loggedNoModule) {
            QD_WARN("net", "steam_api64.dll is not loaded in this process; Steam transport unavailable");
            g_loggedNoModule = true;
        }
        return false;
    }

    Api  api{};
    bool ok = true;
#define QD_STEAM_RESOLVE_REQUIRED(ret, name, params)                                          \
    api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(mod, #name));             \
    if (!api.name) {                                                                          \
        QD_ERROR("net", "steam_api64.dll does not export {}", #name);                         \
        ok = false;                                                                           \
    }
    QD_STEAM_REQUIRED_FUNCS(QD_STEAM_RESOLVE_REQUIRED)
#undef QD_STEAM_RESOLVE_REQUIRED

#define QD_STEAM_RESOLVE_OPTIONAL(ret, name, params)                                          \
    api.name = reinterpret_cast<decltype(api.name)>(GetProcAddress(mod, #name));             \
    if (!api.name) QD_WARN("net", "steam_api64.dll does not export optional {}", #name);
    QD_STEAM_OPTIONAL_FUNCS(QD_STEAM_RESOLVE_OPTIONAL)
#undef QD_STEAM_RESOLVE_OPTIONAL

    if (!ok) {
        g_unavailable = true;
        QD_ERROR("net", "Steam flat API incomplete; Steam networking disabled for this run");
        return false;
    }

    api.matchmaking = api.SteamAPI_SteamMatchmaking_v009();
    api.messages = api.SteamAPI_SteamNetworkingMessages_SteamAPI_v002();
    api.friends = api.SteamAPI_SteamFriends_v018();
    api.user = api.SteamAPI_SteamUser_v023();
    api.utils = api.SteamAPI_SteamUtils_v010();
    if (!api.matchmaking || !api.messages || !api.friends || !api.user || !api.utils) {
        if (!g_loggedNoIfaces) {
            QD_WARN("net",
                    "Steam interfaces not ready (matchmaking={} messages={} friends={} user={} utils={}); "
                    "is SteamAPI initialized? will retry",
                    api.matchmaking != nullptr, api.messages != nullptr, api.friends != nullptr,
                    api.user != nullptr, api.utils != nullptr);
            g_loggedNoIfaces = true;
        }
        return false;
    }

    g_api = api;
    g_loaded.store(true, std::memory_order_release);
    QD_INFO("net", "Steam flat API ready");
    return true;
}

const Api* Get() { return g_loaded.load(std::memory_order_acquire) ? &g_api : nullptr; }

bool ModuleStillLoaded() { return GetModuleHandleW(kSteamDll) != nullptr; }

}  // namespace qd::net::steam
