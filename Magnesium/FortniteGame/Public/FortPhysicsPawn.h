#pragma once
#include "../../pch.h"
#include "FortInventory.h"
#include "../../../Magnesium/Erbium/Public/Configuration.h"

class AFortPhysicsPawn : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortPhysicsPawn);

    static void ServerMove(UObject*, FFrame&);

    InitHooks;
};

struct FWeaponSeatDefinition
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FWeaponSeatDefinition);

    DEFINE_STRUCT_PROP(SeatIndex, int32);
    DEFINE_STRUCT_PROP(VehicleWeapon, UFortWeaponItemDefinition*);
};

struct FAthenaCarPlayerSlot
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FAthenaCarPlayerSlot);

    DEFINE_STRUCT_NEWOBJ_PROP(Player, AFortPlayerPawnAthena);
};

class UFortVehicleSeatWeaponComponent : public UActorComponent
{
public:
    UCLASS_COMMON_MEMBERS(UFortVehicleSeatWeaponComponent);

    DEFINE_PROP(WeaponSeatDefinitions, TArray<FWeaponSeatDefinition>);
};

class UFortVehicleSeatComponent : public UActorComponent
{
public:
    UCLASS_COMMON_MEMBERS(UFortVehicleSeatComponent);

    DEFINE_PROP(PlayerSlots, TArray<FAthenaCarPlayerSlot>);

    int32 FindSeatIndex(AFortPlayerPawnAthena* Pawn)
    {
        for (int i = 0; i < PlayerSlots.Num(); i++)
        {
            auto& PlayerSlot = PlayerSlots.Get(i, FAthenaCarPlayerSlot::Size());

            if (PlayerSlot.Player == Pawn)
                return i;
        }

        return -1;
    }
};

class UPrimitiveComponent : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UPrimitiveComponent);

    DEFINE_BITFIELD_PROP(bComponentToWorldUpdated);

    DEFINE_FUNC(CanCharacterStepUp, bool);
    DEFINE_FUNC(K2_SetWorldLocationAndRotation, void);
    DEFINE_FUNC(K2_SetWorldTransform, void);
    DEFINE_FUNC(SetPhysicsLinearVelocity, void);
    DEFINE_FUNC(SetPhysicsAngularVelocityInDegrees, void);
    DEFINE_FUNC(SetPhysicsAngularVelocityInRadians, void);
};

enum class ECollisionEnabled : uint8
{
    NoCollision = 0,
    QueryOnly = 1,
    PhysicsOnly = 2,
    QueryAndPhysics = 3,
	ECollisionEnabled_MAX = 4
};

enum class ECollisionResponse : uint8
{
    ECR_Ignore = 0,
    ECR_Overlap = 1,
    ECR_Block = 2,
	ECollisionResponse_MAX = 3
};

enum class EWalkableSlopeBehavior : uint8
{
    WalkableSlope_Default = 0,
    WalkableSlope_Increase = 1,
    WalkableSlope_Decrease = 2,
    WalkableSlope_Unwalkable = 3,
	WalkableSlope_Max = 4
};

struct FWalkableSlopeOverride
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FWalkableSlopeOverride);

    DEFINE_STRUCT_PROP(WalkableSlopeBehavior, EWalkableSlopeBehavior)
    DEFINE_STRUCT_PROP(WalkableSlopeAngle, float);
};

class AFortAthenaVehicle : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaVehicle);

    DEFINE_PROP(HealthSet, UFortHealthSet*);

    DEFINE_FUNC(FindSeatIndex, int32);
    DEFINE_FUNC(OnRep_HealthSet, void);
    DEFINE_FUNC(DestroyVehicle, void);
};

class AFortAthenaSKPushCannon : public AFortAthenaVehicle
{
public:

    UCLASS_COMMON_MEMBERS(AFortAthenaSKPushCannon);

	DEFINE_FUNC(MultiCastPushCannonLaunchedPlayer, void);
    DEFINE_FUNC(OnLaunchPawn, void);
    DEFINE_FUNC(OnPreLaunchPawn, void);
    DEFINE_FUNC(GetPawnAtSeat, AFortPlayerPawnAthena*);
};

class AFortCharacterVehicle : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortCharacterVehicle);

    DEFINE_PROP(OverrideAbilitySystemComponent, UAbilitySystemComponent*);
};

struct FNetTowhookAttachState
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FNetTowhookAttachState);

    DEFINE_STRUCT_PROP(Component, UActorComponent*);
    DEFINE_STRUCT_PROP(LocalLocation, FVector);
    DEFINE_STRUCT_PROP(LocalNormal, FVector);
};

class IFortVehicleInterface : IInterface
{
public:
    UCLASS_COMMON_MEMBERS(IFortVehicleInterface);
};

struct FMountedWeaponInfoRepped
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FMountedWeaponInfoRepped);
    uint8_t Padding[0x48];

    DEFINE_STRUCT_PROP(HostVehicleCached, TScriptInterface<IFortVehicleInterface>);
    DEFINE_STRUCT_PROP(HostVehicleCachedActor, AActor*);
    DEFINE_STRUCT_PROP(HostVehicleSeatIndexCached, int32);
};

class AFortOctopusVehicle : public AFortPhysicsPawn
{
public:
    UCLASS_COMMON_MEMBERS(AFortOctopusVehicle);

    DEFINE_PROP(NetTowhookAimDir, FVector);
    DEFINE_PROP(ReplicatedAttachState, FNetTowhookAttachState);
    DEFINE_PROP(LocalAttachState, FNetTowhookAttachState);

    DEFINE_FUNC(OnRep_NetTowhookAimDir, void);
    DEFINE_FUNC(OnRep_ReplicatedAttachState, void);
    DEFINE_FUNC(BreakTowhook, void);

    static void ServerUpdateTowhook(UObject*, FFrame&);
};

class AFortSpaghettiVehicle : public AFortPhysicsPawn
{
public:
    UCLASS_COMMON_MEMBERS(AFortSpaghettiVehicle);

    DEFINE_PROP(NetTowhookAimDir, FVector);

    DEFINE_FUNC(OnRep_NetTowhookAimDir, void);

    static void ServerUpdateTowhook(UObject*, FFrame&);
};

class AFortDagwoodVehicle : public AFortAthenaVehicle
{
public:
    UCLASS_COMMON_MEMBERS(AFortDagwoodVehicle);

    DEFINE_FUNC(SetFuel, float);
};

class AFortMountedCannon : public AFortAthenaVehicle
{
public:
    UCLASS_COMMON_MEMBERS(AFortMountedCannon);

    DEFINE_FUNC(OnLaunchPawn, void);

    AFortPlayerPawnAthena* GetPawnAtSeat(int32 SeatIndex)
    {
        auto* SeatComponent = (UFortVehicleSeatComponent*)this->GetComponentByClass(UFortVehicleSeatComponent::StaticClass());

        if (!SeatComponent)
            return nullptr;

        if (SeatIndex < 0 || SeatIndex >= SeatComponent->PlayerSlots.Num())
            return nullptr;

        auto& PlayerSlot = SeatComponent->PlayerSlots.Get(SeatIndex, FAthenaCarPlayerSlot::Size());
        return PlayerSlot.Player;
    }
};

class AFortWeaponRangedMountedCannon : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortWeaponRangedMountedCannon);

    static void ServerFireActorInCannon(UObject* Context, FFrame& Stack);
};
