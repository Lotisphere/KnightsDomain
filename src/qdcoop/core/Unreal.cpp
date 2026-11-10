#include "qdcoop/core/Unreal.h"

#include "qdcoop/core/Log.h"
#include "qdcoop/core/Util.h"

#include <Windows.h>

#include <cmath>

#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"
#include "SDK/CoreUObject_structs.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/Engine_structs.hpp"

namespace qd::ue {
namespace {
constexpr char kTag[] = "ue";

// EInternalObjectFlags (UE 4.27, FUObjectItem::Flags at +0x08)
constexpr int32_t kInternalUnreachable = 1 << 28;
constexpr int32_t kInternalPendingKill = 1 << 29;
constexpr size_t  kObjectItemFlagsOffset = 0x08;

// EObjectFlags RF_BeginDestroyed | RF_FinishDestroyed
constexpr uint32_t kDestroyedFlags = 0x00008000u | 0x00010000u;

bool HasCastFlag(const SDK::UObject* obj, SDK::EClassCastFlags flag) {
    return obj && obj->Class && (static_cast<uint64_t>(obj->Class->CastFlags) & static_cast<uint64_t>(flag)) != 0;
}

bool IsPackage(const SDK::UObject* obj) { return HasCastFlag(obj, SDK::EClassCastFlags::Package); }

// No C++ objects with destructors in this frame (required for __try).
bool IsValidRaw(const SDK::UObject* obj) {
    __try {
        const int32_t index = obj->Index;
        SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
        if (index < 0 || index >= arr->NumElements) return false;
        const int32_t chunk = index / SDK::TUObjectArray::ElementsPerChunk;
        const int32_t within = index % SDK::TUObjectArray::ElementsPerChunk;
        if (chunk >= arr->NumChunks) return false;
        SDK::FUObjectItem* chunkPtr = arr->GetDecrytedObjPtr()[chunk];
        if (!chunkPtr) return false;
        const SDK::FUObjectItem* item = &chunkPtr[within];
        if (item->Object != obj) return false;
        const int32_t internalFlags =
            *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(item) + kObjectItemFlagsOffset);
        if (internalFlags & (kInternalPendingKill | kInternalUnreachable)) return false;
        if (static_cast<uint32_t>(obj->Flags) & kDestroyedFlags) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Splits "Pkg.Asset:Sub.Path" into asset path + sub-object path (FSoftObjectPath convention).
void SplitSoftPath(std::string_view full, std::string_view& asset, std::string_view& sub) {
    const size_t colon = full.find(':');
    if (colon == std::string_view::npos) {
        asset = full;
        sub = {};
    } else {
        asset = full.substr(0, colon);
        sub = full.substr(colon + 1);
    }
}

// Builds a TSoftObjectPtr for `fullPath`. `subHolder` must outlive every use of the returned value
// (the SDK FString does not own its buffer; the engine deep-copies it when the exec thunk reads the param).
SDK::TSoftObjectPtr<SDK::UObject> MakeSoftPtr(std::string_view fullPath, std::wstring& subHolder) {
    std::string_view asset, sub;
    SplitSoftPath(fullPath, asset, sub);
    SDK::TSoftObjectPtr<SDK::UObject> soft{};
    soft.WeakPtr.ObjectIndex = 0;
    soft.WeakPtr.ObjectSerialNumber = 0;
    soft.TagAtLastTest = -1;  // never equals FSoftObjectPath::CurrentTag -> forces a resolve
    soft.ObjectID.AssetPathName = MakeName(asset);
    subHolder = util::Utf8ToWide(sub);
    if (!subHolder.empty()) soft.ObjectID.SubPathString = SDK::FString(subHolder.c_str());
    return soft;
}

SDK::UClass* PlayerCharacterClass() {
    static SDK::UClass* cached = nullptr;
    if (cached && IsValid(cached)) return cached;
    cached = FindClass("/Script/QueensDomain.FS_PlayerCharacter");
    return cached;
}

// FRotator::Quaternion() from UE 4.27 (degrees -> quaternion).
SDK::FQuat RotatorToQuat(const SDK::FRotator& r) {
    constexpr float kDegToRadHalf = 3.14159265358979323846f / 180.f / 2.f;
    const float sp = std::sin(r.Pitch * kDegToRadHalf), cp = std::cos(r.Pitch * kDegToRadHalf);
    const float sy = std::sin(r.Yaw * kDegToRadHalf), cy = std::cos(r.Yaw * kDegToRadHalf);
    const float sr = std::sin(r.Roll * kDegToRadHalf), cr = std::cos(r.Roll * kDegToRadHalf);
    SDK::FQuat q{};
    q.X = cr * sp * sy - sr * cp * cy;
    q.Y = -cr * sp * cy - sr * cp * sy;
    q.Z = cr * cp * sy - sr * sp * cy;
    q.W = cr * cp * cy + sr * sp * sy;
    return q;
}
}  // namespace

SDK::UWorld* World() {
    // GWorld is NOT stable in this game. FB_RenderWorlds renders the compass into its own UWorld
    // ("CompassWidget_RWM_*") and swaps GWorld to it every few frames, so "world changed" fired ~8x/s and puppets
    // were respawned endlessly. The game viewport's world is the real gameplay world.
    if (SDK::UEngine* engine = SDK::UEngine::GetEngine()) {
        if (SDK::UGameViewportClient* vp = engine->GameViewport; IsValid(vp)) {
            if (SDK::UWorld* w = vp->World; IsValid(w)) return w;
        }
    }
    SDK::UWorld* w = SDK::UWorld::GetWorld();  // fallback: GWorld (Offsets::GWorld)
    return IsValid(w) ? w : nullptr;
}

SDK::APlayerController* LocalController() {
    SDK::UWorld* w = World();
    if (!w) return nullptr;
    SDK::UGameInstance* gi = w->OwningGameInstance;
    if (!IsValid(gi)) return nullptr;
    const auto& players = gi->LocalPlayers;
    if (players.Num() <= 0 || !players.GetDataPtr()) return nullptr;
    SDK::ULocalPlayer* lp = players.GetDataPtr()[0];
    if (!IsValid(lp)) return nullptr;
    SDK::APlayerController* pc = lp->PlayerController;
    return IsValid(pc) ? pc : nullptr;
}

SDK::ACharacter* LocalCharacter() {
    SDK::APlayerController* pc = LocalController();
    if (!pc) return nullptr;
    SDK::ACharacter* ch = pc->Character;
    if (!IsValid(ch)) return nullptr;
    SDK::UClass* cls = PlayerCharacterClass();
    if (!cls) return nullptr;
    return ch->IsA(cls) ? ch : nullptr;
}

bool IsValid(const SDK::UObject* obj) { return obj && IsValidRaw(obj); }

std::string Name(const SDK::UObject* obj) { return obj ? obj->Name.GetRawString() : std::string("None"); }

// Same rule as UObjectBaseUtility::GetPathName: ':' after an outer that is not a package but whose own outer is.
std::string PathName(const SDK::UObject* obj) {
    if (!obj) return "None";
    const SDK::UObject* chain[64];
    int n = 0;
    for (const SDK::UObject* o = obj; o && n < 64; o = o->Outer) chain[n++] = o;
    std::string out;
    out.reserve(128);
    for (int i = n - 1; i >= 0; --i) {
        if (i != n - 1) {
            const SDK::UObject* outer = chain[i + 1];
            const bool subobject = !IsPackage(outer) && outer->Outer && IsPackage(outer->Outer);
            out += subobject ? ':' : '.';
        }
        out += chain[i]->Name.GetRawString();
    }
    return out;
}

std::string ClassName(const SDK::UObject* obj) {
    return (obj && obj->Class) ? Name(obj->Class) : std::string("None");
}

std::string ToString(const SDK::FName& name) { return name.GetRawString(); }

SDK::FName MakeName(std::string_view s) {
    if (s.empty()) return SDK::FName();
    const std::wstring w = util::Utf8ToWide(s);
    return SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(w.c_str()));
}

uint64_t Fnv1a64(std::string_view s) {
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// Fast lookup without loading: KismetSystemLibrary::Conv_SoftObjectReferenceToObject -> TSoftObjectPtr::Get()
// -> FSoftObjectPath::ResolveObject() -> StaticFindObject (hash lookup; handles both '.' and ':' delimiters).
SDK::UObject* FindObject(std::string_view fullPath) {
    if (fullPath.empty()) return nullptr;
    std::wstring subHolder;
    const SDK::TSoftObjectPtr<SDK::UObject> soft = MakeSoftPtr(fullPath, subHolder);
    if (soft.ObjectID.AssetPathName.IsNone()) return nullptr;
    SDK::UObject* obj = SDK::UKismetSystemLibrary::Conv_SoftObjectReferenceToObject(soft);
    return IsValid(obj) ? obj : nullptr;
}

SDK::UClass* FindClass(std::string_view fullPath) {
    SDK::UObject* obj = FindObject(fullPath);
    return HasCastFlag(obj, SDK::EClassCastFlags::Class) ? static_cast<SDK::UClass*>(obj) : nullptr;
}

SDK::UFunction* FindFunction(const SDK::UObject* obj, std::string_view funcName) {
    if (!IsValid(obj) || !obj->Class || funcName.empty()) return nullptr;
    const SDK::FName name = MakeName(funcName);
    if (name.IsNone()) return nullptr;
    return obj->Class->GetFunction(name);  // walks the SuperStruct chain
}

void CallFunction(SDK::UObject* obj, SDK::UFunction* fn, void* params) {
    if (!obj || !fn) return;
    obj->ProcessEvent(fn, params);
}

SDK::AActor* SpawnActor(SDK::UClass* cls, const SDK::FVector& loc, const SDK::FRotator& rot) {
    SDK::UWorld* w = World();
    if (!w || !IsValid(cls)) return nullptr;
    if (!HasCastFlag(cls, SDK::EClassCastFlags::Class)) return nullptr;
    SDK::FTransform t{};
    t.Rotation = RotatorToQuat(rot);
    t.Translation.X = loc.X;
    t.Translation.Y = loc.Y;
    t.Translation.Z = loc.Z;
    t.Scale3D.X = t.Scale3D.Y = t.Scale3D.Z = 1.f;
    SDK::AActor* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        w, cls, t, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr);
    if (!actor) {
        QD_WARN(kTag, "BeginDeferredActorSpawnFromClass failed for {}", PathName(cls));
        return nullptr;
    }
    return SDK::UGameplayStatics::FinishSpawningActor(actor, t);
}

// Method: KismetSystemLibrary::LoadAsset_Blocking(TSoftObjectPtr) == TSoftObjectPtr::LoadSynchronous()
// -> FSoftObjectPath::TryLoad() -> LoadObject<UObject>. Already-loaded objects are returned via FindObject first.
void SeedGameRand(uint32_t seed) {
    using SrandFn = void(__cdecl*)(unsigned int);
    static SrandFn fn = [] {
        HMODULE m = GetModuleHandleW(L"ucrtbase.dll");
        return m ? reinterpret_cast<SrandFn>(GetProcAddress(m, "srand")) : nullptr;
    }();
    if (fn) fn(seed);
}

SDK::UObject* LoadObject(std::string_view fullPath) {
    if (SDK::UObject* found = FindObject(fullPath)) return found;
    std::wstring subHolder;
    const SDK::TSoftObjectPtr<SDK::UObject> soft = MakeSoftPtr(fullPath, subHolder);
    if (soft.ObjectID.AssetPathName.IsNone()) return nullptr;
    SDK::UObject* obj = SDK::UKismetSystemLibrary::LoadAsset_Blocking(soft);
    if (!IsValid(obj)) {
        QD_WARN(kTag, "LoadObject failed: {}", fullPath);
        return nullptr;
    }
    return obj;
}

}  // namespace qd::ue
