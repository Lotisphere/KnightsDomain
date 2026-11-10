// QD Coop — the slice of the Steamworks flat API (SDK 1.63, steam_api_flat.h) that the net layer uses.
//
// We do NOT link steam_api64.lib and we do NOT ship a second Steam SDK. The functions are declared below as
// `extern "C" __declspec(dllimport)` purely so their signatures live in one place; nothing ever calls those
// declarations directly (that would need the import lib). Instead steam::Load() resolves each export from the
// steam_api64.dll the game already loaded (GetModuleHandleW + GetProcAddress) into steam::Api, whose member types
// are derived from the declarations with decltype — unevaluated, so no link-time reference is created.
//
// Threading: the game's OnlineSubsystemSteam pumps SteamAPI_RunCallbacks on its own thread. CallbackQueue<T>::Run
// therefore only copies the struct into a mutex-protected vector; the game thread drains it (Session::Tick /
// SteamTransport::Poll).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace qd::net::steam {

using SteamAPICall_t = uint64_t;
constexpr SteamAPICall_t k_uAPICallInvalid = 0;

// Opaque interface pointers (the flat API passes them as the first argument).
struct ISteamMatchmaking;
struct ISteamNetworkingMessages;
struct ISteamFriends;
struct ISteamUser;
struct ISteamUtils;
struct ISteamApps;

// ---------------------------------------------------------------- constants (SDK 1.63)
constexpr int32_t k_EResultOK = 1;
constexpr int     k_ELobbyTypeFriendsOnly = 1;
constexpr int     k_EFriendFlagImmediate = 0x04;
constexpr uint32_t k_EChatRoomEnterResponseSuccess = 1;
constexpr uint32_t k_EChatMemberStateChangeEntered = 0x0001;
constexpr uint32_t k_EChatMemberStateChangeLeft = 0x0002;
constexpr uint32_t k_EChatMemberStateChangeDisconnected = 0x0004;
constexpr uint32_t k_EChatMemberStateChangeKicked = 0x0008;
constexpr uint32_t k_EChatMemberStateChangeBanned = 0x0010;
constexpr int k_nSteamNetworkingSend_Unreliable = 0;
constexpr int k_nSteamNetworkingSend_NoNagle = 1;
constexpr int k_nSteamNetworkingSend_NoDelay = 4;
constexpr int k_nSteamNetworkingSend_UnreliableNoDelay =
    k_nSteamNetworkingSend_Unreliable | k_nSteamNetworkingSend_NoDelay | k_nSteamNetworkingSend_NoNagle;  // 5
constexpr int k_nSteamNetworkingSend_Reliable = 8;
// events (attacks, montages, damage) are small and time-critical; do not wait up to 5 ms to batch them.
constexpr int k_nSteamNetworkingSend_ReliableNoNagle = k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_NoNagle;  // 9

// ---------------------------------------------------------------- structs
// Windows Steam callback structs use VALVE_CALLBACK_PACK_LARGE == #pragma pack(8).
#pragma pack(push, 8)

// SDK 1.63 layout: ESteamNetworkingIdentityType m_eType; int m_cbSize; union { uint64 m_steamID64; ...;
// char m_szUnknownRaw[128]; }. The union holds a uint64, hence alignas(8). Never poke the fields by hand: use
// SteamAPI_SteamNetworkingIdentity_SetSteamID64 / GetSteamID64. Zero-initialised == Clear() (type Invalid = 0).
struct alignas(8) SteamNetworkingIdentity {
    int32_t m_eType;
    int32_t m_cbSize;
    char    m_szUnknownRaw[128];
};
static_assert(sizeof(SteamNetworkingIdentity) == 136, "SteamNetworkingIdentity must be 136 bytes (SDK 1.63)");

// Only the leading fields are declared; Steam owns these objects (we only read through pointers it hands us and
// give them back with SteamAPI_SteamNetworkingMessage_t_Release). Never create or copy one.
struct SteamNetworkingMessage_t {
    void*                   m_pData;         // +0x00
    int32_t                 m_cbSize;        // +0x08
    uint32_t                m_conn;          // +0x0C HSteamNetConnection (unused, keeps the padding explicit)
    SteamNetworkingIdentity m_identityPeer;  // +0x10
    // ... m_nConnUserData, m_usecTimeReceived, m_nMessageNumber, m_pfnFreeData, m_pfnRelease, m_nChannel, ...
    //     intentionally not declared.
};
static_assert(offsetof(SteamNetworkingMessage_t, m_pData) == 0x00, "assumed SDK 1.63 offset");
static_assert(offsetof(SteamNetworkingMessage_t, m_cbSize) == 0x08, "assumed SDK 1.63 offset");
static_assert(offsetof(SteamNetworkingMessage_t, m_identityPeer) == 0x10, "assumed SDK 1.63 offset");

// isteamfriends.h. Real members are CGameID / CSteamID (8-byte, pack(1) classes); offsets are identical.
struct FriendGameInfo_t {
    uint64_t m_gameID;
    uint32_t m_unGameIP;
    uint16_t m_usGamePort;
    uint16_t m_usQueryPort;
    uint64_t m_steamIDLobby;
};
static_assert(sizeof(FriendGameInfo_t) == 24);
static_assert(offsetof(FriendGameInfo_t, m_steamIDLobby) == 16);

// k_iSteamFriendsCallbacks (300) + 33
struct GameLobbyJoinRequested_t {
    enum { k_iCallback = 333 };
    uint64_t m_steamIDLobby;
    uint64_t m_steamIDFriend;
};
static_assert(sizeof(GameLobbyJoinRequested_t) == 16);

// k_iSteamFriendsCallbacks (300) + 37. Fired (instead of 333) when the user clicks "Join Game" on a friend whose
// rich presence has a "connect" key while our game is already running; m_rgchConnect is that "connect" string.
struct GameRichPresenceJoinRequested_t {
    enum { k_iCallback = 337 };
    uint64_t m_steamIDFriend;
    char     m_rgchConnect[256];  // k_cchMaxRichPresenceValueLength
};
static_assert(sizeof(GameRichPresenceJoinRequested_t) == 264);

// k_iSteamMatchmakingCallbacks (500) + 6
struct LobbyChatUpdate_t {
    enum { k_iCallback = 506 };
    uint64_t m_ulSteamIDLobby;
    uint64_t m_ulSteamIDUserChanged;
    uint64_t m_ulSteamIDMakingChange;
    uint32_t m_rgfChatMemberStateChange;
};
static_assert(sizeof(LobbyChatUpdate_t) == 32);

// k_iSteamMatchmakingCallbacks (500) + 4 — result of JoinLobby (polled via GetAPICallResult).
struct LobbyEnter_t {
    enum { k_iCallback = 504 };
    uint64_t m_ulSteamIDLobby;
    uint32_t m_rgfChatPermissions;
    bool     m_bLocked;
    uint32_t m_EChatRoomEnterResponse;
};
static_assert(sizeof(LobbyEnter_t) == 24);
static_assert(offsetof(LobbyEnter_t, m_EChatRoomEnterResponse) == 16);

// k_iSteamMatchmakingCallbacks (500) + 13 — result of CreateLobby (polled via GetAPICallResult).
struct LobbyCreated_t {
    enum { k_iCallback = 513 };
    int32_t  m_eResult;  // EResult
    uint64_t m_ulSteamIDLobby;
};
static_assert(sizeof(LobbyCreated_t) == 16);

// k_iSteamNetworkingMessagesCallbacks (1250) + 1
struct SteamNetworkingMessagesSessionRequest_t {
    enum { k_iCallback = 1251 };
    SteamNetworkingIdentity m_identityRemote;
};
static_assert(sizeof(SteamNetworkingMessagesSessionRequest_t) == 136);

#pragma pack(pop)

// ---------------------------------------------------------------- legacy callback object
// Binary-compatible with steam_api_internal.h's CCallbackBase. The virtual declaration order MUST match the official
// header (MSVC lays out overloaded virtuals by declaration order) and there must be NO virtual destructor.
class CCallbackBase {
public:
    CCallbackBase() { m_nCallbackFlags = 0; m_iCallback = 0; }
    virtual void Run(void* pvParam) = 0;
    virtual void Run(void* pvParam, bool bIOFailure, uint64_t hSteamAPICall) = 0;
    virtual int  GetCallbackSizeBytes() = 0;
protected:
    enum { k_ECallbackFlagsRegistered = 0x01, k_ECallbackFlagsGameServer = 0x02 };
    uint8_t m_nCallbackFlags;
    int     m_iCallback;
};

// ---------------------------------------------------------------- flat functions
// X(return type, export name, (parameters)). Signatures follow SDK 1.63 steam_api_flat.h: interface pointer first,
// CSteamID passed/returned as uint64_t, C++ references passed as pointers, enums as int.
#define QD_STEAM_REQUIRED_FUNCS(X)                                                                                    \
    X(ISteamMatchmaking*, SteamAPI_SteamMatchmaking_v009, ())                                                         \
    X(ISteamNetworkingMessages*, SteamAPI_SteamNetworkingMessages_SteamAPI_v002, ())                                  \
    X(ISteamFriends*, SteamAPI_SteamFriends_v018, ())                                                                 \
    X(ISteamUser*, SteamAPI_SteamUser_v023, ())                                                                       \
    X(ISteamUtils*, SteamAPI_SteamUtils_v010, ())                                                                     \
    X(void, SteamAPI_RegisterCallback, (CCallbackBase * pCallback, int iCallback))                                    \
    X(void, SteamAPI_UnregisterCallback, (CCallbackBase * pCallback))                                                 \
    X(SteamAPICall_t, SteamAPI_ISteamMatchmaking_CreateLobby, (ISteamMatchmaking * self, int eLobbyType, int cMaxMembers)) \
    X(SteamAPICall_t, SteamAPI_ISteamMatchmaking_JoinLobby, (ISteamMatchmaking * self, uint64_t steamIDLobby))        \
    X(void, SteamAPI_ISteamMatchmaking_LeaveLobby, (ISteamMatchmaking * self, uint64_t steamIDLobby))                 \
    X(uint64_t, SteamAPI_ISteamMatchmaking_GetLobbyOwner, (ISteamMatchmaking * self, uint64_t steamIDLobby))          \
    X(int, SteamAPI_ISteamMatchmaking_GetNumLobbyMembers, (ISteamMatchmaking * self, uint64_t steamIDLobby))          \
    X(uint64_t, SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex,                                                     \
      (ISteamMatchmaking * self, uint64_t steamIDLobby, int iMember))                                                 \
    X(bool, SteamAPI_ISteamMatchmaking_SetLobbyData,                                                                  \
      (ISteamMatchmaking * self, uint64_t steamIDLobby, const char* pchKey, const char* pchValue))                    \
    X(const char*, SteamAPI_ISteamMatchmaking_GetLobbyData,                                                           \
      (ISteamMatchmaking * self, uint64_t steamIDLobby, const char* pchKey))                                          \
    X(bool, SteamAPI_ISteamMatchmaking_RequestLobbyData, (ISteamMatchmaking * self, uint64_t steamIDLobby))           \
    X(int32_t, SteamAPI_ISteamNetworkingMessages_SendMessageToUser,                                                   \
      (ISteamNetworkingMessages * self, const SteamNetworkingIdentity* identityRemote, const void* pubData,           \
       uint32_t cubData, int nSendFlags, int nRemoteChannel))                                                         \
    X(int, SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel,                                                \
      (ISteamNetworkingMessages * self, int nLocalChannel, SteamNetworkingMessage_t** ppOutMessages, int nMaxMessages)) \
    X(bool, SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser,                                                  \
      (ISteamNetworkingMessages * self, const SteamNetworkingIdentity* identityRemote))                               \
    X(bool, SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser,                                                   \
      (ISteamNetworkingMessages * self, const SteamNetworkingIdentity* identityRemote))                               \
    X(void, SteamAPI_SteamNetworkingMessage_t_Release, (SteamNetworkingMessage_t * self))                             \
    X(void, SteamAPI_SteamNetworkingIdentity_SetSteamID64, (SteamNetworkingIdentity * self, uint64_t steamID))        \
    X(uint64_t, SteamAPI_SteamNetworkingIdentity_GetSteamID64, (SteamNetworkingIdentity * self))                      \
    X(const char*, SteamAPI_ISteamFriends_GetPersonaName, (ISteamFriends * self))                                     \
    X(int, SteamAPI_ISteamFriends_GetFriendCount, (ISteamFriends * self, int iFriendFlags))                           \
    X(uint64_t, SteamAPI_ISteamFriends_GetFriendByIndex, (ISteamFriends * self, int iFriend, int iFriendFlags))       \
    X(bool, SteamAPI_ISteamFriends_GetFriendGamePlayed,                                                               \
      (ISteamFriends * self, uint64_t steamIDFriend, FriendGameInfo_t * pFriendGameInfo))                             \
    X(const char*, SteamAPI_ISteamFriends_GetFriendPersonaName, (ISteamFriends * self, uint64_t steamIDFriend))       \
    X(bool, SteamAPI_ISteamFriends_SetRichPresence, (ISteamFriends * self, const char* pchKey, const char* pchValue)) \
    X(void, SteamAPI_ISteamFriends_ClearRichPresence, (ISteamFriends * self))                                         \
    X(uint64_t, SteamAPI_ISteamUser_GetSteamID, (ISteamUser * self))                                                  \
    X(bool, SteamAPI_ISteamUtils_IsAPICallCompleted, (ISteamUtils * self, SteamAPICall_t hSteamAPICall, bool* pbFailed)) \
    X(bool, SteamAPI_ISteamUtils_GetAPICallResult,                                                                    \
      (ISteamUtils * self, SteamAPICall_t hSteamAPICall, void* pCallback, int cubCallback, int iCallbackExpected,     \
       bool* pbFailed))

// Nice-to-have exports outside the confirmed list: a missing one is logged but does not disable Steam.
#define QD_STEAM_OPTIONAL_FUNCS(X) \
    X(int, SteamAPI_ISteamMatchmaking_GetLobbyMemberLimit, (ISteamMatchmaking * self, uint64_t steamIDLobby))      \
    X(int, SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo,                                                \
      (ISteamNetworkingMessages * self, const SteamNetworkingIdentity* identityRemote, void* pConnectionInfo,         \
       void* pQuickStatus))

// exports used by the ownership check (core/Ownership.cpp) and the first-run UI language detection. They are
// NOT part of steam::Api / steam::Load() (the net layer does not need them, and a missing one must not disable
// networking); core/Ownership.cpp resolves them itself with the same GetProcAddress + decltype pattern.
// SteamAPI_SteamUser_v023 / SteamAPI_SteamUtils_v010 are declared in the required list above. AppId_t = uint32.
#define QD_STEAM_OWNERSHIP_FUNCS(X)                                                                                  \
    X(ISteamApps*, SteamAPI_SteamApps_v008, ())                                                                      \
    X(uint32_t, SteamAPI_ISteamUtils_GetAppID, (ISteamUtils * self))                                                 \
    X(bool, SteamAPI_ISteamUser_BLoggedOn, (ISteamUser * self))                                                      \
    X(bool, SteamAPI_ISteamApps_BIsSubscribedApp, (ISteamApps * self, uint32_t appID))                               \
    X(const char*, SteamAPI_ISteamApps_GetCurrentGameLanguage, (ISteamApps * self))

namespace decl {
extern "C" {
#define QD_STEAM_DECLARE(ret, name, params) __declspec(dllimport) ret name params;
QD_STEAM_REQUIRED_FUNCS(QD_STEAM_DECLARE)
QD_STEAM_OPTIONAL_FUNCS(QD_STEAM_DECLARE)
QD_STEAM_OWNERSHIP_FUNCS(QD_STEAM_DECLARE)
#undef QD_STEAM_DECLARE
}
}  // namespace decl

struct Api {
#define QD_STEAM_MEMBER(ret, name, params) decltype(&::qd::net::steam::decl::name) name = nullptr;
    QD_STEAM_REQUIRED_FUNCS(QD_STEAM_MEMBER)
    QD_STEAM_OPTIONAL_FUNCS(QD_STEAM_MEMBER)
#undef QD_STEAM_MEMBER

    // Interface pointers, fetched once by Load().
    ISteamMatchmaking*        matchmaking = nullptr;
    ISteamNetworkingMessages* messages = nullptr;
    ISteamFriends*            friends = nullptr;
    ISteamUser*               user = nullptr;
    ISteamUtils*              utils = nullptr;
};

// Game thread. Resolves every export from the already-loaded steam_api64.dll. Returns false (and logs) if the DLL is
// not loaded yet or the Steam interfaces are still null (retried on the next call), or if any required export is
// missing (permanent: Steam support is marked unavailable for the whole process).
bool Load();
// nullptr until Load() has succeeded.
const Api* Get();
// True while steam_api64.dll is still mapped (guards calls made during shutdown).
bool ModuleStillLoaded();

// Registered legacy callback that just queues copies of T for the game thread.
template <class T>
class CallbackQueue final : public CCallbackBase {
public:
    CallbackQueue() = default;
    CallbackQueue(const CallbackQueue&) = delete;
    CallbackQueue& operator=(const CallbackQueue&) = delete;
    ~CallbackQueue() { Unregister(); }  // deliberately non-virtual (see CCallbackBase)

    bool Register() {
        if (registered_) return true;
        const Api* api = Get();
        if (!api) return false;
        api->SteamAPI_RegisterCallback(this, T::k_iCallback);  // sets k_ECallbackFlagsRegistered itself
        registered_ = true;
        return true;
    }
    void Unregister() {
        if (!registered_) return;
        registered_ = false;
        const Api* api = Get();
        if (api && ModuleStillLoaded()) api->SteamAPI_UnregisterCallback(this);
    }

    // Steam's callback thread.
    void Run(void* pvParam) override {
        if (!pvParam) return;
        T copy;
        std::memcpy(&copy, pvParam, sizeof(T));
        std::lock_guard lock(mutex_);
        if (items_.size() < kMaxQueued) items_.push_back(copy);
    }
    void Run(void* pvParam, bool /*bIOFailure*/, uint64_t /*hSteamAPICall*/) override { Run(pvParam); }
    int  GetCallbackSizeBytes() override { return static_cast<int>(sizeof(T)); }

    // Game thread.
    std::vector<T> Drain() {
        std::lock_guard lock(mutex_);
        return std::exchange(items_, {});
    }

private:
    static constexpr size_t kMaxQueued = 256;
    std::mutex     mutex_;
    std::vector<T> items_;
    bool           registered_ = false;
};

}  // namespace qd::net::steam
