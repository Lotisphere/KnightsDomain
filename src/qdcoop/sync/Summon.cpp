// QD Coop — teleport to the host. See Summon.h.
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"
#include "SDK/QueensDomain_classes.hpp"
#include "SDK/QueensDomain_parameters.hpp"
#include "SDK/FS_PlayerChar_classes.hpp"
#include "SDK/FS_PlayerChar_parameters.hpp"

#include "qdcoop/sync/Summon.h"

#include "qdcoop/core/Core.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/sync/Coop.h"
#include "qdcoop/sync/EnemySync.h"
#include "qdcoop/ui/I18n.h"
#include "qdcoop/ui/Overlay.h"

#include "imgui.h"

#include <atomic>
#include <chrono>
#include <cmath>

namespace qd::sync {
namespace {

constexpr const char* kTag = "summon";
constexpr const char* kPlayerClass = "/Game/Player/FS_PlayerChar.FS_PlayerChar_C";

constexpr uint8_t kHostSlot     = 0;
constexpr float   kSideOffset   = 150.0f;   // cm beside the host
constexpr float   kLift         = 10.0f;    // cm above the capsule's resting height
constexpr float   kMaxMissCm    = 300.0f;   // Teleport_to_Loc landed this far away -> retry with K2_TeleportTo
constexpr double  kHoldMinSec   = 1.0;      // keep the player still at least this long after teleporting ...
constexpr double  kSettleSec    = 0.5;      // ... and until the level manager has been idle this long ...
constexpr double  kHoldMaxSec   = 10.0;     // ... but never longer than this
constexpr double  kDeathWindowSec = 180.0;  // died this recently before a world reload -> "back at your save point"

std::atomic<int>  g_mode{0};   // 0 not in a session, 1 host, 2 client (read by the render thread)
std::atomic<bool> g_busy{false};

using Clock = std::chrono::steady_clock;
double Now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

struct State {
    bool inited = false;
    gamethread::TickerId ticker = 0;
    SDK::UWorld* world = nullptr;
    SDK::UClass* player = nullptr;
    double lastDeadAt = -1e9;
    bool   deathToastPending = false;
    bool   holding = false;
    double holdStart = 0;
    double settleSince = -1;
};
State& S() {
    static State s;
    return s;
}

bool IsFsPlayer(const SDK::UObject* o) {
    auto& s = S();
    if (!ue::IsValid(s.player)) s.player = ue::FindClass(kPlayerClass);
    return o && s.player && o->IsA(s.player);
}

bool IsDeadPlayer(SDK::ACharacter* ch) {
    return IsFsPlayer(ch) && static_cast<SDK::AFS_PlayerChar_C*>(ch)->IsDead;
}

bool IsProcessingLevels(SDK::UWorld* world) {
    SDK::UObject* cdo = SDK::AFS_LevelManager::GetDefaultObj();
    if (!cdo || !world) return false;
    static SDK::UFunction* fn = nullptr;
    if (!ue::IsValid(fn)) fn = ue::FindFunction(cdo, "IsProcessingLevelChanges");
    if (!fn) return false;
    SDK::Params::FS_LevelManager_IsProcessingLevelChanges p{};
    p.WorldContextObject = world;
    ue::CallFunction(cdo, fn, &p);
    return p.ReturnValue;
}

float Dist(const SDK::FVector& a, const SDK::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void EndHold(bool success) {
    auto& s = S();
    if (!s.holding) return;
    s.holding = false;
    g_busy = false;
    SDK::ACharacter* ch = ue::LocalCharacter();
    if (ch && ue::IsValid(ch) && ch->CharacterMovement && ue::IsValid(ch->CharacterMovement))
        ch->CharacterMovement->SetMovementMode(SDK::EMovementMode::MOVE_Falling, 0);   // finds the floor next tick
    if (success) ui::Toast(ui::Tr("summon.done"), ui::ToastKind::Success, 3.0f);
}

void DoSummon() {
    auto& s = S();
    if (!coop::InSession() || coop::IsHost()) {
        ui::Toast(ui::Tr("summon.only_client"), ui::ToastKind::Warning);
        return;
    }
    if (s.holding) return;
    SDK::ACharacter* ch = ue::LocalCharacter();
    if (!ch || !ue::IsValid(ch)) {
        ui::Toast(ui::Tr("summon.no_world"), ui::ToastKind::Warning);
        return;
    }
    if (IsDeadPlayer(ch)) {
        ui::Toast(ui::Tr("summon.dead"), ui::ToastKind::Warning);
        return;
    }
    bool hostDead = false, sameArea = false;
    net::Vec3f feet{};
    float yaw = 0;
    if (!RemotePlayerInfo(kHostSlot, hostDead, sameArea) || !playersync::RemotePose(kHostSlot, feet, yaw)) {
        ui::Toast(ui::Tr("summon.no_pos"), ui::ToastKind::Warning);
        return;
    }
    if (!sameArea) {
        ui::Toast(ui::Tr("summon.other_map"), ui::ToastKind::Warning);
        return;
    }

    float half = 90.0f;
    if (ch->CapsuleComponent && ue::IsValid(ch->CapsuleComponent)) {
        const float hh = ch->CapsuleComponent->GetScaledCapsuleHalfHeight();
        if (hh > 1.0f) half = hh;
    }
    const float rad = (yaw + 90.0f) * 3.14159265f / 180.0f;   // to the host's right, so we don't overlap
    const SDK::FVector dest(feet.x + std::cos(rad) * kSideOffset, feet.y + std::sin(rad) * kSideOffset,
                            feet.z + half + kLift);
    const SDK::FRotator rot(0.0f, yaw, 0.0f);

    // TeleportToLocation(UFS_LocationData*, bool) needs a fast-travel data asset, not a position, so the game's own
    // positional teleport (Teleport_to_Loc: actor location + rotation + control rotation) is used; K2_TeleportTo is
    // the fallback when that BP function is missing or did not land where asked.
    const char* how = nullptr;
    if (IsFsPlayer(ch)) {
        if (SDK::UFunction* fn = ue::FindFunction(ch, "Teleport_to_Loc")) {
            SDK::Params::FS_PlayerChar_C_Teleport_to_Loc p{};
            p.NewLocation = dest;
            p.NewRotation = rot;
            p.UpdateTPLight = false;
            p.TPLightRadius = 0.0f;
            p.TPLightWingCrystalColor = false;
            ue::CallFunction(ch, fn, &p);
            if (Dist(ch->K2_GetActorLocation(), dest) <= kMaxMissCm) how = "Teleport_to_Loc";
        }
    }
    if (!how && ch->K2_TeleportTo(dest, rot)) how = "K2_TeleportTo";
    if (!how) {
        QD_WARN(kTag, "teleport to ({:.0f}, {:.0f}, {:.0f}) failed", dest.X, dest.Y, dest.Z);
        ui::Toast(ui::Tr("summon.failed"), ui::ToastKind::Error);
        return;
    }

    // Hold still while the level manager streams the destination in, so we don't fall through unloaded floors.
    if (ch->CharacterMovement && ue::IsValid(ch->CharacterMovement)) ch->CharacterMovement->DisableMovement();
    s.holding = true;
    s.holdStart = Now();
    s.settleSince = -1;
    g_busy = true;
    QD_INFO(kTag, "teleported next to the host via {} -> ({:.0f}, {:.0f}, {:.0f})", how, dest.X, dest.Y, dest.Z);
    ui::Toast(ui::Tr("summon.in_progress"), ui::ToastKind::Info, 2.5f);
}

void Tick(float) {
    auto& s = S();
    const double now = Now();
    const bool inSession = coop::InSession();
    const bool client = inSession && !coop::IsHost();
    g_mode = !inSession ? 0 : client ? 2 : 1;

    SDK::UWorld* world = ue::World();
    SDK::ACharacter* ch = ue::LocalCharacter();
    const bool hasChar = ch && ue::IsValid(ch);
    const bool dead = hasChar && IsDeadPlayer(ch);
    if (dead) s.lastDeadAt = now;

    if (world != s.world) {
        s.world = world;
        if (s.holding) {   // the old world (and its player) is gone
            s.holding = false;
            g_busy = false;
        }
        if (client && now - s.lastDeadAt < kDeathWindowSec) s.deathToastPending = true;
    }

    // Client died and the game reloaded their own save: tell them how to get back.
    if (s.deathToastPending && hasChar && !dead && world) {
        s.deathToastPending = false;
        s.lastDeadAt = -1e9;
        if (client) ui::Toast(ui::Tr("summon.back_at_save"), ui::ToastKind::Info, 8.0f);
    }

    if (s.holding) {
        if (!hasChar) {
            EndHold(false);
            return;
        }
        if (IsProcessingLevels(world)) s.settleSince = -1;
        else if (s.settleSince < 0) s.settleSince = now;
        const double held = now - s.holdStart;
        const bool settled = s.settleSince >= 0 && now - s.settleSince >= kSettleSec;
        if ((held >= kHoldMinSec && settled) || held >= kHoldMaxSec) {
            if (held >= kHoldMaxSec) QD_WARN(kTag, "level streaming still busy after {:.0f} s; releasing the player", held);
            EndHold(true);
        }
    }
}

void DrawTab() {
    const int mode = g_mode.load();
    if (mode == 2) {
        ImGui::TextWrapped("%s", ui::Tr("summon.desc"));
        const bool busy = g_busy.load();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(ui::Tr("summon.button"))) gamethread::Post([] { DoSummon(); });
        ImGui::EndDisabled();
        if (busy) ImGui::TextDisabled("%s", ui::Tr("summon.busy"));
    } else if (mode == 1) {
        ImGui::TextDisabled("%s", ui::Tr("summon.you_host"));
    } else {
        ImGui::TextDisabled("%s", ui::Tr("summon.solo"));
    }
}

}  // namespace

void SummonInit() {
    auto& s = S();
    if (s.inited) return;
    s.inited = true;
    s.ticker = gamethread::AddTicker("summon", [](float dt) { Tick(dt); });
    ui::AddMenuTab("summon", "tab.summon", DrawTab, 30);   // title = i18n key
    QD_INFO(kTag, "initialized");
}

void SummonShutdown() {
    auto& s = S();
    if (!s.inited) return;
    if (s.holding) EndHold(false);
    if (s.ticker) {
        gamethread::RemoveTicker(s.ticker);
        s.ticker = 0;
    }
    s.inited = false;
}

}  // namespace qd::sync
