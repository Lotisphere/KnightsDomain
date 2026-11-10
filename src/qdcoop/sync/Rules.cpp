// QD Coop — co-op rules. See Rules.h.
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"
#include "SDK/QueensDomain_classes.hpp"
#include "SDK/EnemyBase_classes.hpp"
#include "SDK/EnemyBase_parameters.hpp"
#include "SDK/EnemySpawner_parameters.hpp"
#include "SDK/Base_LD45_GM_classes.hpp"
#include "SDK/Base_LD45_GM_parameters.hpp"

#include "qdcoop/sync/Rules.h"

#include "qdcoop/core/Core.h"
#include "qdcoop/core/Hooks.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/sync/Coop.h"
#include "qdcoop/sync/EnemySync.h"

#include <chrono>
#include <vector>

namespace qd::sync {
namespace {

constexpr const char* kTag = "rules";

constexpr const char* kEnemyBaseClass     = "/Game/Enemies/EnemyBase.EnemyBase_C";
constexpr const char* kFnIsPausedTD       = "/Game/LD45/Blueprints/Base_LD45_GM.Base_LD45_GM_C:IsPausedTimeDilationEnabled";
constexpr const char* kFnIsEnemyPauseTDL  = "/Game/LD45/Blueprints/Base_LD45_GM.Base_LD45_GM_C:IsEnemyPauseTDLEnabled";
constexpr const char* kFnSetEnemiesPaused = "/Game/Enemies/EnemySpawner.EnemySpawner_C:SetEnemiesPaused";
constexpr const char* kFnEnemySetPauseTD  = "/Game/Enemies/EnemyBase.EnemyBase_C:SetPauseTimeDilation";

constexpr float  kScanInterval = 0.5f;    // fallback scan for frozen enemies
constexpr float  kFrozenCTD    = 0.01f;
constexpr double kLogEverySec  = 10.0;

using Clock = std::chrono::steady_clock;
double Now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

enum class Field : uint8_t { PauseTD, EnemyPauseTDL };

struct Saved {
    SDK::ABase_LD45_GM_C* gm = nullptr;   // nullptr: nothing was overridden for this call
    Field field = Field::PauseTD;
    bool  value = false;
};

struct State {
    bool inited = false;
    gamethread::TickerId ticker = 0;
    std::vector<hooks::HookId> hookIds;
    bool active = false;
    SDK::UWorld* world = nullptr;
    SDK::UClass* enemyBase = nullptr;
    std::vector<Saved> saved;
    std::vector<SDK::AActor*> enemies;
    float  scanAccum = 0;
    double lastPauseLog = -100, lastFreezeLog = -100;
    int    unpauses = 0, unfrozen = 0, skippedFreezes = 0;
};
State& S() {
    static State s;
    return s;
}

bool& FieldRef(SDK::ABase_LD45_GM_C* gm, Field f) { return f == Field::PauseTD ? gm->PauseTD : gm->EnemyPauseTDLEnabled; }

// IsPausedTimeDilationEnabled / IsEnemyPauseTDLEnabled answer through an OUT parameter (bool* Paused). A post hook only
// sees the frame's Locals copy, so rewriting it there does not reach the caller. Instead, for the duration of the call
// the game mode's backing flag (PauseTD / EnemyPauseTDLEnabled) is forced to false, so the BP body itself answers
// "false"; the post hook puts the real value back, leaving the game mode's state untouched.
bool PreIsPaused(hooks::CallInfo& ci, Field field) {
    auto& s = S();
    Saved sv;
    sv.field = field;
    if (s.active && ci.object && ue::IsValid(ci.object)) {
        auto* gm = static_cast<SDK::ABase_LD45_GM_C*>(ci.object);
        bool& flag = FieldRef(gm, field);
        sv.gm = gm;
        sv.value = flag;
        flag = false;
    }
    if (s.saved.size() > 64) s.saved.clear();   // a lost post callback must not grow this forever
    s.saved.push_back(sv);
    return false;
}

void PostIsPaused(hooks::CallInfo& ci) {
    auto& s = S();
    if (s.saved.empty()) return;
    const Saved sv = s.saved.back();
    s.saved.pop_back();
    if (sv.gm && ue::IsValid(sv.gm)) FieldRef(sv.gm, sv.field) = sv.value;
    // As the work order asks: also write false into the param block we were given (no effect on BP->BP callers,
    // whose out value lives in their own frame — the flag override above is what actually works).
    if (sv.gm && ci.params) static_cast<SDK::Params::Base_LD45_GM_C_IsPausedTimeDilationEnabled*>(ci.params)->Paused = false;
}

// Spawner -> enemies freeze and the per-enemy freeze: skip only the "pause" direction while in a session.
bool PreSetEnemiesPaused(hooks::CallInfo& ci) {
    auto& s = S();
    if (!s.active || !ci.params) return false;
    if (!static_cast<SDK::Params::EnemySpawner_C_SetEnemiesPaused*>(ci.params)->Paused) return false;
    ++s.skippedFreezes;
    return true;
}

bool PreEnemySetPauseTD(hooks::CallInfo& ci) {
    auto& s = S();
    if (!s.active || !ci.params) return false;
    if (!static_cast<SDK::Params::EnemyBase_C_SetPauseTimeDilation*>(ci.params)->Paused_0) return false;
    ++s.skippedFreezes;
    return true;
}

void UnfreezeEnemies(double now) {
    auto& s = S();
    if (!ue::IsValid(s.enemyBase)) s.enemyBase = ue::FindClass(kEnemyBaseClass);
    ActorsOfClass(SDK::AFS_EnemyCharacterBase::StaticClass(), s.enemies);
    int fixed = 0;
    for (SDK::AActor* a : s.enemies) {
        auto* en = static_cast<SDK::AFS_EnemyCharacterBase*>(a);
        if (!ue::IsValid(en) || en->bDead || en->bIsParalyzed) continue;   // paralysis may legitimately use CTD
        if (en->CustomTimeDilation >= kFrozenCTD) continue;
        if (s.enemyBase && en->IsA(s.enemyBase) && static_cast<SDK::AEnemyBase_C*>(en)->Paused) {
            // Let the enemy BP clear its own Paused flag and recompute its dilation.
            SDK::Params::EnemyBase_C_SetPauseTimeDilation p{};
            p.Paused_0 = false;
            if (SDK::UFunction* fn = ue::FindFunction(en, "SetPauseTimeDilation")) ue::CallFunction(en, fn, &p);
        }
        if (en->CustomTimeDilation < kFrozenCTD) en->CustomTimeDilation = 1.0f;
        ++fixed;
    }
    if (fixed) {
        s.unfrozen += fixed;
        if (now - s.lastFreezeLog > kLogEverySec) {
            s.lastFreezeLog = now;
            QD_INFO(kTag, "unfroze {} enemies (total {}, skipped freeze calls {})", fixed, s.unfrozen, s.skippedFreezes);
        }
    }
}

void Tick(float dt) {
    auto& s = S();
    SDK::UWorld* world = ue::World();
    if (world != s.world) {
        s.world = world;
        s.enemies.clear();
        s.saved.clear();
    }
    SDK::ACharacter* ch = ue::LocalCharacter();
    const bool active = coop::InSession() && world && ch && ue::IsValid(ch);
    if (active != s.active) {
        s.active = active;
        QD_INFO(kTag, "{}", active ? "co-op rules ON: no pause, no enemy freeze" : "co-op rules OFF: original pause behaviour");
    }
    if (!active) {
        s.scanAccum = 0;
        return;
    }
    const double now = Now();

    // 1. No real pause: menus stay open, the world keeps running.
    if (SDK::UGameplayStatics::IsGamePaused(world)) {
        SDK::UGameplayStatics::SetGamePaused(world, false);
        ++s.unpauses;
        if (now - s.lastPauseLog > kLogEverySec) {
            s.lastPauseLog = now;
            QD_INFO(kTag, "game pause cancelled (co-op), total {}", s.unpauses);
        }
    }

    // 2. Fallback: no frozen enemies (CustomTimeDilation ~ 0).
    s.scanAccum += dt;
    if (s.scanAccum >= kScanInterval) {
        s.scanAccum = 0;
        UnfreezeEnemies(now);
    }
}

}  // namespace

void RulesInit() {
    auto& s = S();
    if (s.inited) return;
    s.inited = true;
    auto& ids = s.hookIds;
    ids.push_back(hooks::AddPre(kFnIsPausedTD, [](hooks::CallInfo& ci) { return PreIsPaused(ci, Field::PauseTD); }));
    ids.push_back(hooks::AddPost(kFnIsPausedTD, PostIsPaused));
    ids.push_back(hooks::AddPre(kFnIsEnemyPauseTDL, [](hooks::CallInfo& ci) { return PreIsPaused(ci, Field::EnemyPauseTDL); }));
    ids.push_back(hooks::AddPost(kFnIsEnemyPauseTDL, PostIsPaused));
    ids.push_back(hooks::AddPre(kFnSetEnemiesPaused, PreSetEnemiesPaused));
    ids.push_back(hooks::AddPre(kFnEnemySetPauseTD, PreEnemySetPauseTD));
    s.ticker = gamethread::AddTicker("rules", [](float dt) { Tick(dt); });
    QD_INFO(kTag, "initialized");
}

void RulesShutdown() {
    auto& s = S();
    if (!s.inited) return;
    for (hooks::HookId id : s.hookIds) hooks::Remove(id);
    s.hookIds.clear();
    if (s.ticker) {
        gamethread::RemoveTicker(s.ticker);
        s.ticker = 0;
    }
    s.active = false;
    s.inited = false;
}

}  // namespace qd::sync
