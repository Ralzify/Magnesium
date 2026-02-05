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


class AFortAthenaVehicle : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaVehicle);

    DEFINE_FUNC(FindSeatIndex, int32);
};

class AFortAthenaSKPushCannon : public AFortAthenaVehicle
{
public:

    UCLASS_COMMON_MEMBERS(AFortAthenaSKPushCannon);

	DEFINE_FUNC(MultiCastPushCannonLaunchedPlayer, void);
    DEFINE_FUNC(OnLaunchPawn, void);
    DEFINE_FUNC(OnPreLaunchPawn, void);

    static bool FireActorInCannon(UObject* Context, FFrame& Stack);

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

    void ShootPawnOut(const FVector& LaunchDir)
    {
        auto PawnToShoot = this->GetPawnAtSeat(1);

        if (!PawnToShoot)
            return;

        this->OnPreLaunchPawn(PawnToShoot, LaunchDir);
        PawnToShoot->ServerOnExitVehicle();

        if (FConfiguration::bFModCannon)
        {
            PawnToShoot->LaunchCharacterJump(LaunchDir.X * 6000 * FConfiguration::CannonLaunchMultiplier, LaunchDir.Y * 5000 * FConfiguration::CannonLaunchMultiplier, LaunchDir.Z * 7500 * FConfiguration::CannonLaunchMultiplier);
        }
        else
        {
			this->OnLaunchPawn(PawnToShoot, LaunchDir);
        }

        this->MultiCastPushCannonLaunchedPlayer();
    }
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

    void ShootPawnOut()
    {
        auto PawnToShoot = this->GetPawnAtSeat(1);

        if (!PawnToShoot)
            return;

		PawnToShoot->ServerOnExitVehicle();
		this->OnLaunchPawn(PawnToShoot);
    }
};

class AFortWeaponRangedMountedCannon : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortWeaponRangedMountedCannon);

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