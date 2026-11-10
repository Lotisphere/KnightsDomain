#include "qdcoop/core/Core.h"

#include "qdcoop/core/Config.h"
#include "qdcoop/core/Guard.h"
#include "qdcoop/core/Hooks.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/core/Util.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"
#include "SDK/Engine_classes.hpp"

namespace qd::core {
namespace {
constexpr char kTag[] = "core";
constexpr DWORD kPollMs = 250;
constexpr DWORD kReadyTimeoutMs = 120000;
constexpr DWORD kPumpCheckTimeoutMs = 15000;

std::atomic<bool>     g_started{false};
std::atomic<bool>     g_stop{false};
std::atomic<bool>     g_ready{false};
std::atomic<uint32_t> g_gameThreadId{0};
std::atomic<bool>     g_pumpConfirmed{false};

// Function-local statics: OnReady may be called from other TUs' static initializers.
struct ReadyState {
    std::mutex                         mutex;
    std::vector<std::function<void()>> handlers;
    bool                               flushed = false;  // after this, OnReady posts directly
};
ReadyState& Ready() {
    static ReadyState s;
    return s;
}

// ---- SEH-protected engine probes (no C++ objects with destructors in these frames) ----
enum class Probe { NoObjects, NoEngine, NoViewport, Ready, Fault };

Probe ProbeEngineRaw() {
    __try {
        SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
        if (arr->NumElements <= 0 || arr->NumChunks <= 0 || !arr->Objects) return Probe::NoObjects;
        SDK::UEngine* engine = SDK::UEngine::GetEngine();  // scans GObjects once, then cached by the SDK
        if (!engine) return Probe::NoEngine;
        if (!engine->GameViewport) return Probe::NoViewport;
        return Probe::Ready;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Probe::Fault;
    }
}

// Resolve the SDK's cached StaticClass() pointers off the game thread: FindClassFast is a full GObjects scan
// with string compares (tens of ms) and would otherwise hitch the first game-thread call.
bool PrewarmRaw() {
    __try {
        SDK::UGameplayStatics::StaticClass();
        SDK::UKismetSystemLibrary::StaticClass();
        SDK::UKismetStringLibrary::StaticClass();
        return SDK::UGameplayStatics::StaticClass() && SDK::UKismetSystemLibrary::StaticClass() &&
               SDK::UKismetStringLibrary::StaticClass();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* ProbeName(Probe p) {
    switch (p) {
        case Probe::NoObjects: return "GObjects empty";
        case Probe::NoEngine: return "no UEngine instance";
        case Probe::NoViewport: return "GEngine->GameViewport is null";
        case Probe::Ready: return "ready";
        default: return "access fault while probing";
    }
}

void FlushReadyHandlers() {
    std::vector<std::function<void()>> handlers;
    {
        ReadyState& s = Ready();
        std::lock_guard lock(s.mutex);
        handlers.swap(s.handlers);
        s.flushed = true;
    }
    for (auto& h : handlers) gamethread::Post(std::move(h));
    QD_INFO(kTag, "{} OnReady handler(s) posted to the game thread", handlers.size());
}

DWORD WINAPI InitThread(LPVOID) {
    const uint64_t t0 = util::NowMs();
    Probe last = Probe::NoObjects;
    for (;;) {
        if (g_stop.load()) return 0;
        last = ProbeEngineRaw();
        if (last == Probe::Ready) break;
        if (util::NowMs() - t0 >= kReadyTimeoutMs) {
            QD_ERROR(kTag, "engine not ready after {} s (last state: {}); giving up", kReadyTimeoutMs / 1000,
                     ProbeName(last));
            return 0;
        }
        Sleep(kPollMs);
    }
    QD_INFO(kTag, "engine ready after {} ms (GObjects: {} objects)", util::NowMs() - t0,
            SDK::UObject::GObjects->Num());

    if (!PrewarmRaw()) QD_WARN(kTag, "SDK class prewarm failed; first game-thread calls may hitch");

    if (!hooks::Init()) {
        QD_ERROR(kTag, "hook installation failed; {} stays inactive", kModName);
        return 0;
    }
    FlushReadyHandlers();
    g_ready.store(true);
    QD_INFO(kTag, "core ready");

    // Diagnostic: confirm the game-thread pump actually runs (wrong main-thread id would silently stall everything).
    gamethread::Post([] { g_pumpConfirmed.store(true); });
    const uint64_t t1 = util::NowMs();
    while (!g_pumpConfirmed.load() && !g_stop.load() && util::NowMs() - t1 < kPumpCheckTimeoutMs) Sleep(100);
    if (g_pumpConfirmed.load()) {
        QD_INFO(kTag, "game-thread pump confirmed on thread {}", g_gameThreadId.load());
    } else if (!g_stop.load()) {
        QD_WARN(kTag, "game-thread pump has not run within {} s; thread {} may not be the UE game thread",
                kPumpCheckTimeoutMs / 1000, g_gameThreadId.load());
    }
    return 0;
}

std::string ComputeGameBuild() {
    std::wstring exe(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        if (n == 0) return "unknown";
        if (n < exe.size()) {
            exe.resize(n);
            break;
        }
        exe.resize(exe.size() * 2);
    }
    uint64_t size = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &fad))
        size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    // The mapped image keeps the on-disk PE headers, so TimeDateStamp is read from memory.
    const auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    return std::format("{}-{:08x}", size, stamp);
}

log::Level ParseLevel(const std::string& s, bool& ok) {
    ok = true;
#if QD_DEV
    if (_stricmp(s.c_str(), "debug") == 0) return log::Level::Debug;
#endif
    // Release: the lowest level is info; "debug" is reported as unknown and falls back to info.
    if (_stricmp(s.c_str(), "info") == 0) return log::Level::Info;
    if (_stricmp(s.c_str(), "warn") == 0 || _stricmp(s.c_str(), "warning") == 0) return log::Level::Warn;
    if (_stricmp(s.c_str(), "error") == 0) return log::Level::Error;
    ok = false;
    return log::Level::Info;
}
}  // namespace

void Start(uint32_t mainThreadId) {
    if (g_started.exchange(true)) return;
    g_gameThreadId.store(mainThreadId);

    const bool configOk = config::Load();
    log::Init(paths::LogFile());
    bool levelOk = true;
    log::SetMinLevel(ParseLevel(config::Get().logLevel, levelOk));

    QD_INFO(kTag, "{} {} by {} starting ({} build)", kModName, kModVersion, kModAuthor, kDevBuild ? "dev" : "release");
#if QD_DEV
    QD_INFO(kTag, "game build {}; profile {}; transport {}; game thread id {}", GameBuild(), config::Get().profile,
            config::Get().transport, mainThreadId);
#else
    QD_INFO(kTag, "game build {}; game thread id {}", GameBuild(), mainThreadId);
#endif
    QD_INFO(kTag, "command line: {}", util::WideToUtf8(GetCommandLineW()));
    QD_INFO(kTag, "mod dir: {}; saved dir: {}", util::WideToUtf8(paths::ModDir()), util::WideToUtf8(paths::ModSaved()));
    if (!configOk) QD_WARN(kTag, "config.json could not be parsed; running with defaults (file left untouched)");
    if (!levelOk) QD_WARN(kTag, "unknown log_level '{}' (accepted: {}), using info", config::Get().logLevel,
                          kDevBuild ? "debug|info|warn|error" : "info|warn|error");
    if (mainThreadId == 0) QD_ERROR(kTag, "main thread id is 0; game-thread work will never run");

    HANDLE h = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr);
    if (!h) {
        QD_ERROR(kTag, "CreateThread(init) failed: {}", GetLastError());
        return;
    }
    CloseHandle(h);
}

void Stop() {
    g_stop.store(true);
    g_ready.store(false);
    hooks::Shutdown();
    QD_INFO(kTag, "core stopped");
}

bool IsReady() { return g_ready.load(); }
uint32_t GameThreadId() { return g_gameThreadId.load(std::memory_order_relaxed); }
bool IsInGameThread() { return GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed); }

void OnReady(std::function<void()> handler) {
    if (!handler) return;
    {
        ReadyState& s = Ready();
        std::lock_guard lock(s.mutex);
        if (!s.flushed) {
            s.handlers.push_back(std::move(handler));
            return;
        }
    }
    gamethread::Post(std::move(handler));  // engine already ready: run on the next pump
}

const std::string& GameBuild() {
    static const std::string build = ComputeGameBuild();
    return build;
}

}  // namespace qd::core

// =====================================================================================================================
namespace qd::gamethread {
namespace {
constexpr char  kTag[] = "gamethread";
constexpr float kMaxDt = 0.25f;

struct Ticker {
    TickerId                   id = 0;
    std::string                name;
    std::function<void(float)> fn;
    bool                       removed = false;
};

// Cross-thread queues (function-local: usable from static initializers).
struct Queues {
    std::mutex                         postMutex;
    std::vector<std::function<void()>> posts;
    std::atomic<bool>                  hasPosts{false};

    std::mutex            tickerMutex;
    std::vector<Ticker>   pendingAdd;
    std::vector<TickerId> pendingRemove;
    std::atomic<bool>     tickersDirty{false};
    std::atomic<TickerId> nextTickerId{1};
};
Queues& Q() {
    static Queues q;
    return q;
}

// Game-thread-only state.
std::vector<Ticker> g_tickers;
bool                g_inPump = false;
bool                g_ticking = false;
LARGE_INTEGER       g_qpcFreq{};
LARGE_INTEGER       g_lastCheck{};
SDK::UWorld*        g_lastWorld = nullptr;
float               g_lastRealTime = -1.f;

void ApplyTickerChanges() {
    Queues& q = Q();
    if (!q.tickersDirty.exchange(false)) return;
    std::vector<Ticker> adds;
    std::vector<TickerId> removes;
    {
        std::lock_guard lock(q.tickerMutex);
        adds.swap(q.pendingAdd);
        removes.swap(q.pendingRemove);
    }
    for (auto& t : adds) g_tickers.push_back(std::move(t));
    for (TickerId id : removes) {
        for (auto& t : g_tickers)
            if (t.id == id) t.removed = true;
    }
    std::erase_if(g_tickers, [](const Ticker& t) { return t.removed; });
}

void RunPosts() {
    Queues& q = Q();
    if (!q.hasPosts.load(std::memory_order_acquire)) return;
    std::vector<std::function<void()>> work;
    {
        std::lock_guard lock(q.postMutex);
        work.swap(q.posts);
        q.hasPosts.store(false, std::memory_order_release);
    }
    for (auto& fn : work) detail::GuardedCall(kTag, "posted task", fn);
}

void RunTickers(float dt) {
    ApplyTickerChanges();
    g_ticking = true;
    // Index loop: g_tickers is not resized while g_ticking (adds/removes are deferred), only `removed` flips.
    for (size_t i = 0; i < g_tickers.size(); ++i) {
        Ticker& t = g_tickers[i];
        if (t.removed) continue;
        const auto r = detail::GuardedCall(kTag, t.name.c_str(), [&] { t.fn(dt); });
        if (r == detail::GuardResult::SehException) {
            QD_ERROR(kTag, "ticker '{}' removed after a structured exception", t.name);
            t.removed = true;
        }
    }
    g_ticking = false;
    // End of frame: apply registrations/removals made during this frame.
    if (std::any_of(g_tickers.begin(), g_tickers.end(), [](const Ticker& t) { return t.removed; }))
        std::erase_if(g_tickers, [](const Ticker& t) { return t.removed; });
    ApplyTickerChanges();
}
}  // namespace

void Post(std::function<void()> fn) {
    if (!fn) return;
    Queues& q = Q();
    {
        std::lock_guard lock(q.postMutex);
        q.posts.push_back(std::move(fn));
    }
    q.hasPosts.store(true, std::memory_order_release);
}

TickerId AddTicker(const char* name, std::function<void(float dt)> fn) {
    if (!fn) return 0;
    Queues& q = Q();
    Ticker t;
    t.id = q.nextTickerId.fetch_add(1);
    t.name = name ? name : "ticker";
    t.fn = std::move(fn);
    const TickerId id = t.id;
    {
        std::lock_guard lock(q.tickerMutex);
        q.pendingAdd.push_back(std::move(t));
    }
    q.tickersDirty.store(true, std::memory_order_release);
    return id;
}

void RemoveTicker(TickerId id) {
    if (id == 0) return;
    Queues& q = Q();
    // On the game thread, stop it from running for the rest of this frame right away.
    if (core::IsInGameThread()) {
        for (auto& t : g_tickers)
            if (t.id == id) t.removed = true;
    }
    {
        std::lock_guard lock(q.tickerMutex);
        std::erase_if(q.pendingAdd, [id](const Ticker& t) { return t.id == id; });
        q.pendingRemove.push_back(id);
    }
    q.tickersDirty.store(true, std::memory_order_release);
}

void Pump() {
    if (!core::IsInGameThread() || g_inPump) return;

    // Throttle: at most one frame check per millisecond.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_qpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_qpcFreq);
    if (g_lastCheck.QuadPart != 0 && (now.QuadPart - g_lastCheck.QuadPart) * 1000 < g_qpcFreq.QuadPart) return;
    g_lastCheck = now;

    g_inPump = true;
    SDK::UWorld* world = ue::World();
    if (!world) {
        RunPosts();  // no world (loading / transition): posted tasks only
        g_inPump = false;
        return;
    }
    // GetRealTimeSeconds goes through ProcessEvent -> our detour -> Pump(), which returns at once (g_inPump).
    // Not SEH-wrapped on purpose: it is a game call on a validated world; a fault there is a real crash.
    const float realTime = SDK::UGameplayStatics::GetRealTimeSeconds(world);
    bool newFrame = false;
    float dt = 0.f;
    if (world != g_lastWorld || realTime < g_lastRealTime) {
        // World changed (map travel) or its clock restarted: count as a new frame with dt = 0.
        g_lastWorld = world;
        g_lastRealTime = realTime;
        newFrame = true;
    } else if (realTime > g_lastRealTime) {
        dt = std::clamp(realTime - g_lastRealTime, 0.f, kMaxDt);
        g_lastRealTime = realTime;
        newFrame = true;
    }
    if (newFrame) {
        RunPosts();
        RunTickers(dt);
    }
    g_inPump = false;
}

}  // namespace qd::gamethread
