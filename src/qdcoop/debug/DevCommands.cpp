// QD Coop — dev command file (test tooling; active only in dev mode: transport "udp" or profile >= 2).
// Write lines into <ModSaved>\devcmd.txt; the file is consumed (deleted) and each line executed on the game thread:
//   equip <ItemDataPath>        give 1 and equip it in hand (e.g. /Game/Items/Weapons/Spear/X_Item.X_Item)
//   give <ItemDataPath> [count]
//   look <slot> [distance_cm]   teleport the local player in front of slot's puppet, facing it (for screenshots)
//   tp <x> <y> <z> [yaw]
//   swing <slot>                make that remote puppet attack now (local only)
// Results go to QDCoop.log with tag [dev].
#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/FS_ItemData_classes.hpp"
#include "SDK/QueensDomain_classes.hpp"
#include "SDK/FS_PlayerChar_parameters.hpp"
#include "SDK/SaveFunctions_parameters.hpp"

#include "qdcoop/core/Config.h"
#include "qdcoop/core/Core.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/net/Protocol.h"
#include "SDK/FS_PlayerChar_classes.hpp"
#include "qdcoop/sync/EnemySync.h"
#include "qdcoop/sync/PoseDriver.h"
#include "qdcoop/sync/Puppet.h"
#include "qdcoop/ui/Overlay.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace qd::playersync {
SDK::AActor* PuppetActor(uint8_t slot);
bool RemotePose(uint8_t slot, net::Vec3f& feet, float& yaw);
void DebugTriggerAttack(uint8_t slot);
}  // namespace qd::playersync

namespace {
constexpr const char* kTag = "dev";

bool DevMode() {
    const auto& c = qd::config::Get();
    return c.transport == "udp" || c.profile >= 2;
}

std::wstring CmdPath() { return qd::paths::ModSaved() + L"\\devcmd.txt"; }

SDK::UObject* LoadItem(const std::string& path) {
    SDK::UObject* item = qd::ue::LoadObject(path);
    if (!item) QD_WARN(kTag, "item not found: {}", path);
    return item;
}

bool CallOn(SDK::UObject* obj, const char* fn, void* params) {
    SDK::UFunction* f = obj ? qd::ue::FindFunction(obj, fn) : nullptr;
    if (!f) { QD_WARN(kTag, "function {} not found on {}", fn, qd::ue::ClassName(obj)); return false; }
    qd::ue::CallFunction(obj, f, params);
    return true;
}

void Give(const std::string& path, int count) {
    SDK::ACharacter* ch = qd::ue::LocalCharacter();
    if (!ch || !LoadItem(path)) return;
    SDK::UClass* lib = qd::ue::FindClass("/Game/Blueprints/SaveSystem/SaveFunctions.SaveFunctions_C");
    SDK::UObject* cdo = lib ? lib->ClassDefaultObject : nullptr;
    if (!cdo) { QD_WARN(kTag, "SaveFunctions_C not loaded"); return; }
    SDK::Params::SaveFunctions_C_AddItemToInventory p{};
    p.Item.WeakPtr.ObjectIndex = 0;
    p.Item.WeakPtr.ObjectSerialNumber = 0;
    p.Item.TagAtLastTest = -1;
    p.Item.ObjectID.AssetPathName = qd::ue::MakeName(path);   // full "Package.Object" path, no sub-path
    p.Count = count;
    p.__WorldContext = ch;
    if (CallOn(cdo, "AddItemToInventory", &p)) QD_INFO(kTag, "gave {} x{}", path, count);
}

void Equip(const std::string& path) {
    Give(path, 1);
    SDK::ACharacter* ch = qd::ue::LocalCharacter();
    SDK::UObject* item = LoadItem(path);
    if (!ch || !item) return;
    auto* data = static_cast<SDK::UFS_ItemData_C*>(item);
    // A fresh save has no weapon slots filled and weapons put away: slot it, auto-equip, set in hand, take it out.
    SDK::Params::FS_PlayerChar_C_EquipItem slotP{};
    slotP.Item = data;
    slotP.SlotIndex = 0;
    CallOn(ch, "EquipItem", &slotP);
    SDK::Params::FS_PlayerChar_C_AutoEquipItem autoP{};
    autoP.Item = data;
    CallOn(ch, "AutoEquipItem", &autoP);
    SDK::Params::FS_PlayerChar_C_EquipItemWeapon p{};
    p.Item = data;
    CallOn(ch, "EquipItemWeapon", &p);
    CallOn(ch, "TakeOutWeapons", nullptr);
    QD_INFO(kTag, "equipped {}", path);
}

void Look(uint8_t slot, float dist, float angleDeg) {
    SDK::ACharacter* ch = qd::ue::LocalCharacter();
    SDK::APlayerController* pc = qd::ue::LocalController();
    qd::net::Vec3f feet; float yaw = 0;
    if (!ch || !pc || !qd::playersync::RemotePose(slot, feet, yaw)) { QD_WARN(kTag, "look: no pose for slot {}", slot); return; }
    // Stand `dist` in front of the puppet (along its facing), look back at its chest.
    const float rad = (yaw + angleDeg) * 3.14159265f / 180.f;   // 0 = in front of the puppet, 90 = its right side
    SDK::FVector at(feet.x + std::cos(rad) * dist, feet.y + std::sin(rad) * dist, feet.z + 100.f);
    ch->K2_SetActorLocation(at, false, nullptr, true);
    const float dx = feet.x - at.X, dy = feet.y - at.Y, dz = (feet.z + 110.f) - (at.Z + 60.f);
    SDK::FRotator r{};
    r.Yaw = std::atan2(dy, dx) * 180.f / 3.14159265f;
    r.Pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 180.f / 3.14159265f;
    pc->SetControlRotation(r);
    QD_INFO(kTag, "look at slot {} from {:.0f} cm", slot, dist);
}

// Teleport 500 cm in front of the nearest living enemy, facing it.
void GotoEnemy() {
    SDK::ACharacter* ch = qd::ue::LocalCharacter();
    SDK::APlayerController* pc = qd::ue::LocalController();
    SDK::UWorld* world = qd::ue::World();
    if (!ch || !pc || !world) return;
    const SDK::FVector me = ch->K2_GetActorLocation();
    SDK::AFS_EnemyCharacterBase* best = nullptr; float bestD = 1e30f;
    auto& objs = SDK::UObject::GObjects;
    for (int i = 0; i < objs->Num(); ++i) {
        SDK::UObject* o = objs->GetByIndex(i);
        if (!o || !o->IsA(SDK::AFS_EnemyCharacterBase::StaticClass()) || o->IsDefaultObject()) continue;
        auto* e = static_cast<SDK::AFS_EnemyCharacterBase*>(o);
        if (!qd::ue::IsValid(e) || e->bDead || e->bSpawnerPreviewInstance) continue;
        const SDK::FVector p = e->K2_GetActorLocation();
        const float d = (p.X - me.X) * (p.X - me.X) + (p.Y - me.Y) * (p.Y - me.Y) + (p.Z - me.Z) * (p.Z - me.Z);
        if (d < bestD) { bestD = d; best = e; }
    }
    if (!best) { QD_WARN(kTag, "enemy: none found"); return; }
    const SDK::FVector e = best->K2_GetActorLocation();
    const float dx = me.X - e.X, dy = me.Y - e.Y, len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
    const SDK::FVector at(e.X + dx / len * 500.f, e.Y + dy / len * 500.f, e.Z + 50.f);
    ch->K2_SetActorLocation(at, false, nullptr, true);
    SDK::FRotator r{}; r.Yaw = std::atan2(e.Y - at.Y, e.X - at.X) * 180.f / 3.14159265f; pc->SetControlRotation(r);
    QD_INFO(kTag, "enemy: moved next to {} ({:.0f} m away before)", qd::ue::Name(best), std::sqrt(bestD) / 100.f);
}

// god mode for boss tests: tops the local player's health up every poll (0.1 s)
bool g_god = false;
void GodTick() {
    if (!g_god) return;
    SDK::ACharacter* ch = qd::ue::LocalCharacter();
    if (!ch) return;
    auto* pc = static_cast<SDK::AFS_PlayerChar_C*>(ch);
    if (pc->Health < 1.0e5f) pc->Health = 1.0e6f;
}

void Run(const std::string& line) {
    std::istringstream in(line);
    std::string cmd; in >> cmd;
    if (cmd.empty() || cmd[0] == '#') return;
    if (cmd == "help") {
        const char* lines[] = {
            "equip <物品路径>            给并装备武器",
            "give <物品路径> [数量]      给物品",
            "look <槽位> [距离] [角度]   传送到某个队友面前（0 = 房主）",
            "enemy                       传送到最近的敌人面前",
            "tp <x> <y> <z> [yaw]        传送到坐标",
            "swing <槽位>                让那个队友的骑士挥一下",
            "freeze <0-1> / variant <0-2>  定格攻击动作 / 指定连击段（-1 取消）",
            "tune <类别> <段> <幅度> <俯仰> <偏航> <翻滚>  调攻击手腕",
            "fit <x> <y> <z> <俯仰> <偏航> <翻滚> <缩放>  调本地武器握持",
            "call <函数名>               调用本地角色的无参蓝图函数",
        };
        for (const char* l : lines) qd::ui::ConsolePrint(l);
        return;
    }
    QD_INFO(kTag, "> {}", line);
    if (cmd == "equip") { std::string p; in >> p; Equip(p); }
    else if (cmd == "give") { std::string p; int n = 1; in >> p >> n; Give(p, n < 1 ? 1 : n); }
    else if (cmd == "look") { int s = 0; float d = 300, ang = 0; in >> s >> d >> ang; Look(static_cast<uint8_t>(s), d, ang); }
    else if (cmd == "freeze") { float t = -1; in >> t; qd::motion::DebugFreeze(t); QD_INFO(kTag, "freeze {:.2f}", t); }
    else if (cmd == "variant") { int v = -1; in >> v; qd::motion::DebugForceVariant(v); QD_INFO(kTag, "variant {}", v); }
    else if (cmd == "call") {   // call <BP function with no parameters> on the local character
        std::string fn; in >> fn;
        if (auto* ch = qd::ue::LocalCharacter()) { if (CallOn(ch, fn.c_str(), nullptr)) QD_INFO(kTag, "called {}", fn); }
    }
    else if (cmd == "tune") {   // tune <ClassName> <variant 0-2> <amp> <handPitch> <handYaw> <handRoll>
        std::string cls; int v = 0; float amp = 1, p = 0, y = 0, r = 0; in >> cls >> v >> amp >> p >> y >> r;
        for (int i = 0; i <= static_cast<int>(qd::motion::WeaponClass::Unknown); ++i) {
            const auto wc = static_cast<qd::motion::WeaponClass>(i);
            if (cls != qd::motion::WeaponClassName(wc)) continue;
            qd::motion::ClassTune t = qd::motion::GetClassTune(wc);
            t.amp = amp; t.hand[v % 3][0] = p; t.hand[v % 3][1] = y; t.hand[v % 3][2] = r;
            qd::motion::SetClassTune(wc, t);
            QD_INFO(kTag, "tune {} v{}: amp {:.2f} hand ({:.0f}, {:.0f}, {:.0f})", cls, v, amp, p, y, r);
        }
    }
    else if (cmd == "fit") {   // fit <x> <y> <z> <pitch> <yaw> <roll> <scale> : grip of the LOCAL player's current weapon
        qd::sync::WeaponFit f; in >> f.loc[0] >> f.loc[1] >> f.loc[2] >> f.rot[0] >> f.rot[1] >> f.rot[2] >> f.scale;
        qd::sync::DebugSetWeaponFit(f);
        qd::sync::DebugLogWeaponFitCpp();
    }
    else if (cmd == "enemy") { GotoEnemy(); }
    else if (cmd == "open") {   // open <map name>, e.g. open BirdsTestZone
        std::string map; in >> map;
        if (auto* w = qd::ue::World()) {
            QD_INFO(kTag, "open level {}", map);
            SDK::UGameplayStatics::OpenLevel(w, qd::ue::MakeName(map), true, SDK::FString());
        }
    }
    else if (cmd == "god") { int v = 1; in >> v; g_god = v != 0; QD_INFO(kTag, "god {}", g_god ? 1 : 0); }
    else if (cmd == "where") {
        if (auto* ch = qd::ue::LocalCharacter()) {
            const SDK::FVector p = ch->K2_GetActorLocation();
            QD_INFO(kTag, "where: ({:.0f},{:.0f},{:.0f}) world {} health {:.0f}", p.X, p.Y, p.Z, qd::ue::Name(qd::ue::World()),
                    static_cast<SDK::AFS_PlayerChar_C*>(ch)->Health);
        } else QD_INFO(kTag, "where: no local character (world {})", qd::ue::World() ? qd::ue::Name(qd::ue::World()) : std::string("-"));
    }
    else if (cmd == "elist") { std::string f; in >> f; qd::sync::DebugEnemyList(f); }
    else if (cmd == "egoto") { std::string f; float d = 900; in >> f >> d; qd::sync::DebugEnemyGoto(f, d); }
    else if (cmd == "ehp") { std::string f; float fr = 0.5f; in >> f >> fr; qd::sync::DebugEnemyHp(f, fr); }
    else if (cmd == "ehit") { std::string f; float dmg = 10; in >> f >> dmg; qd::sync::DebugEnemyHit(f, dmg); }
    else if (cmd == "ewake") { std::string f; in >> f; qd::sync::DebugEnemyWake(f); }
    else if (cmd == "einteract") { std::string f; in >> f; qd::sync::DebugEnemyInteract(f); }
    else if (cmd == "rva") {   // rva <Class path> : log ExecFunction RVAs of every UFunction of that class
        std::string cls; in >> cls;
        SDK::UClass* c = qd::ue::FindClass(cls);
        if (!c) { QD_WARN(kTag, "rva: class {} not found", cls); return; }
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        int n = 0;
        for (SDK::UField* f = c->Children; f; f = f->Next) {
            if (!f->IsA(SDK::UFunction::StaticClass())) continue;
            auto* fn = static_cast<SDK::UFunction*>(f);
            const auto addr = reinterpret_cast<uintptr_t>(fn->ExecFunction);
            QD_INFO(kTag, "rva {} {:08X} flags {:08X}", qd::ue::Name(fn), addr ? addr - base : 0, static_cast<uint32_t>(fn->FunctionFlags));
            ++n;
        }
        QD_INFO(kTag, "rva: {} functions in {}", n, cls);
    }
    else if (cmd == "swing") { int sl = 1; in >> sl; qd::playersync::DebugTriggerAttack(static_cast<uint8_t>(sl)); }
    else if (cmd == "tp") {
        float x, y, z, yaw = 0; in >> x >> y >> z >> yaw;
        if (auto* ch = qd::ue::LocalCharacter()) ch->K2_SetActorLocation(SDK::FVector(x, y, z), false, nullptr, true);
        if (auto* pc = qd::ue::LocalController()) { SDK::FRotator r{}; r.Yaw = yaw; pc->SetControlRotation(r); }
    }
    else QD_WARN(kTag, "unknown command '{}'", cmd);
}

void Poll(float dt) {
    static float acc = 0;
    acc += dt;
    if (acc < 0.1f) return;
    acc = 0;
    GodTick();
    const std::wstring path = CmdPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        for (std::string l; std::getline(f, l);) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }
    DeleteFileW(path.c_str());
    for (auto& l : lines) Run(l);
}

struct Registrar {
    Registrar() {
        qd::core::OnReady([] {
            qd::ui::SetConsoleHandler([](const std::string& line) { Run(line); });   // ~ console, any mode
            if (!DevMode()) return;
            qd::gamethread::AddTicker("devcmd", &Poll);
            { const std::wstring p = CmdPath(); QD_INFO(kTag, "dev command file: {}", std::string(p.begin(), p.end())); }
        });
    }
} g_registrar;
}  // namespace
