// M0 smoke test. Logging only; never blocks or changes game behaviour.
//   1. On ready: World() name and LocalCharacter() class (null on the title screen).
//   2. Ticker: every 5 s, local player position + FPS (frames / 5 s). Also logs world changes.
//   3. Post hook FS_EnemyCharacterBase:GetTargetPlayer (fires for BP overrides via the SuperStruct chain),
//      at most once per 5 s: which enemy picked which target (checks BP-event dispatch and the result pointer).
//   4. Pre hook ItemPickup_Base_C:Interact: logs the pickup's path name (does not intercept).
#include "qdcoop/core/Core.h"
#include "qdcoop/core/Hooks.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"
#include "SDK/CoreUObject_structs.hpp"
#include "SDK/Engine_classes.hpp"

namespace {
constexpr char kTag[] = "smoke";
constexpr double kReportSeconds = 5.0;

using Clock = std::chrono::steady_clock;

std::string DescribeLocal() {
    SDK::ACharacter* ch = qd::ue::LocalCharacter();
    return ch ? std::format("{} ({})", qd::ue::PathName(ch), qd::ue::ClassName(ch)) : std::string("<none>");
}

void OnEngineReady() {
    SDK::UWorld* world = qd::ue::World();
    QD_INFO(kTag, "World: {}", world ? qd::ue::Name(world) : std::string("<null>"));
    QD_INFO(kTag, "LocalCharacter: {}", DescribeLocal());

    // 2. position + FPS every 5 s; world changes as they happen.
    struct TickState {
        Clock::time_point windowStart = Clock::now();
        uint32_t          frames = 0;
        SDK::UWorld*      lastWorld = nullptr;
    };
    auto state = std::make_shared<TickState>();
    state->lastWorld = world;
    qd::gamethread::AddTicker("smoke.report", [state](float) {
        ++state->frames;
        SDK::UWorld* w = qd::ue::World();
        if (w != state->lastWorld) {
            state->lastWorld = w;
            QD_INFO(kTag, "World changed: {}; LocalCharacter: {}", w ? qd::ue::Name(w) : std::string("<null>"),
                    DescribeLocal());
        }
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - state->windowStart).count();
        if (elapsed < kReportSeconds) return;
        const double fps = state->frames / elapsed;
        state->frames = 0;
        state->windowStart = now;
        if (SDK::ACharacter* ch = qd::ue::LocalCharacter()) {
            const SDK::FVector p = ch->K2_GetActorLocation();
            QD_INFO(kTag, "local player at ({:.0f}, {:.0f}, {:.0f}); {:.1f} fps", p.X, p.Y, p.Z, fps);
        } else {
            QD_INFO(kTag, "no local player; {:.1f} fps", fps);
        }
    });

    // 3. GetTargetPlayer post hook (BlueprintImplementableEvent; overridden by EnemyBase_C).
    auto lastTarget = std::make_shared<Clock::time_point>();
    qd::hooks::AddPost("/Script/QueensDomain.FS_EnemyCharacterBase:GetTargetPlayer",
                       [lastTarget](qd::hooks::CallInfo& ci) {
                           const auto now = Clock::now();
                           if (*lastTarget != Clock::time_point{} &&
                               std::chrono::duration<double>(now - *lastTarget).count() < kReportSeconds)
                               return;
                           *lastTarget = now;
                           SDK::AActor* target = ci.result ? *static_cast<SDK::AActor**>(ci.result) : nullptr;
                           QD_INFO(kTag, "GetTargetPlayer: enemy {} ({}) -> target {} [fn {}]",
                                   qd::ue::Name(ci.object), qd::ue::ClassName(ci.object),
                                   qd::ue::IsValid(target) ? qd::ue::Name(target) + " (" + qd::ue::ClassName(target) + ")"
                                                           : std::string(ci.result ? "null" : "<no result ptr>"),
                                   qd::ue::PathName(ci.function));
                       });

    // 4. Interact pre hook (does not intercept). First param: AActor* PlayerInteracting.
    qd::hooks::AddPre("/Game/Blueprints/ItemPickup_Base.ItemPickup_Base_C:Interact", [](qd::hooks::CallInfo& ci) {
        SDK::AActor* who = ci.params ? *static_cast<SDK::AActor**>(ci.params) : nullptr;
        QD_INFO(kTag, "Interact: {} by {}", qd::ue::PathName(ci.object),
                qd::ue::IsValid(who) ? qd::ue::Name(who) : std::string("?"));
        return false;
    });
}

struct Registrar {
    Registrar() { qd::core::OnReady(&OnEngineReady); }
} g_registrar;
}  // namespace
