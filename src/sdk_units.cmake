# SDK translation units compiled into QDCoop.dll.
# Rule: when you call an SDK function whose body lives in SDK/<Package>_functions.cpp, add that file here.
#   Basic.cpp                 - Basic.hpp helpers (FindClassByName, FWeakObjectPtr, GetStaticName, ...)
#   CoreUObject_functions.cpp - UObject::FindObject*/GetName/IsA, UStruct::IsSubclassOf, UClass::GetFunction
#   Engine_functions.cpp      - UEngine::GetEngine, UWorld::GetWorld, UGameplayStatics, UKismetSystemLibrary,
#                               UKismetStringLibrary (also required by Basic.cpp), AActor::K2_GetActorLocation
set(QD_SDK_UNITS
    ${QD_SDK}/SDK/Basic.cpp
    ${QD_SDK}/SDK/CoreUObject_functions.cpp
    ${QD_SDK}/SDK/Engine_functions.cpp
)
# sync/ui: needs nothing beyond the three units above. Engine_functions.cpp covers AActor (AddComponentByClass,
# K2_SetActorLocationAndRotation, ...), USceneComponent/UPrimitiveComponent/USkinnedMeshComponent/USkeletalMeshComponent/
# UStaticMeshComponent setters, UCapsuleComponent, APlayerController::ProjectWorldLocationToScreen, APlayerCameraManager.
# FS_PlayerChar_C::GetMaxHP and FS_GameInstance_C::GetActiveSaveSlotIndex are called through ue::CallFunction with the
# SDK/*_parameters.hpp structs, so FS_PlayerChar_functions.cpp / FS_GameInstance_functions.cpp are NOT needed.
# sync/EnemySync, Rules, Summon: nothing new. Direct SDK calls are all Engine (UGameplayStatics::IsGamePaused/
# SetGamePaused, AActor::K2_*, ACharacter::PlayAnimMontage, UCharacterMovementComponent::DisableMovement/SetMovementMode,
# UCapsuleComponent) or Basic/CoreUObject (FName::GetRawString, UObject::IsA, GetDefaultObj). Every game/plugin
# function (EnemyBase_C, EnemySpawner_C, FS_PlayerChar_C, FS_EnemyCharacterBase, FS_LevelManager,
# FB_DamageAttributeUtilities, BlueprintGameplayTagLibrary, GameplayStatics::GetAllActorsOfClass) goes through
# ue::FindFunction + ue::CallFunction with the matching SDK/*_parameters.hpp struct.
# weapon on puppets: nothing new either. Only fields of FS_PlayerChar_C / WeaponScriptBase_C / FS_ItemData_C are
# read (+ their inline StaticClass()); AActor::K2_GetComponentsByClass/K2_GetRootComponent, UActorComponent::
# SetComponentTickEnabled, USceneComponent::SetMobility/K2_AttachToComponent/K2_SetRelativeLocationAndRotation/
# SetRelativeScale3D/SetVisibility and the UPrimitiveComponent setters are in Engine_functions.cpp; UStruct::IsSubclassOf
# is in CoreUObject_functions.cpp; FWeakObjectPtr::Get (TSoftClassPtr::Get) is in Basic.cpp.
