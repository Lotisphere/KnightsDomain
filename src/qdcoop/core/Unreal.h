// QD Coop — small helpers over the game SDK headers. Game thread only unless noted.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace SDK {
class UObject; class UClass; class UFunction; class UWorld; class APlayerController; class ACharacter; class AActor;
// FName is declared `class FName` in the SDK headers (Basic.hpp). Forward-declaring it as `struct` changes
// MSVC name mangling (V vs U) depending on which declaration a TU sees first -> C4099 + LNK2019. Keep it `class`.
struct FVector; struct FRotator; class FName;
}

namespace qd::ue {

SDK::UWorld*            World();
SDK::APlayerController* LocalController();
SDK::ACharacter*        LocalCharacter();      // FS_PlayerChar_C instance or nullptr (title screen, loading)

bool        IsValid(const SDK::UObject* obj);   // non-null, still in GObjects at the same index, not pending kill
std::string Name(const SDK::UObject* obj);
std::string PathName(const SDK::UObject* obj);  // "/Game/Maps/100_GR.100_GR:PersistentLevel.EnemySpawner_12"
std::string ClassName(const SDK::UObject* obj);
std::string ToString(const SDK::FName& name);
SDK::FName  MakeName(std::string_view s);

uint64_t Fnv1a64(std::string_view s);           // thread-safe

SDK::UObject*   FindObject(std::string_view fullPath);                       // by path name
SDK::UClass*    FindClass(std::string_view fullPath);                        // e.g. "/Game/Enemies/EnemyBase.EnemyBase_C"
SDK::UFunction* FindFunction(const SDK::UObject* obj, std::string_view funcName);
// Generic reflected call through ProcessEvent; params must match the function's param struct layout.
void CallFunction(SDK::UObject* obj, SDK::UFunction* fn, void* params);

// Deferred spawn + finish (UGameplayStatics::BeginDeferredActorSpawnFromClass / FinishSpawningActor), no collision checks.
SDK::AActor* SpawnActor(SDK::UClass* cls, const SDK::FVector& loc, const SDK::FRotator& rot);

// Seeds the GAME's C runtime rand() (ucrtbase; FMath::Rand / FRand and every Kismet Random* node use it). Game thread
// only (UCRT keeps rand state per thread). No-op if ucrtbase is not loaded.
void SeedGameRand(uint32_t seed);

// Loads a UObject by path (StaticLoadObject via UKismetSystemLibrary soft refs or LoadObject equivalent). May hitch.
SDK::UObject* LoadObject(std::string_view fullPath);

}  // namespace qd::ue
