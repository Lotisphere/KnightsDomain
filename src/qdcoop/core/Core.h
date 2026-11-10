// QD Coop 鈥?core contracts. Signatures here are the contract; implementations live in the matching .cpp.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace SDK { class UObject; class UFunction; }

namespace qd::core {

// Entry point exported from KnightsDomain.dll as `extern "C" __declspec(dllexport) void KnightsDomain_Start(uint32_t)`.
// Called by the xinput1_3 proxy from a worker thread (never under the loader lock).
// Spawns the init thread: waits for GObjects + GEngine->GameViewport, then installs hooks, then calls every OnReady handler.
void Start(uint32_t mainThreadId);
void Stop();
bool IsReady();
uint32_t GameThreadId();
bool IsInGameThread();
// Modules register here before Start(); handlers run once on the game thread when the engine is ready.
void OnReady(std::function<void()> handler);

// The build fingerprint sent in Hello: "<exe size>-<exe PE TimeDateStamp hex>".
const std::string& GameBuild();
constexpr const char* kModVersion = "1.0.3";
// hard-coded identity. Shown in the F5 About page and checked in the network handshake (MsgHello) and the Steam
// lobby data (kd_mod / kd_author); a peer with a different name or author is rejected as RejectReason::ModVersion.
constexpr const char* kModName = "Knights' Domain";
constexpr const char* kModAuthor = "Mangolotis";
constexpr uint32_t    kSteamAppId = 1799840;   // Queen's Domain on Steam
// True in the developer flavour (CMake QD_DEV_BUILD=ON).
#if QD_DEV
constexpr bool kDevBuild = true;
#else
constexpr bool kDevBuild = false;
#endif

}  // namespace qd::core

namespace qd::gamethread {

// Thread-safe. Runs fn on the game thread during the next pump.
void Post(std::function<void()> fn);
// Tickers run once per rendered frame, in registration order, on the game thread. dt = real seconds since last frame (clamped to 0.25).
using TickerId = uint32_t;
TickerId AddTicker(const char* name, std::function<void(float dt)> fn);
void RemoveTicker(TickerId id);
// Internal: called by the hook layer on the game thread. Detects a new frame (world real-time advanced) and runs posts + tickers.
void Pump();

}  // namespace qd::gamethread
