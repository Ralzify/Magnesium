#include "pch.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortGameMode.h"
#include "../../Erbium/PlayerAI/Public/MagnesiumPlayerAIIntegration.h"
#include "../../Erbium/PlayerAI/Public/AIDebugLogger.h"
#include "../../Erbium/PlayerAI/Public/PlayerAIManager.h"
#include "../../Erbium/PlayerAI/Public/VersionFeatureAdapter.h"
#include "../Public/FortWeapon.h"
#include "../Public/BuildingSMActor.h"
#include "../Public/FortKismetLibrary.h"
#include "../../Erbium/Public/Configuration.h"
#include "../Public/FortLootPackage.h"
#include "../../Erbium/Public/Events.h"
#include "../../Erbium/Public/LateGame.h"
#include "../Public/BuildingItemCollectorActor.h"
#include "../Public/FortPhysicsPawn.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortAthenaMutator.h"
#include "../../Erbium/Public/GUI.h"
#include "../../Erbium/Public/PlayerLoadout.h"
#include "../Public/FortAthenaCreativePortal.h"
#include "../Public/FortInventory.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../Erbium/Public/Misc.h"
#include "../Public/FortControllerComponent_VictoryCrowns.h"
#include "../Public/FortWeaponMods.h"
#include "../Public/FortVehicleMods.h"

#include <d3d11.h>
#include <sstream>
#include <fstream>

#include <unordered_set>
#include <random>
#include <chrono>
#include <algorithm>
#include <vector>
#include <iostream>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <cctype>
#include <limits>
#include <functional>

static const UClass* GetRemoteControlledPawnClass();
static bool IsNativeVehiclePossessionPawn(AActor* Actor);
static bool IsUsableDeathObject(const UObject* Object);
static bool IsManagedNonRespawningBot(
	AFortPlayerControllerAthena* PlayerController);
static bool IsTerminalManagedBot(
	AFortPlayerControllerAthena* PlayerController);
static bool ClientForceViewTarget(
	AFortPlayerControllerAthena* PlayerController,
	AActor* Target);
static void ForEachSquadController(
	AFortPlayerControllerAthena* InstigatingController,
	const std::function<void(AFortPlayerControllerAthena*)>& Visitor);

// A guided missile's controller Pawn fields are not reliable on legacy builds:
// Season 4 can leave both of them pointing at the character for the entire
// remote-control session. This lifecycle map is populated by the confirmed
// remote-pawn acknowledgement and cleared by the native return acknowledgement.
static std::unordered_map<AFortPlayerControllerAthena*, AActor*> GRemoteControlReturnPawn;
// Character vehicles (notably the B.R.U.T.E. on legacy builds) temporarily
// replace Controller->Pawn just like a guided missile does. Keep that
// possession separate from real player-pawn replacement so entering/exiting a
// vehicle cannot run spawn/respawn initialization.
static std::unordered_map<AFortPlayerControllerAthena*, AActor*> GVehiclePossessionReturnPawn;
// Keep the authoritative host actor as well as the return pawn. A B.R.U.T.E.
// driver-to-gunner transition returns controller possession to the character
// while that character is still occupying the vehicle, so the return pawn by
// itself cannot distinguish a seat change from a real exit.
static std::unordered_map<AFortPlayerControllerAthena*, AActor*> GVehiclePossessionVehicle;

static AActor* ResolveVehicleForPawn(AActor* Pawn)
{
	if (!IsUsableDeathObject(Pawn))
		return nullptr;

	// Controller possession alternates between the character and the mech.
	// Resolve the function on the current object every time; caching a UFunction
	// from one of those classes and invoking it on the other is invalid.
	auto GetVehicleFunction = Pawn->GetFunction("GetVehicleActor");
	if (!GetVehicleFunction)
		GetVehicleFunction = Pawn->GetFunction("GetVehicle");
	if (!GetVehicleFunction)
		GetVehicleFunction = Pawn->GetFunction("BP_GetVehicle");

	return GetVehicleFunction
		? Pawn->Call<AActor*>(GetVehicleFunction) : nullptr;
}

static AFortPlayerPawnAthena* ResolveVehicleRiderPawn(
	AFortPlayerControllerAthena* PlayerController,
	AActor* FallbackPawn = nullptr)
{
	if (!PlayerController)
		return nullptr;

	auto PlayerPawnClass = AFortPlayerPawnAthena::StaticClass();
	if (!PlayerPawnClass)
		return nullptr;

	auto ReturnPawn =
		GVehiclePossessionReturnPawn.find(PlayerController);
	if (ReturnPawn != GVehiclePossessionReturnPawn.end() &&
		IsUsableDeathObject(ReturnPawn->second) &&
		ReturnPawn->second->IsA(PlayerPawnClass))
	{
		return (AFortPlayerPawnAthena*)ReturnPawn->second;
	}

	auto MyFortPawn = (AActor*)PlayerController->MyFortPawn;
	if (IsUsableDeathObject(MyFortPawn) &&
		MyFortPawn->IsA(PlayerPawnClass))
		return (AFortPlayerPawnAthena*)MyFortPawn;

	auto ControlledPawn = (AActor*)PlayerController->Pawn;
	if (IsUsableDeathObject(ControlledPawn) &&
		ControlledPawn->IsA(PlayerPawnClass))
		return (AFortPlayerPawnAthena*)ControlledPawn;

	return IsUsableDeathObject(FallbackPawn) &&
		FallbackPawn->IsA(PlayerPawnClass)
		? (AFortPlayerPawnAthena*)FallbackPawn : nullptr;
}

static int32 FindTrackedVehicleSeat(
	AActor* Vehicle, AFortPlayerPawnAthena* RiderPawn)
{
	if (!IsUsableDeathObject(Vehicle) ||
		!IsUsableDeathObject(RiderPawn))
		return -1;

	auto SeatComponent = (UFortVehicleSeatComponent*)
		Vehicle->GetComponentByClass(
			UFortVehicleSeatComponent::StaticClass());
	return SeatComponent
		? SeatComponent->FindSeatIndex(RiderPawn) : -1;
}

static bool IsControllerStillUsingTrackedVehicle(
	AFortPlayerControllerAthena* PlayerController,
	AActor* FallbackRider = nullptr)
{
	if (!PlayerController)
		return false;

	auto VehicleState =
		GVehiclePossessionVehicle.find(PlayerController);
	if (VehicleState == GVehiclePossessionVehicle.end() ||
		!IsUsableDeathObject(VehicleState->second))
	{
		return false;
	}

	auto Vehicle = VehicleState->second;
	if ((AActor*)PlayerController->Pawn == Vehicle)
		return true;

	auto RiderPawn = ResolveVehicleRiderPawn(
		PlayerController, FallbackRider);
	if (!RiderPawn)
		return false;

	// The replicated slot is authoritative when present. GetVehicle can lag
	// behind a successful ordinary-car exit, so only use it as a fallback for
	// vehicle classes that do not expose the common seat component.
	auto SeatComponent = (UFortVehicleSeatComponent*)
		Vehicle->GetComponentByClass(
			UFortVehicleSeatComponent::StaticClass());
	if (SeatComponent)
		return SeatComponent->FindSeatIndex(RiderPawn) >= 0;

	return ResolveVehicleForPawn(RiderPawn) == Vehicle;
}

struct FTrackedVehicleLoadout
{
	bool bHasOriginalEquippedItem = false;
	FGuid OriginalEquippedItem{};
	std::vector<FGuid> TemporaryItemGuids;
};

static std::unordered_map<AFortPlayerControllerAthena*,
	FTrackedVehicleLoadout> GTrackedVehicleLoadouts;

static bool VehicleLoadoutGuidsEqual(
	const FGuid& Left, const FGuid& Right)
{
	return Left.A == Right.A && Left.B == Right.B &&
		Left.C == Right.C && Left.D == Right.D;
}

static bool VehicleLoadoutContainsGuid(
	const std::vector<FGuid>& Guids, const FGuid& Guid)
{
	return std::any_of(
		Guids.begin(), Guids.end(),
		[&](const FGuid& Existing)
		{
			return VehicleLoadoutGuidsEqual(Existing, Guid);
		});
}

static bool VehicleInventoryContainsGuid(
	AFortInventory* Inventory, const FGuid& Guid)
{
	if (!Inventory)
		return false;

	for (int32 Index = 0;
		Index < Inventory->Inventory.ReplicatedEntries.Num(); ++Index)
	{
		auto& Entry = Inventory->Inventory.ReplicatedEntries.Get(
			Index, FFortItemEntry::Size());
		if (VehicleLoadoutGuidsEqual(Entry.ItemGuid, Guid))
			return true;
	}

	return false;
}

static std::vector<FGuid> SnapshotVehicleInventoryGuids(
	AFortInventory* Inventory)
{
	std::vector<FGuid> Guids;
	if (!Inventory)
		return Guids;

	Guids.reserve(
		Inventory->Inventory.ReplicatedEntries.Num());
	for (int32 Index = 0;
		Index < Inventory->Inventory.ReplicatedEntries.Num(); ++Index)
	{
		auto& Entry = Inventory->Inventory.ReplicatedEntries.Get(
			Index, FFortItemEntry::Size());
		Guids.push_back(Entry.ItemGuid);
	}

	return Guids;
}

static FTrackedVehicleLoadout CaptureVehicleLoadout(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* RiderPawn)
{
	FTrackedVehicleLoadout State{};
	if (!PlayerController || !PlayerController->WorldInventory ||
		!RiderPawn || !RiderPawn->HasCurrentWeapon() ||
		!RiderPawn->CurrentWeapon)
	{
		return State;
	}

	auto CurrentWeapon =
		RiderPawn->CurrentWeapon->Cast<AFortWeapon>();
	if (!CurrentWeapon ||
		!VehicleInventoryContainsGuid(
			PlayerController->WorldInventory,
			CurrentWeapon->ItemEntryGuid))
	{
		return State;
	}

	State.bHasOriginalEquippedItem = true;
	State.OriginalEquippedItem = CurrentWeapon->ItemEntryGuid;
	return State;
}

static void TrackNewVehicleItems(
	FTrackedVehicleLoadout& State,
	AFortInventory* Inventory,
	const std::vector<FGuid>& BeforeGuids,
	const UFortItemDefinition* RequiredDefinition = nullptr)
{
	if (!Inventory)
		return;

	for (int32 Index = 0;
		Index < Inventory->Inventory.ReplicatedEntries.Num(); ++Index)
	{
		auto& Entry = Inventory->Inventory.ReplicatedEntries.Get(
			Index, FFortItemEntry::Size());
		if ((RequiredDefinition &&
				Entry.ItemDefinition != RequiredDefinition) ||
			VehicleLoadoutContainsGuid(BeforeGuids, Entry.ItemGuid) ||
			VehicleLoadoutContainsGuid(
				State.TemporaryItemGuids, Entry.ItemGuid))
		{
			continue;
		}

		State.TemporaryItemGuids.push_back(Entry.ItemGuid);
	}
}

static void RemoveTrackedVehicleItems(
	AFortPlayerControllerAthena* PlayerController,
	FTrackedVehicleLoadout& State)
{
	if (!PlayerController || !PlayerController->WorldInventory)
		return;

	for (const auto& Guid : State.TemporaryItemGuids)
	{
		if (VehicleInventoryContainsGuid(
			PlayerController->WorldInventory, Guid))
		{
			PlayerController->WorldInventory->Remove(Guid);
		}
	}
	State.TemporaryItemGuids.clear();
}

static bool EquipTrackedVehicleOriginalItem(
	AFortPlayerControllerAthena* PlayerController,
	const FTrackedVehicleLoadout& State)
{
	if (!State.bHasOriginalEquippedItem ||
		!PlayerController || !PlayerController->WorldInventory ||
		!VehicleInventoryContainsGuid(
			PlayerController->WorldInventory,
			State.OriginalEquippedItem))
	{
		return false;
	}

	PlayerController->ServerExecuteInventoryItem(
		State.OriginalEquippedItem);
	PlayerController->ClientEquipItem(
		State.OriginalEquippedItem, true);
	return true;
}

void AFortPlayerControllerAthena::RestoreVehicleLoadoutAfterExit(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!PlayerController)
		return;

	// ServerOnExitVehicle can be rejected, and returning possession to the
	// B.R.U.T.E. gunner is a seat transition rather than an exit. In both cases
	// the character remains in the stored vehicle slot, so retain the temporary
	// Ostrich weapon and the original-loadout snapshot.
	if (IsControllerStillUsingTrackedVehicle(PlayerController))
	{
		SDK::DbgLog(
			"[Vehicles] deferred loadout restore for occupied vehicle "
			"controller=%p vehicle=%p\n",
			(void*)PlayerController,
			(void*)GVehiclePossessionVehicle[PlayerController]);
		return;
	}

	GVehiclePossessionReturnPawn.erase(PlayerController);
	GVehiclePossessionVehicle.erase(PlayerController);

	auto Tracked = GTrackedVehicleLoadouts.find(PlayerController);
	if (Tracked == GTrackedVehicleLoadouts.end())
		return;

	auto State = std::move(Tracked->second);
	GTrackedVehicleLoadouts.erase(Tracked);
	RemoveTrackedVehicleItems(PlayerController, State);

	if (!EquipTrackedVehicleOriginalItem(
		PlayerController, State))
		return;

	SDK::DbgLog(
		"[Vehicles] restored exact pre-entry equipped item "
		"controller=%p guid=%08x-%08x-%08x-%08x\n",
		(void*)PlayerController,
		(unsigned)State.OriginalEquippedItem.A,
		(unsigned)State.OriginalEquippedItem.B,
		(unsigned)State.OriginalEquippedItem.C,
		(unsigned)State.OriginalEquippedItem.D);
}

static bool IsRemoteControlledPawn(AActor* Actor)
{
	auto RemoteControlledPawnClass = GetRemoteControlledPawnClass();
	return Actor && RemoteControlledPawnClass && Actor->IsA(RemoteControlledPawnClass);
}

// Infinite Render changes every real client's replication viewpoint to the
// newest live player-like pawn. Spawned bots do not (and must not) own a
// UNetConnection, but they are registered in GameMode->AlivePlayers, so this
// target lookup includes both connected players and mid-match spawnbot actors.
static AActor* FindInfiniteRenderViewPawn()
{
	auto World = UWorld::GetWorld();
	if (!World)
		return nullptr;

	// GetPlayerViewPoint also runs in the frontend, where AuthorityGameMode is a
	// different class. Casting that object to AFortGameMode and reading the
	// cached AlivePlayers offset caused an invalid TArray access during launch.
	auto AuthorityGameMode = World->AuthorityGameMode;
	auto AthenaGameModeClass = AFortGameModeAthena::StaticClass();
	if (AuthorityGameMode && AthenaGameModeClass && AuthorityGameMode->IsA(AthenaGameModeClass))
	{
		auto GameMode = (AFortGameMode*)AuthorityGameMode;
		if (GameMode->HasAlivePlayers())
		{
			auto& AlivePlayers = GameMode->AlivePlayers;
			for (int i = AlivePlayers.Num() - 1; i >= 0; --i)
			{
				auto Controller = (AFortPlayerControllerAthena*)AlivePlayers[i];
				if (!Controller)
					continue;

				if (auto Pawn = Controller->GetPawn())
					return Pawn;
			}
		}
	}

	// Keep the original behavior available while the game mode/alive list is
	// not ready (for example during early connection setup).
	auto Driver = (UNetDriver*)World->NetDriver;
	if (!Driver)
		return nullptr;

	for (int i = Driver->ClientConnections.Num() - 1; i >= 0; --i)
	{
		auto Connection = Driver->ClientConnections[i];
		if (!Connection)
			continue;

		auto Controller = Connection->GetPlayerController();
		if (Controller)
			if (auto Pawn = Controller->GetPawn())
				return Pawn;
	}

	return nullptr;
}

void AFortPlayerControllerAthena::GetPlayerViewPoint(AFortPlayerControllerAthena* PlayerController, FVector& Loc, FRotator& Rot)
{
	// A guided missile keeps MyFortPawn pointed at the character while the
	// controller's actual Pawn/AcknowledgedPawn and camera belong to the remote
	// projectile. Combining MyFortPawn's location with the missile's banked
	// ControlRotation rolls the whole view sideways and bypasses its native
	// camera/steering behavior. Let the engine camera path handle only this
	// temporary possession; normal and Infinite Render viewpoints stay intact.
	auto ControlledPawn = (AActor*)PlayerController->Pawn;
	const bool bRemoteControlActive =
		GRemoteControlReturnPawn.find(PlayerController) != GRemoteControlReturnPawn.end();
	const bool bVehiclePossessionActive =
		GVehiclePossessionReturnPawn.find(PlayerController) !=
		GVehiclePossessionReturnPawn.end();
	if (bRemoteControlActive || bVehiclePossessionActive ||
		IsRemoteControlledPawn(ControlledPawn) ||
		IsRemoteControlledPawn(PlayerController->AcknowledgedPawn))
	{
		if (GetPlayerViewPointOG)
		{
			GetPlayerViewPointOG(PlayerController, Loc, Rot);
			return;
		}

		if (auto ViewTarget = PlayerController->GetViewTarget())
		{
			Loc = ViewTarget->K2_GetActorLocation();
			Rot = ViewTarget->K2_GetActorRotation();
			return;
		}
	}

	// Once native death handling has entered spectator state, its camera and
	// view-target selection are authoritative. In particular, MyFortPawn can
	// still point at the dead character while the native spectator target has
	// already changed, so the normal Magnesium override below would pin the
	// view (and relevancy origin) to the corpse.
	static const FName SpectatingState(L"Spectating");
	const bool bNativeSpectating =
		(PlayerController->HasStateName() &&
			PlayerController->StateName == SpectatingState) ||
		(PlayerController->HasPlayerState() &&
			IsUsableDeathObject(PlayerController->PlayerState) &&
			PlayerController->PlayerState->HasbIsSpectator() &&
			PlayerController->PlayerState->bIsSpectator);
	if (bNativeSpectating && GetPlayerViewPointOG)
	{
		GetPlayerViewPointOG(PlayerController, Loc, Rot);
		return;
	}

	if (FConfiguration::bInfiniteRender)
	{
		if (auto ViewPawn = FindInfiniteRenderViewPawn())
		{
			Loc = ViewPawn->K2_GetActorLocation();
			Rot = PlayerController->GetControlRotation();
			return;
		}
	}

	if (auto Pawn = PlayerController->MyFortPawn)
	{
		Loc = Pawn->K2_GetActorLocation();
		Rot = PlayerController->GetControlRotation();
		return;
	}

	if (GetPlayerViewPointOG)
	{
		GetPlayerViewPointOG(PlayerController, Loc, Rot);
		return;
	}

	static auto SFName = FName(L"Spectating");

	if (PlayerController->StateName == SFName)
	{
		Loc = PlayerController->LastSpectatorSyncLocation;
		Rot = PlayerController->LastSpectatorSyncRotation;
		return;
	}

	auto ViewTarget = PlayerController->GetViewTarget();

	if (ViewTarget)
	{
		Loc = ViewTarget->K2_GetActorLocation();
		Rot = ViewTarget->K2_GetActorRotation();
		return;
	}
}
static std::unordered_set<AFortPlayerControllerAthena*> PlayersInitialized;
static std::unordered_set<AFortPlayerControllerAthena*> GPendingRespawnLandingFinalization;
static std::unordered_set<AFortPlayerControllerAthena*> GRespawnSkydivingObserved;
// 1.7.2 and 2.50 do not provide a usable EndSkydiving callback for the
// replacement pawn created by the late-game aircraft. Track that transition
// independently so its held-item state is refreshed once the replicated
// skydive flags actually clear.
static std::unordered_set<AFortPlayerControllerAthena*> GPendingLegacyAircraftLandingEquipment;
static std::unordered_set<AFortPlayerControllerAthena*> GLegacyAircraftSkydivingObserved;
// Aircraft lifecycle functions can be replayed while the bus is still flying.
// Remember the authoritative match in which each controller was cleaned so a
// duplicate RPC can never erase loot collected after that player jumped.
static TWeakObjectPtr<UObject> GAircraftInventoryCleanupMatchToken;
static std::unordered_set<uint64>
	GAircraftInventoryCleanedControllerIds;
static std::unordered_set<uint64>
	GAircraftInventoryCleanupInProgressIds;

static AFortPlayerControllerAthena* ResolveAircraftPlayerController(
	UObject* Context)
{
	if (!Context)
		return nullptr;

	auto AircraftComponentClass =
		FindClass("FortControllerComponent_Aircraft");
	if (AircraftComponentClass &&
		Context->IsA(AircraftComponentClass))
	{
		return (AFortPlayerControllerAthena*)
			((UActorComponent*)Context)->GetOwner();
	}

	auto PlayerControllerClass =
		AFortPlayerControllerAthena::StaticClass();
	return PlayerControllerClass &&
		Context->IsA(PlayerControllerClass)
		? (AFortPlayerControllerAthena*)Context
		: nullptr;
}

static bool CanDropInventoryItem(
	const UFortItemDefinition* ItemDefinition)
{
	if (!IsUsableDeathObject(ItemDefinition))
		return false;

	if (ItemDefinition->HasbCanBeDropped())
		return ItemDefinition->bCanBeDropped;

	auto PickupComponent =
		ItemDefinition->GetPickupComponent();
	return PickupComponent &&
		PickupComponent->bCanBeDroppedFromInventory;
}

static std::string LowerAscii(const char* Value)
{
	std::string Result = Value ? Value : "";
	std::transform(
		Result.begin(), Result.end(), Result.begin(),
		[](unsigned char Character)
		{
			return (char)std::tolower(Character);
		});
	return Result;
}

static bool ContainsConfirmedStormIdentifier(
	const FName& Name,
	bool bAllowGenericStorm)
{
	const auto Value =
		LowerAscii(Name.ToString().c_str());
	std::string Compact;
	Compact.reserve(Value.size());
	for (const unsigned char Character : Value)
	{
		if (std::isalnum(Character))
			Compact.push_back((char)Character);
	}

	return
		Compact.find("outsidesafezone") !=
			std::string::npos ||
		Compact.find("safezonedamage") !=
			std::string::npos ||
		(bAllowGenericStorm &&
			Compact.find("storm") !=
				std::string::npos);
}

static bool HasConfirmedStormDeathEvidence(
	const FGameplayTagContainer& DeathTags,
	const FFortPlayerDeathReport& DeathReport)
{
	auto ContainsStormTag =
		[](const TArray<FGameplayTag>& Tags)
		{
			for (int32 Index = 0;
				Index < Tags.Num();
				++Index)
			{
				const auto& Tag =
					Tags.Get(
						Index,
						FGameplayTag::Size());
				if (ContainsConfirmedStormIdentifier(
						Tag.TagName, true))
				{
					return true;
				}
			}
			return false;
		};

	if (ContainsStormTag(DeathTags.GameplayTags) ||
		ContainsStormTag(DeathTags.ParentTags))
	{
		return true;
	}

	// The outside-safe-zone gameplay effect is sometimes supplied only as the
	// report's causer. Do not accept a generic "storm" substring here because
	// legitimate weapons such as the Storm Scout can be damage causers.
	auto DamageCauser =
		DeathReport.HasDamageCauser()
			? DeathReport.DamageCauser
			: nullptr;
	return IsUsableDeathObject(DamageCauser) &&
		ContainsConfirmedStormIdentifier(
			DamageCauser->Name, false);
}

static bool ShouldRemoveForAircraftTransition(
	const UFortItemDefinition* ItemDefinition)
{
	if (!IsUsableDeathObject(ItemDefinition))
		return false;

	// ItemType remains reflected on the seasons where Epic removed both
	// bCanBeDropped and the pickup component. Preserve only the built-in tools;
	// weapons, ammo, traps, resources, and consumables collected during warmup
	// all need to be reset before the live match.
	if (ItemDefinition->HasItemType())
	{
		const uint8 ItemType = ItemDefinition->ItemType;
		if (ItemType == EFortItemType::GetWeaponHarvest() ||
			ItemType == EFortItemType::GetBuildingPiece() ||
			ItemType == EFortItemType::GetEditTool())
		{
			return false;
		}
	}

	const auto ObjectName =
		LowerAscii(ItemDefinition->Name.ToString().c_str());
	// These account/match-owned entries legitimately cross the warmup-to-bus
	// transition and are not spawn-island loot.
	if (ObjectName.find("victorycrown") != std::string::npos ||
		ObjectName.find("victory_crown") != std::string::npos ||
		ObjectName.find("athena_wadsitemdata") != std::string::npos ||
		ObjectName.find("athena_golditemdata") != std::string::npos)
	{
		return false;
	}

	// Very early/forked builds may not expose ItemType either. Retain a narrow
	// name fallback for their built-in tools.
	if (!ItemDefinition->HasItemType() &&
		(ObjectName.find("harvest") != std::string::npos ||
			ObjectName.find("pickaxe") != std::string::npos ||
			ObjectName.find("buildingitemdata") != std::string::npos ||
			ObjectName.find("edittool") != std::string::npos))
	{
		return false;
	}

	// Where native drop capability exists, keep its more precise answer. On
	// versions where both representations are absent, semantic removal is the
	// safe fallback that prevents warmup inventory from leaking into the bus.
	if (ItemDefinition->HasbCanBeDropped())
		return ItemDefinition->bCanBeDropped;

	auto PickupComponent = ItemDefinition->GetPickupComponent();
	if (PickupComponent)
		return PickupComponent->bCanBeDroppedFromInventory;

	return true;
}

static bool IsGetawayJewelDefinition(
	const UFortItemDefinition* ItemDefinition)
{
	if (!IsUsableDeathObject(ItemDefinition))
		return false;

	const auto ObjectName =
		LowerAscii(
			ItemDefinition->Name.ToString().c_str());

	// Both supported Getaway generations (5.40/5.41 and 10.40) carry the
	// objective as an item definition named Athena_Bling_Pack, even though its
	// package moved between those releases. Match that exact object only:
	// Bling_Pack is a character part, while generic "jewel"/"crystal" matches
	// can incorrectly remove unrelated inventory.
	return ObjectName == "athena_bling_pack";
}

void AFortPlayerControllerAthena::
	BeginAircraftInventoryCleanupForMatch(
		UObject* MatchToken)
{
	GAircraftInventoryCleanupMatchToken = MatchToken;
	GAircraftInventoryCleanedControllerIds.clear();
	GAircraftInventoryCleanupInProgressIds.clear();
}

int32 AFortPlayerControllerAthena::ClearDroppableInventoryForAircraft(
	AFortPlayerControllerAthena* PlayerController,
	const char* Source,
	bool bRequireAircraftPassenger)
{
	// Keep Inventory is specifically "Keep Inventory on Respawn"; it must not
	// preserve loot collected on the warmup island. Aircraft transition cleanup
	// therefore applies regardless of that respawn setting.
	if (!IsUsableDeathObject(PlayerController) ||
		!IsUsableDeathObject(PlayerController->WorldInventory) ||
		MagnesiumPlayerAIIntegration::IsPlayerAIController(
			PlayerController))
	{
		return 0;
	}

	auto World = UWorld::GetWorld();
	auto GameState =
		World && IsUsableDeathObject(World->GameState)
			? (AFortGameStateAthena*)World->GameState
			: nullptr;
	if (bRequireAircraftPassenger &&
		!PlayerController->IsInAircraft())
	{
		return 0;
	}
	if (GameState && GameState->HasGamePhase() &&
		GameState->GamePhase >
			(uint8)EAthenaGamePhase::Aircraft)
	{
		// EnterAircraft and the jump RPC are callable functions. Reject stale
		// or replayed calls after the bus phase so they cannot erase inventory
		// collected during the live match.
		return 0;
	}

	UObject* MatchToken = GameState
		? (UObject*)GameState
		: (UObject*)World;
	if (!MatchToken)
		return 0;

	if (GAircraftInventoryCleanupMatchToken.Get() != MatchToken)
	{
		BeginAircraftInventoryCleanupForMatch(MatchToken);
	}

	TWeakObjectPtr<AFortPlayerControllerAthena>
		WeakPlayerController(PlayerController);
	if (WeakPlayerController.Get() != PlayerController)
		return 0;
	const uint64 ControllerId =
		((uint64)(uint32)WeakPlayerController.ObjectIndex << 32) |
		(uint32)WeakPlayerController.ObjectSerialNumber;
	if (GAircraftInventoryCleanedControllerIds.contains(
			ControllerId) ||
		!GAircraftInventoryCleanupInProgressIds
			.insert(ControllerId).second)
	{
		return 0;
	}

	auto Inventory = PlayerController->WorldInventory;
	const int32 EntriesBefore =
		Inventory->Inventory.ReplicatedEntries.Num();
	std::vector<FGuid> GuidsToRemove;
	GuidsToRemove.reserve(EntriesBefore);
	int32 CandidateAttempts = 0;

	// Inventory update callbacks can replace an entry with a new GUID while an
	// old one is being removed. Rescan the live array after every pass instead
	// of verifying only the original GUIDs. The bound prevents a broken native
	// callback from trapping the aircraft transition in an infinite loop; an
	// incomplete controller remains retryable from the next phase/jump hook.
	constexpr int32 MaxCleanupPasses = 4;
	for (int32 Pass = 0; Pass < MaxCleanupPasses; ++Pass)
	{
		GuidsToRemove.clear();
		for (int32 Index = 0;
			Index < Inventory->Inventory.ReplicatedEntries.Num();
			++Index)
		{
			auto& Entry =
				Inventory->Inventory.ReplicatedEntries.Get(
					Index, FFortItemEntry::Size());
			if (ShouldRemoveForAircraftTransition(
					Entry.ItemDefinition) &&
				!FFortAthenaNativeLTMCompatibility::
					ShouldPreserveAshtonInventoryItem(
						PlayerController,
						Entry.ItemDefinition))
			{
				GuidsToRemove.push_back(Entry.ItemGuid);
			}
		}

		if (GuidsToRemove.empty())
			break;

		CandidateAttempts += (int32)GuidsToRemove.size();
		for (auto Guid : GuidsToRemove)
			Inventory->Remove(Guid);
	}

	bool bCleanupComplete = true;
	for (int32 Index = 0;
		Index < Inventory->Inventory.ReplicatedEntries.Num();
		++Index)
	{
		auto& Entry =
			Inventory->Inventory.ReplicatedEntries.Get(
				Index, FFortItemEntry::Size());
		if (ShouldRemoveForAircraftTransition(
				Entry.ItemDefinition) &&
			!FFortAthenaNativeLTMCompatibility::
				ShouldPreserveAshtonInventoryItem(
					PlayerController,
					Entry.ItemDefinition))
		{
			bCleanupComplete = false;
			break;
		}
	}
	GAircraftInventoryCleanupInProgressIds.erase(
		ControllerId);
	if (bCleanupComplete)
	{
		GAircraftInventoryCleanedControllerIds.insert(
			ControllerId);
	}

	const int32 EntriesAfter =
		Inventory->Inventory.ReplicatedEntries.Num();
	const int32 RemovedCount =
		max(EntriesBefore - EntriesAfter, 0);
	SDK::DbgLog(
		"[AircraftInventory] source=%s controller=%p "
		"candidates=%d entries=%d->%d removed=%d "
		"complete=%d FN=%.2f\n",
		Source ? Source : "unknown",
		(void*)PlayerController,
		CandidateAttempts,
		EntriesBefore, EntriesAfter, RemovedCount,
		(int)bCleanupComplete,
		VersionInfo.FortniteVersion);

	return RemovedCount;
}
static bool UsesEarlyAthenaLandingClientRefresh()
{
	return VersionInfo.FortniteVersion == 1.72 ||
		VersionInfo.FortniteVersion == 2.50;
}
static bool UsesTrackedLegacySpawnedBotLifecycle()
{
	return VersionInfo.FortniteVersion == 1.72 ||
		VersionInfo.FortniteVersion == 2.50;
}
// Controllers created by the `spawnbot` command have no UNetConnection and
// therefore need explicit equipment replication and deferred actor teardown.
// Track them on every version so the global player-respawn override can never
// mistake one for a client-owned participant.
static std::unordered_set<AFortPlayerControllerAthena*> GSpawnedBotControllers;
// If 1.7.2 native pawn-death handling leaves a synthetic controller in
// AlivePlayers, its native removal fallback must be attempted at most once.
static std::unordered_set<AFortPlayerControllerAthena*> G172SpawnedBotRemovalAttempts;
// Raw controller identities are safe only inside the world that created them.
// Keep both a serial-aware token and the address so a recycled UWorld address
// still forces a reset before any pointer-only bot membership check.
static TWeakObjectPtr<UWorld> GSpawnedBotTrackingWorld;
static UWorld* GSpawnedBotTrackingWorldIdentity = nullptr;
struct FPendingSpawnedBotCleanup
{
	TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
	AFortPlayerControllerAthena* ControllerIdentity = nullptr;
	TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
	TWeakObjectPtr<AFortPlayerStateAthena> PlayerState;
	TWeakObjectPtr<AFortInventory> Inventory;
	float RemainingSeconds = 3.f;
};
static std::vector<FPendingSpawnedBotCleanup> GPendingSpawnedBotCleanup;
static void RefreshSpawnedBotTrackingWorld();
static bool IsTrackedSpawnedBotController(
	AFortPlayerControllerAthena* PlayerController)
{
	RefreshSpawnedBotTrackingWorld();
	return PlayerController &&
		GSpawnedBotControllers.contains(PlayerController);
}
static void RegisterTrackedSpawnedBotController(
	AFortPlayerControllerAthena* PlayerController)
{
	RefreshSpawnedBotTrackingWorld();
	if (PlayerController)
		GSpawnedBotControllers.insert(PlayerController);
}
static void UnregisterTrackedSpawnedBotController(
	AFortPlayerControllerAthena* PlayerController)
{
	RefreshSpawnedBotTrackingWorld();
	if (!PlayerController)
		return;
	GSpawnedBotControllers.erase(PlayerController);
	G172SpawnedBotRemovalAttempts.erase(PlayerController);
}
// The forced post-respawn equip happens after native skydiving hid the previous
// weapon. Hide only that newly equipped actor and keep a weak reference so a
// destroyed/replaced weapon can never become a stale-pointer crash.
static std::unordered_map<AFortPlayerControllerAthena*, TWeakObjectPtr<AFortWeapon>> GRespawnHiddenWeapons;
// Custom respawn setup is per replacement pawn, not per acknowledgement. A
// client can acknowledge the same pawn again while finishing possession; doing
// all of the setup again reinitializes abilities and can start a restart loop.
static std::unordered_map<AFortPlayerControllerAthena*, AActor*> GLastAcknowledgedPawn;
static const UClass* GetRemoteControlledPawnClass()
{
	static auto RemoteControlledPawnClass =
		FindClass("FortRemoteControlledPawnAthena");
	return RemoteControlledPawnClass;
}
static bool IsNativeVehiclePossessionPawn(AActor* Actor)
{
	if (!Actor)
		return false;

	// Do not use the generated StaticClass helpers here. They negatively cache a
	// lookup made before a version-specific vehicle package is loaded. This is a
	// cold possession path, so retrying a missing class is safe.
	static const UClass* CharacterVehicleClass = nullptr;
	static const UClass* AthenaVehicleClass = nullptr;
	static const UClass* PhysicsPawnClass = nullptr;
	if (!CharacterVehicleClass)
		CharacterVehicleClass = FindClass("FortCharacterVehicle");
	if (!AthenaVehicleClass)
		AthenaVehicleClass = FindClass("FortAthenaVehicle");
	if (!PhysicsPawnClass)
		PhysicsPawnClass = FindClass("FortPhysicsPawn");

	return (CharacterVehicleClass && Actor->IsA(CharacterVehicleClass)) ||
		(AthenaVehicleClass && Actor->IsA(AthenaVehicleClass)) ||
		(PhysicsPawnClass && Actor->IsA(PhysicsPawnClass));
}
static bool IsLiveRemoteControlReturnPawn(AActor* Actor);
// A late-game aircraft jump replaces the lobby pawn with the real match pawn.
// Keep that possession distinct from both the initial lobby acknowledgement and
// a death respawn so it always receives the configured late-game loadout once.
static std::unordered_set<AFortPlayerControllerAthena*> GPendingLateGameAircraftLoadout;

static bool IsFiniteRespawnLocation(FVector Location)
{
	return std::isfinite(static_cast<double>(Location.X)) &&
		std::isfinite(static_cast<double>(Location.Y)) &&
		std::isfinite(static_cast<double>(Location.Z));
}

static AFortSafeZoneIndicator* GetRespawnSafeZoneIndicator(AFortGameMode* GameMode)
{
	if (!GameMode)
		return nullptr;

	if (GameMode->HasSafeZoneIndicator() && GameMode->SafeZoneIndicator)
		return GameMode->SafeZoneIndicator;

	auto GameState = GameMode->GameState;

	if (GameState && GameState->HasSafeZoneIndicator() && GameState->SafeZoneIndicator)
		return (AFortSafeZoneIndicator*)GameState->SafeZoneIndicator;

	return nullptr;
}

static bool TryGetSafeZoneRespawnCenter(AFortGameMode* GameMode, FVector& OutCenter)
{
	auto SafeZoneIndicator = GetRespawnSafeZoneIndicator(GameMode);

	if (!SafeZoneIndicator)
		return false;

	auto TryUseCenter = [&](FVector Center, bool bAllowZero) -> bool
	{
		if (!IsFiniteRespawnLocation(Center) || (!bAllowZero && Center.IsZero()))
			return false;

		OutCenter = Center;
		return true;
	};

	FVector PhaseCenter{};
	bool bHasPhaseCenter = false;

	if (SafeZoneIndicator->HasSafeZonePhases() && SafeZoneIndicator->HasCurrentPhase())
	{
		auto& SafeZonePhases = SafeZoneIndicator->SafeZonePhases;
		auto CurrentPhase = SafeZoneIndicator->CurrentPhase;

		if (SafeZonePhases.IsValidIndex(CurrentPhase))
		{
			auto& PhaseInfo = SafeZonePhases.Get(CurrentPhase, FFortSafeZonePhaseInfo::Size());
			PhaseCenter = PhaseInfo.Center;
			bHasPhaseCenter = true;
		}
	}

	if (auto GetSafeZoneCenterFn = SafeZoneIndicator->GetFunction("GetSafeZoneCenter"))
	{
		auto Center = SafeZoneIndicator->Call<FVector>(GetSafeZoneCenterFn);

		if (TryUseCenter(Center, bHasPhaseCenter || !Center.IsZero()))
			return true;
	}

	if (bHasPhaseCenter && TryUseCenter(PhaseCenter, true))
		return true;

	if (SafeZoneIndicator->HasNextCenter() && TryUseCenter(SafeZoneIndicator->NextCenter, false))
		return true;

	if (SafeZoneIndicator->HasLastCenter() && TryUseCenter(SafeZoneIndicator->LastCenter, false))
		return true;

	if (SafeZoneIndicator->HasPreviousCenter() && TryUseCenter(SafeZoneIndicator->PreviousCenter, false))
		return true;

	return false;
}

static bool TryGetConfiguredRespawnLocation(AFortGameMode* GameMode, FVector& OutLocation)
{
	if (FConfiguration::bForceRespawns && FConfiguration::bMidZoneRespawning)
	{
		FVector SafeZoneCenter{};

		if (TryGetSafeZoneRespawnCenter(GameMode, SafeZoneCenter))
		{
			OutLocation = SafeZoneCenter;
			OutLocation.Z = SafeZoneCenter.Z + static_cast<double>(FConfiguration::RespawnHeight);
			return true;
		}
	}

	if (FConfiguration::HasCustomRespawnPoint)
	{
		OutLocation = FConfiguration::CustomRespawnPoint;
		return true;
	}

	return false;
}

static const UFortItemDefinition* GetDefaultAthenaPickaxe()
{
	// Do not negatively cache this lookup: on some builds the asset is not present
	// in the object array until Athena content has finished loading.
	static const UFortItemDefinition* DefaultPickaxe = nullptr;

	if (!DefaultPickaxe)
		DefaultPickaxe = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");

	return DefaultPickaxe;
}

static FFortItemEntry* FindHarvestingToolEntry(AFortInventory* Inventory)
{
	if (!Inventory)
		return nullptr;

	// Prefer the actual harvesting-tool item type. A melee-class-only search can
	// select a sword or other special melee weapon before the player's pickaxe.
	auto HarvestingTool = Inventory->Inventory.ReplicatedEntries.Search([](FFortItemEntry& Entry)
		{
			return Entry.ItemDefinition &&
				Entry.ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest();
		}, FFortItemEntry::Size());

	if (HarvestingTool)
		return HarvestingTool;

	// Compatibility fallback for builds whose pickaxe definition does not expose
	// the expected item type but still derives from the melee weapon class.
	return Inventory->Inventory.ReplicatedEntries.Search([](FFortItemEntry& Entry)
		{
			return Entry.ItemDefinition && Entry.ItemDefinition->IsA<UFortWeaponMeleeItemDefinition>();
		}, FFortItemEntry::Size());
}

static void RestoreEquipmentAfterRespawn(AFortPlayerControllerAthena* PlayerController,
	bool bForceLegacyReequip = false);

static void ClearLegacyPawnDeathFlags(AFortPlayerPawnAthena* Pawn)
{
	if (!Pawn)
		return;

	if (Pawn->HasbPlayedDying())
		Pawn->bPlayedDying = false;
	if (Pawn->HasbIsDying())
		Pawn->bIsDying = false;
	if (Pawn->HasbWasDBNOOnDeath())
		Pawn->bWasDBNOOnDeath = false;
	if (Pawn->HasbIsHiddenForDeath())
		Pawn->bIsHiddenForDeath = false;
	if (Pawn->HasbIsDBNO() && Pawn->bIsDBNO)
	{
		Pawn->bIsDBNO = false;
		Pawn->OnRep_IsDBNO();
	}
}

static void ClearLegacyPlayerStateDeathFlags(AFortPlayerStateAthena* PlayerState)
{
	if (!PlayerState || !PlayerState->HasDeathInfo())
		return;

	bool bChanged = false;
	if (FDeathInfo::HasbInitialized() && PlayerState->DeathInfo.bInitialized)
	{
		PlayerState->DeathInfo.bInitialized = false;
		bChanged = true;
	}
	if (FDeathInfo::HasbDBNO() && PlayerState->DeathInfo.bDBNO)
	{
		PlayerState->DeathInfo.bDBNO = false;
		bChanged = true;
	}

	if (bChanged)
	{
		PlayerState->OnRep_DeathInfo();
		PlayerState->ForceNetUpdate();
	}
}

static bool IsRespawnBlockingAbility(const UFortGameplayAbility* Ability)
{
	if (!Ability)
		return false;

	const auto Name = Ability->Name.ToString();
	return Name.find("DBNO") != std::string::npos ||
		Name.find("DefaultPlayer_Death") != std::string::npos ||
		Name.find("GenericDeath") != std::string::npos;
}

static bool IsRespawnBlockingEffect(const UGameplayEffect* Effect)
{
	if (!Effect)
		return false;

	const auto Name = Effect->Name.ToString();
	return Name.find("DBNO") != std::string::npos ||
		Name.find("Downed") != std::string::npos ||
		Name.find("Death") != std::string::npos ||
		Name.find("Dying") != std::string::npos;
}

static void ClearRespawnBlockingAbilityState(AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* Pawn)
{
	if (!PlayerController || !PlayerController->PlayerState ||
		!PlayerController->PlayerState->AbilitySystemComponent)
	{
		ClearLegacyPawnDeathFlags(Pawn);
		return;
	}

	auto AbilitySystemComponent = PlayerController->PlayerState->AbilitySystemComponent;
	struct FPendingAbilityCancel
	{
		FGameplayAbilitySpecHandle Handle{};
		std::vector<uint8_t> ActivationInfoBytes;
		bool bWasActive = false;
	};
	std::vector<FPendingAbilityCancel> AbilitiesToCancel;
	const auto ActivationInfoSize = FGameplayAbilityActivationInfo::Size();
	const bool bValidActivationInfoSize = ActivationInfoSize > 0 && ActivationInfoSize <= 0x100;
	int ActiveAbilityCount = 0;
	for (int Index = 0; Index < AbilitySystemComponent->ActivatableAbilities.Items.Num(); Index++)
	{
		auto& Spec = AbilitySystemComponent->ActivatableAbilities.Items.Get(
			Index, FGameplayAbilitySpec::Size());
		if (IsRespawnBlockingAbility(Spec.Ability))
		{
			if (!bValidActivationInfoSize)
				continue;

			FPendingAbilityCancel Pending{};
			Pending.Handle = Spec.Handle;
			Pending.ActivationInfoBytes.resize(ActivationInfoSize);
			memcpy(Pending.ActivationInfoBytes.data(), &Spec.ActivationInfo,
				ActivationInfoSize);
			Pending.bWasActive = !Spec.HasActiveCount() || Spec.ActiveCount > 0;
			ActiveAbilityCount += Pending.bWasActive ? 1 : 0;
			AbilitiesToCancel.push_back(Pending);
		}
	}

	for (auto& Ability : AbilitiesToCancel)
	{
		auto& ActivationInfo = *reinterpret_cast<FGameplayAbilityActivationInfo*>(
			Ability.ActivationInfoBytes.data());
		// Legacy death/DBNO abilities can reject cancellation while still owning
		// BlockAbilitiesWithTag entries such as ActionPlayerChangeEquipment. Match
		// the native/Reboot revive sequence. Always notify the client because its
		// predicted copy can remain active even when the server's ActiveCount is 0.
		AbilitySystemComponent->ClientCancelAbility(Ability.Handle, ActivationInfo);
		AbilitySystemComponent->ClientEndAbility(Ability.Handle, ActivationInfo);
		if (Ability.bWasActive)
		{
			FPredictionKey EmptyPredictionKey{};
			AbilitySystemComponent->ServerEndAbility(
				Ability.Handle, ActivationInfo, EmptyPredictionKey);
		}
	}

	int RemainingActiveAbilityCount = 0;
	for (int Index = 0; Index < AbilitySystemComponent->ActivatableAbilities.Items.Num(); Index++)
	{
		auto& Spec = AbilitySystemComponent->ActivatableAbilities.Items.Get(
			Index, FGameplayAbilitySpec::Size());
		if (IsRespawnBlockingAbility(Spec.Ability) &&
			(!Spec.HasActiveCount() || Spec.ActiveCount > 0))
			RemainingActiveAbilityCount++;
	}

	std::vector<FActiveGameplayEffectHandle> EffectsToRemove;
	auto& Effects = AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal;
	for (int Index = 0; Index < Effects.Num(); Index++)
	{
		auto& Effect = Effects.Get(Index, FActiveGameplayEffect::Size());
		if (!IsRespawnBlockingEffect(Effect.Spec.Def))
			continue;

		auto Handle = *(FActiveGameplayEffectHandle*)(__int64(&Effect) + 0xc);
		if (Handle.Handle > 0)
			EffectsToRemove.push_back(Handle);
	}

	int RemovedEffectCount = 0;
	auto RemoveActiveEffectFn = AbilitySystemComponent->GetFunction("RemoveActiveGameplayEffect");
	if (RemoveActiveEffectFn)
	{
		for (auto& Handle : EffectsToRemove)
			if (AbilitySystemComponent->Call<bool>(RemoveActiveEffectFn, Handle, -1))
				RemovedEffectCount++;
	}

	ClearLegacyPawnDeathFlags(Pawn);
	SDK::DbgLog(
		"[Respawn] cleared legacy death state controller=%p pawn=%p abilities=%d active=%d->%d effects=%d/%d\n",
		(void*)PlayerController, (void*)Pawn, (int)AbilitiesToCancel.size(),
		ActiveAbilityCount, RemainingActiveAbilityCount,
		RemovedEffectCount, (int)EffectsToRemove.size());
}

static bool IsConfiguredOneShotPlaylist()
{
	if (IsOneShot())
		return true;

	// Also recognize a playlist supplied directly through configuration instead
	// of the GUI enum, including short paths and tournament variants.
	const auto Playlist = FConfiguration::Playlist;
	return Playlist &&
		(wcsstr(Playlist, L"Playlist_Low_") ||
			wcsstr(Playlist, L"Playlist_ShowdownTournament_Low_"));
}

// Late-game normally starts players at full shield, but mode modifiers can
// lower that capacity. One Shot is the important zero-capacity case: invoking
// SetShield(100) there can leave native GAS with an active/duplicated shield
// aggregator even though the replicated maximum remains zero. Damage is then
// deposited into that invalid layer (for example 0 - 86) instead of health.
static float ClampShieldToInitializedPawnCapacity(
	AFortPlayerPawnAthena* Pawn, float RequestedShield)
{
	if (!Pawn || !FPlatformMath::IsFinite(RequestedShield) ||
		RequestedShield <= 0.f || IsConfiguredOneShotPlaylist())
	{
		return 0.f;
	}

	const float MaxShield = Pawn->GetMaxShield();
	if (!FPlatformMath::IsFinite(MaxShield))
		return 0.f;
	if (MaxShield <= 0.f)
		return 0.f;
	if (RequestedShield > MaxShield)
		return MaxShield;

	return RequestedShield;
}

static void ApplyLateGameSpawnShield(AFortPlayerPawnAthena* Pawn)
{
	if (Pawn)
		Pawn->SetShield(
			ClampShieldToInitializedPawnCapacity(Pawn, 100.f));
}

// Run after the default and playlist gameplay effects have initialized. This
// both enforces ordinary capacity bounds and explicitly clears One Shot's
// native shield aggregator, even when its visible current value is already 0.
static void NormalizeShieldAfterGameplayInitialization(
	AFortPlayerPawnAthena* Pawn)
{
	if (!Pawn)
		return;

	const float CurrentShield = Pawn->GetShield();
	const float MaxShield = Pawn->GetMaxShield();
	const float NormalizedShield =
		ClampShieldToInitializedPawnCapacity(Pawn, CurrentShield);
	if (IsConfiguredOneShotPlaylist() ||
		!FPlatformMath::IsFinite(MaxShield) ||
		MaxShield <= 0.f ||
		!FPlatformMath::IsFinite(CurrentShield) ||
		CurrentShield != NormalizedShield)
	{
		Pawn->SetShield(NormalizedShield);
	}
}

static bool IsGameplayEffectClassForCommand(const UClass* Class);

// One Shot's playlist already supplies its movement/gravity behavior. This
// separate persistent effect owns the purple low-gravity leg cue and fall
// immunity without changing jump height. Loading an absent asset can fault on
// a few legacy builds, so keep the loader free of unwindable C++ locals.
static const UClass* TryLoadOneShotLowGravityVfxEffectByPath()
{
	const UClass* Result = nullptr;

	__try
	{
		Result = FindObject<UClass>(
			L"/Game/Athena/Playlists/Low/GE_Low_SetFallImmune.GE_Low_SetFallImmune_C");
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Result = nullptr;
	}

	return Result;
}

// The canonical path is shared by the known One Shot releases. A short-name
// fallback also covers a build that has already loaded the same class from a
// differently mounted package.
static const UClass* TryFindLoadedOneShotLowGravityVfxEffectByName()
{
	// A typed object-array name lookup is required here: StaticFindObject with
	// Outer == nullptr does not reliably accept an unqualified object name.
	// Rescan only if the object array changed, and never more often than every
	// five seconds, so a differently mounted late-loaded class remains
	// discoverable without turning every pawn retry into a global scan.
	static const UWorld* LastScannedWorld = nullptr;
	static int32 LastScannedObjectCount = -1;
	static ULONGLONG LastScanTime = 0;

	auto CurrentWorld = UWorld::GetWorld();
	if (CurrentWorld != LastScannedWorld)
	{
		LastScannedWorld = CurrentWorld;
		LastScannedObjectCount = -1;
		LastScanTime = 0;
	}

	const int32 ObjectCount = TUObjectArray::Num();
	const ULONGLONG Now = GetTickCount64();
	if (ObjectCount <= 0 || ObjectCount == LastScannedObjectCount ||
		(LastScanTime != 0 && Now - LastScanTime < 5000))
	{
		return nullptr;
	}

	LastScannedObjectCount = ObjectCount;
	LastScanTime = Now;

	const UClass* Result = nullptr;

	__try
	{
		Result =
			TUObjectArray::FindObject<UClass>("GE_Low_SetFallImmune_C");
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Result = nullptr;
	}

	return Result;
}

static const UClass* GetOneShotLowGravityVfxEffectClass()
{
	// Do not negatively cache this lookup: playlist content can finish loading
	// after the first pawn acknowledgement on older versions.
	static TWeakObjectPtr<UClass> CachedEffectClass;
	static ULONGLONG NextResolveAttemptTime = 0;
	if (auto CachedClass = CachedEffectClass.Get())
		if (IsGameplayEffectClassForCommand(CachedClass))
			return CachedClass;

	// All pending pawns share this resolver. On a build where the asset is
	// missing, only one guarded load is attempted per second for the lobby.
	const ULONGLONG Now = GetTickCount64();
	if (NextResolveAttemptTime != 0 && Now < NextResolveAttemptTime)
		return nullptr;
	NextResolveAttemptTime = Now + 1000;

	auto EffectClass = TryLoadOneShotLowGravityVfxEffectByPath();
	if (!EffectClass)
		EffectClass = TryFindLoadedOneShotLowGravityVfxEffectByName();

	if (!IsGameplayEffectClassForCommand(EffectClass))
		return nullptr;

	CachedEffectClass =
		TWeakObjectPtr<UClass>(const_cast<UClass*>(EffectClass));
	NextResolveAttemptTime = 0;
	return CachedEffectClass.Get();
}

struct FPendingOneShotLowGravityVfxApplication
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AFortPlayerControllerAthena> PlayerController;
	TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
	float RemainingSeconds = 0.25f;
	int Attempts = 0;
};

static std::vector<FPendingOneShotLowGravityVfxApplication>
	GPendingOneShotLowGravityVfxApplications;

struct FOneShotLowGravityVfxPawnState
{
	TWeakObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;
	TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
};

static std::vector<FOneShotLowGravityVfxPawnState>
	GOneShotLowGravityVfxPawnStates;

static bool OneShotLowGravityVfxBelongsToPreviousPawn(
	UAbilitySystemComponent* AbilitySystemComponent,
	AFortPlayerPawnAthena* Pawn)
{
	for (auto It = GOneShotLowGravityVfxPawnStates.begin();
		It != GOneShotLowGravityVfxPawnStates.end();)
	{
		auto ExistingAbilitySystem = It->AbilitySystemComponent.Get();
		if (!ExistingAbilitySystem)
		{
			It = GOneShotLowGravityVfxPawnStates.erase(It);
			continue;
		}

		if (ExistingAbilitySystem == AbilitySystemComponent)
			return It->Pawn.Get() != Pawn;

		++It;
	}

	return false;
}

static void RememberOneShotLowGravityVfxPawn(
	UAbilitySystemComponent* AbilitySystemComponent,
	AFortPlayerPawnAthena* Pawn)
{
	for (auto It = GOneShotLowGravityVfxPawnStates.begin();
		It != GOneShotLowGravityVfxPawnStates.end();)
	{
		auto ExistingAbilitySystem = It->AbilitySystemComponent.Get();
		if (!ExistingAbilitySystem)
		{
			It = GOneShotLowGravityVfxPawnStates.erase(It);
			continue;
		}

		if (ExistingAbilitySystem == AbilitySystemComponent)
		{
			It->Pawn = TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
			return;
		}

		++It;
	}

	FOneShotLowGravityVfxPawnState State;
	State.AbilitySystemComponent =
		TWeakObjectPtr<UAbilitySystemComponent>(AbilitySystemComponent);
	State.Pawn = TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
	GOneShotLowGravityVfxPawnStates.emplace_back(State);
}

static bool TryEnsureOneShotLowGravityVfx(
	AFortPlayerControllerAthena* PlayerController, AFortPlayerPawnAthena* Pawn)
{
	if (!IsConfiguredOneShotPlaylist() || !PlayerController || !Pawn ||
		!PlayerController->PlayerState ||
		!PlayerController->PlayerState->HasAbilitySystemComponent())
	{
		return false;
	}

	auto AbilitySystemComponent =
		PlayerController->PlayerState->AbilitySystemComponent;
	if (!AbilitySystemComponent)
		return false;

	auto LowGravityVfxEffectClass = GetOneShotLowGravityVfxEffectClass();
	if (!LowGravityVfxEffectClass)
	{
		static bool bLoggedMissingEffect = false;
		if (!bLoggedMissingEffect)
		{
			bLoggedMissingEffect = true;
			SDK::DbgLog(
				"[OneShot] GE_Low_SetFallImmune_C could not be resolved; "
				"will retry shortly\n");
		}
		return false;
	}

	// The PlayerState ASC survives pawn replacement on several versions. Its
	// old looping cue actor belongs to the destroyed pawn, so refresh that one
	// effect once for a replacement pawn; otherwise retain the existing effect.
	const bool bNeedsPawnRefresh =
		OneShotLowGravityVfxBelongsToPreviousPawn(
			AbilitySystemComponent, Pawn);
	FActiveGameplayEffectHandle ExistingEffectHandle{};
	bool bFoundExistingEffect = false;

	if (AbilitySystemComponent->HasActiveGameplayEffects())
	{
		auto& ActiveGameplayEffects = AbilitySystemComponent->ActiveGameplayEffects;
		if (ActiveGameplayEffects.HasGameplayEffects_Internal())
		{
			auto& Effects = ActiveGameplayEffects.GameplayEffects_Internal;
			const int EffectCount = Effects.Num();
			if (EffectCount >= 0 && EffectCount < 100000)
			{
				for (int EffectIndex = 0; EffectIndex < EffectCount; EffectIndex++)
				{
					auto& Effect = Effects.Get(
						EffectIndex, FActiveGameplayEffect::Size());
					if (!Effect.HasSpec())
						break;

					auto& Spec = Effect.Spec;
					if (!Spec.HasDef())
						break;

					if (Spec.Def &&
						Spec.Def->IsA(LowGravityVfxEffectClass))
					{
						if (!bNeedsPawnRefresh)
						{
							RememberOneShotLowGravityVfxPawn(
								AbilitySystemComponent, Pawn);
							return true;
						}

						ExistingEffectHandle =
							*(FActiveGameplayEffectHandle*)((char*)&Effect + 0xc);
						bFoundExistingEffect =
							ExistingEffectHandle.Handle > 0;
						break;
					}
				}
			}
		}
	}

	if (bFoundExistingEffect)
	{
		auto RemoveActiveEffectFn =
			AbilitySystemComponent->GetFunction("RemoveActiveGameplayEffect");
		if (!RemoveActiveEffectFn ||
			!AbilitySystemComponent->Call<bool>(
				RemoveActiveEffectFn, ExistingEffectHandle, -1))
		{
			return false;
		}
	}

	auto Context = AbilitySystemComponent->MakeEffectContext();
	Context.Instigator = PlayerController;
	Context.Causer = Pawn;
	Context.AddSourceObject(Pawn);

	auto Handle = AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(
		LowGravityVfxEffectClass, 1.0f, Context);
	SDK::DbgLog(
		"[OneShot] purple-leg VFX effect %s controller=%p pawn=%p handle=%d\n",
		Handle.bPassedFiltersAndWasExecuted ? "applied" : "rejected",
		(void*)PlayerController, (void*)Pawn, Handle.Handle);
	if (Handle.bPassedFiltersAndWasExecuted)
		RememberOneShotLowGravityVfxPawn(
			AbilitySystemComponent, Pawn);
	return Handle.bPassedFiltersAndWasExecuted;
}

static void RemovePendingOneShotLowGravityVfxApplication(
	AFortPlayerControllerAthena* PlayerController, AFortPlayerPawnAthena* Pawn)
{
	for (auto It = GPendingOneShotLowGravityVfxApplications.begin();
		It != GPendingOneShotLowGravityVfxApplications.end();)
	{
		auto PendingController = It->PlayerController.Get();
		auto PendingPawn = It->Pawn.Get();

		if (!PendingController || !PendingPawn ||
			(PendingController == PlayerController && PendingPawn == Pawn))
		{
			It = GPendingOneShotLowGravityVfxApplications.erase(It);
			continue;
		}

		++It;
	}
}

static void EnsureOneShotLowGravityVfx(
	AFortPlayerControllerAthena* PlayerController, AFortPlayerPawnAthena* Pawn)
{
	if (!IsConfiguredOneShotPlaylist() || !PlayerController || !Pawn)
		return;

	if (TryEnsureOneShotLowGravityVfx(PlayerController, Pawn))
	{
		RemovePendingOneShotLowGravityVfxApplication(PlayerController, Pawn);
		return;
	}

	for (auto& Pending : GPendingOneShotLowGravityVfxApplications)
	{
		if (Pending.PlayerController.Get() == PlayerController &&
			Pending.Pawn.Get() == Pawn)
		{
			return;
		}
	}

	FPendingOneShotLowGravityVfxApplication Pending;
	Pending.World = TWeakObjectPtr<UWorld>(UWorld::GetWorld());
	Pending.PlayerController =
		TWeakObjectPtr<AFortPlayerControllerAthena>(PlayerController);
	Pending.Pawn = TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
	GPendingOneShotLowGravityVfxApplications.emplace_back(Pending);
}

static void TickOneShotLowGravityVfxRetries(float DeltaSeconds)
{
	for (int Index =
		static_cast<int>(GPendingOneShotLowGravityVfxApplications.size()) - 1;
		Index >= 0; --Index)
	{
		auto& Pending = GPendingOneShotLowGravityVfxApplications[Index];
		auto PlayerController = Pending.PlayerController.Get();
		auto Pawn = Pending.Pawn.Get();

		if (!IsConfiguredOneShotPlaylist() ||
			Pending.World.Get() != UWorld::GetWorld() ||
			!PlayerController || !Pawn ||
			(PlayerController->Pawn != Pawn &&
				PlayerController->MyFortPawn != Pawn))
		{
			GPendingOneShotLowGravityVfxApplications.erase(
				GPendingOneShotLowGravityVfxApplications.begin() + Index);
			continue;
		}

		Pending.RemainingSeconds -= DeltaSeconds;
		if (Pending.RemainingSeconds > 0.f)
			continue;

		const bool bEffectReady =
			TryEnsureOneShotLowGravityVfx(PlayerController, Pawn);
		// A failed refresh can still remove the previous pawn's effect and
		// rebuild GAS aggregators, so normalize after every attempted mutation.
		NormalizeShieldAfterGameplayInitialization(Pawn);
		if (bEffectReady)
		{
			GPendingOneShotLowGravityVfxApplications.erase(
				GPendingOneShotLowGravityVfxApplications.begin() + Index);
			continue;
		}

		Pending.Attempts++;
		if (Pending.Attempts >= 20)
		{
			SDK::DbgLog(
				"[OneShot] stopped retrying purple-leg VFX effect "
				"controller=%p pawn=%p\n",
				(void*)PlayerController, (void*)Pawn);
			GPendingOneShotLowGravityVfxApplications.erase(
				GPendingOneShotLowGravityVfxApplications.begin() + Index);
			continue;
		}

		Pending.RemainingSeconds = 1.f;
	}
}

extern uint64_t ApplyCharacterCustomization;
uint64_t InitializePlayerGameplayAbilities_;

struct FGameplayAbilityInitializationState
{
	TWeakObjectPtr<AFortPlayerControllerAthena> PlayerController;
	TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
};

static std::vector<FGameplayAbilityInitializationState>
	GGameplayAbilityInitializationStates;

// The PlayerState ASC can survive pawn replacement, and more than one pawn
// lifecycle callback may observe the replacement. Apply the default/playlist
// ability sets exactly once for each controller+pawn pair so their persistent
// modifiers cannot stack or be reordered.
static bool EnsurePawnGameplayAbilitiesInitialized(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* Pawn)
{
	if (!PlayerController || !Pawn || !PlayerController->PlayerState)
		return false;

	for (auto It = GGameplayAbilityInitializationStates.begin();
		It != GGameplayAbilityInitializationStates.end();)
	{
		auto ExistingController = It->PlayerController.Get();
		auto ExistingPawn = It->Pawn.Get();
		if (!ExistingController || !ExistingPawn)
		{
			It = GGameplayAbilityInitializationStates.erase(It);
			continue;
		}

		if (ExistingController == PlayerController &&
			ExistingPawn == Pawn)
		{
			return true;
		}

		if (ExistingController == PlayerController)
		{
			It = GGameplayAbilityInitializationStates.erase(It);
			continue;
		}

		++It;
	}

	bool bInitialized = false;
	auto Interface = PlayerController->PlayerState->GetInterface(
		IFortAbilitySystemInterface::StaticClass());
	if (InitializePlayerGameplayAbilities_ && Interface)
	{
		auto InitializePlayerGameplayAbilities =
			(void (*&)(const IInterface*))
			InitializePlayerGameplayAbilities_;
		InitializePlayerGameplayAbilities(Interface);
		bInitialized = true;
	}
	else if (auto AbilitySystemComponent =
		PlayerController->PlayerState->AbilitySystemComponent)
	{
		for (auto& AbilitySet : AFortGameMode::AbilitySets)
			AbilitySystemComponent->GiveAbilitySet(AbilitySet);
		bInitialized = true;
	}

	if (!bInitialized)
		return false;

	FGameplayAbilityInitializationState State;
	State.PlayerController =
		TWeakObjectPtr<AFortPlayerControllerAthena>(PlayerController);
	State.Pawn = TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
	GGameplayAbilityInitializationStates.emplace_back(State);
	return true;
}

// Controllers whose NEXT possession-ack should skip the respawn-point teleport.
// Set by commands that re-possess deliberately ("size", "possess"), consumed in
// ServerAcknowledgePossession below.
//
// This is a per-controller SET, not a single flag, on purpose: a single possess
// command can re-possess two controllers at once (yours plus a player you took
// a pawn from). ServerAcknowledgePossession fires once per controller on a
// network round-trip, so a lone global flag gets consumed by whichever ack
// arrives first and the other controller is flung to the respawn point. That is
// exactly why a possessed player did not stay where they were left.
static std::unordered_set<AFortPlayerControllerAthena*> GSkipPossessRespawnControllers;

static void SkipNextPossessRespawn(AFortPlayerControllerAthena* PC)
{
	if (PC)
		GSkipPossessRespawnControllers.insert(PC);
}

static bool ConsumePossessRespawnSkip(AFortPlayerControllerAthena* PC)
{
	return PC && GSkipPossessRespawnControllers.erase(PC) > 0;
}

// Controllers whose next possession-ack is a "possess" command takeover of a
// live pawn. ServerAcknowledgePossession fires AFTER the command finishes (a
// client round-trip) and re-runs the native possession setup, which resets the
// pawn's health and movement mode - so finalizing in the command alone gets
// undone. This flag lets the ack re-apply the fixups as its very last step,
// after nothing else can touch the pawn. Defined here, consumed at the bottom
// of ServerAcknowledgePossession.
static std::unordered_set<AFortPlayerControllerAthena*> GFinalizePossessTakeover;
static void FinalizePossessedPawnForCommand(AFortPlayerControllerAthena* PC, AFortPlayerPawnAthena* Pawn); // fwd

void AFortPlayerControllerAthena::ServerAcknowledgePossession(UObject* Context, FFrame& Stack)
{
	AActor* Pawn;
	Stack.StepCompiledIn(&Pawn);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (!Pawn)
		return;

	static auto FortPCServerAcknowledgePossession = (void(*)(AFortPlayerControllerAthena*, AActor*))DefaultObjImpl("FortPlayerController")->Vft[Stack.GetCurrentNativeFunction()->GetVTableIndex()];
	FortPCServerAcknowledgePossession(PlayerController, Pawn);

	// Guided missiles temporarily possess a FortRemoteControlledPawnAthena. It
	// is not a replacement player pawn, so none of the custom death-respawn
	// setup (especially the configured mid-zone teleport) belongs here. Do not
	// update GLastAcknowledgedPawn either: when native code returns control to
	// the original character, that acknowledgement remains a duplicate and is
	// likewise left completely to the native remote-control lifecycle.
	auto RemoteControlledPawnClass = GetRemoteControlledPawnClass();
	if (RemoteControlledPawnClass && Pawn->IsA(RemoteControlledPawnClass))
	{
		// A command-triggered temporary possession may have queued the normal
		// possess fixups. This acknowledgement is handled natively instead, so
		// do not let those one-shot flags leak into a later real respawn.
		GSkipPossessRespawnControllers.erase(PlayerController);
		GFinalizePossessTakeover.erase(PlayerController);

		AActor* ReturnPawn = nullptr;
		auto ExistingReturn = GRemoteControlReturnPawn.find(PlayerController);
		if (ExistingReturn != GRemoteControlReturnPawn.end())
			ReturnPawn = ExistingReturn->second;
		else
		{
			auto LastAcknowledged = GLastAcknowledgedPawn.find(PlayerController);
			if (LastAcknowledged != GLastAcknowledgedPawn.end() &&
				LastAcknowledged->second != Pawn &&
				IsLiveRemoteControlReturnPawn(LastAcknowledged->second))
			{
				ReturnPawn = LastAcknowledged->second;
			}
			else if (PlayerController->MyFortPawn != Pawn &&
				IsLiveRemoteControlReturnPawn(PlayerController->MyFortPawn))
			{
				ReturnPawn = PlayerController->MyFortPawn;
			}

			if (ReturnPawn)
				GRemoteControlReturnPawn[PlayerController] = ReturnPawn;
		}

		SDK::DbgLog("[Possession] native-only remote-control acknowledgement controller=%p pawn=%p return=%p\n",
			(void*)PlayerController, (void*)Pawn, (void*)ReturnPawn);
		return;
	}

	// Legacy character vehicles, including the B.R.U.T.E., are true temporary
	// possessions: the controller acknowledges the vehicle on entry and the
	// original character again on exit. Treating the vehicle as a fresh
	// AFortPlayerPawnAthena below clamps its health to 100, teleports it to the
	// configured respawn point, and may rebuild the player's inventory.
	if (IsNativeVehiclePossessionPawn(Pawn))
	{
		GSkipPossessRespawnControllers.erase(PlayerController);
		GFinalizePossessTakeover.erase(PlayerController);

		AActor* ReturnPawn = nullptr;
		auto ExistingReturn =
			GVehiclePossessionReturnPawn.find(PlayerController);
		if (ExistingReturn != GVehiclePossessionReturnPawn.end())
			ReturnPawn = ExistingReturn->second;
		else
		{
			auto LastAcknowledged =
				GLastAcknowledgedPawn.find(PlayerController);
			if (LastAcknowledged != GLastAcknowledgedPawn.end() &&
				LastAcknowledged->second != Pawn &&
				IsLiveRemoteControlReturnPawn(LastAcknowledged->second))
			{
				ReturnPawn = LastAcknowledged->second;
			}
			else if (PlayerController->MyFortPawn != Pawn &&
				IsLiveRemoteControlReturnPawn(
					PlayerController->MyFortPawn))
			{
				ReturnPawn = PlayerController->MyFortPawn;
			}

			if (ReturnPawn)
				GVehiclePossessionReturnPawn[PlayerController] =
					ReturnPawn;
		}

		GVehiclePossessionVehicle[PlayerController] = Pawn;

		SDK::DbgLog(
			"[Possession] native-only vehicle acknowledgement "
			"controller=%p vehicle=%p return=%p\n",
			(void*)PlayerController, (void*)Pawn, (void*)ReturnPawn);
		return;
	}

	// Returning from a guided missile is not a pawn replacement. Even if the
	// missile's death callback erased GLastAcknowledgedPawn, restore its identity
	// and leave the complete native possession lifecycle untouched.
	auto RemoteReturn = GRemoteControlReturnPawn.find(PlayerController);
	if (RemoteReturn != GRemoteControlReturnPawn.end())
	{
		auto ExpectedPawn = RemoteReturn->second;
		GRemoteControlReturnPawn.erase(RemoteReturn);
		if (ExpectedPawn == Pawn && IsLiveRemoteControlReturnPawn(Pawn))
		{
			auto ReturnFortPawn = (AFortPlayerPawnAthena*)Pawn;
			FRotator ReturnRotation = PlayerController->GetControlRotation();
			const bool bUsedStoredRotation = ReturnFortPawn->HasStoredControlRotation();
			if (bUsedStoredRotation)
				ReturnRotation = ReturnFortPawn->StoredControlRotation;

			// The remote projectile is allowed to bank, but a character camera is
			// not. The old custom respawn teleport happened to clear this roll;
			// preserve native Pitch/Yaw while explicitly restoring an upright view.
			ReturnRotation.Roll = 0.0;
			PlayerController->SetControlRotation(ReturnRotation);
			PlayerController->ClientSetRotation(ReturnRotation, true);

			GLastAcknowledgedPawn[PlayerController] = Pawn;
			SDK::DbgLog(
				"[Possession] native-only remote-control return controller=%p pawn=%p rotation=(%.2f,%.2f,%.2f) stored=%d\n",
				(void*)PlayerController, (void*)Pawn,
				(double)ReturnRotation.Pitch, (double)ReturnRotation.Yaw,
				(double)ReturnRotation.Roll, (int)bUsedStoredRotation);
			return;
		}
	}

	// Exiting a character vehicle returns to the same live character, not to a
	// newly spawned pawn. Leave location, health, abilities, equipped item, and
	// every inventory entry to the native vehicle exit path.
	auto VehicleReturn =
		GVehiclePossessionReturnPawn.find(PlayerController);
	if (VehicleReturn != GVehiclePossessionReturnPawn.end())
	{
		auto ExpectedPawn = VehicleReturn->second;
		if (ExpectedPawn == Pawn &&
			IsLiveRemoteControlReturnPawn(Pawn))
		{
			GLastAcknowledgedPawn[PlayerController] = Pawn;

			// The B.R.U.T.E. gives the gunner back their character pawn while
			// retaining that pawn in its passenger slot. Preserve the vehicle
			// session and its Ostrich weapon until the slot is actually cleared.
			if (IsControllerStillUsingTrackedVehicle(
				PlayerController, Pawn))
			{
				SDK::DbgLog(
					"[Possession] native-only vehicle gunner return "
					"controller=%p pawn=%p vehicle=%p seat=%d\n",
					(void*)PlayerController, (void*)Pawn,
					(void*)GVehiclePossessionVehicle[PlayerController],
					(int)FindTrackedVehicleSeat(
						GVehiclePossessionVehicle[PlayerController],
						(AFortPlayerPawnAthena*)Pawn));
				return;
			}

			GVehiclePossessionReturnPawn.erase(VehicleReturn);
			GVehiclePossessionVehicle.erase(PlayerController);
			RestoreVehicleLoadoutAfterExit(PlayerController);
			SDK::DbgLog(
				"[Possession] native-only vehicle return "
				"controller=%p pawn=%p\n",
				(void*)PlayerController, (void*)Pawn);
			return;
		}

		// The controller returned to a different pawn, so this is a real pawn
		// replacement rather than a vehicle exit. Do not let entry bookkeeping
		// survive into a later, unrelated vehicle lifecycle.
		GVehiclePossessionReturnPawn.erase(VehicleReturn);
		GVehiclePossessionVehicle.erase(PlayerController);
		GTrackedVehicleLoadouts.erase(PlayerController);
	}

	// ServerAcknowledgePossession is also used for AI, wildlife, and other
	// object-pawn takeovers. The custom block below is exclusively player spawn
	// initialization and must never reinterpret an arbitrary pawn as an
	// AFortPlayerPawnAthena.
	auto PlayerPawnClass = AFortPlayerPawnAthena::StaticClass();
	if (!PlayerPawnClass || !Pawn->IsA(PlayerPawnClass))
	{
		SDK::DbgLog(
			"[Possession] native-only non-player acknowledgement "
			"controller=%p pawn=%p class=%s\n",
			(void*)PlayerController, (void*)Pawn,
			Pawn->Class ? Pawn->Class->Name.ToString().c_str() :
				"<null>");
		return;
	}

	const bool bPendingLateGameAircraftLoadout =
		GPendingLateGameAircraftLoadout.erase(PlayerController) > 0;
	const auto LastAcknowledgedPawn = GLastAcknowledgedPawn.find(PlayerController);
	const bool bHadAcknowledgedPawn =
		LastAcknowledgedPawn != GLastAcknowledgedPawn.end();
	const bool bNewAcknowledgedPawn = LastAcknowledgedPawn == GLastAcknowledgedPawn.end() ||
		LastAcknowledgedPawn->second != Pawn;
	GLastAcknowledgedPawn[PlayerController] = Pawn;

	auto FortPawn = (AFortPlayerPawnAthena*)Pawn;

	// The native acknowledgement still has to run for retries, but all custom
	// initialization below must run only once for this pawn. In particular,
	// repeating InitializePlayerGameplayAbilities continually restarts the Fall
	// action and prevents the client from completing possession.
	if (!bNewAcknowledgedPawn && !bPendingLateGameAircraftLoadout &&
		!GFinalizePossessTakeover.contains(PlayerController))
	{
		AFortInventory::HandlePendingCarmineFocus(
			PlayerController);
		if (AFortPlayerPawnAthena::
				HasMinimumHealthGodMode(PlayerController))
		{
			AFortPlayerPawnAthena::
				SetMinimumHealthGodMode(
					PlayerController, true);
		}
		return;
	}

	auto Num = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num();

	auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
	auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;

	if (wcsstr(FConfiguration::Playlist, L"/Game/Gav/Levels/GM_1v1/Playlist_Arena_DefaultSolo_Respawn.Playlist_Arena_DefaultSolo_Respawn") && VersionInfo.FortniteVersion == 27.11)
	{
		FortPawn->SetShield(100.f);

		if (UAbilitySystemComponent* AbilitySystemComponent = PlayerController->PlayerState->AbilitySystemComponent)
		{
			static auto FallDamageGE = FindObject<UClass>(L"/Game/Athena/Items/Gameplay/Backpacks/Ashton/GE_AshtonPack_FallDamageImmune.GE_AshtonPack_FallDamageImmune_C");

			if (FallDamageGE)
			{
				FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

				Context.Instigator = PlayerController;
				Context.Causer = FortPawn;
				Context.AddSourceObject(FortPawn);

				AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(FallDamageGE, 1.0f, Context);
			}
		}

	}

	static auto IsRespawningAllowedFunc = GameState->GetFunction("IsRespawningAllowed");

	bool bRespawnAllowed = false;

	if (!IsRespawningAllowedFunc)
	{
		auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;

		bRespawnAllowed = Playlist
			? (Playlist->HasRespawnType() ? Playlist->RespawnType > 0 : FConfiguration::bForceRespawns.load())
			: FConfiguration::bForceRespawns.load();
	}
	else
		bRespawnAllowed = GameState->Call<bool>(IsRespawningAllowedFunc, PlayerController->PlayerState);

	bool bWaxRespawnAllowed = false;
	const bool bWaxRespawnManaged =
		FFortAthenaNativeLTMCompatibility::
			TryGetWaxRespawnAllowed(
				PlayerController->PlayerState,
				bWaxRespawnAllowed);
	if (bWaxRespawnManaged)
	{
		bRespawnAllowed = bWaxRespawnAllowed;
	}
	else
	{
		bool bDiscoRespawnAllowed = false;
		if (FFortAthenaNativeLTMCompatibility::
				TryGetDiscoRespawnAllowed(
					PlayerController->PlayerState,
					bDiscoRespawnAllowed))
		{
			bRespawnAllowed = bDiscoRespawnAllowed;
		}
		else
		{
			bool bFoodFightRespawnAllowed = false;
			if (FFortAthenaNativeLTMCompatibility::
					TryGetFoodFightRespawnAllowed(
						PlayerController->PlayerState,
						bFoodFightRespawnAllowed))
			{
				bRespawnAllowed =
					bFoodFightRespawnAllowed;
			}
			else
			{
				bRespawnAllowed |=
					FConfiguration::bForceRespawns;
			}
		}
	}

	if (IsManagedNonRespawningBot(PlayerController))
		bRespawnAllowed = false;

	// Inventory count is not a lifecycle signal: death may legitimately leave a
	// respawning player with zero entries. ClientOnPawnDied clears the last-pawn
	// record specifically for a real player death, while an aircraft jump keeps
	// it. Requiring that cleared record prevents the bus pawn replacement from
	// being mistaken for a death respawn even if its acknowledgement arrives
	// after the game has advanced to SafeZones.
	const bool bRestoringRespawnPawn = !bPendingLateGameAircraftLoadout &&
		!bHadAcknowledgedPawn && bNewAcknowledgedPawn && bRespawnAllowed &&
		PlayersInitialized.contains(PlayerController);
	if (bRestoringRespawnPawn)
	{
		GPendingRespawnLandingFinalization.insert(PlayerController);

		// RestartPlayer/Possess already completed the native replacement-pawn
		// lifecycle before this client acknowledgement. Calling
		// RespawnPlayerAfterDeath here starts another pawn transition and feeds back
		// into ServerAcknowledgePossession on legacy builds.
		ClearRespawnBlockingAbilityState(PlayerController, FortPawn);

		PlayerController->StateName = FName(L"Playing");
		PlayerController->ClientGotoState(FName(L"Playing"));
		PlayerController->OnRep_Pawn();
	}

	FVector ConfiguredRespawnLocation{};
	bool bSkipRespawn = ConsumePossessRespawnSkip(PlayerController);
	if (bRestoringRespawnPawn && !bWaxRespawnManaged &&
		!bSkipRespawn &&
		TryGetConfiguredRespawnLocation(GameMode, ConfiguredRespawnLocation))
		FortPawn->K2_TeleportTo(ConfiguredRespawnLocation, FRotator(0.f, 0.f, 0.f));

	if (wcsstr(FConfiguration::Playlist, L"/Buddy/Playlist/Playlist_Retrac_1v1.Playlist_Retrac_1v1") && VersionInfo.FortniteVersion == 14.40)
	{
		FortPawn->SetShield(100.f);
		FortPawn->K2_TeleportTo(FVector(1025.170532, 1032.200562, 3732.324951), FRotator(0.f, 0.f, 0.f));

		std::vector<std::pair<FGuid, int>> GuidsAndCountsToRemove;
		auto& ItemInstances = PlayerController->WorldInventory->Inventory.ItemInstances;

		for (int i = 0; i < ItemInstances.Num(); ++i)
		{
			auto ItemInstance = ItemInstances[i];
			auto& ItemEntry = ItemInstance->GetItemEntry();
			const auto ItemDefinition = ItemEntry.ItemDefinition;

			if (ItemDefinition->HasbCanBeDropped() ? ItemDefinition->bCanBeDropped : (ItemDefinition->GetPickupComponent() ? ItemDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false))
			{
				GuidsAndCountsToRemove.push_back({ ItemEntry.ItemGuid, ItemEntry.Count });
			}
		}

		for (auto& [Guid, Count] : GuidsAndCountsToRemove)
		{
			PlayerController->WorldInventory->Remove(Guid);
		}

		auto Rifle = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_R_Ore_T03.WID_Assault_Auto_Athena_R_Ore_T03");
		auto Shotgun = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_SR_Ore_T03.WID_Shotgun_Standard_Athena_SR_Ore_T03");
		auto Grappler = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_GH.WID_Boss_Adventure_GH");
		auto Shields = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/ShieldSmall/Athena_ShieldSmall.Athena_ShieldSmall");
		auto RifleAmmo = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium");
		auto Shells = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells");

		int32 RifleClipSize = 0;
		int32 ShotgunClipSize = 0;
		int32 GrapClipSize = 0;

		if (auto WeaponDef = Rifle->Cast<UFortWeaponItemDefinition>())
		{
			auto Stats = AFortInventory::GetStats(WeaponDef);

			if (Stats && Stats != (void*)-1)
			{
				RifleClipSize = Stats->ClipSize;
			}
		}

		if (auto WeaponDef = Shotgun->Cast<UFortWeaponItemDefinition>())
		{
			auto Stats = AFortInventory::GetStats(WeaponDef);

			if (Stats && Stats != (void*)-1)
			{
				ShotgunClipSize = Stats->ClipSize;
			}
		}

		if (auto WeaponDef = Grappler->Cast<UFortWeaponItemDefinition>())
		{
			auto Stats = AFortInventory::GetStats(WeaponDef);

			if (Stats && Stats != (void*)-1)
			{
				GrapClipSize = Stats->ClipSize;
			}
		}

		PlayerController->WorldInventory->GiveItem(Rifle, 1, RifleClipSize);
		PlayerController->WorldInventory->GiveItem(Shotgun, 1, ShotgunClipSize);
		PlayerController->WorldInventory->GiveItem(Grappler, 1, GrapClipSize);
		PlayerController->WorldInventory->GiveItem(Shields, 6);
		PlayerController->WorldInventory->GiveItem(RifleAmmo, 500);
		PlayerController->WorldInventory->GiveItem(Shells, 150);

		if (UAbilitySystemComponent* AbilitySystemComponent = PlayerController->PlayerState->AbilitySystemComponent)
		{
			static auto FallDamageGE = FindObject<UClass>(L"/Game/Athena/Items/Gameplay/Backpacks/Ashton/GE_AshtonPack_FallDamageImmune.GE_AshtonPack_FallDamageImmune_C");

			if (FallDamageGE)
			{
				FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

				Context.Instigator = PlayerController;
				Context.Causer = FortPawn;
				Context.AddSourceObject(FortPawn);

				AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(FallDamageGE, 1.0f, Context);
			}
		}
	}

	if (wcsstr(FConfiguration::Playlist, L"/Buddy/Playlist/Playlist_Retrac_Turtle.Playlist_Retrac_Turtle") && VersionInfo.FortniteVersion == 14.40)
	{
		FortPawn->SetShield(100.f);

		static bool bSeeded = false;

		if (!bSeeded)
		{
			std::srand(static_cast<unsigned>(std::time(nullptr)));
			bSeeded = true;
		}

		FVector Locations[10] =
		{
			FVector(-512.153992, 3585.492188, 252.123230),
			FVector(-4096.658203, 1032.745239, 635.234924),
			FVector(515.153687, 3591.450195, 660.417908),
			FVector(2047.682983, 3579.970947, 276.955139),
			FVector(3588.712158, 1525.675903, 659.587708),
			FVector(2043.231689, -1022.355164, 1044.846069),
			FVector(1.765957, -1030.864868, 276.547546),
			FVector(-1020.748230, -1042.795898, 656.359070),
			FVector(-2571.271240, -1023.585022, 275.827240),
			FVector(-2562.445312, 3585.545410, 1045.050293)
		};

		int RandomIndex = std::rand() % 10;
		FVector RandomLoc = Locations[RandomIndex];

		FortPawn->K2_TeleportTo(RandomLoc, FRotator(0.f, 0.f, 0.f));

		std::vector<std::pair<FGuid, int>> GuidsAndCountsToRemove;
		auto& ItemInstances = PlayerController->WorldInventory->Inventory.ItemInstances;

		for (int i = 0; i < ItemInstances.Num(); ++i)
		{
			auto ItemInstance = ItemInstances[i];
			auto& ItemEntry = ItemInstance->GetItemEntry();
			const auto ItemDefinition = ItemEntry.ItemDefinition;

			if (ItemDefinition->HasbCanBeDropped() ? ItemDefinition->bCanBeDropped : (ItemDefinition->GetPickupComponent() ? ItemDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false))
			{
				GuidsAndCountsToRemove.push_back({ ItemEntry.ItemGuid, ItemEntry.Count });
			}
		}

		for (auto& [Guid, Count] : GuidsAndCountsToRemove)
		{
			PlayerController->WorldInventory->Remove(Guid);
		}

		auto Rifle = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_R_Ore_T03.WID_Assault_Auto_Athena_R_Ore_T03");
		auto Shotgun = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_SR_Ore_T03.WID_Shotgun_Standard_Athena_SR_Ore_T03");
		auto SMG = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Pistol_AutoHeavyPDW_Athena_R_Ore_T03.WID_Pistol_AutoHeavyPDW_Athena_R_Ore_T03");
		auto Shields = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/ShieldSmall/Athena_ShieldSmall.Athena_ShieldSmall");
		auto RifleAmmo = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium");
		auto Shells = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells");
		auto LightAmmo = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight");

		int32 RifleClipSize = 0;
		int32 ShotgunClipSize = 0;
		int32 SMGClipSize = 0;

		if (auto WeaponDef = Rifle->Cast<UFortWeaponItemDefinition>())
		{
			auto Stats = AFortInventory::GetStats(WeaponDef);

			if (Stats && Stats != (void*)-1)
			{
				RifleClipSize = Stats->ClipSize;
			}
		}

		if (auto WeaponDef = Shotgun->Cast<UFortWeaponItemDefinition>())
		{
			auto Stats = AFortInventory::GetStats(WeaponDef);

			if (Stats && Stats != (void*)-1)
			{
				ShotgunClipSize = Stats->ClipSize;
			}
		}

		if (auto WeaponDef = SMG->Cast<UFortWeaponItemDefinition>())
		{
			auto Stats = AFortInventory::GetStats(WeaponDef);

			if (Stats && Stats != (void*)-1)
			{
				SMGClipSize = Stats->ClipSize;
			}
		}

		PlayerController->WorldInventory->GiveItem(Rifle, 1, RifleClipSize);
		PlayerController->WorldInventory->GiveItem(Shotgun, 1, ShotgunClipSize);
		PlayerController->WorldInventory->GiveItem(SMG, 1, SMGClipSize);
		PlayerController->WorldInventory->GiveItem(Shields, 6);
		PlayerController->WorldInventory->GiveItem(RifleAmmo, 500);
		PlayerController->WorldInventory->GiveItem(Shells, 150);
		PlayerController->WorldInventory->GiveItem(LightAmmo, 500);
	}

	if (wcsstr(FConfiguration::Playlist, L"/Game/Jett/TiltedZW/Playlist_TiltedZW_Jett.Playlist_TiltedZW_Jett") && VersionInfo.FortniteVersion == 27.11)
	{
		FortPawn->SetShield(100.f);

		std::random_device rd;
		std::mt19937 rng(rd());

		std::uniform_int_distribution<int> Heavy(50, 186);
		std::uniform_int_distribution<int> ShellAmmo(87, 576);
		std::uniform_int_distribution<int> Medium(124, 824);
		std::uniform_int_distribution<int> Light(186, 824);

		std::uniform_int_distribution<int> Mats(186, 646);
		std::uniform_int_distribution<int> Gold(1200, 7500);

		auto RifleAmmo = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium");
		auto Shells = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells");
		auto LightAmmo = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight");
		auto HeavyAmmo = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy");
		auto RocketAmmo = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AmmoDataRockets.AmmoDataRockets");

		auto Wood = UFortKismetLibrary::K2_GetResourceItemDefinition(
			EFortResourceType::Wood);
		auto Stone = UFortKismetLibrary::K2_GetResourceItemDefinition(
			EFortResourceType::Stone);
		auto Metal = UFortKismetLibrary::K2_GetResourceItemDefinition(
			EFortResourceType::Metal);

		PlayerController->WorldInventory->GiveItem(RifleAmmo, Medium(rng));
		PlayerController->WorldInventory->GiveItem(Shells, ShellAmmo(rng));
		PlayerController->WorldInventory->GiveItem(LightAmmo, Light(rng));
		PlayerController->WorldInventory->GiveItem(HeavyAmmo, Heavy(rng));
		PlayerController->WorldInventory->GiveItem(RocketAmmo, 12);

		PlayerController->WorldInventory->GiveItemToSingleStack(
			Wood, Mats(rng), false);
		PlayerController->WorldInventory->GiveItemToSingleStack(
			Stone, Mats(rng), false);
		PlayerController->WorldInventory->GiveItemToSingleStack(
			Metal, Mats(rng), false);
	}

	if (wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Respawn/Playlist_Respawn_Solo.Playlist_Respawn_Solo") && VersionInfo.FortniteVersion == 30.00 && GUI::GetSelectedPlaylist() == static_cast<int>(Playlist::Boxfight))
	{
		FortPawn->SetShield(100.f);

		std::vector<std::pair<FGuid, int>> GuidsAndCountsToRemove;
		auto& ItemInstances = PlayerController->WorldInventory->Inventory.ItemInstances;

		for (int i = 0; i < ItemInstances.Num(); ++i)
		{
			auto ItemInstance = ItemInstances[i];
			auto& ItemEntry = ItemInstance->GetItemEntry();
			const auto ItemDefinition = ItemEntry.ItemDefinition;

			if (ItemDefinition->HasbCanBeDropped() ? ItemDefinition->bCanBeDropped : (ItemDefinition->GetPickupComponent() ? ItemDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false))
			{
				GuidsAndCountsToRemove.push_back({ ItemEntry.ItemGuid, ItemEntry.Count });
			}
		}

		for (auto& [Guid, Count] : GuidsAndCountsToRemove)
		{
			PlayerController->WorldInventory->Remove(Guid);
		}

		auto Rifle = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_R_Ore_T03.WID_Assault_Auto_Athena_R_Ore_T03");
		auto Shotgun = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_SR_Ore_T03.WID_Shotgun_Standard_Athena_SR_Ore_T03");
		auto SMG = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Pistol_AutoHeavyPDW_Athena_R_Ore_T03.WID_Pistol_AutoHeavyPDW_Athena_R_Ore_T03");
		auto Shields = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Consumables/ShieldSmall/Athena_ShieldSmall.Athena_ShieldSmall");
		auto RifleAmmo = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium");
		auto Shells = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells");
		auto LightAmmo = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight");

		auto Wood = UFortKismetLibrary::K2_GetResourceItemDefinition(
			EFortResourceType::Wood);
		auto Stone = UFortKismetLibrary::K2_GetResourceItemDefinition(
			EFortResourceType::Stone);
		auto Metal = UFortKismetLibrary::K2_GetResourceItemDefinition(
			EFortResourceType::Metal);

		int32 RifleClipSize = 0;
		int32 ShotgunClipSize = 0;
		int32 SMGClipSize = 0;

		if (auto WeaponDef = Rifle->Cast<UFortWeaponItemDefinition>())
		{
			auto Stats = AFortInventory::GetStats(WeaponDef);

			if (Stats && Stats != (void*)-1)
			{
				RifleClipSize = Stats->ClipSize;
			}
		}

		if (auto WeaponDef = Shotgun->Cast<UFortWeaponItemDefinition>())
		{
			auto Stats = AFortInventory::GetStats(WeaponDef);

			if (Stats && Stats != (void*)-1)
			{
				ShotgunClipSize = Stats->ClipSize;
			}
		}

		if (auto WeaponDef = SMG->Cast<UFortWeaponItemDefinition>())
		{
			auto Stats = AFortInventory::GetStats(WeaponDef);

			if (Stats && Stats != (void*)-1)
			{
				SMGClipSize = Stats->ClipSize;
			}
		}

		PlayerController->WorldInventory->GiveItem(Rifle, 1, RifleClipSize);
		PlayerController->WorldInventory->GiveItem(Shotgun, 1, ShotgunClipSize);
		PlayerController->WorldInventory->GiveItem(SMG, 1, SMGClipSize);
		PlayerController->WorldInventory->GiveItem(Shields, 6);
		PlayerController->WorldInventory->GiveItem(RifleAmmo, 500);
		PlayerController->WorldInventory->GiveItem(Shells, 150);
		PlayerController->WorldInventory->GiveItem(LightAmmo, 500);

		PlayerController->WorldInventory->GiveItemToSingleStack(
			Wood, 500, false);
		PlayerController->WorldInventory->GiveItemToSingleStack(
			Stone, 500, false);
		PlayerController->WorldInventory->GiveItemToSingleStack(
			Metal, 500, false);
	}

	if ((!FConfiguration::bKeepInventory || FConfiguration::bLateGame) && PlayerController->WorldInventory)
	{	
		if (bPendingLateGameAircraftLoadout || !PlayersInitialized.contains(PlayerController))
		{
			UEAllocatedVector<FGuid> GuidsToRemove;
			for (int i = 0; i < PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num(); i++)
			{
				auto& Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

				if (Entry.ItemDefinition->HasbCanBeDropped() ? Entry.ItemDefinition->bCanBeDropped : (Entry.ItemDefinition->GetPickupComponent() ? Entry.ItemDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false))
				{
					//NewPlayer->WorldInventory->Inventorxy.ReplicatedEntries.Remove(i, FFortItemEntry::Size());
					//i--;

					GuidsToRemove.push_back(Entry.ItemGuid);
				}
				if (VersionInfo.FortniteVersion < 3 && Entry.ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest())
				{
					//PlayerController->ServerExecuteInventoryItem(Entry.ItemGuid);
					//PlayerController->QuickBars->ServerActivateSlotInternal(0, 0, 0.f, true);
				}
			}

			for (auto& Guid : GuidsToRemove)
				PlayerController->WorldInventory->Remove(Guid);
		}
	}

	if (VersionInfo.FortniteVersion >= 18)
	{
		if (VersionInfo.FortniteVersion >= 25.20)
		{
			static auto Effect = FindObject<UClass>(L"/Game/Athena/SafeZone/GE_OutsideSafeZoneDamage.GE_OutsideSafeZoneDamage_C");

			bool Found = false;
			auto AbilitySystemComponent = PlayerController->PlayerState->AbilitySystemComponent;

			for (int i = 0; i < AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal.Num(); i++)
			{
				auto& ActiveEffect = AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal.Get(i, FActiveGameplayEffect::Size());

				if (ActiveEffect.Spec.Def)
					if (ActiveEffect.Spec.Def->IsA(Effect))
					{
						Found = true;
						break;
					}
			}

			if (!Found)
			{
				auto EffectHandle = FGameplayEffectContextHandle();
				auto SpecHandle = AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(Effect, 0.f, EffectHandle);

				//AbilitySystemComponent->SetActiveGameplayEffectLevel(SpecHandle, 1);

				AbilitySystemComponent->UpdateActiveGameplayEffectSetByCallerMagnitude(SpecHandle,
					FGameplayTag(FName(L"SetByCaller.StormCampingDamage")), 1.f);
			}
		}

		FortPawn->bIsInAnyStorm = false;
		FortPawn->OnRep_IsInAnyStorm();
		FortPawn->bIsInsideSafeZone = true;
		FortPawn->OnRep_IsInsideSafeZone();
	}

	EnsurePawnGameplayAbilitiesInitialized(
		PlayerController, FortPawn);
	AFortInventory::HandlePendingCarmineFocus(
		PlayerController);

	if (bRestoringRespawnPawn)
	{
		// Ability initialization can restore persistent death/DBNO state, so final
		// cleanup must run after it and before any item is equipped.
		ClearRespawnBlockingAbilityState(PlayerController, FortPawn);
		ResetLowerSeasonStormStateForRespawn(PlayerController, nullptr, FortPawn);

		auto AbilitySystemComponent = PlayerController->PlayerState
			? PlayerController->PlayerState->AbilitySystemComponent : nullptr;
		bool bWasActivationInhibited = false;
		bool bIsActivationInhibited = false;
		if (AbilitySystemComponent)
		{
			if (auto GetInhibitedFn = AbilitySystemComponent->GetFunction("GetUserAbilityActivationInhibited"))
				bWasActivationInhibited = AbilitySystemComponent->Call<bool>(GetInhibitedFn);

			if (bWasActivationInhibited)
				if (auto SetInhibitedFn = AbilitySystemComponent->GetFunction("SetUserAbilityActivationInhibited"))
					AbilitySystemComponent->Call<void>(SetInhibitedFn, false);

			if (auto GetInhibitedFn = AbilitySystemComponent->GetFunction("GetUserAbilityActivationInhibited"))
				bIsActivationInhibited = AbilitySystemComponent->Call<bool>(GetInhibitedFn);
		}

		SDK::DbgLog(
			"[Respawn] ASC lifecycle controller=%p owner=%p avatar=%p expectedOwner=%p expectedAvatar=%p inhibited=%d->%d\n",
			(void*)PlayerController,
			(void*)(AbilitySystemComponent && AbilitySystemComponent->HasOwnerActor()
				? AbilitySystemComponent->OwnerActor : nullptr),
			(void*)(AbilitySystemComponent && AbilitySystemComponent->HasAvatarActor()
				? AbilitySystemComponent->AvatarActor : nullptr),
			(void*)PlayerController->PlayerState, (void*)FortPawn,
			(int)bWasActivationInhibited, (int)bIsActivationInhibited);

		if (PlayerController->HasbMarkedAlive())
			PlayerController->bMarkedAlive = true;
		if (PlayerController->HasbClientNotifiedOfPawnDied())
			PlayerController->bClientNotifiedOfPawnDied = false;

		ClearLegacyPlayerStateDeathFlags(
			(AFortPlayerStateAthena*)PlayerController->PlayerState);

		// This reliable client notification performs the client-side half of the
		// replacement-pawn lifecycle (including releasing death input/action
		// state). Keep it one-shot here; ClientRestart or another server respawn
		// from this acknowledgement would create a possession feedback loop.
		PlayerController->ClientOnPawnSpawned();

		bool bControllerInputIgnored = false;
		if (auto IsIgnoredFn = PlayerController->GetFunction("IsActionInputIgnored"))
			bControllerInputIgnored = PlayerController->Call<bool>(IsIgnoredFn);
		bool bPawnInputIgnored = false;
		if (auto IsIgnoredFn = FortPawn->GetFunction("IsActionInputIgnored"))
			bPawnInputIgnored = FortPawn->Call<bool>(IsIgnoredFn);
		SDK::DbgLog(
			"[Respawn] client lifecycle controller=%p deathInput=%p actionIgnored=%d pawnIgnored=%d\n",
			(void*)PlayerController,
			(void*)(PlayerController->HasDeathInputComponent()
				? PlayerController->DeathInputComponent : nullptr),
			(int)bControllerInputIgnored, (int)bPawnInputIgnored);

		if (FConfiguration::bLateGame && !FConfiguration::bKeepInventory)
			LateGame::EquipLoadout(PlayerController);

		RestoreEquipmentAfterRespawn(PlayerController);
	}

	if (FConfiguration::bForceRespawns && PlayersInitialized.contains(PlayerController))
	{
		FortPawn->SetHealth(100.f);
		// Infinite respawns should not turn a normal BR aircraft jump into a
		// full-shield spawn. Match native Erbium and leave the fresh pawn's zero
		// shield untouched unless late game explicitly grants full shield.
		if (FConfiguration::bLateGame)
			ApplyLateGameSpawnShield(FortPawn);
	}

	if (Num == 0)
	{
		static auto SmartItemDefClass = FindClass("FortSmartBuildingItemDefinition");
		static bool HasCosmeticLoadoutPC = PlayerController->HasCosmeticLoadoutPC();
		static bool HasCustomizationLoadout = PlayerController->HasCustomizationLoadout();

		if (HasCosmeticLoadoutPC && PlayerController->CosmeticLoadoutPC.Pickaxe)
			PlayerController->WorldInventory->GiveItem(PlayerController->CosmeticLoadoutPC.Pickaxe->WeaponDefinition);
		else if (HasCustomizationLoadout && PlayerController->CustomizationLoadout.Pickaxe)
			PlayerController->WorldInventory->GiveItem(PlayerController->CustomizationLoadout.Pickaxe->WeaponDefinition);
		else if (HasCosmeticLoadoutPC || HasCustomizationLoadout) // fix ur backend gng
		{
			PlayerController->WorldInventory->GiveItem(GetDefaultAthenaPickaxe());
		}
		
		if (GameMode->StartingItems.Num() == 0)
		{
			static auto WallBuild = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Wall.BuildingItemData_Wall");
			static auto FloorBuild = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Floor.BuildingItemData_Floor");
			static auto StairBuild = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Stair_W.BuildingItemData_Stair_W");
			static auto ConeBuild = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_RoofS.BuildingItemData_RoofS");
			static auto EditTool = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/EditTool.EditTool");

			PlayerController->WorldInventory->GiveItem(WallBuild);
			PlayerController->WorldInventory->GiveItem(FloorBuild);
			PlayerController->WorldInventory->GiveItem(StairBuild);
			PlayerController->WorldInventory->GiveItem(ConeBuild);
			PlayerController->WorldInventory->GiveItem(EditTool);
		}
		else
			for (int i = 0; i < GameMode->StartingItems.Num(); i++)
			{
				auto& StartingItem = GameMode->StartingItems.Get(i, FItemAndCount::Size());

				if (StartingItem.Item && StartingItem.Count && (!SmartItemDefClass || !StartingItem.Item->IsA(SmartItemDefClass)))
					PlayerController->WorldInventory->GiveItem(StartingItem.Item, StartingItem.Count);
			}

		auto pickaxeEntry = FindHarvestingToolEntry(PlayerController->WorldInventory);

		// Early builds can have StartingItems without a harvesting tool and do not
		// expose either cosmetic-loadout property. Always guarantee one pickaxe.
		if (!pickaxeEntry)
		{
			auto DefaultPickaxe = GetDefaultAthenaPickaxe();
			auto PickaxeItem = PlayerController->WorldInventory->GiveItem(DefaultPickaxe);
			pickaxeEntry = FindHarvestingToolEntry(PlayerController->WorldInventory);

			SDK::DbgLog("[Possession] Default pickaxe fallback: Def=%p Item=%p Entry=%p FN=%.2f\n",
				(void*)DefaultPickaxe, (void*)PickaxeItem, (void*)pickaxeEntry, VersionInfo.FortniteVersion);
		}

		// Forcing EquipWeaponDefinition and then activating slot zero creates two
		// distinct weapon transitions on the earliest clients and can leave the
		// animation graph referencing the discarded actor. Let their native
		// quickbar own the initial focus transition; preserve the established
		// path elsewhere.
		if (pickaxeEntry && !UsesEarlyAthenaLandingClientRefresh())
		{
			PlayerController->ServerExecuteInventoryItem(pickaxeEntry->ItemGuid);

			if (VersionInfo.FortniteVersion > 3.00)
				PlayerController->ClientEquipItem(pickaxeEntry->ItemGuid, true);
			else if (PlayerController->QuickBars)
				PlayerController->QuickBars->ServerActivateSlotInternal(0, 0, 0.f, true);
		}

		UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization(PlayerController->PlayerState);
		if (!UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization__Ptr && ApplyCharacterCustomization)
		{
			((void (*)(AActor*, AActor*)) ApplyCharacterCustomization)(PlayerController->PlayerState, Pawn);
		}

		// Gameplay abilities were initialized once above for this pawn. Repeating
		// that work here used to apply the base and playlist ability sets twice
		// on an empty-inventory spawn, corrupting One Shot's shield aggregator.
	}
	if (FConfiguration::bLateGame && (bPendingLateGameAircraftLoadout || Num != 0) &&
		(!FConfiguration::bKeepInventory || FConfiguration::bLateGame))
	{
		if (bPendingLateGameAircraftLoadout || !PlayersInitialized.contains(PlayerController))
		{
			PlayersInitialized.insert(PlayerController);

			ApplyLateGameSpawnShield(FortPawn);

			LateGame::EquipLoadout(PlayerController);
			SDK::DbgLog("[LateGame] aircraft loadout granted controller=%p pending=%d initialEntries=%d finalEntries=%d\n",
				(void*)PlayerController, (int)bPendingLateGameAircraftLoadout, Num,
				PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num());

			if (UsesEarlyAthenaLandingClientRefresh() &&
				bPendingLateGameAircraftLoadout)
			{
				GPendingLegacyAircraftLandingEquipment.insert(PlayerController);
				GLegacyAircraftSkydivingObserved.erase(PlayerController);
			}

			if (GUI::IsArenaPlaylist() && FConfiguration::RandomizeArenaPoints && !FConfiguration::bForceRespawns)
			{
				std::random_device rd;
				std::mt19937 rng(rd());

				std::uniform_int_distribution<int> RandomAmount(6732, 14684);

				int AlivePlayers = GameMode->AlivePlayers.Num();

				PlayerController->ClientReportTournamentPlacementPointsScored(AlivePlayers, RandomAmount(rng));
			}

			if (GUI::IsTournamentPlaylist() && FConfiguration::RandomizeArenaPoints && !FConfiguration::bForceRespawns)
			{
				std::random_device rd;
				std::mt19937 rng(rd());

				std::uniform_int_distribution<int> RandomAmount(30, 120);

				int AlivePlayers = GameMode->AlivePlayers.Num();

				PlayerController->ClientReportTournamentPlacementPointsScored(AlivePlayers, RandomAmount(rng));
			}

			if (FConfiguration::RandomizeKills)
			{
				auto PlayerState = PlayerController->PlayerState;

				std::random_device rd;
				std::mt19937 rng(rd());

				std::uniform_int_distribution<int> RandomAmount(8, 31);

				int RandomKills = RandomAmount(rng);

				if (PlayerState->HasKillScore())
					PlayerState->KillScore = RandomKills;
				else
					PlayerState->Kills = RandomKills;
			}
		}
	}

	if (wcsstr(FConfiguration::Playlist, L"/Game/Jett/Playlist_OnlyUp_Jett.Playlist_OnlyUp_Jett") && VersionInfo.FortniteVersion == 27.11)
	{
		if (!FConfiguration::bHasPickaxe)
		{
			for (auto& UncastedPC : GameMode->AlivePlayers)
			{
				auto PlayerController = (AFortPlayerControllerAthena*)UncastedPC;

				auto PickaxeInstance = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
					{
						return entry.ItemDefinition->Cast<UFortWeaponMeleeItemDefinition>();
					}, FFortItemEntry::Size());

				if (PickaxeInstance)
					PlayerController->WorldInventory->Remove(PickaxeInstance->ItemGuid);

				auto Hands = FindObject<UFortItemDefinition>(L"/EventMode/Items/WID_EventMode_Hands_Activator.WID_EventMode_Hands_Activator");

				PlayerController->WorldInventory->GiveItem(Hands);
			}
		}

		if (UAbilitySystemComponent* AbilitySystemComponent = PlayerController->PlayerState->AbilitySystemComponent)
		{
			static auto NoBuildGE = FindObject<UClass>(L"/Game/Athena/Items/Quests/HardcoreChallenges/NoBuilding/GE_HCChallenge_NoBuilding.GE_HCChallenge_NoBuilding_C");

			if (NoBuildGE)
			{
				FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

				Context.Instigator = PlayerController;
				Context.Causer = FortPawn;
				Context.AddSourceObject(FortPawn);

				AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(NoBuildGE, 1.0f, Context);
			}
		}

	}

	EnsureOneShotLowGravityVfx(PlayerController, FortPawn);
	NormalizeShieldAfterGameplayInitialization(FortPawn);

	// Mark every successful first possession, including Num == 0. Previously
	// this happened only in the late-game/non-empty-inventory branch, causing a
	// later empty respawn to be mistaken for another initial possession.
	PlayersInitialized.insert(PlayerController);

	// Arsenal's GG mutator can be registered after its native phase callback.
	// Reconcile the already-earned tier after the authoritative pawn/inventory
	// lifecycle is complete. The helper is exact-10.40/exact-playlist gated and
	// ignores spawn-island acknowledgements before the aircraft phase.
	FFortAthenaNativeLTMCompatibility::
		HandleArsenalPlayerReady(PlayerController);
	FFortAthenaNativeLTMCompatibility::
		HandleWaxPlayerReady(PlayerController);
	FFortAthenaNativeLTMCompatibility::
		HandleDiscoPlayerReady(PlayerController);
	FFortAthenaNativeLTMCompatibility::
		HandleAshtonPlayerReady(PlayerController);

	// Very last thing: if this ack is a "possess" command takeover, put the pawn
	// on the ground and give it health. Nothing above overrode it because this
	// runs after the native possession setup and the ability re-init that reset
	// them.
	if (GFinalizePossessTakeover.erase(PlayerController) > 0)
		FinalizePossessedPawnForCommand(PlayerController, FortPawn);

	// Reattach controller-scoped minimum-health god mode only after native
	// possession, abilities, playlist effects, and respawn health setup have
	// all finished. This also covers the listen-server controller, which is
	// not guaranteed to appear in NetDriver::ClientConnections.
	if (AFortPlayerPawnAthena::
			HasMinimumHealthGodMode(PlayerController))
	{
		AFortPlayerPawnAthena::
			SetMinimumHealthGodMode(
				PlayerController, true);
	}
}

uint32 ServerAttemptAircraftJumpVft;
void AFortPlayerControllerAthena::ServerAttemptAircraftJump_(UObject* Context, FFrame& Stack)
{
	FRotator Rotation;
	Stack.StepCompiledIn(&Rotation);
	Stack.IncrementCode();

	AFortPlayerControllerAthena* PlayerController = nullptr;
	auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
	auto GameState = (AFortGameStateAthena*)GameMode->GameState;
	PlayerController = ResolveAircraftPlayerController(Context);

	// EnterAircraft is not reached reliably on every native aircraft path
	// (notably 5.41). The jump RPC is the final per-player authority checkpoint
	// before the replacement pawn is spawned, so repeat the idempotent cleanup.
	ClearDroppableInventoryForAircraft(
		PlayerController, "jump", true);

	if (VersionInfo.FortniteVersion >= 11.00 || FConfiguration::bLateGame)
	{
		if (!PlayerController)
			return;

		if (FConfiguration::bLateGame && PlayerController)
			GPendingLateGameAircraftLoadout.insert(PlayerController);

		if (FConfiguration::IsKnownS27CustomMapPlaylist())
		{
			GameMode->RestartPlayer(PlayerController);
			PlayerController->ClientSetRotation(Rotation, true);
		}
		else
		{
			PlayerController->StateName = FName(L"Inactive");

			if (PlayerController->Pawn)
				PlayerController->UnPossess(PlayerController->Pawn);

			GameMode->RestartPlayer(PlayerController);
			//PlayerController->ServerRestartPlayer();
			PlayerController->SetControlRotation(Rotation);
		}

		if (PlayerController->MyFortPawn)
		{
			//PlayerController->MyFortPawn->BeginSkydiving(true);
			//PlayerController->MyFortPawn->SetHealth(100.f);
		}
	}
	else
	{
		static auto ServerAttemptAircraftJumpOG = (void(*)(AFortPlayerControllerAthena*, FRotator&)) ((AFortPlayerControllerAthena*)Context)->Vft[ServerAttemptAircraftJumpVft];

		ServerAttemptAircraftJumpOG(
			PlayerController
				? PlayerController
				: (AFortPlayerControllerAthena*)Context,
			Rotation);
	}

	// The spawn-island pawn can remain possessed through the 10.40 aircraft
	// transition, so ServerAcknowledgePossession is not guaranteed to run here.
	// Grant the authored Arsenal tier only after the pregame inventory cleanup.
	FFortAthenaNativeLTMCompatibility::HandleArsenalPlayerReady(PlayerController);
	FFortAthenaNativeLTMCompatibility::HandleWaxPlayerReady(PlayerController);
	FFortAthenaNativeLTMCompatibility::HandleDiscoPlayerReady(PlayerController);
	FFortAthenaNativeLTMCompatibility::HandleAshtonPlayerReady(PlayerController);
	
	if (FConfiguration::bLateGame)
	{
		ApplyLateGameSpawnShield(PlayerController->MyFortPawn);
		auto Aircraft = GameState->HasAircrafts() ? GameState->Aircrafts[0] : (GameState->HasAircraft() ? GameState->Aircraft : nullptr);
		if (!Aircraft) // gamephaselogic builds
		{
			auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

			Aircraft = GamePhaseLogic->Aircrafts_GameState[0].Get();
		}

		FVector AircraftLocation = Aircraft->K2_GetActorLocation();

		float Angle = (float)rand() / 5215.03002625f;
		float Radius = (float)(rand() % 1000);

		float OffsetX = cosf(Angle) * Radius;
		float OffsetY = sinf(Angle) * Radius;

		FVector Offset;
		Offset.X = OffsetX;
		Offset.Y = OffsetY;
		Offset.Z = 0.0f;

		FVector NewLoc = AircraftLocation + Offset;

		PlayerController->MyFortPawn->K2_SetActorLocation(NewLoc, false, nullptr, true);

	}
}

void AFortPlayerControllerAthena::ServerExecuteInventoryItem_(UObject* Context, FFrame& Stack)
{
	FGuid ItemGuid;
	Stack.StepCompiledIn(&ItemGuid);
	Stack.IncrementCode();

	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	if (!PlayerController || !PlayerController->MyFortPawn)
		return;

	auto entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
		{
			return entry.ItemGuid == ItemGuid;
		}, FFortItemEntry::Size());

	if (!entry)
		return;

	UFortItemDefinition* RealDef = (UFortItemDefinition*)entry->ItemDefinition;

	// Special gadgets carry their own server execution path. The Infinity
	// Gauntlet and Getaway Jewel both depend on it for their native gameplay
	// state; treating either as an ordinary weapon only equips its backing
	// weapon. The validated finder fails closed when a build has no known
	// ServerExecute virtual.
	if (auto Gadget = RealDef->Cast<UFortGadgetItemDefinition>())
	{
		auto ItemInstance = PlayerController->WorldInventory->Inventory.ItemInstances.Search(
			[&](UFortWorldItem* Item)
			{
				return Item && Item->ItemEntry.ItemGuid == ItemGuid;
			});

		if (ItemInstance && *ItemInstance &&
			Gadget->ServerExecute((UFortItem*)*ItemInstance, PlayerController))
		{
			return;
		}
	}

	auto UncastedDef = RealDef;

	if (auto Gadget = RealDef->Cast<UFortGadgetItemDefinition>())
		UncastedDef = Gadget->GetWeaponItemDefinition();

	auto ItemDefinition = UncastedDef->Cast<UFortWeaponItemDefinition>();

	if (!ItemDefinition)
		return;

	auto Weapon = PlayerController->MyFortPawn->EquipWeaponDefinition(ItemDefinition, ItemGuid, entry->HasTrackerGuid() ? entry->TrackerGuid : FGuid(), false);

	if (!Weapon)
		return;

	FFortWeaponMods::ApplyEntrySlotsAfterEquip(
		(AFortWeapon*)Weapon, *entry);

	if (VersionInfo.FortniteVersion <= 2.5)
	{
		static auto BuildingToolClass = FindClass("FortWeap_BuildingTool");
		if (Weapon->IsA(BuildingToolClass))
		{
			static auto RoofPiece = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_RoofS.BuildingItemData_RoofS");
			static auto FloorPiece = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Floor.BuildingItemData_Floor");
			static auto WallPiece = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Wall.BuildingItemData_Wall");
			static auto StairPiece = FindObject<UFortItemDefinition>(L"/Game/Items/Weapons/BuildingTools/BuildingItemData_Stair_W.BuildingItemData_Stair_W");

			static auto RoofMetadata = FindObject<UObject>(L"/Game/Building/EditModePatterns/Roof/EMP_Roof_RoofC.EMP_Roof_RoofC");
			static auto StairMetadata = FindObject<UObject>(L"/Game/Building/EditModePatterns/Stair/EMP_Stair_StairW.EMP_Stair_StairW");
			static auto WallMetadata = FindObject<UObject>(L"/Game/Building/EditModePatterns/Wall/EMP_Wall_Solid.EMP_Wall_Solid");
			static auto FloorMetadata = FindObject<UObject>(L"/Game/Building/EditModePatterns/Floor/EMP_Floor_Floor.EMP_Floor_Floor");

			static auto DefaultMetadataOffset = Weapon->GetOffset("DefaultMetadata");
			static auto OnRep_DefaultMetadata = Weapon->GetFunction("OnRep_DefaultMetadata");

			if (ItemDefinition == RoofPiece)
				GetFromOffset<const UObject*>(Weapon, DefaultMetadataOffset) = RoofMetadata;
			else if (ItemDefinition == StairPiece)
				GetFromOffset<const UObject*>(Weapon, DefaultMetadataOffset) = StairMetadata;
			else if (ItemDefinition == WallPiece)
				GetFromOffset<const UObject*>(Weapon, DefaultMetadataOffset) = WallMetadata;
			else if (ItemDefinition == FloorPiece)
				GetFromOffset<const UObject*>(Weapon, DefaultMetadataOffset) = FloorMetadata;

			Weapon->ProcessEvent(OnRep_DefaultMetadata, nullptr);
		}
	}

	if (auto DecoTool = Weapon->Cast<AFortDecoTool>())
	{
		DecoTool->SetDecoObjectPreview(ItemDefinition, true);
		if (!AFortDecoTool::SetDecoObjectPreview__Ptr)
			DecoTool->ItemDefinition = ItemDefinition;

		if (auto ContextTrapTool = Weapon->Cast<AFortDecoTool_ContextTrap>())
			ContextTrapTool->ContextTrapItemDefinition = (UFortContextTrapItemDefinition*)ItemDefinition;
	}
	Weapon->ForceNetUpdate();

	if (PlayerController->MyFortPawn->HasVehicleInputComponent())
	{
		if (auto Gadget = RealDef->Cast<UFortGadgetItemDefinition>())
		{
			if (!Gadget->bValidForLastEquipped)
				return;
		}
		else if (!ItemDefinition->bValidForLastEquipped)
			return;

		*(FGuid*)(__int64(&PlayerController->MyFortPawn->VehicleInputComponent) - 0x20) = ItemGuid;
	}
}

void AFortPlayerControllerAthena::ServerExecuteInventoryWeapon(UObject* Context, FFrame& Stack)
{
	AFortWeapon* Weapon;
	Stack.StepCompiledIn(&Weapon);
	Stack.IncrementCode();

	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	if (!PlayerController || !Weapon || !PlayerController->WorldInventory)
		return;

	auto entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
		{
			return entry.ItemGuid == Weapon->ItemEntryGuid;
		}, FFortItemEntry::Size());

	if (!entry || !PlayerController->MyFortPawn)
		return;

	UFortItemDefinition* RealDef = (UFortItemDefinition*)entry->ItemDefinition;
	auto UncastedDef = RealDef;

	if (auto Gadget = RealDef->Cast<UFortGadgetItemDefinition>())
		UncastedDef = Gadget->GetWeaponItemDefinition();

	auto ItemDefinition = UncastedDef->Cast<UFortWeaponItemDefinition>();
	if (!ItemDefinition)
		return;

	auto EquippedWeapon = (AFortWeapon*)PlayerController->MyFortPawn->EquipWeaponDefinition(
		ItemDefinition,
		entry->ItemGuid,
		entry->HasTrackerGuid() ? entry->TrackerGuid : FGuid(),
		false);
	if (!EquippedWeapon)
		return;

	Weapon = EquippedWeapon;
	FFortWeaponMods::ApplyEntrySlotsAfterEquip(Weapon, *entry);

	if (auto DecoTool = Weapon->Cast<AFortDecoTool>())
	{
		DecoTool->SetDecoObjectPreview(ItemDefinition, true);
		if (!AFortDecoTool::SetDecoObjectPreview__Ptr)
			DecoTool->ItemDefinition = ItemDefinition;

		if (auto ContextTrapTool = Weapon->Cast<AFortDecoTool_ContextTrap>())
			ContextTrapTool->ContextTrapItemDefinition = (UFortContextTrapItemDefinition*)ItemDefinition;
	}
	Weapon->ForceNetUpdate();

	if (PlayerController->MyFortPawn->HasVehicleInputComponent())
	{
		if (auto Gadget = RealDef->Cast<UFortGadgetItemDefinition>())
		{
			if (!Gadget->bValidForLastEquipped)
				return;
		}
		else if (!ItemDefinition->bValidForLastEquipped)
			return;

		*(FGuid*)(__int64(&PlayerController->MyFortPawn->VehicleInputComponent) - 0x20) = entry->ItemGuid;
	}
}

bool CanBePlacedByPlayer(TSubclassOf<AActor> BuildClass)
{
	return ((ABuildingSMActor*)BuildClass->GetDefaultObj())->bIsPlayerBuildable;
	/*auto GameState = ((AFortGameStateAthena*)UWorld::GetWorld()->GameState);
	static auto HasAllPlayerBuildableClasses = GameState->HasAllPlayerBuildableClasses();
	return HasAllPlayerBuildableClasses ? GameState->AllPlayerBuildableClasses.Search([BuildClass](TSubclassOf<AActor> Class)
		{ return Class == BuildClass; }) != 0 : true;*/
}

uint64_t CantBuild_ = 0;
uint64_t CanAffordToPlaceBuildableClass_;
uint64_t PayBuildableClassPlacementCost_;
uint64_t CanPlaceBuildableClassInStructuralGrid_;
void AFortPlayerControllerAthena::ServerCreateBuildingActor(UObject* Context, FFrame& Stack)
{
	TSubclassOf<AActor> BuildingClass;
	FVector BuildLoc;
	FRotator BuildRot;
	bool bMirrored;
	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	struct _Pad_0xC
	{
		uint8_t Padding[0xC];
	};
	struct _Pad_0x18
	{
		uint8_t Padding[0x18];
	};

	FBuildingClassData BuildingClassData;
	if (VersionInfo.FortniteVersion >= 8.30)
	{
		struct FCreateBuildingActorData { uint32_t BuildingClassHandle; _Pad_0xC BuildLoc; _Pad_0xC BuildRot; bool bMirrored; uint8_t Pad_1[0x3]; float SyncKey; uint8 Pad_2[0x4]; FBuildingClassData BuildingClassData; };
		struct FCreateBuildingActorData_New { uint32_t BuildingClassHandle; uint8_t Pad_1[0x4]; _Pad_0x18 BuildLoc; _Pad_0x18 BuildRot; bool bMirrored; uint8_t Pad_2[0x3]; float SyncKey; FBuildingClassData BuildingClassData; };

		if (VersionInfo.FortniteVersion >= 20.00)
		{
			FCreateBuildingActorData_New CreateBuildingData;
			Stack.StepCompiledIn(&CreateBuildingData);

			BuildLoc = *(FVector*)&CreateBuildingData.BuildLoc;
			BuildRot = *(FRotator*)&CreateBuildingData.BuildRot;
			bMirrored = CreateBuildingData.bMirrored;
			BuildingClassData = CreateBuildingData.BuildingClassData;

			BuildingClass = AFortGameStateAthena::BuildingClassMap[CreateBuildingData.BuildingClassHandle];
			if (!BuildingClass)
			{
				Stack.IncrementCode();
				return;
			}
		}
		else
		{
			FCreateBuildingActorData CreateBuildingData;
			Stack.StepCompiledIn(&CreateBuildingData);

			BuildLoc = *(FVector*)&CreateBuildingData.BuildLoc;
			BuildRot = *(FRotator*)&CreateBuildingData.BuildRot;
			bMirrored = CreateBuildingData.bMirrored;
			BuildingClassData = CreateBuildingData.BuildingClassData;

			BuildingClass = AFortGameStateAthena::BuildingClassMap[CreateBuildingData.BuildingClassHandle];
			if (!BuildingClass)
			{
				Stack.IncrementCode();
				return;
			}
		}
		BuildingClassData.BuildingClass = BuildingClass;
	}
	else
	{
		Stack.StepCompiledIn(&BuildingClassData);
		Stack.StepCompiledIn(&BuildLoc);
		Stack.StepCompiledIn(&BuildRot);
		Stack.StepCompiledIn(&bMirrored);

		auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
		static auto HasAllPlayerBuildableClasses = GameState->HasAllPlayerBuildableClasses();
		if (HasAllPlayerBuildableClasses && !GameState->AllPlayerBuildableClasses.Contains(BuildingClassData.BuildingClass))
		{
			Stack.IncrementCode();
			return;
		}

		BuildingClass = BuildingClassData.BuildingClass;
	}
	Stack.IncrementCode();

	if (!BuildingClass)
		return;

	UFortWorldItem* Item = nullptr;
	auto Resource = UFortKismetLibrary::K2_GetResourceItemDefinition(((ABuildingSMActor*)BuildingClass->GetDefaultObj())->ResourceType);
	if (!FConfiguration::bInfiniteMats)
	{
		auto CanAffordToPlaceBuildableClass = (bool(*)(AFortPlayerControllerAthena*, FBuildingClassData)) CanAffordToPlaceBuildableClass_;

		if (CanAffordToPlaceBuildableClass)
		{
			if (!CanAffordToPlaceBuildableClass(PlayerController, BuildingClassData))
				return;
		}
		else if (!PlayerController->bBuildFree && !FConfiguration::bInfiniteMats)
		{
			auto ItemP = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
				{ return entry->ItemEntry.ItemDefinition == Resource; });

			if (!ItemP)
				return;

			Item = *ItemP;

			if (Item->ItemEntry.Count < 10)
				return;
		}
	}

	TArray<ABuildingSMActor*> RemoveBuildings;
	if (VersionInfo.FortniteVersion >= 27)
	{
		char _Unk_OutVar1;
		auto CantBuild = (__int64 (*)(UWorld*, TSubclassOf<AActor>&, _Pad_0x18, _Pad_0x18, bool, TArray<ABuildingSMActor*> *, char*))CantBuild_;

		if (CantBuild(UWorld::GetWorld(), BuildingClass, *(_Pad_0x18*)&BuildLoc, *(_Pad_0x18*)&BuildRot, bMirrored, &RemoveBuildings, &_Unk_OutVar1))
			return;
	}
	else
	{
		char _Unk_OutVar1;
		auto CantBuild = (__int64 (*)(UWorld*, const UClass*, _Pad_0xC, _Pad_0xC, bool, TArray<ABuildingSMActor*> *, char*))CantBuild_;
		auto CantBuildNew = (__int64 (*)(UWorld*, const UClass*, _Pad_0x18, _Pad_0x18, bool, TArray<ABuildingSMActor*> *, char*))CantBuild_;

		if (VersionInfo.FortniteVersion >= 20.00 ? CantBuildNew(UWorld::GetWorld(), BuildingClass, *(_Pad_0x18*)&BuildLoc, *(_Pad_0x18*)&BuildRot, bMirrored, &RemoveBuildings, &_Unk_OutVar1) : CantBuild(UWorld::GetWorld(), BuildingClass, *(_Pad_0xC*)&BuildLoc, *(_Pad_0xC*)&BuildRot, bMirrored, &RemoveBuildings, &_Unk_OutVar1))
			return;
	}

	for (auto& RemoveBuilding : RemoveBuildings)
		RemoveBuilding->K2_DestroyActor();
	RemoveBuildings.Free();

	static auto K2_SpawnBuildingActor = ABuildingSMActor::GetDefaultObj()->GetFunction("K2_SpawnBuildingActor");

	ABuildingSMActor* Building = nullptr;
	/*if (K2_SpawnBuildingActor)
	{
		FTransform SpawnTransform(BuildLoc, BuildRot);
		Building = ABuildingSMActor::K2_SpawnBuildingActor(PlayerController, BuildingClass, SpawnTransform, PlayerController, nullptr, false, false);
	}
	else*/
		Building = UWorld::SpawnActorUnfinished<ABuildingSMActor>(BuildingClass, BuildLoc, BuildRot, PlayerController);

		Building->InitializeKismetSpawnedBuildingActor(Building, PlayerController, true, nullptr, false);
		UWorld::FinishSpawnActor(Building, BuildLoc, BuildRot);

	if (!Building)
		return;

	static auto UpgradeLevelOffset = FBuildingClassData::StaticStruct()->GetOffset("UpgradeLevel");
	Building->CurrentBuildingLevel = VersionInfo.EngineVersion >= 5.3 ? *(uint8*)(__int64(&BuildingClassData) + UpgradeLevelOffset) : *(uint32*)(__int64(&BuildingClassData) + UpgradeLevelOffset);
	Building->OnRep_CurrentBuildingLevel();

	Building->SetMirrored(bMirrored);

	Building->bPlayerPlaced = true;

	if (!PlayerController->bBuildFree && !FConfiguration::bInfiniteMats)
	{
		auto PayBuildableClassPlacementCost = (int(*)(AFortPlayerControllerAthena*, FBuildingClassData)) PayBuildableClassPlacementCost_;

		if (PayBuildableClassPlacementCost)
		{
			PayBuildableClassPlacementCost(PlayerController, BuildingClassData);
		}
		else if (Item)
		{
			Item->ItemEntry.Count -= 10;
			PlayerController->WorldInventory->Update(&Item->ItemEntry);
		}
	}

	FGameplayTagContainer TargetTags{};

	auto Interface = (IGameplayTagAssetInterface*)Building->GetInterface(IGameplayTagAssetInterface::StaticClass());
	if (Interface)
	{
		auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface->Vft[0x2];
		GetOwnedGameplayTags(Interface, &TargetTags);
		//Interface->GetOwnedGameplayTags(&TargetTags);
	}

	PlayerController->GetQuestManager(1)->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetBuild(), 1, Building);

	TargetTags.GameplayTags.Free();
	TargetTags.ParentTags.Free();

	if (((AFortPlayerStateAthena*)PlayerController->PlayerState)->HasTeamIndex()) 
		Building->Team = ((AFortPlayerStateAthena*)PlayerController->PlayerState)->TeamIndex;
	
	if (Building->HasTeamIndex())
		Building->TeamIndex = Building->Team;

	if (Building->HasOwnerPersistentID() && ((AFortPlayerStateAthena*)PlayerController->PlayerState)->HasWorldPlayerId())
		Building->OwnerPersistentID = ((AFortPlayerStateAthena*)PlayerController->PlayerState)->WorldPlayerId;
}

void SetEditingPlayer(ABuildingSMActor* _this, AFortPlayerStateAthena* NewEditingPlayer)
{
	if (!IsUsableDeathObject(_this) || _this->Role != 3 ||
		(_this->EditingPlayer && NewEditingPlayer))
	{
		return;
	}

	auto OwningState = _this->EditingPlayer
		? _this->EditingPlayer : NewEditingPlayer;
	if (OwningState)
	{
		auto Handle = OwningState->Owner;
		if (!IsUsableDeathObject(Handle) ||
			!Handle->Cast<AFortPlayerControllerAthena>())
		{
			return;
		}
	}

	// Wake the actor before changing its replicated owner. The old ordering
	// forced the previous value and then made a cleared build DormantAll, so
	// legacy clients could retain an edit owner indefinitely.
	_this->SetNetDormancy(ENetDormancy::DORM_Awake);
	_this->FlushNetDormancy();
	_this->EditingPlayer = NewEditingPlayer;
	VersionFeatureAdapter::MarkReplicatedPropertyDirty(
		_this, L"EditingPlayer");
	_this->OnRep_EditingPlayer();
	_this->ForceNetUpdate();
}

struct FEditingToolSession
{
	uint64 Generation = 0;
	TWeakObjectPtr<ABuildingSMActor> Building;
	// Keep the original identity after the weak pointer becomes unusable. RPCs
	// for a just-replaced actor can still carry that exact pointer value.
	ABuildingSMActor* BuildingIdentity = nullptr;
	ULONGLONG ClosedAtMs = 0;
	bool bClosed = false;
};

struct FEditingToolRestoreState
{
	TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
	TWeakObjectPtr<AFortWeap_EditingTool> ObservedTool;
	FGuid RestoreItemGuid{};
	std::vector<FEditingToolSession> Sessions;
	uint64 Generation = 0;
	uint64 ActiveGeneration = 0;
	int32 InactiveTicks = 0;
	int32 RepairAttempts = 0;
	int32 AwaitingToolTicks = 0;
	bool bHasRestoreItem = false;
	bool bAwaitingEditTool = false;
};

static std::unordered_map<
	AFortPlayerControllerAthena*, FEditingToolRestoreState>
	GEditingToolRestoreStates;
static UWorld* GEditingToolRestoreWorld = nullptr;

static bool IsRestorableAfterEditing(
	const FFortItemEntry* Entry)
{
	if (!Entry || !IsUsableDeathObject(Entry->ItemDefinition) ||
		!Entry->ItemDefinition->IsA<UFortWeaponItemDefinition>())
	{
		return false;
	}

	auto EditToolDefinitionClass =
		UFortEditToolItemDefinition::StaticClass();
	return !EditToolDefinitionClass ||
		!Entry->ItemDefinition->IsA(EditToolDefinitionClass);
}

static FFortItemEntry* FindEditingRestoreEntry(
	AFortPlayerControllerAthena* PlayerController,
	const FGuid& ItemGuid)
{
	if (!PlayerController || !PlayerController->WorldInventory)
		return nullptr;

	return PlayerController->WorldInventory
		->Inventory.ReplicatedEntries.Search(
			[&](FFortItemEntry& Entry)
			{
				return VehicleLoadoutGuidsEqual(
					Entry.ItemGuid, ItemGuid) &&
					IsRestorableAfterEditing(&Entry);
			}, FFortItemEntry::Size());
}

static bool CaptureEditingRestoreItem(
	AFortPlayerControllerAthena* PlayerController,
	AActor* WeaponActor,
	FEditingToolRestoreState& State)
{
	if (!IsUsableDeathObject(WeaponActor) ||
		WeaponActor->IsA<AFortWeap_EditingTool>())
	{
		return false;
	}

	auto Weapon = WeaponActor->Cast<AFortWeapon>();
	auto Entry = Weapon
		? FindEditingRestoreEntry(
			PlayerController, Weapon->ItemEntryGuid)
		: nullptr;
	if (!Entry)
		return false;

	State.RestoreItemGuid = Entry->ItemGuid;
	State.bHasRestoreItem = true;
	return true;
}

static ABuildingSMActor* BeginEditingToolRestoreSession(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* Pawn,
	ABuildingSMActor* Building = nullptr)
{
	if (!PlayerController || !Pawn)
		return nullptr;

	auto& State = GEditingToolRestoreStates[PlayerController];
	if (State.Pawn.Get() != Pawn)
		State = {};

	State.Pawn = TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
	State.InactiveTicks = 0;
	State.RepairAttempts = 0;
	State.AwaitingToolTicks = 0;
	State.bAwaitingEditTool = false;
	ABuildingSMActor* SupersededBuilding = nullptr;
	if (Building)
	{
		FEditingToolSession* ActiveSession = nullptr;
		for (auto& Session : State.Sessions)
		{
			if (!Session.bClosed &&
				Session.Generation == State.ActiveGeneration)
			{
				ActiveSession = &Session;
				break;
			}
		}

		const bool bRepeatedBegin =
			ActiveSession &&
			ActiveSession->BuildingIdentity == Building;
		if (!bRepeatedBegin)
		{
			// Re-entering the same UObject identity proves this Begin is the new
			// lifecycle boundary. An older marker for that identity must not eat
			// the new session's legitimate End RPC.
			State.Sessions.erase(
				std::remove_if(
					State.Sessions.begin(), State.Sessions.end(),
					[&](const FEditingToolSession& Session)
					{
						return Session.bClosed &&
							Session.BuildingIdentity == Building;
					}),
				State.Sessions.end());

			const ULONGLONG Now = GetTickCount64();
			for (auto& Session : State.Sessions)
			{
				if (Session.bClosed)
					continue;
				if (Session.Generation == State.ActiveGeneration)
					SupersededBuilding = Session.BuildingIdentity;
				Session.bClosed = true;
				Session.ClosedAtMs = Now;
			}

			State.Generation++;
			State.ActiveGeneration = State.Generation;
			State.Sessions.push_back({
				State.Generation,
				TWeakObjectPtr<ABuildingSMActor>(Building),
				Building,
				0,
				false });
			if (State.Sessions.size() > 16)
				State.Sessions.erase(State.Sessions.begin());
		}
	}

	auto CurrentWeapon = Pawn->HasCurrentWeapon()
		? Pawn->CurrentWeapon : nullptr;
	if (CaptureEditingRestoreItem(
			PlayerController, CurrentWeapon, State))
	{
		return SupersededBuilding;
	}

	// Repeated Begin RPCs can arrive while the edit tool is already equipped.
	// Preserve the first session snapshot; otherwise use native PreviousWeapon.
	if (State.bHasRestoreItem &&
		FindEditingRestoreEntry(
			PlayerController, State.RestoreItemGuid))
	{
		return SupersededBuilding;
	}

	State.bHasRestoreItem = false;
	if (Pawn->HasPreviousWeapon() &&
		CaptureEditingRestoreItem(
			PlayerController, Pawn->PreviousWeapon, State))
	{
		return SupersededBuilding;
	}

	auto HarvestingTool =
		FindHarvestingToolEntry(PlayerController->WorldInventory);
	if (IsRestorableAfterEditing(HarvestingTool))
	{
		State.RestoreItemGuid = HarvestingTool->ItemGuid;
		State.bHasRestoreItem = true;
	}
	return SupersededBuilding;
}

static void PruneClosedEditingSessions(
	FEditingToolRestoreState& State)
{
	const ULONGLONG Now = GetTickCount64();
	State.Sessions.erase(
		std::remove_if(
			State.Sessions.begin(), State.Sessions.end(),
			[&](const FEditingToolSession& Session)
			{
				return Session.bClosed &&
					Session.Generation !=
						State.ActiveGeneration &&
					Session.ClosedAtMs != 0 &&
					Now - Session.ClosedAtMs > 5000ULL;
			}),
		State.Sessions.end());
}

static int32 FindEditingSessionForRpc(
	FEditingToolRestoreState& State,
	ABuildingSMActor* Building,
	bool bPreferClosed)
{
	PruneClosedEditingSessions(State);
	// A null target normally means a just-replaced actor no longer resolves on
	// the server. End RPCs likewise trail a completed Edit. Let that older
	// tombstone absorb the RPC instead of terminating a newer live session.
	if (bPreferClosed)
	{
		for (int32 Index = 0;
			Index < (int32)State.Sessions.size(); Index++)
		{
			auto& Session = State.Sessions[Index];
			if (Session.bClosed &&
				(!Building || Session.BuildingIdentity == Building))
			{
				return Index;
			}
		}
	}

	// Otherwise a live session wins over retained tombstones. A normal Edit RPC
	// with a resolved target must not be swallowed by an older marker.
	for (int32 Index = 0;
		Index < (int32)State.Sessions.size(); Index++)
	{
		auto& Session = State.Sessions[Index];
		if (!Session.bClosed &&
			Session.Generation == State.ActiveGeneration &&
			(!Building || Session.BuildingIdentity == Building))
		{
			return Index;
		}
	}

	for (int32 Index = 0;
		Index < (int32)State.Sessions.size(); Index++)
	{
		auto& Session = State.Sessions[Index];
		if (!Building || Session.BuildingIdentity == Building)
			return Index;
	}
	return -1;
}

static bool IsCurrentEditingSessionRpc(
	AFortPlayerControllerAthena* PlayerController,
	ABuildingSMActor* Building,
	bool bCloseSession,
	bool bConsumeSession,
	ABuildingSMActor** ResolvedBuilding = nullptr)
{
	if (ResolvedBuilding)
		*ResolvedBuilding = nullptr;
	auto StateIt =
		GEditingToolRestoreStates.find(PlayerController);
	if (StateIt == GEditingToolRestoreStates.end() ||
		StateIt->second.Sessions.empty())
	{
		// Preserve compatibility for a session that began before this state was
		// initialized (for example after hot injection).
		return true;
	}

	auto& State = StateIt->second;
	const int32 SessionIndex =
		FindEditingSessionForRpc(
			State, Building,
			bConsumeSession || !Building);
	if (SessionIndex < 0)
		return false;

	auto& Session = State.Sessions[SessionIndex];
	if (ResolvedBuilding)
		*ResolvedBuilding = Session.Building.Get();
	const uint64 SessionGeneration = Session.Generation;
	const bool bWasClosed = Session.bClosed;
	const bool bIsCurrent =
		!bWasClosed &&
		SessionGeneration == State.ActiveGeneration;
	if (bCloseSession && !bWasClosed)
	{
		Session.bClosed = true;
		Session.ClosedAtMs = GetTickCount64();
		if (bIsCurrent)
			State.ActiveGeneration = 0;
	}
	if (bConsumeSession)
	{
		State.Sessions.erase(
			State.Sessions.begin() + SessionIndex);
	}
	return bIsCurrent;
}

static ABuildingSMActor* GetActiveEditingSessionBuilding(
	FEditingToolRestoreState& State)
{
	for (auto& Session : State.Sessions)
	{
		if (!Session.bClosed &&
			Session.Generation == State.ActiveGeneration)
		{
			return Session.Building.Get();
		}
	}
	return nullptr;
}

static void SetEditingToolActor(
	AFortWeap_EditingTool* EditTool,
	ABuildingSMActor* Building)
{
	if (!IsUsableDeathObject(EditTool) ||
		!EditTool->HasEditActor())
	{
		return;
	}

	EditTool->SetNetDormancy(ENetDormancy::DORM_Awake);
	EditTool->FlushNetDormancy();
	EditTool->EditActor = Building;
	VersionFeatureAdapter::MarkReplicatedPropertyDirty(
		EditTool, L"EditActor");
	EditTool->OnRep_EditActor();
	EditTool->ForceNetUpdate();
}

static bool HasActiveEditingTarget(
	AFortPlayerControllerAthena* PlayerController,
	AFortWeap_EditingTool* EditTool)
{
	if (!PlayerController || !IsUsableDeathObject(EditTool) ||
		!EditTool->HasEditActor())
	{
		return false;
	}

	auto Building = EditTool->EditActor;
	return IsUsableDeathObject(Building) &&
		Building->IsA<ABuildingSMActor>() &&
		!Building->bDestroyed &&
		Building->EditingPlayer == PlayerController->PlayerState;
}

template <typename Visitor>
static void ForEachEditingTool(
	AFortPlayerPawnAthena* Pawn, Visitor&& Visit)
{
	if (!IsUsableDeathObject(Pawn))
		return;

	std::unordered_set<AFortWeap_EditingTool*> Seen;
	auto VisitCandidate = [&](AActor* Candidate)
	{
		if (!IsUsableDeathObject(Candidate))
			return;
		auto EditTool = Candidate->Cast<AFortWeap_EditingTool>();
		if (EditTool && Seen.insert(EditTool).second)
			Visit(EditTool);
	};

	if (Pawn->HasCurrentWeapon())
		VisitCandidate(Pawn->CurrentWeapon);
	if (!Pawn->HasCurrentWeaponList())
		return;

	auto& WeaponList = Pawn->CurrentWeaponList;
	for (int32 Index = 0; Index < WeaponList.Num(); Index++)
		VisitCandidate(WeaponList.Get(Index));
}

static AFortWeap_EditingTool* GetCurrentEditingTool(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!PlayerController ||
		!IsUsableDeathObject(PlayerController->MyFortPawn))
	{
		return nullptr;
	}

	auto Pawn = PlayerController->MyFortPawn;
	if (!Pawn->HasCurrentWeapon() ||
		!IsUsableDeathObject(Pawn->CurrentWeapon))
	{
		return nullptr;
	}
	return Pawn->CurrentWeapon
		->Cast<AFortWeap_EditingTool>();
}

static bool HasCurrentActiveEditingTarget(
	AFortPlayerControllerAthena* PlayerController)
{
	return HasActiveEditingTarget(
		PlayerController,
		GetCurrentEditingTool(PlayerController));
}

static bool ClearEditingToolActor(
	AFortPlayerControllerAthena* PlayerController,
	ABuildingSMActor* ExpectedBuilding)
{
	if (!PlayerController ||
		!IsUsableDeathObject(PlayerController->MyFortPawn))
	{
		return false;
	}

	auto Pawn = PlayerController->MyFortPawn;
	auto CurrentEditTool =
		GetCurrentEditingTool(PlayerController);
	bool bCleared = false;
	ForEachEditingTool(
		Pawn,
		[&](AFortWeap_EditingTool* EditTool)
		{
			if (!EditTool->HasEditActor())
				return;

			auto Target = EditTool->EditActor;
			const bool bMatchesExpected =
				ExpectedBuilding && Target == ExpectedBuilding;
			const bool bStaleWithoutExpected =
				!ExpectedBuilding && Target &&
				(EditTool != CurrentEditTool ||
					!HasActiveEditingTarget(
						PlayerController, EditTool));
			if (bMatchesExpected || bStaleWithoutExpected)
			{
				if (!ExpectedBuilding &&
					IsUsableDeathObject(Target) &&
					Target->IsA<ABuildingSMActor>() &&
					Target->EditingPlayer ==
						PlayerController->PlayerState)
				{
					SetEditingPlayer(Target, nullptr);
				}
				SetEditingToolActor(EditTool, nullptr);
				bCleared = true;
			}
		});

	return bCleared;
}

static void ClearNonCurrentEditingTools(
	AFortPlayerControllerAthena* PlayerController,
	AFortWeap_EditingTool* CurrentEditTool)
{
	if (!PlayerController ||
		!IsUsableDeathObject(PlayerController->MyFortPawn))
	{
		return;
	}

	auto ActiveBuilding =
		HasActiveEditingTarget(
			PlayerController, CurrentEditTool)
		? CurrentEditTool->EditActor : nullptr;
	ForEachEditingTool(
		PlayerController->MyFortPawn,
		[&](AFortWeap_EditingTool* EditTool)
		{
			if (EditTool == CurrentEditTool ||
				!EditTool->HasEditActor() ||
				!EditTool->EditActor)
			{
				return;
			}

			auto StaleBuilding = EditTool->EditActor;
			if (StaleBuilding != ActiveBuilding &&
				IsUsableDeathObject(StaleBuilding) &&
				StaleBuilding->IsA<ABuildingSMActor>() &&
				StaleBuilding->EditingPlayer ==
					PlayerController->PlayerState)
			{
				SetEditingPlayer(StaleBuilding, nullptr);
			}
			SetEditingToolActor(EditTool, nullptr);
		});
}

static bool ActivateLegacyEditingRestoreSlot(
	AFortPlayerControllerAthena* PlayerController,
	const FFortItemEntry& Entry)
{
	auto QuickBars = PlayerController
		? PlayerController->QuickBars : nullptr;
	if (!QuickBars || !Entry.ItemDefinition ||
		!QuickBars->HasPrimaryQuickBar() ||
		!QuickBars->HasSecondaryQuickBar())
	{
		return false;
	}

	const bool bPrimary =
		AFortInventory::IsPrimaryQuickbar(Entry.ItemDefinition) ||
		(Entry.ItemDefinition->HasItemType() &&
			Entry.ItemDefinition->ItemType ==
				EFortItemType::GetWeaponHarvest());
	auto& QuickBar = bPrimary
		? QuickBars->PrimaryQuickBar
		: QuickBars->SecondaryQuickBar;
	if (!FQuickBar::HasSlots())
		return false;

	for (int32 SlotIndex = 0;
		SlotIndex < QuickBar.Slots.Num(); SlotIndex++)
	{
		auto& Slot = QuickBar.Slots.Get(
			SlotIndex, FQuickBarSlot::Size());
		if (!FQuickBarSlot::HasItems())
			continue;
		for (const auto& SlotItem : Slot.Items)
		{
			if (VehicleLoadoutGuidsEqual(
					SlotItem, Entry.ItemGuid))
			{
				QuickBars->ServerActivateSlotInternal(
					!bPrimary, SlotIndex, 0.f, true);
				return true;
			}
		}
	}
	return false;
}

static void RefreshEditingRestoreOnClient(
	AFortPlayerControllerAthena* PlayerController,
	const FFortItemEntry& Entry)
{
	if (!PlayerController)
		return;

	if (VersionInfo.FortniteVersion > 3.0)
	{
		auto ClientEquipItem =
			PlayerController->GetFunction("ClientEquipItem");
		if (ClientEquipItem)
		{
			PlayerController->Call<void>(
				ClientEquipItem, Entry.ItemGuid, true);
			return;
		}
	}

	// The three-parameter owner RPC is schema-confirmed only on 1.7.2/2.50.
	// Other early builds retain their native quickbar activation path.
	const bool bKnownLegacyExecuteSignature =
		VersionInfo.FortniteVersion == 1.72 ||
		VersionInfo.FortniteVersion == 2.50;
	auto ClientExecuteInventoryItem =
		bKnownLegacyExecuteSignature
		? PlayerController->GetFunction(
			"ClientExecuteInventoryItem")
		: nullptr;
	if (ClientExecuteInventoryItem)
	{
		PlayerController->Call<void>(
			ClientExecuteInventoryItem,
			Entry.ItemGuid, 0.f, true);
		return;
	}

	ActivateLegacyEditingRestoreSlot(
		PlayerController, Entry);
}

static bool RestoreInactiveEditingTool(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!PlayerController || !PlayerController->WorldInventory ||
		!IsUsableDeathObject(PlayerController->MyFortPawn) ||
		HasCurrentActiveEditingTarget(PlayerController))
	{
		return false;
	}

	auto Pawn = PlayerController->MyFortPawn;
	if (!Pawn->HasCurrentWeapon() ||
		!IsUsableDeathObject(Pawn->CurrentWeapon) ||
		!Pawn->CurrentWeapon->IsA<AFortWeap_EditingTool>())
	{
		return false;
	}

	ClearEditingToolActor(PlayerController, nullptr);
	auto StateIt =
		GEditingToolRestoreStates.find(PlayerController);
	FFortItemEntry* RestoreEntry = nullptr;
	if (StateIt != GEditingToolRestoreStates.end() &&
		StateIt->second.bHasRestoreItem)
	{
		RestoreEntry = FindEditingRestoreEntry(
			PlayerController,
			StateIt->second.RestoreItemGuid);
	}

	if (!RestoreEntry && Pawn->HasPreviousWeapon())
	{
		auto PreviousWeapon = IsUsableDeathObject(Pawn->PreviousWeapon)
			? Pawn->PreviousWeapon->Cast<AFortWeapon>() : nullptr;
		if (PreviousWeapon &&
			!PreviousWeapon->IsA<AFortWeap_EditingTool>())
		{
			RestoreEntry = FindEditingRestoreEntry(
				PlayerController,
				PreviousWeapon->ItemEntryGuid);
		}
	}

	if (!RestoreEntry)
		RestoreEntry =
			FindHarvestingToolEntry(
				PlayerController->WorldInventory);
	if (!IsRestorableAfterEditing(RestoreEntry))
		return false;

	PlayerController->ServerExecuteInventoryItem(
		RestoreEntry->ItemGuid);
	RefreshEditingRestoreOnClient(
		PlayerController, *RestoreEntry);
	Pawn->ForceNetUpdate();
	PlayerController->ForceNetUpdate();
	return true;
}

void AFortPlayerControllerAthena::TickEditingToolStateRepair(
	UNetDriver* Driver)
{
	auto World = UWorld::GetWorld();
	if (!World)
	{
		GEditingToolRestoreStates.clear();
		GEditingToolRestoreWorld = nullptr;
		return;
	}
	if (Driver != World->NetDriver)
		return;

	if (GEditingToolRestoreWorld != World)
	{
		GEditingToolRestoreStates.clear();
		GEditingToolRestoreWorld = World;
	}

	std::unordered_set<AFortPlayerControllerAthena*>
		ConnectedControllers;
	for (int32 Index = 0;
		Index < Driver->ClientConnections.Num(); Index++)
	{
		auto Connection = Driver->ClientConnections[Index];
		auto PlayerController =
			Connection && Connection->PlayerController
			? Connection->PlayerController
				->Cast<AFortPlayerControllerAthena>()
			: nullptr;
		if (!IsUsableDeathObject(PlayerController))
			continue;

		ConnectedControllers.insert(PlayerController);
		auto Pawn = PlayerController->MyFortPawn;
		auto StateIt = GEditingToolRestoreStates.find(
			PlayerController);
		if (!IsUsableDeathObject(Pawn) ||
			(IsUsableDeathObject(PlayerController->Pawn) &&
				PlayerController->Pawn != Pawn) ||
			Pawn->GetHealth() <= 0.f)
		{
			GEditingToolRestoreStates.erase(
				PlayerController);
			continue;
		}

		auto EditTool =
			GetCurrentEditingTool(PlayerController);
		if (StateIt != GEditingToolRestoreStates.end() &&
			StateIt->second.bAwaitingEditTool)
		{
			auto& PendingState = StateIt->second;
			auto PendingBuilding =
				GetActiveEditingSessionBuilding(PendingState);
			const bool bCanFinishBegin =
				EditTool && EditTool->HasEditActor() &&
				IsUsableDeathObject(PendingBuilding) &&
				PendingBuilding->EditingPlayer ==
					PlayerController->PlayerState;
			if (bCanFinishBegin)
			{
				SetEditingToolActor(
					EditTool, PendingBuilding);
				PendingState.ObservedTool =
					TWeakObjectPtr<AFortWeap_EditingTool>(
						EditTool);
				PendingState.bAwaitingEditTool = false;
				PendingState.AwaitingToolTicks = 0;
				PendingState.InactiveTicks = 0;
				PendingState.RepairAttempts = 0;
			}
			else if (++PendingState.AwaitingToolTicks < 3)
			{
				continue;
			}
			else
			{
				if (IsUsableDeathObject(PendingBuilding) &&
					PendingBuilding->EditingPlayer ==
						PlayerController->PlayerState)
				{
					SetEditingPlayer(
						PendingBuilding, nullptr);
				}
				IsCurrentEditingSessionRpc(
					PlayerController, PendingBuilding,
					true, false);
				PendingState.bAwaitingEditTool = false;
			}
		}

		if (!EditTool)
		{
			// A weapon swap is also an edit exit. Remove any orphaned edit-tool
			// target left in the pawn's weapon list before forgetting the session.
			ClearNonCurrentEditingTools(
				PlayerController, nullptr);
			GEditingToolRestoreStates.erase(
				PlayerController);
			continue;
		}

		StateIt = GEditingToolRestoreStates.find(
			PlayerController);
		if (StateIt == GEditingToolRestoreStates.end() ||
			StateIt->second.Pawn.Get() != Pawn)
		{
			BeginEditingToolRestoreSession(
				PlayerController, Pawn);
			StateIt = GEditingToolRestoreStates.find(
				PlayerController);
		}
		if (StateIt == GEditingToolRestoreStates.end())
			continue;

		auto& State = StateIt->second;
		if (State.ObservedTool.Get() != EditTool)
		{
			State.ObservedTool =
				TWeakObjectPtr<AFortWeap_EditingTool>(EditTool);
			State.InactiveTicks = 0;
			State.RepairAttempts = 0;
		}

		ClearNonCurrentEditingTools(
			PlayerController, EditTool);
		if (HasCurrentActiveEditingTarget(PlayerController))
		{
			State.InactiveTicks = 0;
			State.RepairAttempts = 0;
			continue;
		}

		// Wait for two fully settled server ticks. A newer Begin RPC received in
		// the same burst re-establishes an authoritative target and cancels this
		// repair before it can unequip that newer session.
		State.InactiveTicks++;
		if (State.InactiveTicks < 2 ||
			State.RepairAttempts >= 5 ||
			((State.InactiveTicks - 2) % 3) != 0)
		{
			continue;
		}

		State.RepairAttempts++;
		const bool bRestored =
			RestoreInactiveEditingTool(PlayerController);
		SDK::DbgLog(
			"[EditRepair] controller=%p pawn=%p tool=%p "
			"generation=%llu attempt=%d restored=%d\n",
			(void*)PlayerController, (void*)Pawn,
			(void*)EditTool,
			(unsigned long long)State.Generation,
			State.RepairAttempts, bRestored ? 1 : 0);
	}

	for (auto It = GEditingToolRestoreStates.begin();
		It != GEditingToolRestoreStates.end();)
	{
		if (ConnectedControllers.find(It->first) ==
			ConnectedControllers.end())
		{
			It = GEditingToolRestoreStates.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void AFortPlayerControllerAthena::ServerBeginEditingBuildingActor(UObject* Context, FFrame& Stack)
{
	ABuildingSMActor* Building = nullptr;
	Stack.StepCompiledIn(&Building);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	if (!PlayerController ||
		!IsUsableDeathObject(PlayerController->MyFortPawn) ||
		!PlayerController->WorldInventory ||
		!IsUsableDeathObject(Building) ||
		!Building->IsA<ABuildingSMActor>())
		return;

	AFortPlayerStateAthena* PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;
	if (!IsUsableDeathObject(PlayerState))
		return;

	SetEditingPlayer(Building, PlayerState);
	if (Building->EditingPlayer != PlayerState)
		return;

	auto Pawn = PlayerController->MyFortPawn;
	auto SupersededBuilding = BeginEditingToolRestoreSession(
		PlayerController, Pawn, Building);
	if (SupersededBuilding && SupersededBuilding != Building)
	{
		if (IsUsableDeathObject(SupersededBuilding) &&
			SupersededBuilding->EditingPlayer == PlayerState)
		{
			SetEditingPlayer(SupersededBuilding, nullptr);
		}
		ClearEditingToolActor(
			PlayerController, SupersededBuilding);
	}
	AFortWeap_EditingTool* EditTool = nullptr;
	if (Pawn->HasCurrentWeapon() &&
		IsUsableDeathObject(Pawn->CurrentWeapon) &&
		Pawn->CurrentWeapon->IsA<AFortWeap_EditingTool>())
	{
		EditTool =
			Pawn->CurrentWeapon
				->Cast<AFortWeap_EditingTool>();
	}

	if (!EditTool)
	{
		auto EditToolEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
			{
				return entry.ItemDefinition &&
					entry.ItemDefinition->IsA<
						UFortEditToolItemDefinition>();
			}, FFortItemEntry::Size());
		if (!EditToolEntry)
		{
			SetEditingPlayer(Building, nullptr);
			return;
		}

		auto EditWeapon = (AFortWeapon*)Pawn->EquipWeaponDefinition(
			(UFortWeaponItemDefinition*)EditToolEntry->ItemDefinition,
			EditToolEntry->ItemGuid,
			EditToolEntry->HasTrackerGuid() ? EditToolEntry->TrackerGuid : FGuid(),
			false);
		if (EditWeapon)
			FFortWeaponMods::ApplyEntrySlotsAfterEquip(
				EditWeapon, *EditToolEntry);

		// EquipWeaponDefinition can return a replacement actor before the pawn's
		// authoritative CurrentWeapon has settled. Never attach an edit target to
		// that non-current actor; the game-thread repair will finish the handoff.
		if (Pawn->HasCurrentWeapon() &&
			IsUsableDeathObject(Pawn->CurrentWeapon) &&
			Pawn->CurrentWeapon
				->IsA<AFortWeap_EditingTool>())
		{
			EditTool =
				Pawn->CurrentWeapon
					->Cast<AFortWeap_EditingTool>();
		}
	}

	if (!EditTool || !EditTool->HasEditActor())
	{
		auto RestoreState =
			GEditingToolRestoreStates.find(PlayerController);
		if (RestoreState != GEditingToolRestoreStates.end())
		{
			RestoreState->second.bAwaitingEditTool = true;
			RestoreState->second.AwaitingToolTicks = 0;
		}
		else
		{
			SetEditingPlayer(Building, nullptr);
		}
		return;
	}

	SetEditingToolActor(EditTool, Building);
	auto RestoreState =
		GEditingToolRestoreStates.find(PlayerController);
	if (RestoreState != GEditingToolRestoreStates.end())
	{
		RestoreState->second.ObservedTool =
			TWeakObjectPtr<AFortWeap_EditingTool>(EditTool);
		RestoreState->second.InactiveTicks = 0;
		RestoreState->second.RepairAttempts = 0;
		RestoreState->second.bAwaitingEditTool = false;
	}
}

uint64_t ReplaceBuildingActor_ = 0;
uint64_t InitializeBuildingActor_ = 0;
uint64_t PostInitializeSpawnedBuildingActor_ = 0;
void AFortPlayerControllerAthena::ServerEditBuildingActor(UObject* Context, FFrame& Stack)
{
	ABuildingSMActor* Building = nullptr;
	TSubclassOf<AActor> NewClass{};
	uint8 RotationIterations = 0;
	bool bMirrored = false;
	Stack.StepCompiledIn(&Building);
	Stack.StepCompiledIn(&NewClass);
	Stack.StepCompiledIn(&RotationIterations);
	Stack.StepCompiledIn(&bMirrored);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (!PlayerController)
		return;
	ABuildingSMActor* SessionBuilding = nullptr;
	if (!IsCurrentEditingSessionRpc(
			PlayerController, Building,
			false, false, &SessionBuilding))
	{
		IsCurrentEditingSessionRpc(
			PlayerController, Building,
			true, false);
		return;
	}
	if (!Building)
	{
		Building = SessionBuilding;
		if (!Building)
		{
			auto CurrentEditTool =
				GetCurrentEditingTool(PlayerController);
			if (CurrentEditTool && CurrentEditTool->HasEditActor())
				Building = CurrentEditTool->EditActor;
		}
	}

	const bool bValidActiveBuilding =
		IsUsableDeathObject(Building) &&
		Building->IsA<ABuildingSMActor>() &&
		!Building->bDestroyed &&
		Building->EditingPlayer ==
			PlayerController->PlayerState;
	if (!bValidActiveBuilding)
	{
		IsCurrentEditingSessionRpc(
			PlayerController, Building,
			true, false);
		// A reset/confirm can race the old actor's replacement. Clear only a
		// tool which still targets that exact actor; a newer edit is untouched.
		ClearEditingToolActor(
			PlayerController, Building);
		return;
	}
	if (!NewClass || !CanBePlacedByPlayer(NewClass))
		return;
	IsCurrentEditingSessionRpc(
		PlayerController, Building,
		true, false);

	SetEditingPlayer(Building, nullptr);

	// End the tool session while the old actor is still live. The replacement
	// RPC destroys that actor, so waiting for the later client End RPC can
	// otherwise strand the blueprint/map in the player's hands.
	ClearEditingToolActor(
		PlayerController, Building);

	auto ReplaceBuildingActor = (ABuildingSMActor * (*&)(ABuildingSMActor*, unsigned int, TSubclassOf<AActor>, unsigned int, int, bool, AFortPlayerControllerAthena*)) ReplaceBuildingActor_;
	auto ReplaceBuildingActor__New = (ABuildingSMActor * (*&)(ABuildingSMActor*, unsigned int, TSubclassOf<AActor>&, unsigned int, int, bool, AFortPlayerControllerAthena*)) ReplaceBuildingActor_;

	ABuildingSMActor* NewBuild = nullptr;

	if (VersionInfo.FortniteVersion < 27)
		NewBuild = ReplaceBuildingActor(Building, 1, NewClass, Building->CurrentBuildingLevel, RotationIterations, bMirrored, PlayerController);
	else
		NewBuild = ReplaceBuildingActor__New(Building, 1, NewClass, Building->CurrentBuildingLevel, RotationIterations, bMirrored, PlayerController);

	/*else
	{
		// reimpl of replacebuildingactor
		NewBuild = ABuildingSMActor::K2_SpawnBuildingActor(PlayerController, NewClass, Building->GetTransform(), nullptr, nullptr, true, false);

		NewBuild->CurrentBuildingLevel = Building->CurrentBuildingLevel;
		NewBuild->SetMirrored(bMirrored);
		NewBuild->SetTeam(Building->TeamIndex);
		auto InitializeBuildingActor = (void(*)(ABuildingSMActor*,
			uint8 Reason,
			int16 InOwnerPersistentID,
			ABuildingSMActor * BuildingOwner,
			const ABuildingSMActor * ReplacedBuilding,
			bool bForcePlayBuildUpAnim)) InitializeBuildingActor_;
		InitializeBuildingActor(NewBuild, 2, ((AFortPlayerStateAthena*)PlayerController->PlayerState)->WorldPlayerId, nullptr, Building, true);
		NewBuild->BuildingReplacementType = 1;

		Building->ReplacementDestructionReason = 1;
		Building->OnReplacementDestruction.Process(1, NewBuild);
		Building->bAutoReleaseCurieContainerOnDestroyed = false;

		auto PostInitializeSpawnedBuildingActor = (void(*)(ABuildingSMActor*, uint8_t Reason)) PostInitializeSpawnedBuildingActor_;
		PostInitializeSpawnedBuildingActor(NewBuild, 2);
		UWorld::FinishSpawnActor(NewBuild, Building->K2_GetActorLocation(), Building->K2_GetActorRotation());
		Building->SilentDie(true);
	}*/

	if (NewBuild)
	{
		NewBuild->bPlayerPlaced = true;
	}
}

void AFortPlayerControllerAthena::ServerEndEditingBuildingActor(UObject* Context, FFrame& Stack)
{
	ABuildingSMActor* Building = nullptr;
	Stack.StepCompiledIn(&Building);
	Stack.IncrementCode();

	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	if (!PlayerController ||
		!IsUsableDeathObject(PlayerController->MyFortPawn))
		return;
	ABuildingSMActor* SessionBuilding = nullptr;
	if (!IsCurrentEditingSessionRpc(
			PlayerController, Building,
			true, true, &SessionBuilding))
	{
		return;
	}
	if (!Building)
	{
		Building = SessionBuilding;
		if (!Building)
		{
			auto CurrentEditTool =
				GetCurrentEditingTool(PlayerController);
			if (CurrentEditTool && CurrentEditTool->HasEditActor())
				Building = CurrentEditTool->EditActor;
		}
	}

	if (IsUsableDeathObject(Building) &&
		Building->IsA<ABuildingSMActor>() &&
		Building->EditingPlayer ==
			PlayerController->PlayerState)
	{
		SetEditingPlayer(Building, nullptr);
	}

	// Cleanup cannot depend on the old actor still being alive or still naming
	// this player. Successful edits intentionally invalidate both conditions.
	ClearEditingToolActor(
		PlayerController, Building);
}


void AFortPlayerControllerAthena::ServerRepairBuildingActor(UObject* Context, FFrame& Stack)
{
	ABuildingSMActor* Building;
	Stack.StepCompiledIn(&Building);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	if (!PlayerController || !Building->IsA<ABuildingSMActor>())
		return;

	auto Price = (int32)std::floor((10.f * (1.f - Building->GetHealthPercent())) * 0.75f);
	auto res = UFortKismetLibrary::K2_GetResourceItemDefinition(Building->ResourceType);
	auto ItemP = PlayerController->WorldInventory->Inventory.ItemInstances.Search([res](UFortWorldItem* entry)
		{
			return entry->ItemEntry.ItemDefinition == res;
		});
	auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([res](FFortItemEntry& entry)
		{
			return entry.ItemDefinition == res;
		}, FFortItemEntry::Size());
	if (!ItemP)
		return;
	auto Item = *ItemP;
	if ((itemEntry->Count - Price) < 0)
		return;

	itemEntry->Count -= Price;
	if (itemEntry->Count <= 0)
		PlayerController->WorldInventory->Remove(itemEntry->ItemGuid);
	else
	{
		Item->ItemEntry.Count = itemEntry->Count;
		PlayerController->WorldInventory->UpdateEntry(*itemEntry);
		Item->ItemEntry.bIsDirty = true;
	}

	Building->RepairBuilding(PlayerController, Price);
}

void AFortPlayerControllerAthena::ServerAttemptInventoryDrop(UObject* Context, FFrame& Stack)
{
	FGuid Guid{};
	int32 Count = 0;
	bool bTrash = false; // this only exists on some newer builds
	bool bReadGuid = false;
	bool bReadCount = false;

	auto Function = Stack.GetCurrentNativeFunction();
	if (!Function)
		Function = Stack.Node;

	if (Function)
	{
		for (const auto& Parameter :
			Function->GetParamsNamed().NameOffsetMap)
		{
			if (Parameter.Name == "ItemGuid" ||
				Parameter.Name == "Guid")
			{
				Stack.StepCompiledIn(&Guid);
				bReadGuid = true;
			}
			else if (Parameter.Name == "Count")
			{
				Stack.StepCompiledIn(&Count);
				bReadCount = true;
			}
			else if (Parameter.Name == "bTrash")
			{
				Stack.StepCompiledIn(&bTrash);
			}
			else if (Parameter.Name != "ReturnValue")
			{
				SDK::DbgLog(
					"[InventoryDrop] discarding unknown parameter %s\n",
					Parameter.Name.c_str());
				Stack.StepCompiledIn();
			}
		}
	}
	else
	{
		// The reflected function is expected on every supported build. Keep
		// the original two-parameter layout as a fail-safe fallback.
		Stack.StepCompiledIn(&Guid);
		Stack.StepCompiledIn(&Count);
		bReadGuid = true;
		bReadCount = true;
	}
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (!bReadGuid || !bReadCount ||
		!PlayerController || !PlayerController->Pawn ||
		!PlayerController->WorldInventory)
		return;

	auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
		{ return entry.ItemGuid == Guid; }, FFortItemEntry::Size());
	if (!itemEntry || itemEntry->Count <= 0 || Count <= 0)
		return;

	if (FFortAthenaNativeLTMCompatibility::
			ShouldBlockAshtonInventoryDrop(
				PlayerController,
				itemEntry->ItemDefinition))
	{
		return;
	}

	if (FFortAthenaNativeLTMCompatibility::ShouldSuppressArsenalWorldLoot() &&
		itemEntry->ItemDefinition &&
		itemEntry->ItemDefinition->IsA<UFortWeaponItemDefinition>())
	{
		return;
	}

	Count = min(Count, itemEntry->Count);
	if (bTrash)
	{
		PlayerController->WorldInventory->RemoveItem(Guid, Count);
		return;
	}

	FVector FinalLoc = PlayerController->Pawn->K2_GetActorLocation();

	static auto WID_Launcher_Petrol = FindObject<UFortWorldItemDefinition>(L"/Game/Athena/Items/Weapons/Prototype/WID_Launcher_Petrol.WID_Launcher_Petrol");

	if (itemEntry->ItemDefinition == WID_Launcher_Petrol)
	{
		static auto BGA_Petrol_PickupClass = FindObject<UClass>(L"/Game/Athena/Items/Weapons/Prototype/PetrolPump/BGA_Petrol_Pickup.BGA_Petrol_Pickup_C");

		AActor* PetrolPickup = UWorld::SpawnActor<AActor>(BGA_Petrol_PickupClass, FinalLoc);

		if (PetrolPickup)
			PlayerController->WorldInventory->RemoveItem(Guid, Count);

		return;
	}

	FVector ForwardVector = PlayerController->Pawn->GetActorForwardVector();
	//ForwardVector.Z = 0.0f;
	//ForwardVector.Normalize();

	FinalLoc = FinalLoc + ForwardVector * 450.f;
	FinalLoc.Z += 50.f;

	const float RandomAngleVariation = ((float)rand() * 0.00109866634f) - 18.f;
	const float FinalAngle = (RandomAngleVariation + 360.f / ((float)rand() * 0.00015259254737998596f)) * 0.017453292519943295f;

	FinalLoc.X += cos(FinalAngle) * 100.f;
	FinalLoc.Y += sin(FinalAngle) * 100.f;

	auto Pickup = AFortInventory::SpawnPickup(
		PlayerController->Pawn->K2_GetActorLocation() +
			PlayerController->Pawn->GetActorForwardVector() * 70.f +
			FVector(0, 0, 50),
		*itemEntry,
		EFortPickupSourceTypeFlag::GetPlayer(),
		EFortPickupSpawnSource::GetTossedByPlayer(),
		PlayerController->MyFortPawn,
		Count,
		true,
		true,
		true,
		nullptr,
		FinalLoc);
	if (Pickup)
	{
		PlayerController->WorldInventory->RemoveItem(Guid, Count);
	}
}

class UAthenaToyItemDefinition : public UObject
{
public:
	UCLASS_COMMON_MEMBERS(UAthenaToyItemDefinition);

	DEFINE_PROP(ToySpawnAbility, TSoftClassPtr<UClass>);
};

extern uint64_t ConstructAbilitySpec;
uint64_t GiveAbilityAndActivateOnce;
void AFortPlayerControllerAthena::ServerPlayEmoteItem_(UObject* Context, FFrame& Stack)
{
	UObject* Asset;
	float RandomNumber = 0.f;
	Stack.StepCompiledIn(&Asset);
	Stack.StepCompiledIn(&RandomNumber);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (!PlayerController || !PlayerController->MyFortPawn || !Asset)
		return;

	auto AbilitySystemComponent = ((AFortPlayerStateAthena*)PlayerController->PlayerState)->AbilitySystemComponent;

	if (auto CharacterVehicle = PlayerController->Pawn->Cast<AFortCharacterVehicle>())
		AbilitySystemComponent = CharacterVehicle->OverrideAbilitySystemComponent;

	UObject* AbilityToUse = nullptr;

	static auto SprayClass = FindClass("AthenaSprayItemDefinition");
	if (Asset->IsA(SprayClass))
	{
		static auto SprayAbilityClass = FindObject<UClass>(L"/Game/Abilities/Sprays/GAB_Spray_Generic.GAB_Spray_Generic_C");
		AbilityToUse = SprayAbilityClass->GetDefaultObj();

		// Do not emulate cosmetic quest progress here. Inline quest-objective
		// layouts vary between versions, and walking a mismatched TagConditions
		// array can crash the server before the cosmetic ability is activated.
	}
	else if (auto ToyAsset = Asset->Cast<UAthenaToyItemDefinition>())
	{
		AbilityToUse = ToyAsset->ToySpawnAbility->GetDefaultObj();
	}
	else if (auto DanceAsset = Asset->Cast<UAthenaDanceItemDefinition>())
	{
		static auto HasbMovingEmote = PlayerController->MyFortPawn->HasbMovingEmote();
		if (HasbMovingEmote)
			PlayerController->MyFortPawn->bMovingEmote = DanceAsset->bMovingEmote;

		static auto HasWalkForwardSpeed = PlayerController->MyFortPawn->HasEmoteWalkSpeed();
		if (HasWalkForwardSpeed)
			PlayerController->MyFortPawn->EmoteWalkSpeed = DanceAsset->WalkForwardSpeed;

		static auto HasbMovingEmoteForwardOnly = PlayerController->MyFortPawn->HasbMovingEmoteForwardOnly();
		if (HasbMovingEmoteForwardOnly)
			PlayerController->MyFortPawn->bMovingEmoteForwardOnly = DanceAsset->bMoveForwardOnly;

		static auto HasbMovingEmoteFollowingOnly = PlayerController->MyFortPawn->HasbMovingEmoteFollowingOnly();
		if (HasbMovingEmoteFollowingOnly)
			PlayerController->MyFortPawn->bMovingEmoteFollowingOnly = DanceAsset->bMoveFollowingOnly;

		auto CustomAbility = DanceAsset->HasCustomDanceAbility() ? DanceAsset->CustomDanceAbility.Get() : nullptr;

		if (CustomAbility)
			AbilityToUse = CustomAbility->GetDefaultObj();
		else
		{
			static auto EmoteAbilityClass = FindObject<UClass>(L"/Game/Abilities/Emotes/GAB_Emote_Generic.GAB_Emote_Generic_C");
			AbilityToUse = EmoteAbilityClass->GetDefaultObj();
		}

	}

	if (AbilityToUse)
	{
		auto Spec = (FGameplayAbilitySpec*)malloc(FGameplayAbilitySpec::Size());
		memset(PBYTE(Spec), 0, FGameplayAbilitySpec::Size());

		if (ConstructAbilitySpec)
			((void (*)(FGameplayAbilitySpec*, const UObject*, int, int, UObject*)) ConstructAbilitySpec)(Spec, AbilityToUse, 1, -1, Asset);
		else
		{
			Spec->MostRecentArrayReplicationKey = -1;
			Spec->ReplicationID = -1;
			Spec->ReplicationKey = -1;
			Spec->Ability = (UFortGameplayAbility*)AbilityToUse;
			Spec->Level = 1;
			Spec->InputID = -1;
			Spec->Handle.Handle = rand();
			Spec->SourceObject = Asset;
		}
		FGameplayAbilitySpecHandle handle;
		((void (*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle*, FGameplayAbilitySpec*, void*)) GiveAbilityAndActivateOnce)(AbilitySystemComponent, &handle, Spec, nullptr);

		free(Spec);

		if (PlayerController->MyFortPawn->HasLastReplicatedEmoteExecuted())
		{
			auto Pawn = PlayerController->MyFortPawn;

			Pawn->bIsPlayingEmote = true;

			auto OldEmote = Pawn->LastReplicatedEmoteExecuted;
			Pawn->LastReplicatedEmoteExecuted = Asset;

			Pawn->OnRep_LastReplicatedEmoteExecuted(OldEmote);

			Pawn->ForceNetUpdate();

			Pawn->EmoteStopped(false);
			Pawn->bMovingEmote = false;
		}
	}
}

void AFortPlayerControllerAthena::PlayEmoteInternal(AFortPlayerControllerAthena* PC, UObject* Asset)
{
	if (!PC || !PC->MyFortPawn || !Asset)
		return;

	auto AbilitySystemComponent = ((AFortPlayerStateAthena*)PC->PlayerState)->AbilitySystemComponent;

	if (auto CharacterVehicle = PC->Pawn->Cast<AFortCharacterVehicle>())
		AbilitySystemComponent = CharacterVehicle->OverrideAbilitySystemComponent;

	UObject* AbilityToUse = nullptr;

	static auto SprayClass = FindClass("AthenaSprayItemDefinition");
	if (Asset->IsA(SprayClass))
	{
		static auto SprayAbilityClass = FindObject<UClass>(L"/Game/Abilities/Sprays/GAB_Spray_Generic.GAB_Spray_Generic_C");
		AbilityToUse = SprayAbilityClass->GetDefaultObj();

		//PC->GetQuestManager(1)->SendStatEvent(PC, EFortQuestObjectiveStatEvent::GetSpray(), 1, true, nullptr);
	}
	else if (auto ToyAsset = Asset->Cast<UAthenaToyItemDefinition>())
	{
		AbilityToUse = ToyAsset->ToySpawnAbility->GetDefaultObj();
		//PC->GetQuestManager(1)->SendStatEvent(PC, EFortQuestObjectiveStatEvent::GetToy(), 1, true, nullptr);
	}
	else if (auto DanceAsset = Asset->Cast<UAthenaDanceItemDefinition>())
	{
		static auto HasbMovingEmote = PC->MyFortPawn->HasbMovingEmote();
		if (HasbMovingEmote)
			PC->MyFortPawn->bMovingEmote = DanceAsset->bMovingEmote;

		static auto HasWalkForwardSpeed = PC->MyFortPawn->HasEmoteWalkSpeed();
		if (HasWalkForwardSpeed)
			PC->MyFortPawn->EmoteWalkSpeed = DanceAsset->WalkForwardSpeed;

		static auto HasbMovingEmoteForwardOnly = PC->MyFortPawn->HasbMovingEmoteForwardOnly();
		if (HasbMovingEmoteForwardOnly)
			PC->MyFortPawn->bMovingEmoteForwardOnly = DanceAsset->bMoveForwardOnly;

		static auto HasbMovingEmoteFollowingOnly = PC->MyFortPawn->HasbMovingEmoteFollowingOnly();
		if (HasbMovingEmoteFollowingOnly)
			PC->MyFortPawn->bMovingEmoteFollowingOnly = DanceAsset->bMoveFollowingOnly;

		auto CustomAbility = DanceAsset->HasCustomDanceAbility() ? DanceAsset->CustomDanceAbility.Get() : nullptr;

		if (CustomAbility)
			AbilityToUse = CustomAbility->GetDefaultObj();
		else
		{
			static auto EmoteAbilityClass = FindObject<UClass>(L"/Game/Abilities/Emotes/GAB_Emote_Generic.GAB_Emote_Generic_C");
			AbilityToUse = EmoteAbilityClass->GetDefaultObj();
		}

		//PC->GetQuestManager(1)->SendStatEvent(PC, EFortQuestObjectiveStatEvent::GetEmote(), 1, true, nullptr);
	}

	if (AbilityToUse)
	{
		auto Spec = (FGameplayAbilitySpec*)malloc(FGameplayAbilitySpec::Size());
		memset(PBYTE(Spec), 0, FGameplayAbilitySpec::Size());

		if (ConstructAbilitySpec)
			((void (*)(FGameplayAbilitySpec*, const UObject*, int, int, UObject*)) ConstructAbilitySpec)(Spec, AbilityToUse, 1, -1, Asset);
		else
		{
			Spec->MostRecentArrayReplicationKey = -1;
			Spec->ReplicationID = -1;
			Spec->ReplicationKey = -1;
			Spec->Ability = (UFortGameplayAbility*)AbilityToUse;
			Spec->Level = 1;
			Spec->InputID = -1;
			Spec->Handle.Handle = rand();
			Spec->SourceObject = Asset;
		}
		FGameplayAbilitySpecHandle handle;
		((void (*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle*, FGameplayAbilitySpec*, void*)) GiveAbilityAndActivateOnce)(AbilitySystemComponent, &handle, Spec, nullptr);

		free(Spec);

		if (PC->MyFortPawn->HasLastReplicatedEmoteExecuted())
		{
			auto Pawn = PC->MyFortPawn;

			Pawn->bIsPlayingEmote = true;

			auto OldEmote = Pawn->LastReplicatedEmoteExecuted;
			Pawn->LastReplicatedEmoteExecuted = Asset;

			Pawn->OnRep_LastReplicatedEmoteExecuted(OldEmote);

			Pawn->ForceNetUpdate();

			Pawn->EmoteStopped(false);
			Pawn->bMovingEmote = false;
		}
	}

	PC->ForceNetUpdate();
}

uint8 ToDeathCause(AFortPlayerPawnAthena* Pawn, FGameplayTagContainer& DeathTags, bool bDBNO)
{
	static auto ToDeathCause = AFortPlayerStateAthena::GetDefaultObj()->GetFunction("ToDeathCause");
	if (ToDeathCause)
	{
		if (!AFortPlayerStateAthena::ToDeathCause__Ptr)
			AFortPlayerStateAthena::ToDeathCause__Ptr = ToDeathCause;

		return AFortPlayerStateAthena::ToDeathCause(DeathTags, bDBNO);
	}
	else if (VersionInfo.EngineVersion >= 4.19)
	{
		static uint64_t ToDeathCauseNative = 0;

		if (!ToDeathCauseNative)
		{
			if (VersionInfo.EngineVersion == 4.19)
				ToDeathCauseNative = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC 20 41 0F B6 F8 48 8B DA 48 8B F1 E8 ? ? ? ? 33 ED").Get();
			else if (VersionInfo.EngineVersion == 4.20)
				ToDeathCauseNative = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 0F B6 FA 48 8B D9 E8 ? ? ? ? 33 F6 48 89 74 24").Get();
			else if (VersionInfo.EngineVersion == 4.21)
				ToDeathCauseNative = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 0F B6 FA 48 8B D9 E8 ? ? ? ? 33").Get();
		}


		if (ToDeathCauseNative)
		{
			if (VersionInfo.EngineVersion == 4.19)
			{
				static uint8(*ToDeathCause_)(AFortPlayerPawnAthena * Pawn, FGameplayTagContainer TagContainer, char bDBNO) = decltype(ToDeathCause_)(ToDeathCauseNative);
				return ToDeathCause_(Pawn, DeathTags, bDBNO);
			}
			else
			{
				static uint8(*ToDeathCause_)(FGameplayTagContainer TagContainer, char bDBNO) = decltype(ToDeathCause_)(ToDeathCauseNative);
				return ToDeathCause_(DeathTags, bDBNO);
			}
		}
	}

	return 0;
}

static bool IsUsableDeathObject(const UObject* Object)
{
	if (!Object || IsBadReadPtr((void*)Object))
		return false;

	if (Object->Index < 0 || Object->Index >= TUObjectArray::Num())
		return false;

	auto Item = TUObjectArray::GetItemByIndex(Object->Index);
	const int32 InvalidObjectFlags =
		Offsets::bEncryptedObjects ? 0x10200000 : 0x20;
	if (!Item || Item->GetObject() != Object ||
		(Item->GetFlags() & InvalidObjectFlags))
		return false;

	return Object->Class && !IsBadReadPtr(Object->Class);
}

static bool IsManagedNonRespawningBot(
	AFortPlayerControllerAthena* PlayerController)
{
	// Both Magnesium PlayerAI backends and `spawnbot` controllers are
	// connectionless, server-owned participants. A player respawn handshake
	// cannot complete normally for them; allowing the global override through
	// creates an untracked replacement pawn that can retain transition
	// invulnerability. Their first terminal death is therefore authoritative.
	return PlayerController &&
		(IsTrackedSpawnedBotController(PlayerController) ||
			MagnesiumPlayerAIIntegration::IsPlayerAIController(
				PlayerController));
}

static bool IsTerminalManagedBot(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!PlayerController)
		return false;

	if (IsTrackedSpawnedBotController(PlayerController))
		return true;

	auto AI = PlayerAIManager::FindByController(PlayerController);
	return AI &&
		(AI->bDeathHandled ||
		 AI->GetState() == EPlayerAIState::Dead);
}

static bool IsHumanVictoryController(
	AFortPlayerControllerAthena* Controller)
{
	if (!IsUsableDeathObject(Controller) ||
		!Controller->HasPlayerState() ||
		!IsUsableDeathObject(Controller->PlayerState))
	{
		return false;
	}

	auto PlayerState =
		Controller->PlayerState->Cast<AFortPlayerStateAthena>();
	if (!PlayerState ||
		(PlayerState->HasbIsABot() && PlayerState->bIsABot) ||
		IsTrackedSpawnedBotController(Controller) ||
		MagnesiumPlayerAIIntegration::IsPlayerAIController(Controller))
	{
		return false;
	}

	return true;
}

static bool IsHumanVictoryCrownController(
	AFortPlayerControllerAthena* Controller)
{
	return FConfiguration::bCrownSlomo &&
		VersionInfo.FortniteVersion >= 19.0 &&
		VersionInfo.FortniteVersion < 26.0 &&
		IsHumanVictoryController(Controller);
}

static UFortControllerComponent_VictoryCrowns*
GetVictoryCrownComponent(AFortPlayerControllerAthena* Controller)
{
	if (!IsHumanVictoryCrownController(Controller))
		return nullptr;

	auto CrownComponentClass =
		UFortControllerComponent_VictoryCrowns::StaticClass();
	if (!CrownComponentClass ||
		!IsUsableDeathObject(CrownComponentClass))
	{
		return nullptr;
	}

	auto RawComponent =
		Controller->GetComponentByClass(CrownComponentClass);
	if (!IsUsableDeathObject(RawComponent) ||
		!RawComponent->IsA(CrownComponentClass))
	{
		return nullptr;
	}

	return (UFortControllerComponent_VictoryCrowns*)RawComponent;
}

static const UFortWorldItemDefinition* ResolveVictoryCrownDefinition(
	AFortPlayerControllerAthena* Controller)
{
	auto CrownComponent = GetVictoryCrownComponent(Controller);
	if (CrownComponent &&
		CrownComponent->HasCrownInventoryItemClass())
	{
		auto& CrownInventoryItemClass =
			CrownComponent->GetCrownInventoryItemClass();
		auto ConfiguredDefinition =
			CrownInventoryItemClass.Get();
		if (IsUsableDeathObject(ConfiguredDefinition))
			return ConfiguredDefinition;
	}

	auto FallbackDefinition =
		FindObject<UFortWorldItemDefinition>(
			L"/VictoryCrownsGameplay/Items/AGID_VictoryCrown.AGID_VictoryCrown");
	return IsUsableDeathObject(FallbackDefinition)
		? FallbackDefinition
		: nullptr;
}

struct FVictoryCrownOwnershipSnapshot
{
	uint64 Generation = 0;
	bool bHadCrown = false;
};

static uint64 GVictoryCrownSnapshotGeneration = 0;
static std::unordered_map<
	AFortPlayerControllerAthena*,
	FVictoryCrownOwnershipSnapshot>
	GVictoryCrownOwnershipSnapshots;

struct FPendingVictoryCrownNotification
{
	UWorld* World = nullptr;
	UNetDriver* NetDriver = nullptr;
	AFortPlayerControllerAthena* Controller = nullptr;
	AFortPlayerPawnAthena* WinnerPawn = nullptr;
	UFortWeaponItemDefinition* FinishingWeapon = nullptr;
	uint8 DeathCause = 0;
	uint64 EarliestReplicationPass = 0;
	bool bPlayWinEffects = false;
	bool bNotifyWon = false;
	bool bNotifyTeamWon = false;
};

static uint64 GVictoryCrownReplicationPass = 0;
static std::vector<FPendingVictoryCrownNotification>
	GPendingVictoryCrownNotifications;

static bool MarkVictoryCrownPropertyDirty(
	const UObject* Object,
	const wchar_t* PropertyName)
{
	if (VersionInfo.FortniteVersion < 19.0 ||
		VersionInfo.FortniteVersion >= 26.0 ||
		!IsUsableDeathObject(Object) ||
		!PropertyName)
		return false;

	static const UObject* PushModelHelpersDefault = nullptr;
	static UFunction* MarkPropertyDirtyFunction = nullptr;
	static bool bLoggedResolution = false;

	if (!IsUsableDeathObject(PushModelHelpersDefault) ||
		!IsUsableDeathObject(MarkPropertyDirtyFunction))
	{
		auto PushModelHelpersClass =
			FindObject<UClass>(
				L"/Script/Engine.NetPushModelHelpers");
		if (!IsUsableDeathObject(PushModelHelpersClass))
		{
			PushModelHelpersClass =
				FindClass("NetPushModelHelpers");
		}
		PushModelHelpersDefault =
			IsUsableDeathObject(PushModelHelpersClass)
				? PushModelHelpersClass->GetDefaultObj()
				: nullptr;
		MarkPropertyDirtyFunction =
			IsUsableDeathObject(PushModelHelpersDefault)
				? PushModelHelpersDefault->GetFunction(
					"MarkPropertyDirty")
				: nullptr;

		if (!bLoggedResolution)
		{
			bLoggedResolution = true;
			SDK::DbgLog(
				"[VictoryCrown] push-model helper class=%p default=%p function=%p\n",
				(void*)PushModelHelpersClass,
				(void*)PushModelHelpersDefault,
				(void*)MarkPropertyDirtyFunction);
		}
	}

	if (!IsUsableDeathObject(PushModelHelpersDefault) ||
		!IsUsableDeathObject(MarkPropertyDirtyFunction))
	{
		return false;
	}

	FName ReplicatedPropertyName(PropertyName);
	PushModelHelpersDefault->Call<void>(
		MarkPropertyDirtyFunction,
		(UObject*)Object,
		ReplicatedPropertyName);
	SDK::DbgLog(
		"[VictoryCrown] marked push property dirty object=%p property=%ls\n",
		(void*)Object, PropertyName);
	return true;
}

static bool TryGetVictoryCrownInInventory(
	AFortPlayerControllerAthena* Controller,
	UFortWorldItem*& OutCrown)
{
	OutCrown = nullptr;
	if (!IsHumanVictoryCrownController(Controller))
		return false;

	auto CrownComponent =
		GetVictoryCrownComponent(Controller);
	if (CrownComponent)
	{
		if (auto GetCrownFunction =
			CrownComponent->GetFunction(
				"GetCrownInPlayerInventory"))
		{
			OutCrown =
				CrownComponent->Call<UFortWorldItem*>(
					GetCrownFunction);
			if (IsUsableDeathObject(OutCrown))
				return true;

			OutCrown = nullptr;
		}
	}

	// Manually inserted lategame crowns can exist before the native crown
	// component has observed its inventory callback. Fall back to the actual
	// item instances so pre-end winner preparation still sees the held crown.
	auto Inventory = Controller->WorldInventory;
	if (!IsUsableDeathObject(Inventory))
		return CrownComponent != nullptr;

	auto CrownDefinition =
		ResolveVictoryCrownDefinition(Controller);
	if (!CrownDefinition)
		return CrownComponent != nullptr;

	for (auto ItemInstance : Inventory->Inventory.ItemInstances)
	{
		if (IsUsableDeathObject(ItemInstance) &&
			ItemInstance->ItemEntry.ItemDefinition ==
				CrownDefinition)
		{
			OutCrown = ItemInstance;
			break;
		}
	}

	return CrownComponent != nullptr ||
		OutCrown != nullptr;
}

static void SnapshotVictoryCrownOwnershipBeforeNativeDeath()
{
	if (!FConfiguration::bCrownSlomo ||
		VersionInfo.FortniteVersion < 19.0 ||
		VersionInfo.FortniteVersion >= 26.0)
	{
		return;
	}

	++GVictoryCrownSnapshotGeneration;
	GVictoryCrownOwnershipSnapshots.clear();
	auto World = UWorld::GetWorld();
	auto Driver = World ? (UNetDriver*)World->NetDriver : nullptr;
	if (!Driver)
		return;

	for (int32 Index = 0;
		Index < Driver->ClientConnections.Num(); Index++)
	{
		auto Connection = Driver->ClientConnections[Index];
		auto Controller =
			Connection && Connection->PlayerController
				? Connection->PlayerController
					->Cast<AFortPlayerControllerAthena>()
				: nullptr;
		if (!IsHumanVictoryCrownController(Controller))
			continue;

		UFortWorldItem* Crown = nullptr;
		if (!TryGetVictoryCrownInInventory(
			Controller, Crown))
		{
			continue;
		}

		GVictoryCrownOwnershipSnapshots[Controller] =
			{ GVictoryCrownSnapshotGeneration,
				IsUsableDeathObject(Crown) };
		SDK::DbgLog(
			"[VictoryCrown] pre-native snapshot generation=%llu controller=%p crown=%p\n",
			(unsigned long long)GVictoryCrownSnapshotGeneration,
			(void*)Controller, (void*)Crown);
	}
}

static bool SetVictoryCrownPlayerStateRoyalRoyale(
	AFortPlayerControllerAthena* WinnerController)
{
	auto WinnerPlayerState =
		WinnerController && WinnerController->PlayerState
			? WinnerController->PlayerState
				->Cast<AFortPlayerStateAthena>()
			: nullptr;
	auto PlayerStateCrownClass =
		UFortPlayerStateComponent_VictoryCrowns::StaticClass();
	auto RawPlayerStateCrownComponent =
		IsUsableDeathObject(WinnerPlayerState) &&
		IsUsableDeathObject(PlayerStateCrownClass)
			? WinnerPlayerState->GetComponentByClass(
				PlayerStateCrownClass)
			: nullptr;
	if (!IsUsableDeathObject(
			RawPlayerStateCrownComponent) ||
		!RawPlayerStateCrownComponent->IsA(
			PlayerStateCrownClass))
	{
		return false;
	}

	auto PlayerStateCrownComponent =
		(UFortPlayerStateComponent_VictoryCrowns*)
			RawPlayerStateCrownComponent;
	if (!PlayerStateCrownComponent->HasbHasWonRoyalRoyale())
	{
		return false;
	}

	PlayerStateCrownComponent->bHasWonRoyalRoyale = true;
	MarkVictoryCrownPropertyDirty(
		PlayerStateCrownComponent,
		L"bHasWonRoyalRoyale");
	WinnerPlayerState->ForceNetUpdate();
	return true;
}

static bool PrepareVictoryCrownRoyalRoyaleBeforeEnd(
	AFortPlayerControllerAthena* WinnerController)
{
	if (!FConfiguration::bCrownSlomo ||
		!IsHumanVictoryCrownController(WinnerController))
	{
		return false;
	}

	auto CrownComponent =
		GetVictoryCrownComponent(WinnerController);
	if (!CrownComponent ||
		!CrownComponent->HasbWonRoyalRoyale())
	{
		return false;
	}

	if (CrownComponent->bWonRoyalRoyale)
	{
		MarkVictoryCrownPropertyDirty(
			CrownComponent,
			L"bWonRoyalRoyale");
		SetVictoryCrownPlayerStateRoyalRoyale(
			WinnerController);
		WinnerController->ForceNetUpdate();
		return true;
	}

	UFortWorldItem* HeldCrown = nullptr;
	if (!TryGetVictoryCrownInInventory(
		WinnerController, HeldCrown) ||
		!IsUsableDeathObject(HeldCrown))
	{
		return false;
	}

	// Native endgame snapshots these values while removing the final victim.
	// Set both the controller and PlayerState component before that transition,
	// not after its victory widget has already been created.
	if (CrownComponent->HasbWonCrownInMatch())
	{
		CrownComponent->bWonCrownInMatch = false;
		MarkVictoryCrownPropertyDirty(
			CrownComponent,
			L"bWonCrownInMatch");
	}
	CrownComponent->bWonRoyalRoyale = true;
	MarkVictoryCrownPropertyDirty(
		CrownComponent,
		L"bWonRoyalRoyale");
	if (auto OnRepRoyalRoyaleFunction =
		CrownComponent->GetFunction(
			"OnRep_WonRoyalRoyale"))
	{
		CrownComponent->Call(
			OnRepRoyalRoyaleFunction);
	}

	const bool bPlayerStateRoyalRoyale =
		SetVictoryCrownPlayerStateRoyalRoyale(
			WinnerController);

	WinnerController->ForceNetUpdate();

	SDK::DbgLog(
		"[VictoryCrown] prepared Royal Royale before end winner=%p crown=%p playerStateFlag=%d\n",
		(void*)WinnerController, (void*)HeldCrown,
		bPlayerStateRoyalRoyale ? 1 : 0);
	return true;
}

static void ApplyVictoryCrownWinState(
	AFortPlayerControllerAthena* WinnerController)
{
	if (!FConfiguration::bCrownSlomo ||
		VersionInfo.FortniteVersion < 19.0 ||
		VersionInfo.FortniteVersion >= 26.0 ||
		!IsHumanVictoryCrownController(WinnerController))
	{
		return;
	}

	auto Inventory = WinnerController->WorldInventory;
	if (!IsUsableDeathObject(Inventory))
	{
		SDK::DbgLog(
			"[VictoryCrown] skipped winner=%p: no usable inventory\n",
			(void*)WinnerController);
		return;
	}

	auto CrownComponent =
		GetVictoryCrownComponent(WinnerController);
	if (!CrownComponent ||
		!CrownComponent->HasbWonRoyalRoyale())
	{
		SDK::DbgLog(
			"[VictoryCrown] skipped winner=%p: component/flags unavailable\n",
			(void*)WinnerController);
		return;
	}

	auto OnRepWonCrownFunction =
		CrownComponent->GetFunction("OnRep_WonCrownInMatch");
	auto OnRepRoyalRoyaleFunction =
		CrownComponent->GetFunction("OnRep_WonRoyalRoyale");
	if (!OnRepRoyalRoyaleFunction)
	{
		SDK::DbgLog(
			"[VictoryCrown] skipped winner=%p: Royal Royale API unavailable\n",
			(void*)WinnerController);
		return;
	}

	bool bHasPreNativeSnapshot = false;
	bool bHadCrownBeforeNativeEnd = false;
	auto Snapshot =
		GVictoryCrownOwnershipSnapshots.find(
			WinnerController);
	if (Snapshot !=
			GVictoryCrownOwnershipSnapshots.end() &&
		Snapshot->second.Generation ==
			GVictoryCrownSnapshotGeneration)
	{
		bHasPreNativeSnapshot = true;
		bHadCrownBeforeNativeEnd =
			Snapshot->second.bHadCrown;
	}

	if (CrownComponent->bWonRoyalRoyale)
	{
		MarkVictoryCrownPropertyDirty(
			CrownComponent,
			L"bWonRoyalRoyale");
		SetVictoryCrownPlayerStateRoyalRoyale(
			WinnerController);
		WinnerController->ForceNetUpdate();
		return;
	}

	UFortWorldItem* ExistingCrown = nullptr;
	TryGetVictoryCrownInInventory(
		WinnerController, ExistingCrown);

	// Trust the pre-RemoveFromAlivePlayers snapshot over postgame inventory.
	// Native can consume or award a crown while starting endgame, making the
	// post-transition inventory alone ambiguous.
	const bool bShouldBeRoyalRoyale =
		bHasPreNativeSnapshot
			? bHadCrownBeforeNativeEnd
			: ExistingCrown != nullptr;
	if (bShouldBeRoyalRoyale)
	{
		if (IsUsableDeathObject(ExistingCrown) &&
			PrepareVictoryCrownRoyalRoyaleBeforeEnd(
				WinnerController))
		{
			return;
		}

		// The crown can be consumed from inventory during native endgame. The
		// pre-native snapshot still proves this was a crowned win.
		if (CrownComponent->HasbWonCrownInMatch())
		{
			CrownComponent->bWonCrownInMatch = false;
			MarkVictoryCrownPropertyDirty(
				CrownComponent,
				L"bWonCrownInMatch");
		}
		CrownComponent->bWonRoyalRoyale = true;
		MarkVictoryCrownPropertyDirty(
			CrownComponent,
			L"bWonRoyalRoyale");
		CrownComponent->Call(OnRepRoyalRoyaleFunction);
		SetVictoryCrownPlayerStateRoyalRoyale(
			WinnerController);
		WinnerController->ForceNetUpdate();
		SDK::DbgLog(
			"[VictoryCrown] restored Royal Royale winner=%p preNative=%d currentCrown=%p\n",
			(void*)WinnerController,
			bHadCrownBeforeNativeEnd ? 1 : 0,
			(void*)ExistingCrown);
		return;
	}

	// The custom-map and normal placement paths can overlap. A false pre-native
	// snapshot plus this flag means the first path already awarded the crown;
	// do not reinterpret that newly granted item as a crowned win.
	if (CrownComponent->HasbWonCrownInMatch() &&
		CrownComponent->bWonCrownInMatch)
		return;

	// Native may already have awarded the ordinary winner a crown after our
	// pre-native snapshot. Keep it as a normal crown award and do not duplicate.
	if (bHasPreNativeSnapshot &&
		!bHadCrownBeforeNativeEnd &&
		IsUsableDeathObject(ExistingCrown))
	{
		if (CrownComponent->HasbWonCrownInMatch())
		{
			CrownComponent->bWonCrownInMatch = true;
			MarkVictoryCrownPropertyDirty(
				CrownComponent,
				L"bWonCrownInMatch");
		}
		if (OnRepWonCrownFunction)
			CrownComponent->Call(OnRepWonCrownFunction);
		WinnerController->ForceNetUpdate();
		SDK::DbgLog(
			"[VictoryCrown] retained native crown award winner=%p crown=%p\n",
			(void*)WinnerController, (void*)ExistingCrown);
		return;
	}

	auto CrownDefinition =
		ResolveVictoryCrownDefinition(WinnerController);
	if (!CrownDefinition ||
		!FFortItemEntryStateValue::StaticStruct() ||
		!FFortItemEntryStateValue::HasIntValue() ||
		!FFortItemEntryStateValue::HasStateType())
	{
		SDK::DbgLog(
			"[VictoryCrown] skipped winner=%p: crown asset/state struct unavailable\n",
			(void*)WinnerController);
		return;
	}

	const int32 StateValueSize =
		FFortItemEntryStateValue::Size();
	if (StateValueSize <= 0 || StateValueSize > 0x1000)
	{
		SDK::DbgLog(
			"[VictoryCrown] skipped winner=%p: invalid state value size=%d\n",
			(void*)WinnerController, StateValueSize);
		return;
	}

	TArray<FFortItemEntryStateValue> StateValues{};
	auto StateValue = (FFortItemEntryStateValue*)malloc(
		StateValueSize);
	if (!StateValue)
		return;

	memset((PBYTE)StateValue, 0, StateValueSize);
	StateValue->IntValue = 1;
	StateValue->StateType = 2;
	StateValues.Add(*StateValue, StateValueSize);
	free(StateValue);

	auto GrantedCrown = Inventory->GiveItem(
		CrownDefinition, 1, 0, 0, true, true, 0, StateValues);
	StateValues.Free();
	if (!GrantedCrown)
	{
		SDK::DbgLog(
			"[VictoryCrown] failed to grant crown winner=%p definition=%p\n",
			(void*)WinnerController, (void*)CrownDefinition);
		return;
	}

	// Winning without a crown awards one for the next match. This is distinct
	// from Royal Royale and must not set both flags.
	if (CrownComponent->HasbWonCrownInMatch())
	{
		CrownComponent->bWonCrownInMatch = true;
		MarkVictoryCrownPropertyDirty(
			CrownComponent,
			L"bWonCrownInMatch");
	}
	if (OnRepWonCrownFunction)
		CrownComponent->Call(OnRepWonCrownFunction);
	WinnerController->ForceNetUpdate();
	SDK::DbgLog(
		"[VictoryCrown] awarded crown winner=%p item=%p\n",
		(void*)WinnerController, (void*)GrantedCrown);
}

static std::vector<AFortPlayerControllerAthena*>
GetHumanVictoryControllersForWinningTeam(
	AFortPlayerControllerAthena* WinnerController)
{
	if (!IsHumanVictoryCrownController(WinnerController))
		return {};

	auto WinnerPlayerState =
		WinnerController->PlayerState->Cast<AFortPlayerStateAthena>();
	if (!WinnerPlayerState)
		return {};

	const uint8 WinnerTeam = WinnerPlayerState->TeamIndex;
	const bool bHasUsableTeam =
		WinnerTeam != 0 && WinnerTeam != 255;
	std::vector<AFortPlayerControllerAthena*> WinningControllers;

	auto AddWinningController =
		[&](AFortPlayerControllerAthena* Candidate)
		{
			if (!IsHumanVictoryCrownController(Candidate))
				return;

			auto CandidatePlayerState =
				Candidate->PlayerState->Cast<AFortPlayerStateAthena>();
			if (!CandidatePlayerState ||
				(Candidate != WinnerController &&
					(!bHasUsableTeam ||
						CandidatePlayerState->TeamIndex != WinnerTeam)) ||
				std::find(
					WinningControllers.begin(),
					WinningControllers.end(),
					Candidate) != WinningControllers.end())
			{
				return;
			}

			WinningControllers.push_back(Candidate);
		};

	AddWinningController(WinnerController);

	auto World = UWorld::GetWorld();
	auto Driver = World ? (UNetDriver*)World->NetDriver : nullptr;
	if (Driver)
	{
		for (int32 Index = 0;
			Index < Driver->ClientConnections.Num(); Index++)
		{
			auto Connection = Driver->ClientConnections[Index];
			AddWinningController(
				Connection && Connection->PlayerController
					? Connection->PlayerController
						->Cast<AFortPlayerControllerAthena>()
					: nullptr);
		}
	}

	return WinningControllers;
}

static bool PrepareVictoryCrownRoyalRoyaleForWinningTeam(
	AFortPlayerControllerAthena* WinnerController)
{
	bool bPreparedRoyalRoyale = false;
	for (auto Controller :
		GetHumanVictoryControllersForWinningTeam(
			WinnerController))
	{
		bPreparedRoyalRoyale |=
			PrepareVictoryCrownRoyalRoyaleBeforeEnd(
				Controller);
	}
	return bPreparedRoyalRoyale;
}

static void ApplyVictoryCrownWinStateToWinningTeam(
	AFortPlayerControllerAthena* WinnerController)
{
	for (auto Controller :
		GetHumanVictoryControllersForWinningTeam(
			WinnerController))
	{
		ApplyVictoryCrownWinState(Controller);
	}
}

static bool HasPreparedRoyalRoyaleState(
	AFortPlayerControllerAthena* WinnerController)
{
	auto CrownComponent =
		GetVictoryCrownComponent(WinnerController);
	return CrownComponent &&
		CrownComponent->HasbWonRoyalRoyale() &&
		CrownComponent->bWonRoyalRoyale;
}

static void SendOrDeferVictoryNotifications(
	AFortPlayerControllerAthena* WinnerController,
	AFortPlayerPawnAthena* WinnerPawn,
	UFortWeaponItemDefinition* FinishingWeapon,
	uint8 DeathCause,
	bool bPlayWinEffects,
	bool bNotifyWon,
	bool bNotifyTeamWon)
{
	if (!IsHumanVictoryController(WinnerController))
		return;

	if (VersionInfo.FortniteVersion >= 19.0 &&
		VersionInfo.FortniteVersion < 26.0 &&
		FConfiguration::bCrownSlomo &&
		HasPreparedRoyalRoyaleState(WinnerController))
	{
		auto Pending = std::find_if(
			GPendingVictoryCrownNotifications.begin(),
			GPendingVictoryCrownNotifications.end(),
			[&](const FPendingVictoryCrownNotification& Entry)
			{
				return Entry.Controller == WinnerController;
			});

		FPendingVictoryCrownNotification Notification{};
		Notification.World = UWorld::GetWorld();
		Notification.NetDriver =
			Notification.World
				? (UNetDriver*)Notification.World->NetDriver
				: nullptr;
		Notification.Controller = WinnerController;
		Notification.WinnerPawn = WinnerPawn;
		Notification.FinishingWeapon = FinishingWeapon;
		Notification.DeathCause = DeathCause;
		// Waiting through the following completed TickFlush guarantees at
		// least one full property-replication pass after crown preparation.
		Notification.EarliestReplicationPass =
			GVictoryCrownReplicationPass + 2;
		Notification.bPlayWinEffects = bPlayWinEffects;
		Notification.bNotifyWon = bNotifyWon;
		Notification.bNotifyTeamWon = bNotifyTeamWon;

		if (Pending !=
			GPendingVictoryCrownNotifications.end())
		{
			Notification.bPlayWinEffects |=
				Pending->bPlayWinEffects;
			Notification.bNotifyWon |=
				Pending->bNotifyWon;
			Notification.bNotifyTeamWon |=
				Pending->bNotifyTeamWon;
			*Pending = Notification;
		}
		else
		{
			GPendingVictoryCrownNotifications.push_back(
				Notification);
		}

		WinnerController->ForceNetUpdate();
		if (IsUsableDeathObject(
				WinnerController->PlayerState))
		{
			((AActor*)WinnerController->PlayerState)
				->ForceNetUpdate();
		}

		SDK::DbgLog(
			"[VictoryCrown] deferred win notification winner=%p untilReplicationPass=%llu currentPass=%llu\n",
			(void*)WinnerController,
			(unsigned long long)
				Notification.EarliestReplicationPass,
			(unsigned long long)
				GVictoryCrownReplicationPass);
		return;
	}

	if (bPlayWinEffects)
	{
		WinnerController->PlayWinEffects(
			WinnerPawn, FinishingWeapon,
			DeathCause, false);
	}
	if (bNotifyWon)
	{
		WinnerController->ClientNotifyWon(
			WinnerPawn, FinishingWeapon,
			DeathCause);
	}
	if (bNotifyTeamWon)
	{
		WinnerController->ClientNotifyTeamWon(
			WinnerPawn, FinishingWeapon,
			DeathCause);
	}
}

void AFortPlayerControllerAthena::
TickPendingVictoryCrownNotifications()
{
	if (VersionInfo.FortniteVersion < 19.0 ||
		VersionInfo.FortniteVersion >= 26.0)
	{
		return;
	}

	++GVictoryCrownReplicationPass;

	for (size_t Index = 0;
		Index < GPendingVictoryCrownNotifications.size();)
	{
		auto Notification =
			GPendingVictoryCrownNotifications[Index];
		if (Notification.EarliestReplicationPass >
			GVictoryCrownReplicationPass)
		{
			++Index;
			continue;
		}

		GPendingVictoryCrownNotifications.erase(
			GPendingVictoryCrownNotifications.begin() +
				Index);

		auto WinnerController = Notification.Controller;
		auto CurrentWorld = UWorld::GetWorld();
		auto CurrentDriver =
			CurrentWorld
				? (UNetDriver*)CurrentWorld->NetDriver
				: nullptr;
		if (Notification.World != CurrentWorld ||
			Notification.NetDriver != CurrentDriver ||
			!CurrentDriver ||
			!IsHumanVictoryController(WinnerController))
			continue;

		bool bControllerStillConnected = false;
		for (auto Connection :
			CurrentDriver->ClientConnections)
		{
			if (Connection &&
				Connection->PlayerController ==
					WinnerController)
			{
				bControllerStillConnected = true;
				break;
			}
		}
		if (!bControllerStillConnected)
			continue;

		auto WinnerPawn =
			IsUsableDeathObject(Notification.WinnerPawn)
				? Notification.WinnerPawn
				: (IsUsableDeathObject(
						WinnerController->MyFortPawn)
					? WinnerController->MyFortPawn
					: nullptr);
		auto FinishingWeapon =
			IsUsableDeathObject(
				Notification.FinishingWeapon)
				? Notification.FinishingWeapon
				: nullptr;

		if (Notification.bPlayWinEffects)
		{
			WinnerController->PlayWinEffects(
				WinnerPawn, FinishingWeapon,
				Notification.DeathCause, false);
		}
		if (Notification.bNotifyWon)
		{
			WinnerController->ClientNotifyWon(
				WinnerPawn, FinishingWeapon,
				Notification.DeathCause);
		}
		if (Notification.bNotifyTeamWon)
		{
			WinnerController->ClientNotifyTeamWon(
				WinnerPawn, FinishingWeapon,
				Notification.DeathCause);
		}

		SDK::DbgLog(
			"[VictoryCrown] sent replicated win notification winner=%p replicationPass=%llu\n",
			(void*)WinnerController,
			(unsigned long long)
				GVictoryCrownReplicationPass);
	}

}

static AFortPlayerControllerAthena*
ResolveImpendingVictoryWinnerBeforeNativeDeath(
	AFortGameMode* GameMode,
	AFortGameStateAthena* GameState,
	AFortPlayerControllerAthena* VictimController,
	AFortPlayerControllerAthena* PreferredKillerController)
{
	if (!IsUsableDeathObject(GameMode) ||
		!IsUsableDeathObject(GameState) ||
		!IsUsableDeathObject(VictimController))
	{
		return nullptr;
	}

	bool bFoundVictim = false;
	std::vector<AFortPlayerControllerAthena*>
		SurvivingControllers;
	auto AddSurvivingController =
		[&](AActor* AliveActor)
		{
			if (!IsUsableDeathObject(AliveActor))
				return;

			auto Candidate =
				AliveActor
					->Cast<AFortPlayerControllerAthena>();
			if (!IsUsableDeathObject(Candidate))
				return;

			if (Candidate == VictimController)
			{
				bFoundVictim = true;
				return;
			}

			if (!Candidate->HasPlayerState() ||
				!IsUsableDeathObject(
					Candidate->PlayerState) ||
				std::find(
					SurvivingControllers.begin(),
					SurvivingControllers.end(),
					Candidate) !=
					SurvivingControllers.end())
			{
				return;
			}

			SurvivingControllers.push_back(Candidate);
		};

	if (GameMode->HasAlivePlayers())
	{
		for (auto AliveActor : GameMode->AlivePlayers)
			AddSurvivingController(AliveActor);
	}
	if (GameMode->HasAliveBots())
	{
		for (auto AliveActor : GameMode->AliveBots)
			AddSurvivingController(AliveActor);
	}

	if (!bFoundVictim ||
		SurvivingControllers.empty() ||
		GameMode->MatchState ==
			FName(L"WaitingPostMatch"))
	{
		return nullptr;
	}

	// If a controller is absent from the alive arrays, an apparently sole team
	// is not trustworthy. Waiting for the post-native fallback is safer than
	// marking a non-final elimination as a Crowned Victory.
	const int32 SurvivingControllerCount =
		(int32)SurvivingControllers.size();
	const bool bPlayersLeftMatchesPreRemoval =
		GameState->PlayersLeft ==
			SurvivingControllerCount + 1;
	const bool bPlayersLeftMatchesEarlyRemoval =
		GameState->PlayersLeft ==
			SurvivingControllerCount;
	if (GameState->PlayersLeft > 0 &&
		!bPlayersLeftMatchesPreRemoval &&
		!bPlayersLeftMatchesEarlyRemoval)
	{
		SDK::DbgLog(
			"[VictoryCrown] impending winner rejected FN=%.2f playersLeft=%d survivors=%d victimFound=%d\n",
			VersionInfo.FortniteVersion,
			GameState->PlayersLeft,
			SurvivingControllerCount,
			bFoundVictim ? 1 : 0);
		return nullptr;
	}

	auto FirstState =
		SurvivingControllers[0]->PlayerState
			->Cast<AFortPlayerStateAthena>();
	if (!FirstState)
		return nullptr;

	const uint8 SurvivingTeam = FirstState->TeamIndex;
	const bool bHasUsableSurvivingTeam =
		SurvivingTeam != 0 && SurvivingTeam != 255;
	for (size_t Index = 1;
		Index < SurvivingControllers.size(); Index++)
	{
		auto CandidateState =
			SurvivingControllers[Index]->PlayerState
				->Cast<AFortPlayerStateAthena>();
		if (!bHasUsableSurvivingTeam ||
			!CandidateState ||
			CandidateState->TeamIndex !=
				SurvivingTeam)
		{
			return nullptr;
		}
	}

	if (std::find(
			SurvivingControllers.begin(),
			SurvivingControllers.end(),
			PreferredKillerController) !=
			SurvivingControllers.end() &&
		IsHumanVictoryCrownController(
			PreferredKillerController))
	{
		return PreferredKillerController;
	}

	for (auto Controller : SurvivingControllers)
	{
		if (IsHumanVictoryCrownController(Controller))
			return Controller;
	}

	return nullptr;
}

static AFortPlayerControllerAthena*
GetHumanVictoryControllerForState(
	AFortPlayerStateAthena* PlayerState,
	AFortPlayerControllerAthena* ExcludedController)
{
	if (!IsUsableDeathObject(PlayerState))
		return nullptr;

	if (PlayerState->HasOwner() &&
		IsUsableDeathObject(PlayerState->Owner))
	{
		auto OwnerController =
			PlayerState->Owner->Cast<AFortPlayerControllerAthena>();
		if (OwnerController != ExcludedController &&
			IsHumanVictoryController(OwnerController))
			return OwnerController;
	}

	auto World = UWorld::GetWorld();
	auto Driver = World ? (UNetDriver*)World->NetDriver : nullptr;
	if (!Driver)
		return nullptr;

	for (int32 Index = 0;
		Index < Driver->ClientConnections.Num(); Index++)
	{
		auto Connection = Driver->ClientConnections[Index];
		auto Candidate =
			Connection && Connection->PlayerController
				? Connection->PlayerController
					->Cast<AFortPlayerControllerAthena>()
				: nullptr;
		if (Candidate != ExcludedController &&
			IsHumanVictoryController(Candidate) &&
			Candidate->PlayerState == PlayerState)
		{
			return Candidate;
		}
	}

	return nullptr;
}

static AFortPlayerControllerAthena*
ResolveNativeVictoryWinner(
	AFortGameMode* GameMode,
	AFortGameStateAthena* GameState,
	AFortPlayerStateAthena* PreferredWinnerState,
	AFortPlayerControllerAthena* ExcludedController)
{
	if (!IsUsableDeathObject(GameMode) ||
		!IsUsableDeathObject(GameState))
	{
		return nullptr;
	}

	if (GameState->HasWinningPlayerState())
	{
		if (auto NativeWinner =
			GetHumanVictoryControllerForState(
				GameState->WinningPlayerState,
				ExcludedController))
		{
			return NativeWinner;
		}
	}

	if (GameState->HasWinningTeam() &&
		GameState->WinningTeam > 0 &&
		GameState->WinningTeam < 255)
	{
		auto World = UWorld::GetWorld();
		auto Driver = World ? (UNetDriver*)World->NetDriver : nullptr;
		if (Driver)
		{
			for (int32 Index = 0;
				Index < Driver->ClientConnections.Num(); Index++)
			{
				auto Connection = Driver->ClientConnections[Index];
				auto Candidate =
					Connection && Connection->PlayerController
						? Connection->PlayerController
							->Cast<AFortPlayerControllerAthena>()
						: nullptr;
				if (Candidate == ExcludedController ||
					!IsHumanVictoryController(Candidate))
					continue;

				auto CandidateState =
					Candidate->PlayerState
						->Cast<AFortPlayerStateAthena>();
				if (CandidateState &&
					CandidateState->TeamIndex ==
						(uint8)GameState->WinningTeam)
				{
					return Candidate;
				}
			}
		}
	}

	if (IsUsableDeathObject(PreferredWinnerState) &&
		PreferredWinnerState->HasPlace() &&
		PreferredWinnerState->Place == 1)
	{
		if (auto PreferredWinner =
			GetHumanVictoryControllerForState(
				PreferredWinnerState,
				ExcludedController))
		{
			return PreferredWinner;
		}
	}

	if (GameMode->HasAlivePlayers())
	{
		for (auto AliveActor : GameMode->AlivePlayers)
		{
			auto Candidate =
				AliveActor
					? AliveActor->Cast<AFortPlayerControllerAthena>()
					: nullptr;
			if (Candidate != ExcludedController &&
				IsHumanVictoryController(Candidate))
				return Candidate;
		}
	}

	return nullptr;
}

static bool IsLiveRemoteControlReturnPawn(AActor* Actor)
{
	if (!IsUsableDeathObject(Actor))
		return false;
	if (IsRemoteControlledPawn(Actor) ||
		IsNativeVehiclePossessionPawn(Actor))
		return false;

	auto Pawn = Actor->Cast<AFortPlayerPawnAthena>();
	if (!Pawn || Pawn->GetHealth() <= 0.f)
		return false;
	if (Pawn->HasbIsDying() && Pawn->bIsDying)
		return false;
	if (Pawn->HasbPlayedDying() && Pawn->bPlayedDying)
		return false;
	if (Pawn->HasbIsDBNO() && Pawn->bIsDBNO)
		return false;

	return true;
}

static AFortWeapon* GetPawnCurrentWeaponSafe(AFortPlayerPawnAthena* Pawn)
{
	if (!IsUsableDeathObject(Pawn) || !Pawn->HasCurrentWeapon())
		return nullptr;

	auto CurrentWeapon = Pawn->CurrentWeapon;
	if (!IsUsableDeathObject(CurrentWeapon))
		return nullptr;

	return CurrentWeapon->Cast<AFortWeapon>();
}

static UFortWeaponItemDefinition* GetWeaponDataSafe(AFortWeapon* Weapon)
{
	if (!IsUsableDeathObject(Weapon) || !Weapon->HasWeaponData())
		return nullptr;

	auto WeaponData = Weapon->WeaponData;
	return IsUsableDeathObject(WeaponData) ? WeaponData : nullptr;
}

static AFortWeapon* ResolveDeathReportWeapon(FFortPlayerDeathReport& DeathReport)
{
	auto DamageCauser = DeathReport.HasDamageCauser() ? DeathReport.DamageCauser : nullptr;
	if (!IsUsableDeathObject(DamageCauser))
		return nullptr;

	static auto ProjectileBaseClass = FindClass("FortProjectileBase");
	if (ProjectileBaseClass && DamageCauser->IsA(ProjectileBaseClass))
	{
		auto Owner = DamageCauser->HasOwner() ? DamageCauser->Owner : nullptr;
		if (!IsUsableDeathObject(Owner))
			return nullptr;

		if (auto Weapon = Owner->Cast<AFortWeapon>())
			return Weapon;

		if (auto Controller = Owner->Cast<AFortPlayerControllerAthena>())
			return Controller->HasPawn() ? GetPawnCurrentWeaponSafe(Controller->Pawn) : nullptr;

		if (auto Pawn = Owner->Cast<AFortPlayerPawnAthena>())
			return GetPawnCurrentWeaponSafe(Pawn);

		return nullptr;
	}

	return DamageCauser->Cast<AFortWeapon>();
}

static bool TryGetItemDefinitionDisplayName(UFortItemDefinition* ItemDefinition, std::string& OutName)
{
	if (!IsUsableDeathObject(ItemDefinition))
		return false;

	FText* NameText = nullptr;
	if (ItemDefinition->HasDisplayName())
		NameText = &ItemDefinition->DisplayName;
	else if (ItemDefinition->HasItemName())
		NameText = &ItemDefinition->ItemName;

	if (!NameText || IsBadReadPtr(NameText))
		return false;

	auto Name = UKismetTextLibrary::Conv_TextToString(*NameText);
	auto ConvertedName = Name.ToString();
	if (ConvertedName.empty())
		return false;

	OutName = ConvertedName;
	return true;
}

uint64 RemoveFromAlivePlayers_ = 0;

static void PurgeExclusiveGadgets(AFortPlayerControllerAthena* PlayerController)
{
	if (!IsUsableDeathObject(PlayerController))
		return;

	auto Inventory = PlayerController->WorldInventory;
	if (!IsUsableDeathObject(Inventory))
		return;

	// Native special-gadget code can retain or restore its item while handling
	// death. Remove every exclusive gadget until neither the replicated entry
	// nor its item instance remains. The S4 Infinity Gauntlet is identified by
	// bDropAllOnEquip, so this does not depend on a hard-coded asset name.
	for (int32 RemovalAttempt = 0; RemovalAttempt < 32; RemovalAttempt++)
	{
		FGuid GadgetGuid{};
		bool bFound = false;
		for (int32 Index = 0; Index < Inventory->Inventory.ReplicatedEntries.Num(); Index++)
		{
			auto& Entry = Inventory->Inventory.ReplicatedEntries.Get(Index, FFortItemEntry::Size());
			auto Gadget = Entry.ItemDefinition ? Entry.ItemDefinition->Cast<UFortGadgetItemDefinition>() : nullptr;
			if (Gadget && Gadget->HasbDropAllOnEquip() && Gadget->bDropAllOnEquip)
			{
				GadgetGuid = Entry.ItemGuid;
				bFound = true;
				break;
			}
		}

		if (!bFound)
			break;

		const int32 PreviousEntryCount = Inventory->Inventory.ReplicatedEntries.Num();
		Inventory->Remove(GadgetGuid);
		if (Inventory->Inventory.ReplicatedEntries.Num() >= PreviousEntryCount)
			break;
	}
}

static void RestoreRespawnHiddenWeapon(AFortPlayerControllerAthena* PlayerController)
{
	if (!PlayerController)
		return;

	auto Existing = GRespawnHiddenWeapons.find(PlayerController);
	if (Existing == GRespawnHiddenWeapons.end())
		return;

	auto Weapon = Existing->second.Get();
	GRespawnHiddenWeapons.erase(Existing);

	if (!IsUsableDeathObject(Weapon))
		return;

	Weapon->SetActorHiddenInGame(false);
	Weapon->ForceNetUpdate();
	SDK::DbgLog("[Respawn] restored weapon visibility controller=%p weapon=%p\n",
		(void*)PlayerController, (void*)Weapon);
}

static void HideRespawnWeaponForGlide(AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* Pawn)
{
	if (!PlayerController || !Pawn)
		return;

	auto Weapon = GetPawnCurrentWeaponSafe(Pawn);
	if (!Weapon)
		return;

	auto Existing = GRespawnHiddenWeapons.find(PlayerController);
	if (Existing != GRespawnHiddenWeapons.end())
	{
		auto PreviousWeapon = Existing->second.Get();
		GRespawnHiddenWeapons.erase(Existing);
		if (PreviousWeapon && PreviousWeapon != Weapon && IsUsableDeathObject(PreviousWeapon))
		{
			PreviousWeapon->SetActorHiddenInGame(false);
			PreviousWeapon->ForceNetUpdate();
		}
	}

	Weapon->SetActorHiddenInGame(true);
	Weapon->ForceNetUpdate();
	GRespawnHiddenWeapons.emplace(PlayerController, TWeakObjectPtr<AFortWeapon>(Weapon));
	SDK::DbgLog("[Respawn] hid equipped weapon for glide controller=%p pawn=%p weapon=%p\n",
		(void*)PlayerController, (void*)Pawn, (void*)Weapon);
}

static bool RequestLegacyClientEquipmentRefresh(
	AFortPlayerControllerAthena* PlayerController, const FGuid& ItemGuid)
{
	if (!PlayerController || !UsesEarlyAthenaLandingClientRefresh())
		return false;
	if (IsTrackedSpawnedBotController(PlayerController))
		return false;
	if (MagnesiumPlayerAIIntegration::IsPlayerAIController(PlayerController))
		return false;

	auto ClientExecuteInventoryItem =
		PlayerController->GetFunction("ClientExecuteInventoryItem");
	if (!ClientExecuteInventoryItem)
		return false;

	FGuid Guid = ItemGuid;
	float Delay = 0.f;
	bool bForceExecute = true;
	PlayerController->Call<void>(
		ClientExecuteInventoryItem, Guid, Delay, bForceExecute);
	return true;
}

static void RestoreEquipmentAfterRespawn(AFortPlayerControllerAthena* PlayerController,
	bool bForceLegacyReequip)
{
	if (!PlayerController || !PlayerController->WorldInventory)
		return;

	auto Pawn = PlayerController->MyFortPawn;
	if (!Pawn)
		return;

	const bool bRespawnGlidePending =
		GPendingRespawnLandingFinalization.find(PlayerController) !=
		GPendingRespawnLandingFinalization.end();
	if (!bRespawnGlidePending)
		RestoreRespawnHiddenWeapon(PlayerController);

	// Legacy quickbars cannot safely perform the forced server equip followed by
	// a second slot activation while the pawn is gliding. Apart from producing a
	// visible harvesting tool in the glider, the two transitions can split
	// CurrentWeapon from the client animation state. Leave the pawn unequipped
	// during the glide and perform one coherent equip after EndSkydiving.
	if (UsesEarlyAthenaLandingClientRefresh() && bRespawnGlidePending)
	{
		SDK::DbgLog("[Equipment] deferred legacy respawn equip controller=%p pawn=%p version=%.2f\n",
			(void*)PlayerController, (void*)Pawn, VersionInfo.FortniteVersion);
		return;
	}

	// Equipping on the new pawn re-grants the item-owned gameplay abilities. Start
	// every respawn with the harvesting tool so a remembered gun/last slot is not
	// rendered in the player's hands while their glider is active.
	auto EntryToEquip = FindHarvestingToolEntry(PlayerController->WorldInventory);

	// A malformed/custom inventory may genuinely have no harvesting tool. Keep a
	// usable weapon fallback so the replacement pawn is never left unequipped.
	if (!EntryToEquip)
		EntryToEquip = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([](FFortItemEntry& Entry)
		{
			return Entry.ItemDefinition && AFortInventory::IsPrimaryQuickbar(Entry.ItemDefinition) &&
				Entry.ItemDefinition->IsA<UFortWeaponItemDefinition>() &&
				!Entry.ItemDefinition->IsA<UFortWeaponMeleeItemDefinition>();
		}, FFortItemEntry::Size());

	if (!EntryToEquip)
		return;

	// Native landing can already restore the selected harvesting tool on these
	// early clients. Re-executing the same entry would replace that valid weapon
	// actor and put the legacy animation graph onto a discarded reference.
	auto CurrentWeapon = GetPawnCurrentWeaponSafe(Pawn);
	if (bForceLegacyReequip && UsesEarlyAthenaLandingClientRefresh() &&
		RequestLegacyClientEquipmentRefresh(PlayerController, EntryToEquip->ItemGuid))
	{
		if (CurrentWeapon)
		{
			CurrentWeapon->SetActorHiddenInGame(false);
			CurrentWeapon->ForceNetUpdate();
		}

		Pawn->ForceNetUpdate();
		PlayerController->ForceNetUpdate();
		SDK::DbgLog("[Equipment] requested early-client refresh version=%.2f controller=%p pawn=%p entry=%p weapon=%p\n",
			VersionInfo.FortniteVersion,
			(void*)PlayerController, (void*)Pawn, (void*)EntryToEquip,
			(void*)CurrentWeapon);
		return;
	}

	if (!bForceLegacyReequip && UsesEarlyAthenaLandingClientRefresh() && CurrentWeapon &&
		CurrentWeapon->ItemEntryGuid == EntryToEquip->ItemGuid)
	{
		CurrentWeapon->SetActorHiddenInGame(false);
		CurrentWeapon->ForceNetUpdate();
		SDK::DbgLog("[Equipment] retained existing early-client pickaxe version=%.2f controller=%p pawn=%p weapon=%p\n",
			VersionInfo.FortniteVersion, (void*)PlayerController,
			(void*)Pawn, (void*)CurrentWeapon);
		return;
	}

	PlayerController->ServerExecuteInventoryItem(EntryToEquip->ItemGuid);
	if (bRespawnGlidePending)
		HideRespawnWeaponForGlide(PlayerController, Pawn);

	if (VersionInfo.FortniteVersion > 3.00)
		PlayerController->ClientEquipItem(EntryToEquip->ItemGuid, true);
	else if (!UsesEarlyAthenaLandingClientRefresh() && PlayerController->QuickBars)
		PlayerController->QuickBars->ServerActivateSlotInternal(0, 0, 0.f, true);

	if (!bRespawnGlidePending)
	{
		CurrentWeapon = GetPawnCurrentWeaponSafe(Pawn);
		if (CurrentWeapon)
		{
			CurrentWeapon->SetActorHiddenInGame(false);
			CurrentWeapon->ForceNetUpdate();
		}
	}

}

void AFortPlayerControllerAthena::FinalizeRespawnAfterLanding(AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* Pawn)
{
	if (!PlayerController || !Pawn)
		return;

	const bool bRespawnLanding =
		GPendingRespawnLandingFinalization.erase(PlayerController) > 0;
	if (!bRespawnLanding)
	{
		// Early Athena replaces the lobby pawn for the late-game aircraft but can
		// retain either no CurrentWeapon or a stale actor from its native
		// skydiving transition. Establish one fresh pickaxe transition after
		// landing.
		const bool bAircraftLanding =
			GPendingLegacyAircraftLandingEquipment.erase(PlayerController) > 0;
		if (UsesEarlyAthenaLandingClientRefresh() && bAircraftLanding)
		{
			GLegacyAircraftSkydivingObserved.erase(PlayerController);
			RestoreEquipmentAfterRespawn(PlayerController, true);
			Pawn->ForceNetUpdate();
			PlayerController->ForceNetUpdate();
			SDK::DbgLog("[Equipment] restored early aircraft landing equip version=%.2f controller=%p pawn=%p weapon=%p\n",
				VersionInfo.FortniteVersion, (void*)PlayerController, (void*)Pawn,
				(void*)GetPawnCurrentWeaponSafe(Pawn));
		}
		return;
	}
	GRespawnSkydivingObserved.erase(PlayerController);

	ClearLegacyPawnDeathFlags(Pawn);

	PlayerController->StateName = FName(L"Playing");
	PlayerController->ClientGotoState(FName(L"Playing"));
	PlayerController->OnRep_Pawn();

	RestoreEquipmentAfterRespawn(PlayerController,
		UsesEarlyAthenaLandingClientRefresh());
	Pawn->ForceNetUpdate();
	PlayerController->ForceNetUpdate();
}

static void ScheduleSpawnedBotCleanup(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* Pawn,
	AFortPlayerStateAthena* PlayerState,
	AFortInventory* Inventory)
{
	RefreshSpawnedBotTrackingWorld();

	if (!PlayerController)
		return;

	for (const auto& Pending : GPendingSpawnedBotCleanup)
	{
		if (Pending.ControllerIdentity == PlayerController)
			return;
	}

	if (IsUsableDeathObject(Pawn))
	{
		if (Pawn->CharacterMovement)
		{
			Pawn->CharacterMovement->Velocity = FVector{};
			if (auto StopMovement =
				Pawn->CharacterMovement->GetFunction("StopMovementImmediately"))
			{
				Pawn->CharacterMovement->ProcessEvent(StopMovement, nullptr);
			}
			if (auto DisableMovement =
				Pawn->CharacterMovement->GetFunction("DisableMovement"))
			{
				Pawn->CharacterMovement->ProcessEvent(DisableMovement, nullptr);
			}
		}

		if (auto SetCollision = Pawn->GetFunction("SetActorEnableCollision"))
			Pawn->Call<void>(SetCollision, false);
		else
			Pawn->bActorEnableCollision = false;

		Pawn->ForceNetUpdate();
	}

	const float CleanupDelay =
		VersionInfo.FortniteVersion == 2.50 ? 3.f : 0.35f;
	GPendingSpawnedBotCleanup.push_back({
		TWeakObjectPtr<AFortPlayerControllerAthena>(PlayerController),
		PlayerController,
		TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn),
		TWeakObjectPtr<AFortPlayerStateAthena>(PlayerState),
		TWeakObjectPtr<AFortInventory>(Inventory),
		CleanupDelay
	});

	SDK::DbgLog("[Elimination] scheduled terminal spawnbot cleanup controller=%p pawn=%p playerState=%p inventory=%p delay=%.2f FN=%.2f\n",
		(void*)PlayerController, (void*)Pawn, (void*)PlayerState,
		(void*)Inventory, CleanupDelay,
		VersionInfo.FortniteVersion);
}

static bool IsControllerInAlivePlayers(
	AFortGameMode* GameMode,
	AFortPlayerControllerAthena* PlayerController)
{
	if (!IsUsableDeathObject(GameMode) ||
		!IsUsableDeathObject(PlayerController) ||
		!GameMode->HasAlivePlayers())
		return false;

	for (auto AliveActor : GameMode->AlivePlayers)
	{
		if (AliveActor == PlayerController)
			return true;
	}
	return false;
}

static bool Remove172SpawnedBotFromAlivePlayersAfterNative(
	AFortGameMode* GameMode,
	AFortGameStateAthena* GameState,
	AFortPlayerControllerAthena* VictimController,
	AFortPlayerPawnAthena* ReportedKillerPawn,
	AFortPlayerControllerAthena* ReportedKillerController)
{
	if (VersionInfo.FortniteVersion != 1.72 ||
		!IsUsableDeathObject(GameMode) ||
		!IsUsableDeathObject(GameState) ||
		!IsUsableDeathObject(VictimController))
	{
		return false;
	}

	if (!IsControllerInAlivePlayers(GameMode, VictimController))
		return true;

	if (!RemoveFromAlivePlayers_)
	{
		SDK::DbgLog(
			"[Elimination] cannot remove 1.72 spawnbot: RemoveFromAlivePlayers finder is null controller=%p\n",
			(void*)VictimController);
		return false;
	}

	auto KillerController =
		IsUsableDeathObject(ReportedKillerController) &&
		ReportedKillerController != VictimController
		? ReportedKillerController : nullptr;
	if (!KillerController && IsUsableDeathObject(ReportedKillerPawn) &&
		IsUsableDeathObject(ReportedKillerPawn->Controller))
	{
		auto PawnController =
			ReportedKillerPawn->Controller->Cast<AFortPlayerControllerAthena>();
		if (PawnController && PawnController != VictimController)
			KillerController = PawnController;
	}

	AFortPlayerStateAthena* KillerPlayerState = nullptr;
	if (KillerController && KillerController->HasPlayerState() &&
		IsUsableDeathObject(KillerController->PlayerState))
	{
		KillerPlayerState =
			(AFortPlayerStateAthena*)KillerController->PlayerState;
	}

	const int AliveBefore = GameMode->AlivePlayers.Num();
	const int PlayersLeftBefore = GameState->PlayersLeft;

	// 1.7.2's actual ABI has three arguments. Its implementation owns the
	// AlivePlayers removal, bMarkedAlive/team bookkeeping, PlayersLeft update,
	// winner selection and the final EndMatch transition.
	using RemoveFromAlivePlayers172 =
		void (*)(AFortGameMode*, AFortPlayerControllerAthena*,
			AFortPlayerStateAthena*);
	((RemoveFromAlivePlayers172)RemoveFromAlivePlayers_)(
		GameMode, VictimController, KillerPlayerState);

	const bool bStillRegistered =
		IsControllerInAlivePlayers(GameMode, VictimController);
	SDK::DbgLog(
		"[Elimination] 1.72 spawnbot removal attempted controller=%p finder=%p alive=%d->%d playersLeft=%d->%d postMatch=%d stillRegistered=%d\n",
		(void*)VictimController, (void*)RemoveFromAlivePlayers_,
		AliveBefore, GameMode->AlivePlayers.Num(),
		PlayersLeftBefore, GameState->PlayersLeft,
		GameMode->MatchState == FName(L"WaitingPostMatch") ? 1 : 0,
		bStillRegistered ? 1 : 0);

	return !bStillRegistered;
}

static void Complete172SpawnedBotVictoryAfterNative(
	AFortGameMode* GameMode,
	AFortGameStateAthena* GameState,
	AFortPlayerControllerAthena* VictimController,
	AFortPlayerStateAthena* ReportedKillerPlayerState,
	AFortPlayerPawnAthena* ReportedKillerPawn,
	AFortPlayerControllerAthena* ReportedKillerController)
{
	if (VersionInfo.FortniteVersion != 1.72 || !GameMode || !GameState ||
		!VictimController || !IsUsableDeathObject(GameMode) ||
		!IsUsableDeathObject(GameState))
	{
		return;
	}

	const bool bWaitingPostMatch =
		GameMode->MatchState == FName(L"WaitingPostMatch");
	const int AlivePlayerCount = GameMode->AlivePlayers.Num();

	SDK::DbgLog(
		"[Elimination] 1.72 native spawnbot death postMatch=%d alive=%d playersLeft=%d forceRespawns=%d killerPS=%p killerPC=%p\n",
		bWaitingPostMatch ? 1 : 0, AlivePlayerCount, GameState->PlayersLeft,
		FConfiguration::bForceRespawns ? 1 : 0,
		(void*)ReportedKillerPlayerState, (void*)ReportedKillerController);

	// Native 1.7.2 already removes the victim from AlivePlayers and moves the
	// server to WaitingPostMatch. Repeating that bookkeeping here would
	// double-decrement PlayersLeft. Its old client does not, however, receive
	// Magnesium's modern winner notification block, so complete only that
	// client-facing part for a final synthetic-bot death.
	if (!bWaitingPostMatch)
	{
		return;
	}

	auto WinnerController =
		IsUsableDeathObject(ReportedKillerController) &&
		!IsTrackedSpawnedBotController(ReportedKillerController)
		? ReportedKillerController : nullptr;
	if (!WinnerController && ReportedKillerPawn &&
		IsUsableDeathObject(ReportedKillerPawn) &&
		IsUsableDeathObject(ReportedKillerPawn->Controller))
	{
		auto PawnController =
			ReportedKillerPawn->Controller->Cast<AFortPlayerControllerAthena>();
		if (PawnController &&
			!IsTrackedSpawnedBotController(PawnController))
		{
			WinnerController = PawnController;
		}
	}

	if (!WinnerController)
	{
		AFortPlayerControllerAthena* SoleSurvivor = nullptr;
		for (auto AliveActor : GameMode->AlivePlayers)
		{
			if (!AliveActor || AliveActor == VictimController)
				continue;
			if (!IsUsableDeathObject(AliveActor))
				continue;

			auto Candidate = AliveActor->Cast<AFortPlayerControllerAthena>();
			if (!Candidate || IsTrackedSpawnedBotController(Candidate))
				continue;

			if (SoleSurvivor)
			{
				SoleSurvivor = nullptr;
				break;
			}
			SoleSurvivor = Candidate;
		}
		WinnerController = SoleSurvivor;
	}

	if (!WinnerController || WinnerController == VictimController)
		return;

	// Always reacquire the state from the validated surviving controller after
	// native death. A pre-native report pointer can be stale, and pairing a
	// fallback controller with a different reported state would announce the
	// wrong team.
	AFortPlayerStateAthena* WinnerPlayerState = nullptr;
	if (WinnerController->HasPlayerState() &&
		IsUsableDeathObject(WinnerController->PlayerState))
	{
		WinnerPlayerState =
			(AFortPlayerStateAthena*)WinnerController->PlayerState;
	}
	if (!WinnerPlayerState)
		return;

	if (WinnerPlayerState->HasPlace())
	{
		WinnerPlayerState->Place = 1;
		WinnerPlayerState->OnRep_Place();
	}

	if (GameState->HasWinningTeam())
	{
		GameState->WinningTeam = WinnerPlayerState->TeamIndex;
		GameState->OnRep_WinningTeam();
	}
	if (GameState->HasWinningPlayerState())
	{
		GameState->WinningPlayerState = WinnerPlayerState;
		GameState->OnRep_WinningPlayerState();
	}

	GUI::gsStatus = Ended;

	// These three UFunctions have zero parameters on 1.7.2. Calling the modern
	// signatures would build the wrong reflected parameter buffer.
	WinnerController->PlayWinEffects();
	WinnerController->ClientNotifyWon();
	WinnerController->ClientNotifyTeamWon();

	WinnerController->ForceNetUpdate();
	GameState->ForceNetUpdate();

	SDK::DbgLog(
		"[Elimination] completed 1.72 spawnbot winner notification controller=%p playerState=%p team=%d\n",
		(void*)WinnerController, (void*)WinnerPlayerState,
		(int)WinnerPlayerState->TeamIndex);
}

static bool IsLateSeasonHumanVictoryController(
	AFortPlayerControllerAthena* Controller,
	AFortPlayerControllerAthena* VictimController)
{
	if (!Controller || Controller == VictimController ||
		!IsUsableDeathObject(Controller) ||
		!Controller->HasPlayerState() ||
		!IsUsableDeathObject(Controller->PlayerState))
	{
		return false;
	}

	auto PlayerState =
		(AFortPlayerStateAthena*)Controller->PlayerState;
	if ((PlayerState->HasbIsABot() && PlayerState->bIsABot) ||
		IsTrackedSpawnedBotController(Controller) ||
		MagnesiumPlayerAIIntegration::IsPlayerAIController(Controller))
	{
		return false;
	}

	return true;
}

static void CompleteLateSeasonBotVictoryAfterNative(
	AFortGameMode* GameMode,
	AFortGameStateAthena* GameState,
	AFortPlayerControllerAthena* VictimController,
	AFortPlayerPawnAthena* ReportedKillerPawn,
	AFortPlayerControllerAthena* ReportedKillerController,
	UFortWeaponItemDefinition* FinishingWeapon,
	uint8 DeathCause)
{
	if (VersionInfo.FortniteVersion < 17.0 ||
		VersionInfo.FortniteVersion >= 19.0 ||
		GUI::gsStatus == Ended ||
		!IsUsableDeathObject(GameMode) ||
		!IsUsableDeathObject(GameState) ||
		GameMode->MatchState != FName(L"WaitingPostMatch"))
	{
		return;
	}

	auto WinnerController =
		IsLateSeasonHumanVictoryController(
			ReportedKillerController, VictimController)
		? ReportedKillerController
		: nullptr;

	if (!WinnerController && IsUsableDeathObject(ReportedKillerPawn) &&
		IsUsableDeathObject(ReportedKillerPawn->Controller))
	{
		auto PawnController =
			ReportedKillerPawn->Controller->Cast<AFortPlayerControllerAthena>();
		if (IsLateSeasonHumanVictoryController(
			PawnController, VictimController))
		{
			WinnerController = PawnController;
		}
	}

	if (!WinnerController && GameState->HasWinningPlayerState() &&
		IsUsableDeathObject(GameState->WinningPlayerState))
	{
		auto NativeWinnerState = GameState->WinningPlayerState;
		auto NativeWinnerController =
			NativeWinnerState->HasOwner() &&
			IsUsableDeathObject(NativeWinnerState->Owner)
			? NativeWinnerState->Owner
				->Cast<AFortPlayerControllerAthena>()
			: nullptr;
		if (IsLateSeasonHumanVictoryController(
			NativeWinnerController, VictimController))
		{
			WinnerController = NativeWinnerController;
		}
	}

	if (!WinnerController && GameState->HasWinningTeam() &&
		GameState->WinningTeam >= 3 && GameState->WinningTeam <= 255)
	{
		const uint8 NativeWinningTeam =
			(uint8)GameState->WinningTeam;
		for (auto AliveActor : GameMode->AlivePlayers)
		{
			auto Candidate = AliveActor
				? AliveActor->Cast<AFortPlayerControllerAthena>()
				: nullptr;
			if (IsLateSeasonHumanVictoryController(
				Candidate, VictimController))
			{
				auto CandidateState =
					(AFortPlayerStateAthena*)Candidate->PlayerState;
				if (CandidateState->TeamIndex == NativeWinningTeam)
				{
					WinnerController = Candidate;
					break;
				}
			}
		}
	}

	if (!WinnerController || !WinnerController->PlayerState)
		return;

	auto WinnerPlayerState =
		(AFortPlayerStateAthena*)WinnerController->PlayerState;
	const uint8 WinnerTeam = WinnerPlayerState->TeamIndex;

	if (WinnerPlayerState->HasPlace())
	{
		WinnerPlayerState->Place = 1;
		WinnerPlayerState->OnRep_Place();
	}

	if (GameState->HasWinningTeam())
	{
		GameState->WinningTeam = WinnerTeam;
		GameState->OnRep_WinningTeam();
	}
	if (GameState->HasWinningPlayerState())
	{
		GameState->WinningPlayerState = WinnerPlayerState;
		GameState->OnRep_WinningPlayerState();
	}

	std::vector<AFortPlayerControllerAthena*> WinningControllers;
	auto AddWinningController =
		[&](AFortPlayerControllerAthena* Candidate)
		{
			if (!IsLateSeasonHumanVictoryController(
				Candidate, VictimController))
			{
				return;
			}

			auto CandidateState =
				(AFortPlayerStateAthena*)Candidate->PlayerState;
			if (CandidateState->TeamIndex != WinnerTeam ||
				std::find(
					WinningControllers.begin(),
					WinningControllers.end(),
					Candidate) != WinningControllers.end())
			{
				return;
			}

			WinningControllers.push_back(Candidate);
		};

	AddWinningController(WinnerController);

	auto World = UWorld::GetWorld();
	auto Driver = World ? (UNetDriver*)World->NetDriver : nullptr;
	if (Driver)
	{
		for (int32 Index = 0;
			Index < Driver->ClientConnections.Num(); Index++)
		{
			auto Connection = Driver->ClientConnections[Index];
			AddWinningController(
				Connection && Connection->PlayerController
				? Connection->PlayerController
					->Cast<AFortPlayerControllerAthena>()
				: nullptr);
		}
	}

	auto FinisherPawn =
		IsUsableDeathObject(ReportedKillerPawn)
		? ReportedKillerPawn
		: WinnerController->MyFortPawn;

	GUI::gsStatus = Ended;
	if (FConfiguration::bUseWinLines)
	{
		WinnerController->PlayWinEffects(
			FinisherPawn, FinishingWeapon, DeathCause, false);
	}
	WinnerController->ClientNotifyWon(
		FinisherPawn, FinishingWeapon, DeathCause);

	for (auto Controller : WinningControllers)
	{
		auto PlayerState =
			(AFortPlayerStateAthena*)Controller->PlayerState;
		if (PlayerState->HasPlace())
		{
			PlayerState->Place = 1;
			PlayerState->OnRep_Place();
		}

		Controller->ClientNotifyTeamWon(
			FinisherPawn, FinishingWeapon, DeathCause);
		Controller->ForceNetUpdate();
	}
	GameState->ForceNetUpdate();

	SDK::DbgLog(
		"[Elimination] completed FN17-18 bot winner notification winner=%p playerState=%p team=%u clients=%d\n",
		(void*)WinnerController,
		(void*)WinnerPlayerState,
		(unsigned)WinnerTeam,
		(int)WinningControllers.size());
}

static bool UsesCoreLegacyDeathSpectating()
{
	const double FortniteVersion = VersionInfo.FortniteVersion;
	return (FortniteVersion >= 1.91 && FortniteVersion <= 6.00) ||
		FortniteVersion == 1.10 ||
		FortniteVersion == 1.11;
}

static bool IsPawnDBNOForSpectating(AFortPlayerPawnAthena* Pawn)
{
	if (!IsUsableDeathObject(Pawn))
		return false;

	if (Pawn->HasbIsDBNO())
		return Pawn->bIsDBNO;

	auto IsDBNOFunction = Pawn->GetFunction("IsDBNO");
	return IsDBNOFunction
		? Pawn->Call<bool>(IsDBNOFunction)
		: false;
}

static AFortPlayerPawnAthena* ResolveValidDeathSpectatePawn(
	AFortGameMode* GameMode,
	AFortPlayerControllerAthena* VictimController,
	AFortPlayerControllerAthena* CandidateController)
{
	if (!IsUsableDeathObject(CandidateController) ||
		CandidateController == VictimController ||
		!IsControllerInAlivePlayers(GameMode, CandidateController) ||
		!CandidateController->HasPlayerState() ||
		!IsUsableDeathObject(CandidateController->PlayerState))
	{
		return nullptr;
	}

	auto CandidatePlayerState = CandidateController->PlayerState;
	if ((CandidateController->HasbMarkedAlive() &&
			!CandidateController->bMarkedAlive) ||
		(CandidatePlayerState->HasbIsSpectator() &&
			CandidatePlayerState->bIsSpectator))
	{
		return nullptr;
	}

	AFortPlayerPawnAthena* CandidatePawn = nullptr;
	if (CandidateController->HasMyFortPawn() &&
		IsUsableDeathObject(CandidateController->MyFortPawn))
	{
		CandidatePawn = CandidateController->MyFortPawn;
	}
	else if (CandidateController->HasPawn() &&
		IsUsableDeathObject(CandidateController->Pawn))
	{
		CandidatePawn = CandidateController->Pawn;
	}

	auto VictimPawn =
		VictimController && VictimController->HasPawn()
		? VictimController->Pawn : nullptr;
	auto VictimFortPawn =
		VictimController && VictimController->HasMyFortPawn()
		? VictimController->MyFortPawn : nullptr;

	if (!CandidatePawn ||
		(VictimController &&
			(CandidatePawn == VictimPawn ||
				CandidatePawn == VictimFortPawn)) ||
		IsPawnDBNOForSpectating(CandidatePawn) ||
		(CandidatePawn->HasbIsDying() && CandidatePawn->bIsDying) ||
		(CandidatePawn->HasbPlayedDying() && CandidatePawn->bPlayedDying) ||
		(CandidatePawn->HasbIsHiddenForDeath() &&
			CandidatePawn->bIsHiddenForDeath))
	{
		return nullptr;
	}

	return CandidatePawn;
}

static bool IsSameDeathSpectateSquad(
	AFortPlayerControllerAthena* VictimController,
	AFortPlayerControllerAthena* CandidateController)
{
	if (!VictimController || !CandidateController ||
		!VictimController->HasPlayerState() ||
		!CandidateController->HasPlayerState() ||
		!IsUsableDeathObject(VictimController->PlayerState) ||
		!IsUsableDeathObject(CandidateController->PlayerState))
	{
		return false;
	}

	auto VictimPlayerState = VictimController->PlayerState;
	auto CandidatePlayerState = CandidateController->PlayerState;

	if (VictimPlayerState->HasTeamIndex() &&
		CandidatePlayerState->HasTeamIndex() &&
		VictimPlayerState->TeamIndex != CandidatePlayerState->TeamIndex)
	{
		return false;
	}

	if (VictimPlayerState->HasSquadId() &&
		CandidatePlayerState->HasSquadId() &&
		VictimPlayerState->SquadId != CandidatePlayerState->SquadId)
	{
		return false;
	}

	return true;
}

static AFortPlayerPawnAthena* ResolveDeathSpectateTarget(
	AFortGameMode* GameMode,
	AFortPlayerControllerAthena* VictimController,
	AFortPlayerControllerAthena* KillerController,
	AFortPlayerPawnAthena* KillerPawn)
{
	AFortPlayerPawnAthena* ResolvedTarget = nullptr;

	// Prefer the first still-alive member in the reflected native squad list.
	// That list is stable and avoids the invalid fixed-layout PlayerTeam wrapper.
	ForEachSquadController(
		VictimController,
		[&](AFortPlayerControllerAthena* SquadController)
		{
			if (ResolvedTarget ||
				!IsSameDeathSpectateSquad(
					VictimController, SquadController))
			{
				return;
			}

			ResolvedTarget = ResolveValidDeathSpectatePawn(
				GameMode, VictimController, SquadController);
		});
	if (ResolvedTarget)
		return ResolvedTarget;

	// Fall back to the reported killer only when its live controller/pawn can
	// be validated. The raw report pointer may instead be a stale pawn.
	if (!IsUsableDeathObject(KillerController) &&
		IsUsableDeathObject(KillerPawn) &&
		KillerPawn->HasController() &&
		IsUsableDeathObject(KillerPawn->Controller))
	{
		KillerController =
			KillerPawn->Controller->Cast<AFortPlayerControllerAthena>();
	}

	ResolvedTarget = ResolveValidDeathSpectatePawn(
		GameMode, VictimController, KillerController);
	if (ResolvedTarget)
		return ResolvedTarget;

	// AlivePlayers order is authoritative and deterministic; do not use a
	// random fallback because it can choose self, stale, or dying entries.
	if (!GameMode || !GameMode->HasAlivePlayers())
		return nullptr;

	auto& AlivePlayers = GameMode->AlivePlayers;
	for (int32 Index = 0; Index < AlivePlayers.Num(); ++Index)
	{
		auto AliveActor = AlivePlayers[Index];
		if (!IsUsableDeathObject(AliveActor))
			continue;

		auto AliveController =
			AliveActor->Cast<AFortPlayerControllerAthena>();
		ResolvedTarget = ResolveValidDeathSpectatePawn(
			GameMode, VictimController, AliveController);
		if (ResolvedTarget)
			return ResolvedTarget;
	}

	return nullptr;
}

static bool IsControllerInSpectatingState(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!IsUsableDeathObject(PlayerController) ||
		!PlayerController->HasStateName())
	{
		return false;
	}

	static const FName SpectatingName(L"Spectating");
	return PlayerController->StateName == SpectatingName;
}

static bool InvokeSpectateOnDeathLifecycle(
	AFortPlayerControllerAthena* PlayerController,
	UFunction* Function)
{
	if (!IsUsableDeathObject(PlayerController) || !Function)
		return false;

	const auto Parameters = Function->GetParamsNamed();
	if (Parameters.NameOffsetMap.empty())
	{
		PlayerController->ProcessEvent(Function, nullptr);
		return true;
	}

	alignas(16) uint8 Params[0x40]{};
	bool bWroteAllowStateChange = false;
	for (const auto& Parameter : Parameters.NameOffsetMap)
	{
		if (Parameter.Name == "ReturnValue")
			continue;

		if (Parameter.Name == "bAllowStateChange" ||
			Parameter.Name == "AllowStateChange")
		{
			if (Parameter.Offset + sizeof(bool) >
				sizeof(Params))
			{
				return false;
			}

			const bool bAllowStateChange = true;
			memcpy(
				Params + Parameter.Offset,
				&bAllowStateChange,
				sizeof(bAllowStateChange));
			bWroteAllowStateChange = true;
			continue;
		}

		// Do not call an unfamiliar overload with an undersized or incorrectly
		// typed parameter block.
		return false;
	}

	if (!bWroteAllowStateChange)
		return false;

	PlayerController->ProcessEvent(Function, Params);
	return true;
}

static bool InvokeDeathSpectateTargetFunction(
	AFortPlayerControllerAthena* PlayerController,
	UFunction* Function,
	AFortPlayerPawnAthena* TargetPawn,
	AFortPlayerStateAthena* TargetPlayerState,
	AFortPlayerControllerAthena* TargetController,
	bool bPreferPlayerState)
{
	if (!IsUsableDeathObject(PlayerController) || !Function ||
		!IsUsableDeathObject(TargetPawn) ||
		(bPreferPlayerState &&
			!IsUsableDeathObject(TargetPlayerState)))
	{
		return false;
	}

	const auto Parameters = Function->GetParamsNamed();
	if (Parameters.NameOffsetMap.empty())
		return false;

	alignas(16) uint8 Params[0x40]{};
	bool bWroteTarget = false;
	for (const auto& Parameter : Parameters.NameOffsetMap)
	{
		if (Parameter.Name == "ReturnValue")
			continue;

		if (Parameter.Name == "bAllowStateChange" ||
			Parameter.Name == "AllowStateChange")
		{
			if (Parameter.Offset + sizeof(bool) >
				sizeof(Params))
			{
				return false;
			}

			const bool bAllowStateChange = true;
			memcpy(
				Params + Parameter.Offset,
				&bAllowStateChange,
				sizeof(bAllowStateChange));
			continue;
		}

		if (bWroteTarget ||
			Parameter.Offset + sizeof(void*) >
				sizeof(Params))
		{
			return false;
		}

		const auto& ParameterName = Parameter.Name;
		const bool bNamesTarget =
			ParameterName.find("Spectate") !=
				UEAllocatedString::npos ||
			ParameterName.find("Target") !=
				UEAllocatedString::npos ||
			ParameterName.find("Player") !=
				UEAllocatedString::npos ||
			ParameterName.find("Actor") !=
				UEAllocatedString::npos ||
			ParameterName.find("Pawn") !=
				UEAllocatedString::npos ||
			ParameterName.find("Controller") !=
				UEAllocatedString::npos;
		if (!bNamesTarget)
			return false;

		UObject* TargetObject = nullptr;
		if (ParameterName.find("Controller") !=
				UEAllocatedString::npos)
		{
			if (!IsUsableDeathObject(TargetController))
				return false;
			TargetObject = TargetController;
		}
		else if (bPreferPlayerState ||
			ParameterName.find("PlayerState") !=
				UEAllocatedString::npos)
		{
			TargetObject = TargetPlayerState;
		}
		else
		{
			TargetObject = TargetPawn;
		}

		if (!IsUsableDeathObject(TargetObject))
			return false;

		memcpy(
			Params + Parameter.Offset,
			&TargetObject,
			sizeof(TargetObject));
		bWroteTarget = true;
	}

	if (!bWroteTarget)
		return false;

	PlayerController->ProcessEvent(Function, Params);
	return true;
}

struct FPendingDeathSpectateCameraHandoff
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AFortPlayerControllerAthena> PlayerController;
	TWeakObjectPtr<AFortPlayerControllerAthena> TargetController;
	TWeakObjectPtr<AFortPlayerStateAthena> TargetPlayerState;
	TWeakObjectPtr<AFortPlayerPawnAthena> TargetPawn;
	TWeakObjectPtr<AFortPlayerPawnAthena> DeadPawn;
	float RemainingSeconds = 0.15f;
	int Attempts = 0;
};

static std::vector<FPendingDeathSpectateCameraHandoff>
	GPendingDeathSpectateCameraHandoffs;

static void QueueDeathSpectateCameraHandoff(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerControllerAthena* TargetController,
	AFortPlayerStateAthena* TargetPlayerState,
	AFortPlayerPawnAthena* TargetPawn,
	AFortPlayerPawnAthena* DeadPawn)
{
	for (int Index =
		static_cast<int>(
			GPendingDeathSpectateCameraHandoffs.size()) - 1;
		Index >= 0; --Index)
	{
		if (GPendingDeathSpectateCameraHandoffs[Index]
				.PlayerController.Get() == PlayerController)
		{
			GPendingDeathSpectateCameraHandoffs.erase(
				GPendingDeathSpectateCameraHandoffs.begin() + Index);
		}
	}

	FPendingDeathSpectateCameraHandoff Pending;
	Pending.World = TWeakObjectPtr<UWorld>(UWorld::GetWorld());
	Pending.PlayerController =
		TWeakObjectPtr<AFortPlayerControllerAthena>(
			PlayerController);
	Pending.TargetController =
		TWeakObjectPtr<AFortPlayerControllerAthena>(
			TargetController);
	Pending.TargetPlayerState =
		TWeakObjectPtr<AFortPlayerStateAthena>(
			TargetPlayerState);
	Pending.TargetPawn =
		TWeakObjectPtr<AFortPlayerPawnAthena>(TargetPawn);
	Pending.DeadPawn =
		TWeakObjectPtr<AFortPlayerPawnAthena>(DeadPawn);
	GPendingDeathSpectateCameraHandoffs.emplace_back(Pending);
}

static void InitializeDeathSpectating(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* TargetPawn,
	AFortPlayerPawnAthena* DeadPawn)
{
	if (!IsUsableDeathObject(PlayerController) ||
		!IsUsableDeathObject(TargetPawn) ||
		TargetPawn == DeadPawn)
	{
		return;
	}

	AFortPlayerControllerAthena* TargetController = nullptr;
	if (TargetPawn->HasController() &&
		IsUsableDeathObject(TargetPawn->Controller))
	{
		TargetController =
			TargetPawn->Controller
				->Cast<AFortPlayerControllerAthena>();
	}

	AFortPlayerStateAthena* TargetPlayerState = nullptr;
	if (TargetPawn->HasPlayerState() &&
		IsUsableDeathObject(TargetPawn->PlayerState))
	{
		TargetPlayerState =
			TargetPawn->PlayerState
				->Cast<AFortPlayerStateAthena>();
	}
	if (!TargetPlayerState &&
		TargetPawn->HasController() &&
		IsUsableDeathObject(TargetPawn->Controller))
	{
		if (TargetController &&
			TargetController->HasPlayerState() &&
			IsUsableDeathObject(
				TargetController->PlayerState))
		{
			TargetPlayerState =
				TargetController->PlayerState
					->Cast<AFortPlayerStateAthena>();
		}
	}

	// 15.30 moved the state-change flag from the ServerSpectate calls onto
	// SpectateOnDeath, while other seasons expose the inverse signature (or a
	// no-argument legacy SpectateOnDeath). Reflection keeps one path valid for
	// all of those layouts. StateName can change before native death handling
	// detaches the dead pawn, so a "Spectating" name alone does not prove that
	// the camera lifecycle completed.
	const bool bAlreadySpectating =
		IsControllerInSpectatingState(PlayerController);
	const bool bStillPossessesDeadPawn =
		DeadPawn && PlayerController->Pawn == DeadPawn;
	if (!bAlreadySpectating || bStillPossessesDeadPawn)
	{
		if (auto SpectateOnDeathFunction =
				PlayerController->GetFunction(
					"SpectateOnDeath"))
		{
			InvokeSpectateOnDeathLifecycle(
				PlayerController,
				SpectateOnDeathFunction);
		}
	}

	// A controller that still possesses its eliminated pawn auto-manages its
	// camera back to that pawn, even after the spectator UI has accepted a
	// different PlayerState. Give the native lifecycle first chance above,
	// then explicitly detach only when it left the captured dead pawn attached.
	bool bDetachedDeadPawn = false;
	if (DeadPawn &&
		PlayerController->Pawn == DeadPawn &&
		PlayerController->GetFunction("UnPossess"))
	{
		PlayerController->UnPossess();
		bDetachedDeadPawn =
			PlayerController->Pawn != DeadPawn;
	}

	const char* HandoffPath = "none";
	bool bInvokedPlayerStateTarget = false;
	if (TargetPlayerState)
	{
		if (auto SpectatePlayerStateFunction =
				PlayerController->GetFunction(
					"ServerSpectatePlayerState"))
		{
			bInvokedPlayerStateTarget =
				InvokeDeathSpectateTargetFunction(
					PlayerController,
					SpectatePlayerStateFunction,
					TargetPawn, TargetPlayerState,
					TargetController, true);
			if (bInvokedPlayerStateTarget)
				HandoffPath = "player-state";
		}
	}

	// PlayerState selection drives the "SPECTATING: <name>" UI on 15.30, but
	// does not reliably move the camera. Always follow it with the available
	// pawn/actor-target RPC instead of treating the correct label as proof of a
	// completed camera handoff.
	bool bInvokedPawnTarget = false;
	auto SpectateActorFunction =
		PlayerController->GetFunction(
			"ServerSpectateActor");
	if (SpectateActorFunction &&
		InvokeDeathSpectateTargetFunction(
			PlayerController,
			SpectateActorFunction,
			TargetPawn, TargetPlayerState,
			TargetController, false))
	{
		bInvokedPawnTarget = true;
		HandoffPath = bInvokedPlayerStateTarget
			? "player-state+actor" : "actor";
	}
	else if (auto SpectatePlayerFunction =
			PlayerController->GetFunction(
				"ServerSpectatePlayer"))
	{
		// 10.40 and nearby builds name the pawn-target RPC
		// ServerSpectatePlayer rather than ServerSpectateActor.
		if (InvokeDeathSpectateTargetFunction(
				PlayerController,
				SpectatePlayerFunction,
				TargetPawn, TargetPlayerState,
				TargetController, false))
		{
			bInvokedPawnTarget = true;
			HandoffPath = bInvokedPlayerStateTarget
				? "player-state+player" : "player";
		}
	}

	// Always finish the handoff on both sides. Server view-target state cannot
	// prove that the owning remote client received its initial camera update;
	// that missing notification is the observed first-killer failure.
	static FName MutableSpectatingName(L"Spectating");
	if (PlayerController->HasStateName() &&
		!IsControllerInSpectatingState(PlayerController))
	{
		PlayerController->StateName =
			MutableSpectatingName;
		HandoffPath = "explicit-state";
	}
	if (PlayerController->GetFunction("ClientGotoState"))
	{
		PlayerController->ClientGotoState(
			MutableSpectatingName);
	}
	if (PlayerController->GetFunction(
			"SetViewTargetWithBlend"))
	{
		PlayerController->SetViewTargetWithBlend(
			(AActor*)TargetPawn,
			0.f, (uint8)0, 0.f, false);
	}
	TargetPawn->ForceNetUpdate();
	const bool bQueuedClientViewTarget =
		ClientForceViewTarget(
		PlayerController, (AActor*)TargetPawn);
	if (strcmp(HandoffPath, "none") == 0)
		HandoffPath = "explicit-camera";

	QueueDeathSpectateCameraHandoff(
		PlayerController, TargetController,
		TargetPlayerState, TargetPawn, DeadPawn);

	auto ActualViewTarget =
		PlayerController->GetViewTarget();
	PlayerController->ForceNetUpdate();
	SDK::DbgLog(
		"[Spectating] initialized first death target "
		"controller=%p targetPawn=%p targetState=%p "
		"viewTarget=%p pawn=%p detached=%d "
		"playerStateRPC=%d pawnRPC=%d clientViewRPC=%d "
		"path=%s FN=%.2f\n",
		(void*)PlayerController,
		(void*)TargetPawn,
		(void*)TargetPlayerState,
		(void*)ActualViewTarget,
		(void*)PlayerController->Pawn,
		(int)bDetachedDeadPawn,
		(int)bInvokedPlayerStateTarget,
		(int)bInvokedPawnTarget,
		(int)bQueuedClientViewTarget,
		HandoffPath,
		VersionInfo.FortniteVersion);
}

static void TickDeathSpectateCameraHandoffs(float DeltaSeconds)
{
	static const float RetryDelays[] =
		{ 0.35f, 0.5f, 1.f, 2.f };

	for (int Index =
		static_cast<int>(
			GPendingDeathSpectateCameraHandoffs.size()) - 1;
		Index >= 0; --Index)
	{
		auto& Pending =
			GPendingDeathSpectateCameraHandoffs[Index];
		auto PlayerController =
			Pending.PlayerController.Get();
		auto TargetController =
			Pending.TargetController.Get();
		auto DeadPawn = Pending.DeadPawn.Get();
		auto World = Pending.World.Get();
		auto CurrentWorld = UWorld::GetWorld();
		auto GameMode =
			CurrentWorld
				? (AFortGameMode*)CurrentWorld->AuthorityGameMode
				: nullptr;

		const bool bInvalidLifecycle =
			World != CurrentWorld ||
			!IsUsableDeathObject(PlayerController) ||
			!IsUsableDeathObject(TargetController) ||
			!IsUsableDeathObject(GameMode) ||
			GUI::gsStatus == Ended ||
			GameMode->MatchState == FName(L"WaitingPostMatch") ||
			(PlayerController->HasStateName() &&
				!IsControllerInSpectatingState(
					PlayerController));
		if (bInvalidLifecycle)
		{
			GPendingDeathSpectateCameraHandoffs.erase(
				GPendingDeathSpectateCameraHandoffs.begin() +
					Index);
			continue;
		}

		// A revive/respawn gives the controller a different live pawn. Never
		// let a delayed death-camera repair pull an active player back into
		// spectating.
		auto PossessedPawn = PlayerController->Pawn;
		if (IsUsableDeathObject(PossessedPawn) &&
			PossessedPawn != DeadPawn &&
			!(PossessedPawn->HasbIsDying() &&
				PossessedPawn->bIsDying))
		{
			GPendingDeathSpectateCameraHandoffs.erase(
				GPendingDeathSpectateCameraHandoffs.begin() +
					Index);
			continue;
		}

		auto TargetPawn =
			ResolveValidDeathSpectatePawn(
				GameMode, PlayerController,
				TargetController);
		if (!TargetPawn)
		{
			GPendingDeathSpectateCameraHandoffs.erase(
				GPendingDeathSpectateCameraHandoffs.begin() +
					Index);
			continue;
		}
		Pending.TargetPawn =
			TWeakObjectPtr<AFortPlayerPawnAthena>(
				TargetPawn);

		// If native spectator cycling has already selected another live pawn,
		// respect that newer choice and retire this first-killer retry.
		auto ViewTargetBefore =
			PlayerController->GetViewTarget();
		auto ViewPawnBefore =
			IsUsableDeathObject(ViewTargetBefore)
				? ViewTargetBefore
					->Cast<AFortPlayerPawnAthena>()
				: nullptr;
		if (ViewPawnBefore &&
			ViewPawnBefore != TargetPawn &&
			ViewPawnBefore != DeadPawn &&
			!IsPawnDBNOForSpectating(ViewPawnBefore) &&
			!(ViewPawnBefore->HasbIsDying() &&
				ViewPawnBefore->bIsDying))
		{
			GPendingDeathSpectateCameraHandoffs.erase(
				GPendingDeathSpectateCameraHandoffs.begin() +
					Index);
			continue;
		}

		Pending.RemainingSeconds -= DeltaSeconds;
		if (Pending.RemainingSeconds > 0.f)
			continue;

		// By now the server view target has had at least one replication frame
		// to make the target pawn relevant to this connection. Re-send only the
		// camera handoff; replaying the death lifecycle would reset UI/state.
		if (PlayerController->GetFunction(
				"SetViewTargetWithBlend"))
		{
			PlayerController->SetViewTargetWithBlend(
				(AActor*)TargetPawn,
				0.f, (uint8)0, 0.f, false);
		}
		TargetPawn->ForceNetUpdate();
		const bool bQueuedClientViewTarget =
			ClientForceViewTarget(
				PlayerController,
				(AActor*)TargetPawn);
		PlayerController->ForceNetUpdate();

		Pending.Attempts++;
		auto ViewTargetAfter =
			PlayerController->GetViewTarget();
		SDK::DbgLog(
			"[Spectating] deferred camera handoff "
			"attempt=%d controller=%p targetPawn=%p "
			"before=%p after=%p clientViewRPC=%d FN=%.2f\n",
			Pending.Attempts,
			(void*)PlayerController,
			(void*)TargetPawn,
			(void*)ViewTargetBefore,
			(void*)ViewTargetAfter,
			(int)bQueuedClientViewTarget,
			VersionInfo.FortniteVersion);

		if (Pending.Attempts >= 5)
		{
			GPendingDeathSpectateCameraHandoffs.erase(
				GPendingDeathSpectateCameraHandoffs.begin() +
					Index);
			continue;
		}

		Pending.RemainingSeconds =
			RetryDelays[Pending.Attempts - 1];
	}
}

struct FConfiguredRespawnPlaylistSnapshot
{
	TWeakObjectPtr<UFortPlaylistAthena> Playlist;
	bool bHasRespawnInAir = false;
	bool bRespawnInAir = false;
	bool bHasRespawnHeight = false;
	FScalableFloat RespawnHeight{};
	bool bHasRespawnTime = false;
	FScalableFloat RespawnTime{};
	bool bHasRespawnType = false;
	uint8 RespawnType = 0;
};

static TWeakObjectPtr<UWorld> GConfiguredRespawnPolicyWorld;
static TWeakObjectPtr<AFortGameStateAthena>
	GConfiguredRespawnPolicyGameState;
static std::vector<FConfiguredRespawnPlaylistSnapshot>
	GConfiguredRespawnPlaylistSnapshots;
static bool GConfiguredRespawnCheatCaptured = false;
static bool GConfiguredRespawnOriginalCheat = false;

static void RestoreConfiguredRespawnPlaylist(
	const FConfiguredRespawnPlaylistSnapshot& Snapshot)
{
	auto Playlist = Snapshot.Playlist.Get();
	if (!IsUsableDeathObject(Playlist))
		return;

	if (Snapshot.bHasRespawnInAir &&
		Playlist->HasbRespawnInAir())
	{
		bool OriginalRespawnInAir =
			Snapshot.bRespawnInAir;
		Playlist->bRespawnInAir =
			OriginalRespawnInAir;
	}
	if (Snapshot.bHasRespawnHeight &&
		Playlist->HasRespawnHeight())
	{
		FScalableFloat OriginalRespawnHeight =
			Snapshot.RespawnHeight;
		Playlist->RespawnHeight =
			OriginalRespawnHeight;
	}
	if (Snapshot.bHasRespawnTime &&
		Playlist->HasRespawnTime())
	{
		FScalableFloat OriginalRespawnTime =
			Snapshot.RespawnTime;
		Playlist->RespawnTime =
			OriginalRespawnTime;
	}
	if (Snapshot.bHasRespawnType &&
		Playlist->HasRespawnType())
	{
		uint8 OriginalRespawnType =
			Snapshot.RespawnType;
		Playlist->RespawnType =
			OriginalRespawnType;
	}
}

static void ResetConfiguredRespawnPolicyTracking(
	bool bRestoreCurrentWorld)
{
	if (bRestoreCurrentWorld)
	{
		for (const auto& Snapshot :
			GConfiguredRespawnPlaylistSnapshots)
		{
			RestoreConfiguredRespawnPlaylist(Snapshot);
		}

		auto GameState =
			GConfiguredRespawnPolicyGameState.Get();
		if (GConfiguredRespawnCheatCaptured &&
			IsUsableDeathObject(GameState) &&
			GameState->HasbCheatRespawnEnabled())
		{
			GameState->bCheatRespawnEnabled =
				GConfiguredRespawnOriginalCheat;
			GameState->ForceNetUpdate();
		}
	}

	GConfiguredRespawnPlaylistSnapshots.clear();
	GConfiguredRespawnCheatCaptured = false;
	GConfiguredRespawnOriginalCheat = false;
}

static std::vector<UFortPlaylistAthena*>
	ResolveAuthoritativeRespawnPlaylists(
		AFortGameStateAthena* GameState)
{
	std::vector<UFortPlaylistAthena*> Playlists;
	auto AddUnique =
		[&](const UFortPlaylistAthena* Candidate)
		{
			auto Playlist =
				const_cast<UFortPlaylistAthena*>(Candidate);
			if (!IsUsableDeathObject(Playlist) ||
				std::find(
					Playlists.begin(), Playlists.end(),
					Playlist) != Playlists.end())
			{
				return;
			}

			Playlists.push_back(Playlist);
		};

	if (GameState)
	{
		if (GameState->HasCurrentPlaylistInfo())
		{
			auto& PlaylistInfo =
				GameState->CurrentPlaylistInfo;
			if (PlaylistInfo.HasBasePlaylist())
				AddUnique(PlaylistInfo.BasePlaylist);
			if (PlaylistInfo.HasOverridePlaylist())
				AddUnique(PlaylistInfo.OverridePlaylist);
		}
		if (GameState->HasCurrentPlaylistData())
			AddUnique(GameState->CurrentPlaylistData);
	}

	// Before publication, SetupPlaylist has loaded the selected asset but the
	// replicated GameState references may not be populated yet. Resolve that
	// same object on the server thread so the toggle is effective throughout
	// startup as well as after native mutator registration.
	if (FConfiguration::Playlist)
	{
		AddUnique(
			FindObject<UFortPlaylistAthena>(
				FConfiguration::Playlist));
	}

	return Playlists;
}

static FConfiguredRespawnPlaylistSnapshot*
	FindOrCaptureConfiguredRespawnPlaylist(
		UFortPlaylistAthena* Playlist)
{
	for (auto& Snapshot :
		GConfiguredRespawnPlaylistSnapshots)
	{
		if (Snapshot.Playlist.Get() == Playlist)
			return &Snapshot;
	}

	FConfiguredRespawnPlaylistSnapshot Snapshot;
	Snapshot.Playlist =
		TWeakObjectPtr<UFortPlaylistAthena>(Playlist);
	Snapshot.bHasRespawnInAir =
		Playlist->HasbRespawnInAir();
	if (Snapshot.bHasRespawnInAir)
		Snapshot.bRespawnInAir =
			Playlist->bRespawnInAir;
	Snapshot.bHasRespawnHeight =
		Playlist->HasRespawnHeight();
	if (Snapshot.bHasRespawnHeight)
		Snapshot.RespawnHeight =
			Playlist->RespawnHeight;
	Snapshot.bHasRespawnTime =
		Playlist->HasRespawnTime();
	if (Snapshot.bHasRespawnTime)
		Snapshot.RespawnTime =
			Playlist->RespawnTime;
	Snapshot.bHasRespawnType =
		Playlist->HasRespawnType();
	if (Snapshot.bHasRespawnType)
		Snapshot.RespawnType =
			Playlist->RespawnType;

	GConfiguredRespawnPlaylistSnapshots.emplace_back(
		Snapshot);
	return &GConfiguredRespawnPlaylistSnapshots.back();
}

// Apply the GUI setting from the authoritative game thread. RespawnType alone
// is insufficient on several native LTMs: Athena's death path also checks the
// replicated GameState cheat-respawn switch before it creates RespawnData.
void AFortPlayerControllerAthena::ApplyConfiguredRespawnPolicy()
{
	auto World = UWorld::GetWorld();
	auto GameMode =
		World
			? (AFortGameMode*)World->AuthorityGameMode
			: nullptr;
	auto GameState =
		GameMode
			? (AFortGameStateAthena*)GameMode->GameState
			: nullptr;

	if (!IsUsableDeathObject(World) ||
		!IsUsableDeathObject(GameState))
	{
		// Playlist assets can outlive the transient World/GameState that
		// published them. Restore those snapshots even when the actor side is
		// already unavailable so a forced value cannot become the next
		// match's newly captured "authored" value.
		for (const auto& Snapshot :
			GConfiguredRespawnPlaylistSnapshots)
		{
			RestoreConfiguredRespawnPlaylist(Snapshot);
		}
		ResetConfiguredRespawnPolicyTracking(false);
		GConfiguredRespawnPolicyWorld =
			TWeakObjectPtr<UWorld>();
		GConfiguredRespawnPolicyGameState =
			TWeakObjectPtr<AFortGameStateAthena>();
		return;
	}

	if (GConfiguredRespawnPolicyWorld.Get() != World ||
		GConfiguredRespawnPolicyGameState.Get() != GameState)
	{
		// Playlist assets can survive world replacement. Restore those UObject
		// values before discarding the old match tracking so a forced setting
		// cannot leak into the next playlist. Do not touch the old GameState:
		// that actor may already be tearing down.
		for (const auto& Snapshot :
			GConfiguredRespawnPlaylistSnapshots)
		{
			RestoreConfiguredRespawnPlaylist(Snapshot);
		}
		ResetConfiguredRespawnPolicyTracking(false);
		GConfiguredRespawnPolicyWorld =
			TWeakObjectPtr<UWorld>(World);
		GConfiguredRespawnPolicyGameState =
			TWeakObjectPtr<AFortGameStateAthena>(GameState);
	}

	if (!FConfiguration::bForceRespawns)
	{
		if (!GConfiguredRespawnPlaylistSnapshots.empty() ||
			GConfiguredRespawnCheatCaptured)
		{
			ResetConfiguredRespawnPolicyTracking(true);
			SDK::DbgLog(
				"[RespawnPolicy] restored authored policy "
				"world=%p gameState=%p FN=%.2f\n",
				(void*)World, (void*)GameState,
				VersionInfo.FortniteVersion);
		}
		return;
	}

	auto CurrentPlaylists =
		ResolveAuthoritativeRespawnPlaylists(GameState);

	bool bChanged = false;
	if (GameState->HasbCheatRespawnEnabled())
	{
		if (!GConfiguredRespawnCheatCaptured)
		{
			GConfiguredRespawnOriginalCheat =
				GameState->bCheatRespawnEnabled;
			GConfiguredRespawnCheatCaptured = true;
		}
		if (!GameState->bCheatRespawnEnabled)
		{
			GameState->bCheatRespawnEnabled = true;
			bChanged = true;
		}
	}

	// Do not leave a previously selected asset modified after the GUI changes
	// modes while force respawn remains enabled.
	for (auto It =
			GConfiguredRespawnPlaylistSnapshots.begin();
		It != GConfiguredRespawnPlaylistSnapshots.end();)
	{
		auto Playlist = It->Playlist.Get();
		if (!Playlist ||
			std::find(
				CurrentPlaylists.begin(),
				CurrentPlaylists.end(),
				Playlist) == CurrentPlaylists.end())
		{
			RestoreConfiguredRespawnPlaylist(*It);
			It =
				GConfiguredRespawnPlaylistSnapshots.erase(
					It);
			continue;
		}
		++It;
	}

	uint8 DesiredRespawnType =
		FConfiguration::PermanentRespawn ? 1 : 2;
	for (auto Playlist : CurrentPlaylists)
	{
		FindOrCaptureConfiguredRespawnPlaylist(Playlist);

		if (Playlist->HasbRespawnInAir() &&
			!Playlist->bRespawnInAir)
		{
			Playlist->bRespawnInAir = true;
			bChanged = true;
		}
		if (Playlist->HasRespawnHeight())
		{
			auto& Height = Playlist->RespawnHeight;
			if (Height.Curve.CurveTable ||
				Height.Curve.RowName.IsValid() ||
				Height.Value !=
					(float)FConfiguration::RespawnHeight)
			{
				Height.Curve.CurveTable = nullptr;
				Height.Curve.RowName = FName();
				Height.Value =
					(float)FConfiguration::RespawnHeight;
				bChanged = true;
			}
		}
		if (Playlist->HasRespawnTime())
		{
			auto& Time = Playlist->RespawnTime;
			if (Time.Curve.CurveTable ||
				Time.Curve.RowName.IsValid() ||
				Time.Value !=
					(float)FConfiguration::RespawnTime)
			{
				Time.Curve.CurveTable = nullptr;
				Time.Curve.RowName = FName();
				Time.Value =
					(float)FConfiguration::RespawnTime;
				bChanged = true;
			}
		}
		if (Playlist->HasRespawnType() &&
			Playlist->RespawnType !=
				DesiredRespawnType)
		{
			Playlist->RespawnType =
				DesiredRespawnType;
			bChanged = true;
		}
	}

	if (bChanged)
	{
		GameState->ForceNetUpdate();
		SDK::DbgLog(
			"[RespawnPolicy] enforced explicit toggle "
			"world=%p gameState=%p playlists=%d "
			"type=%d time=%d height=%d nativeCheat=%d "
			"FN=%.2f\n",
			(void*)World, (void*)GameState,
			(int)CurrentPlaylists.size(),
			(int)DesiredRespawnType,
			FConfiguration::RespawnTime.load(),
			FConfiguration::RespawnHeight.load(),
			GameState->HasbCheatRespawnEnabled() &&
				GameState->bCheatRespawnEnabled
				? 1 : 0,
			VersionInfo.FortniteVersion);
	}
}

static bool IsRespawningAllowedForDeath(
	AFortGameMode* GameMode,
	AFortGameStateAthena* GameState,
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerStateAthena* PlayerState,
	bool bConfirmedStormDeath = false)
{
	// A late native mutator may have rewritten the effective policy after
	// SetupPlaylist. Reassert the explicit GUI override immediately before the
	// engine's own IsRespawningAllowed query.
	AFortPlayerControllerAthena::
		ApplyConfiguredRespawnPolicy();

	const bool bTerminalMatch =
		GUI::gsStatus == Ended ||
		(GameMode &&
		 GameMode->MatchState ==
			 FName(L"WaitingPostMatch")) ||
		(GameState &&
		 GameState->HasGamePhase() &&
		 GameState->GamePhase >=
			 static_cast<uint8>(
				 EAthenaGamePhase::EndGame));
	if (bTerminalMatch)
	{
		// Infinite Respawns never reopens a completed objective/match.
		return false;
	}

	// The global GUI respawn override is a player policy. Native AI, managed
	// PlayerAI, and command-spawned bots all have server-owned lifecycles and
	// no client handshake that can safely finish a replacement pawn. Treat any
	// reflected bot player state as terminal even when it is not registered in
	// one of Magnesium's explicit bot containers.
	if (PlayerState &&
		PlayerState->HasbIsABot() &&
		PlayerState->bIsABot)
	{
		return false;
	}

	if (IsManagedNonRespawningBot(PlayerController))
		return false;

	if (FConfiguration::bForceRespawns &&
		!FConfiguration::PermanentRespawn &&
		PlayerState &&
		PlayerState->HasDeathInfo() &&
		FDeathInfo::HasbInitialized() &&
		PlayerState->DeathInfo.bInitialized &&
		bConfirmedStormDeath &&
		PlayerState->DeathInfo.DeathCause == 0)
	{
		// RespawnType 2 is InfiniteRespawnExceptStorm. Keep this decision in
		// the shared policy so the native path and the watchdog cannot disagree
		// and revive a storm-eliminated player one tick later.
		return false;
	}

	bool bWaxRespawnAllowed = false;
	if (PlayerState &&
		FFortAthenaNativeLTMCompatibility::
			TryGetWaxRespawnAllowed(
				PlayerState,
				bWaxRespawnAllowed))
	{
		// Wax/Bounty owns a finite, per-player lives counter even though the
		// playlist advertises InfiniteRespawn. Neither the global force option
		// nor the playlist fallback may resurrect a permanently eliminated
		// player.
		return bWaxRespawnAllowed;
	}

	bool bDeepFriedRespawnAllowed = false;
	if (PlayerState &&
		FFortAthenaNativeLTMCompatibility::
			TryGetFoodFightRespawnAllowed(
				PlayerState,
				bDeepFriedRespawnAllowed))
	{
		// Objective loss is authoritative for this team. In particular, the
		// global force-respawn option must not resurrect a Food Fight team
		// whose mascot has been destroyed.
		return bDeepFriedRespawnAllowed;
	}

	bool bDiscoRespawnAllowed = false;
	if (PlayerState &&
		FFortAthenaNativeLTMCompatibility::
			TryGetDiscoRespawnAllowed(
				PlayerState,
				bDiscoRespawnAllowed))
	{
		// Disco's authored safe-zone cutoff remains authoritative even when
		// the global force-respawn option is enabled.
		return bDiscoRespawnAllowed;
	}

	if (!GameMode || !GameState || !PlayerState)
	{
		return FConfiguration::bForceRespawns;
	}

	auto IsRespawningAllowedFunction =
		GameState->GetFunction("IsRespawningAllowed");
	bool bRespawnAllowed = false;

	if (IsRespawningAllowedFunction)
	{
		bRespawnAllowed = GameState->Call<bool>(
			IsRespawningAllowedFunction, PlayerState);
	}
	else
	{
		const UFortPlaylistAthena* Playlist = nullptr;
		if (VersionInfo.FortniteVersion >= 3.5 &&
			GameMode->HasWarmupRequiredPlayerCount())
		{
			if (GameState->HasCurrentPlaylistInfo() &&
				GameState->CurrentPlaylistInfo.HasBasePlaylist())
			{
				Playlist =
					GameState->CurrentPlaylistInfo.BasePlaylist;
			}
			else if (GameState->HasCurrentPlaylistData())
			{
				Playlist = GameState->CurrentPlaylistData;
			}
		}

		// Some legacy builds do not expose IsRespawningAllowed. Preserve the
		// existing playlist/config fallback for those builds.
		bRespawnAllowed = Playlist
			? (Playlist->HasRespawnType()
				? Playlist->RespawnType > 0
				: FConfiguration::bForceRespawns.load())
			: FConfiguration::bForceRespawns.load();
	}

	// Outside the explicit objective/lives/cutoff returns above, the visible
	// Infinite Respawns setting is authoritative for every playlist, including
	// the 10.40 native LTMs.
	return bRespawnAllowed ||
		FConfiguration::bForceRespawns;
}

struct FPendingForcedRespawnRepair
{
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AFortGameStateAthena> GameState;
	TWeakObjectPtr<AFortPlayerControllerAthena>
		PlayerController;
	TWeakObjectPtr<AFortPlayerStateAthena> PlayerState;
	TWeakObjectPtr<AFortPlayerPawnAthena> DeadPawn;
	FVector DeathLocation{};
	FRotator DeathRotation{};
	float RemainingSeconds = 0.f;
	bool bPrepareRequested = false;
	bool bSeededByRepair = false;
	bool bCountdownGraceGranted = false;
	int DirectAttempts = 0;
};

static std::vector<FPendingForcedRespawnRepair>
	GPendingForcedRespawnRepairs;
static std::unordered_map<
	AFortPlayerControllerAthena*,
	TWeakObjectPtr<AFortGameStateAthena>>
	GStormRespawnBlockedControllers;

static void RefreshSpawnedBotTrackingWorld()
{
	auto CurrentWorld = UWorld::GetWorld();
	if (GSpawnedBotTrackingWorldIdentity == CurrentWorld &&
		GSpawnedBotTrackingWorld.Get() == CurrentWorld)
	{
		return;
	}

	const int PreviousBotCount =
		(int)GSpawnedBotControllers.size();
	const int PreviousCleanupCount =
		(int)GPendingSpawnedBotCleanup.size();
	auto PreviousWorld = GSpawnedBotTrackingWorldIdentity;

	// Every container below uses a raw controller address as its key. Remove
	// entries belonging to the old world's synthetic controllers before a new
	// actor can reuse one of those addresses and pass a pointer-only membership
	// check. This runs only when the world token changes and remains game-thread
	// local; no hook, worker, or cross-module client dependency is introduced.
	for (auto PlayerController : GSpawnedBotControllers)
	{
		GPendingRespawnLandingFinalization.erase(PlayerController);
		GRespawnSkydivingObserved.erase(PlayerController);
		GPendingLegacyAircraftLandingEquipment.erase(PlayerController);
		GLegacyAircraftSkydivingObserved.erase(PlayerController);
		GRespawnHiddenWeapons.erase(PlayerController);
		GLastAcknowledgedPawn.erase(PlayerController);
		GPendingLateGameAircraftLoadout.erase(PlayerController);
		GSkipPossessRespawnControllers.erase(PlayerController);
		GFinalizePossessTakeover.erase(PlayerController);
		GRemoteControlReturnPawn.erase(PlayerController);
		GVehiclePossessionReturnPawn.erase(PlayerController);
		GVehiclePossessionVehicle.erase(PlayerController);
		GTrackedVehicleLoadouts.erase(PlayerController);
		PlayersInitialized.erase(PlayerController);
		GStormRespawnBlockedControllers.erase(PlayerController);
	}

	// Forced-respawn records are serial-aware and world-bound. Retain any record
	// already created for the new world while discarding all old-world work.
	GPendingForcedRespawnRepairs.erase(
		std::remove_if(
			GPendingForcedRespawnRepairs.begin(),
			GPendingForcedRespawnRepairs.end(),
			[CurrentWorld](const FPendingForcedRespawnRepair& Pending)
			{
				return !CurrentWorld ||
					Pending.World.Get() != CurrentWorld;
			}),
		GPendingForcedRespawnRepairs.end());

	GPendingSpawnedBotCleanup.clear();
	G172SpawnedBotRemovalAttempts.clear();
	GSpawnedBotControllers.clear();
	GSpawnedBotTrackingWorld = CurrentWorld
		? TWeakObjectPtr<UWorld>(CurrentWorld)
		: TWeakObjectPtr<UWorld>{};
	GSpawnedBotTrackingWorldIdentity = CurrentWorld;

	if (PreviousBotCount > 0 || PreviousCleanupCount > 0)
	{
		SDK::DbgLog(
			"[Elimination] reset spawnbot tracking for world transition old=%p new=%p bots=%d pending=%d\n",
			(void*)PreviousWorld, (void*)CurrentWorld,
			PreviousBotCount, PreviousCleanupCount);
	}
}

static void InvalidateRespawnHandshake(
	AFortPlayerStateAthena* PlayerState)
{
	if (!IsUsableDeathObject(PlayerState) ||
		!PlayerState->HasRespawnData())
	{
		return;
	}

	auto& RespawnData = PlayerState->RespawnData;
	if (RespawnData.HasbRespawnDataAvailable())
		RespawnData.bRespawnDataAvailable = false;
	if (RespawnData.HasbServerIsReady())
		RespawnData.bServerIsReady = false;
	if (RespawnData.HasbClientIsReady())
		RespawnData.bClientIsReady = false;
	PlayerState->ForceNetUpdate();
}

static bool HasForcedRespawnReplacementPawn(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* DeadPawn)
{
	if (!IsUsableDeathObject(PlayerController))
		return false;

	auto IsReplacement =
		[&](AActor* Candidate)
		{
			if (!IsUsableDeathObject(Candidate) ||
				Candidate == DeadPawn)
			{
				return false;
			}

			auto CandidatePawn =
				Candidate->Cast<AFortPlayerPawnAthena>();
			return !CandidatePawn ||
				!CandidatePawn->HasbIsDying() ||
				!CandidatePawn->bIsDying;
		};

	return IsReplacement(PlayerController->Pawn) ||
		IsReplacement(PlayerController->MyFortPawn);
}

static bool HasActiveForcedRespawnHandshake(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerStateAthena* PlayerState)
{
	if (!IsUsableDeathObject(PlayerController) ||
		!IsUsableDeathObject(PlayerState) ||
		IsManagedNonRespawningBot(PlayerController))
	{
		return false;
	}

	if (auto IsInRespawnCountdown =
			PlayerController->GetFunction(
				"IsInRespawnCountdown"))
	{
		if (PlayerController->Call<bool>(
				IsInRespawnCountdown))
		{
			return true;
		}
	}

	// RespawnData's available/server-ready bits only mean that the server has
	// prepared a handshake. They are not proof that the client acknowledged it
	// or that a replacement pawn exists. Only an actual countdown is useful as
	// progress telemetry, and even that must not retire the repair watchdog.
	return false;
}

static void QueueForcedRespawnRepair(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerStateAthena* PlayerState,
	AFortPlayerPawnAthena* DeadPawn,
	FVector DeathLocation,
	FRotator DeathRotation)
{
	if (!IsUsableDeathObject(PlayerController) ||
		!IsUsableDeathObject(PlayerState) ||
		IsManagedNonRespawningBot(PlayerController))
	{
		return;
	}

	for (int Index =
		(int)GPendingForcedRespawnRepairs.size() - 1;
		Index >= 0; --Index)
	{
		if (GPendingForcedRespawnRepairs[Index]
				.PlayerController.Get() == PlayerController)
		{
			GPendingForcedRespawnRepairs.erase(
				GPendingForcedRespawnRepairs.begin() +
					Index);
		}
	}

	auto World = UWorld::GetWorld();
	auto GameMode =
		World
			? (AFortGameMode*)World->AuthorityGameMode
			: nullptr;
	FPendingForcedRespawnRepair Pending;
	Pending.World = TWeakObjectPtr<UWorld>(World);
	Pending.GameState =
		TWeakObjectPtr<AFortGameStateAthena>(
			GameMode
				? (AFortGameStateAthena*)GameMode->GameState
				: nullptr);
	Pending.PlayerController =
		TWeakObjectPtr<AFortPlayerControllerAthena>(
			PlayerController);
	Pending.PlayerState =
		TWeakObjectPtr<AFortPlayerStateAthena>(
			PlayerState);
	Pending.DeadPawn =
		TWeakObjectPtr<AFortPlayerPawnAthena>(
			DeadPawn);
	Pending.DeathLocation = DeathLocation;
	Pending.DeathRotation = DeathRotation;
	// Give the native countdown its configured amount of time after the
	// one-tick preparation repair before using the exact-once hard fallback.
	Pending.RemainingSeconds =
		(std::max)(
			1.5f,
			(float)FConfiguration::RespawnTime + 1.f);
	GPendingForcedRespawnRepairs.emplace_back(Pending);

	SDK::DbgLog(
		"[RespawnRepair] queued controller=%p "
		"playerState=%p deadPawn=%p delay=%.2f "
		"FN=%.2f\n",
		(void*)PlayerController, (void*)PlayerState,
		(void*)DeadPawn, Pending.RemainingSeconds,
		VersionInfo.FortniteVersion);
}

static bool SeedForcedRespawnData(
	FPendingForcedRespawnRepair& Pending,
	AFortGameMode* GameMode,
	AFortPlayerStateAthena* PlayerState)
{
	if (!IsUsableDeathObject(PlayerState) ||
		!PlayerState->HasRespawnData())
	{
		return false;
	}

	FVector RespawnLocation = Pending.DeathLocation;
	FVector ConfiguredRespawnLocation{};
	if (TryGetConfiguredRespawnLocation(
			GameMode, ConfiguredRespawnLocation))
	{
		RespawnLocation = ConfiguredRespawnLocation;
	}
	else
	{
		if (!IsFiniteRespawnLocation(RespawnLocation))
			RespawnLocation = FVector();
		RespawnLocation.Z +=
			(double)FConfiguration::RespawnHeight;
	}

	auto& RespawnData = PlayerState->RespawnData;
	if (RespawnData.HasRespawnLocation())
		RespawnData.RespawnLocation =
			RespawnLocation;
	if (RespawnData.HasRespawnRotation())
		RespawnData.RespawnRotation =
			Pending.DeathRotation;
	if (RespawnData.HasbRespawnDataAvailable())
		RespawnData.bRespawnDataAvailable = true;
	if (RespawnData.HasbServerIsReady())
		RespawnData.bServerIsReady = true;
	if (RespawnData.HasbClientIsReady())
		RespawnData.bClientIsReady = false;
	PlayerState->ForceNetUpdate();
	return true;
}

static void TickForcedRespawnRepairs(float DeltaSeconds)
{
	AFortPlayerControllerAthena::
		ApplyConfiguredRespawnPolicy();

	for (int Index =
		(int)GPendingForcedRespawnRepairs.size() - 1;
		Index >= 0; --Index)
	{
		auto& Pending =
			GPendingForcedRespawnRepairs[Index];
		auto CurrentWorld = UWorld::GetWorld();
		auto PlayerController =
			Pending.PlayerController.Get();
		auto PlayerState = Pending.PlayerState.Get();
		auto DeadPawn = Pending.DeadPawn.Get();
		auto GameMode =
			CurrentWorld
				? (AFortGameMode*)
					CurrentWorld->AuthorityGameMode
				: nullptr;
		auto GameState =
			GameMode
				? (AFortGameStateAthena*)GameMode->GameState
				: nullptr;

		const bool bInvalidLifecycle =
			Pending.World.Get() != CurrentWorld ||
			Pending.GameState.Get() != GameState ||
			!IsUsableDeathObject(PlayerController) ||
			!IsUsableDeathObject(PlayerState) ||
			PlayerController->PlayerState != PlayerState ||
			!IsUsableDeathObject(GameMode) ||
			!IsUsableDeathObject(GameState) ||
			!FConfiguration::bForceRespawns ||
			GUI::gsStatus == Ended ||
			GameMode->MatchState ==
				FName(L"WaitingPostMatch");
		if (bInvalidLifecycle ||
			!IsRespawningAllowedForDeath(
				GameMode, GameState, PlayerController,
				PlayerState))
		{
			GPendingForcedRespawnRepairs.erase(
				GPendingForcedRespawnRepairs.begin() +
					Index);
			continue;
		}

		if (HasForcedRespawnReplacementPawn(
				PlayerController, DeadPawn))
		{
			SDK::DbgLog(
				"[RespawnRepair] native replacement "
				"observed controller=%p deadPawn=%p\n",
				(void*)PlayerController,
				(void*)DeadPawn);
			GPendingForcedRespawnRepairs.erase(
				GPendingForcedRespawnRepairs.begin() +
					Index);
			continue;
		}

		if (!Pending.bPrepareRequested)
		{
			Pending.bPrepareRequested = true;
			if (auto PrepareClientForRespawning =
					PlayerController->GetFunction(
						"PrepareClientForRespawning"))
			{
				PlayerController->Call<void>(
					PrepareClientForRespawning);
				SDK::DbgLog(
					"[RespawnRepair] requested native "
					"preparation controller=%p "
					"playerState=%p\n",
					(void*)PlayerController,
					(void*)PlayerState);
			}
			else
			{
				SDK::DbgLog(
					"[RespawnRepair] native preparation "
					"function unavailable controller=%p "
					"FN=%.2f\n",
					(void*)PlayerController,
					VersionInfo.FortniteVersion);
			}

			if (HasForcedRespawnReplacementPawn(
					PlayerController, DeadPawn))
			{
				GPendingForcedRespawnRepairs.erase(
					GPendingForcedRespawnRepairs.begin() +
						Index);
				continue;
			}
		}

		Pending.RemainingSeconds -= DeltaSeconds;
		if (Pending.RemainingSeconds > 0.f)
			continue;

		// A few clients begin their visible countdown late even though the
		// server completed preparation. Give a real countdown one bounded
		// grace window before invoking the hard fallback; handshake bits alone
		// do not qualify because they can remain stuck indefinitely.
		if (!Pending.bCountdownGraceGranted &&
			HasActiveForcedRespawnHandshake(
				PlayerController, PlayerState))
		{
			Pending.bCountdownGraceGranted = true;
			Pending.RemainingSeconds = 2.f;
			SDK::DbgLog(
				"[RespawnRepair] active countdown grace "
				"controller=%p playerState=%p\n",
				(void*)PlayerController,
				(void*)PlayerState);
			continue;
		}

		const bool bSeededRespawnData =
			SeedForcedRespawnData(
				Pending, GameMode, PlayerState);
		Pending.bSeededByRepair |=
			bSeededRespawnData;
		bool bInvokedReadyLifecycle = false;
		if (bSeededRespawnData)
		{
			if (auto ServerClientIsReadyToRespawn =
					PlayerController->GetFunction(
						"ServerClientIsReadyToRespawn"))
			{
				PlayerController->Call<void>(
					ServerClientIsReadyToRespawn);
				bInvokedReadyLifecycle = true;
			}
		}

		bool bRestartRequested = false;
		if (!HasForcedRespawnReplacementPawn(
				PlayerController, DeadPawn))
		{
			auto ControlledPawn =
				PlayerController->Pawn;
			auto ControlledFortPawn =
				IsUsableDeathObject(ControlledPawn)
					? ControlledPawn
						->Cast<AFortPlayerPawnAthena>()
					: nullptr;
			if (IsUsableDeathObject(ControlledPawn) &&
				(ControlledPawn == DeadPawn ||
					(ControlledFortPawn &&
						ControlledFortPawn->HasbIsDying() &&
						ControlledFortPawn->bIsDying)))
			{
				PlayerController->UnPossess();
			}

			GameMode->RestartPlayer(PlayerController);
			bRestartRequested = true;
		}

		Pending.DirectAttempts++;
		const bool bReplacementCreated =
			HasForcedRespawnReplacementPawn(
				PlayerController, DeadPawn);
		const bool bHandshakeCreated =
			HasActiveForcedRespawnHandshake(
				PlayerController, PlayerState);
		SDK::DbgLog(
			"[RespawnRepair] fallback attempt=%d "
			"controller=%p seeded=%d ready=%d restart=%d "
			"replacement=%d handshake=%d\n",
			Pending.DirectAttempts,
			(void*)PlayerController,
			bSeededRespawnData ? 1 : 0,
			bInvokedReadyLifecycle ? 1 : 0,
			bRestartRequested ? 1 : 0,
			bReplacementCreated ? 1 : 0,
			bHandshakeCreated ? 1 : 0);

		if (bReplacementCreated ||
			Pending.DirectAttempts >= 3)
		{
			GPendingForcedRespawnRepairs.erase(
				GPendingForcedRespawnRepairs.begin() +
					Index);
			continue;
		}

		// One bounded retry catches versions where RestartPlayer completes on
		// the following server frame without risking a duplicate-pawn loop.
		Pending.RemainingSeconds = 1.f;
	}
}

static bool IsGetawayPlaylist(
	const UFortPlaylistAthena* Playlist)
{
	if (!IsUsableDeathObject(Playlist))
		return false;

	// Use the mutator-aware detector on the builds it supports, then retain
	// stable playlist identity fallbacks for later Getaway reruns.
	if (FFortAthenaHeistCompatibility::IsSupportedBuild() &&
		FFortAthenaHeistCompatibility::IsHeistPlaylist(
			Playlist))
	{
		return true;
	}

	const auto ObjectName =
		LowerAscii(Playlist->Name.ToString().c_str());
	if (ObjectName.find("playlist_bling_") !=
			std::string::npos ||
		ObjectName.find("playlist_heist_") !=
			std::string::npos ||
		ObjectName.find("getaway") != std::string::npos)
	{
		return true;
	}

	if (Playlist->HasPlaylistName())
	{
		const auto PlaylistName =
			LowerAscii(
				Playlist->PlaylistName.ToString().c_str());
		if (PlaylistName.find("the getaway") !=
				std::string::npos ||
			PlaylistName.find("getaway") !=
				std::string::npos)
		{
			return true;
		}
	}

	return false;
}

static bool IsCurrentGetawayMatch(
	AFortGameStateAthena* GameState)
{
	if (!IsUsableDeathObject(GameState))
		return false;

	if (GameState->HasCurrentPlaylistInfo())
	{
		const UFortPlaylistAthena* OverridePlaylist =
			FPlaylistPropertyArray::HasOverridePlaylist()
				? GameState->CurrentPlaylistInfo.OverridePlaylist
				: nullptr;
		const UFortPlaylistAthena* BasePlaylist =
			FPlaylistPropertyArray::HasBasePlaylist()
				? GameState->CurrentPlaylistInfo.BasePlaylist
				: nullptr;
		if (IsGetawayPlaylist(OverridePlaylist) ||
			IsGetawayPlaylist(BasePlaylist))
		{
			return true;
		}
	}

	return GameState->HasCurrentPlaylistData() &&
		IsGetawayPlaylist(
			GameState->CurrentPlaylistData);
}

static bool IsCurrentNative1040LTM(
	AFortGameStateAthena* GameState)
{
	if (!IsUsableDeathObject(GameState))
		return false;

	if (GameState->HasCurrentPlaylistInfo())
	{
		const UFortPlaylistAthena* OverridePlaylist =
			FPlaylistPropertyArray::HasOverridePlaylist()
				? GameState->CurrentPlaylistInfo.OverridePlaylist
				: nullptr;
		const UFortPlaylistAthena* BasePlaylist =
			FPlaylistPropertyArray::HasBasePlaylist()
				? GameState->CurrentPlaylistInfo.BasePlaylist
				: nullptr;
		if (FFortAthenaNativeLTMCompatibility::
				IsTargetPlaylist(OverridePlaylist) ||
			FFortAthenaNativeLTMCompatibility::
				IsTargetPlaylist(BasePlaylist))
		{
			return true;
		}
	}

	return GameState->HasCurrentPlaylistData() &&
		FFortAthenaNativeLTMCompatibility::IsTargetPlaylist(
			GameState->CurrentPlaylistData);
}

static bool IsAshtonPlaylist(
	const UFortPlaylistAthena* Playlist)
{
	return IsUsableDeathObject(Playlist) &&
		LowerAscii(Playlist->Name.ToString().c_str()) ==
			"playlist_ashton_lg";
}

static bool IsCurrentAshtonMatch(
	AFortGameStateAthena* GameState)
{
	bool bHasAuthoritativePlaylist = false;
	auto CheckPlaylist =
		[&](const UFortPlaylistAthena* Playlist)
		{
			if (!IsUsableDeathObject(Playlist))
				return false;
			bHasAuthoritativePlaylist = true;
			return IsAshtonPlaylist(Playlist);
		};

	if (IsUsableDeathObject(GameState))
	{
		if (GameState->HasCurrentPlaylistInfo())
		{
			const auto OverridePlaylist =
				FPlaylistPropertyArray::HasOverridePlaylist()
					? GameState->CurrentPlaylistInfo
						.OverridePlaylist
					: nullptr;
			const auto BasePlaylist =
				FPlaylistPropertyArray::HasBasePlaylist()
					? GameState->CurrentPlaylistInfo
						.BasePlaylist
					: nullptr;
			if (CheckPlaylist(OverridePlaylist) ||
				CheckPlaylist(BasePlaylist))
			{
				return true;
			}
		}
		if (GameState->HasCurrentPlaylistData() &&
			CheckPlaylist(
				GameState->CurrentPlaylistData))
		{
			return true;
		}
	}
	if (bHasAuthoritativePlaylist)
		return false;

	if (!FConfiguration::Playlist)
		return false;
	const std::wstring ConfiguredPlaylist =
		FConfiguration::Playlist;
	return ConfiguredPlaylist.find(
		L"Playlist_Ashton_Lg") !=
		std::wstring::npos;
}

static bool IsCarmineGauntletDefinition(
	const UFortItemDefinition* Definition)
{
	return VersionInfo.FortniteVersion == 10.40 &&
		IsUsableDeathObject(Definition) &&
		(Definition->Name.ToWString() ==
			 L"AGID_CarminePack" ||
		 Definition->Name.ToWString() ==
			 L"AGID_AshtonPack");
}

static bool IsCarmineGauntletArtifact(
	const UFortItemDefinition* Definition)
{
	if (IsCarmineGauntletDefinition(Definition))
		return true;
	return VersionInfo.FortniteVersion == 10.40 &&
		IsUsableDeathObject(Definition) &&
		(Definition->Name.ToWString() ==
			 L"D_CarminePack" ||
		 Definition->Name.ToWString() ==
			 L"D_AshtonPack");
}

static std::vector<FGuid> SnapshotCarmineGauntletArtifacts(
	AFortPlayerControllerAthena* PlayerController)
{
	std::vector<FGuid> Result;
	if (!IsUsableDeathObject(PlayerController) ||
		!IsUsableDeathObject(
			PlayerController->WorldInventory))
	{
		return Result;
	}

	auto& Entries =
		PlayerController->WorldInventory
			->Inventory.ReplicatedEntries;
	Result.reserve(Entries.Num());
	for (int32 Index = 0;
		Index < Entries.Num();
		++Index)
	{
		auto& Entry =
			Entries.Get(Index, FFortItemEntry::Size());
		if (IsCarmineGauntletArtifact(
				Entry.ItemDefinition))
		{
			Result.push_back(Entry.ItemGuid);
		}
	}
	return Result;
}

static bool ContainsCarmineArtifactGuid(
	const std::vector<FGuid>& Guids,
	const FGuid& Guid)
{
	return std::any_of(
		Guids.begin(),
		Guids.end(),
		[&](const FGuid& Candidate)
		{
			return VehicleLoadoutGuidsEqual(
				Candidate, Guid);
		});
}

static int32 RemoveCarmineGauntletArtifacts(
	AFortPlayerControllerAthena* PlayerController,
	const std::vector<FGuid>* PreserveGuids = nullptr)
{
	if (!IsUsableDeathObject(PlayerController) ||
		!IsUsableDeathObject(
			PlayerController->WorldInventory))
	{
		return 0;
	}

	std::vector<FGuid> RemoveGuids;
	auto& Entries =
		PlayerController->WorldInventory
			->Inventory.ReplicatedEntries;
	for (int32 Index = 0;
		Index < Entries.Num();
		++Index)
	{
		auto& Entry =
			Entries.Get(Index, FFortItemEntry::Size());
		if (!IsCarmineGauntletArtifact(
				Entry.ItemDefinition) ||
			(PreserveGuids &&
			 ContainsCarmineArtifactGuid(
				 *PreserveGuids,
				 Entry.ItemGuid)))
		{
			continue;
		}
		RemoveGuids.push_back(Entry.ItemGuid);
	}

	for (const auto& Guid : RemoveGuids)
		PlayerController->WorldInventory->Remove(Guid);
	return static_cast<int32>(RemoveGuids.size());
}

static bool DropManualCarmineGauntletOnDeath(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* PickupLockoutPawn,
	const FVector& DropLocation)
{
	if (!IsUsableDeathObject(PlayerController) ||
		!IsUsableDeathObject(
			PlayerController->WorldInventory))
	{
		return false;
	}

	auto Entry =
		PlayerController->WorldInventory
			->Inventory.ReplicatedEntries.Search(
				[](FFortItemEntry& Candidate)
				{
					return IsCarmineGauntletDefinition(
						Candidate.ItemDefinition);
				},
				FFortItemEntry::Size());
	if (!Entry)
		return false;

	auto Definition = Entry->ItemDefinition;
	const bool bCanSuppressImmediateAutoPickup =
		Definition &&
		Definition->HasbForceAutoPickup();
	const bool bOriginalForceAutoPickup =
		bCanSuppressImmediateAutoPickup
			? Definition->bForceAutoPickup
			: false;
	if (bCanSuppressImmediateAutoPickup)
	{
		// Carmine is authored for forced auto-pickup. Disable that flag only
		// during TossPickup so a nearby killer cannot consume the actor inside
		// this same death call before stale inventory rows are cleaned.
		Definition->bForceAutoPickup = false;
	}
	auto Pickup = AFortInventory::SpawnPickup(
		DropLocation,
		*Entry,
		EFortPickupSourceTypeFlag::GetPlayer(),
		EFortPickupSpawnSource::GetPlayerElimination(),
		IsUsableDeathObject(PickupLockoutPawn)
			? PickupLockoutPawn
			: nullptr,
		-1,
		true,
		true,
		true,
		nullptr);
	if (bCanSuppressImmediateAutoPickup)
	{
		Definition->bForceAutoPickup =
			bOriginalForceAutoPickup;
	}
	if (!Pickup)
		return false;

	const int32 Removed =
		RemoveCarmineGauntletArtifacts(
			PlayerController);
	SDK::DbgLog(
		"[Ashton1040] manual Carmine death drop "
		"controller=%p pickup=%p removedRows=%d "
		"keepInventory=%d\n",
		(void*)PlayerController,
		(void*)Pickup,
		Removed,
		(int)FConfiguration::bKeepInventory);
	return true;
}

static bool HasNearbyCarmineGauntletPickup(
	const FVector& Location)
{
	const UClass* PickupClass =
		AFortPickupAthena::StaticClass();
	if (!PickupClass)
		return false;

	TArray<AActor*> Pickups{};
	Utils::GetAll(
		const_cast<UClass*>(PickupClass),
		Pickups);
	if (Pickups.Num() < 0 ||
		Pickups.Max() < Pickups.Num() ||
		Pickups.Num() > 4096)
	{
		Pickups.Free();
		return false;
	}
	constexpr double PickupRadiusSquared =
		750.0 * 750.0;
	bool bFound = false;
	for (auto Actor : Pickups)
	{
		auto Pickup =
			IsUsableDeathObject(Actor) &&
					Actor->IsA(PickupClass)
				? static_cast<AFortPickupAthena*>(
					  Actor)
				: nullptr;
		if (!Pickup ||
			(Pickup->HasbPickedUp() &&
			 Pickup->bPickedUp) ||
			(Pickup->HasbActorIsBeingDestroyed() &&
			 Pickup->bActorIsBeingDestroyed) ||
			!Pickup->HasPrimaryPickupItemEntry() ||
			!IsCarmineGauntletDefinition(
				Pickup->PrimaryPickupItemEntry
					.ItemDefinition))
		{
			continue;
		}

		const FVector Delta =
			Pickup->K2_GetActorLocation() -
			Location;
		if (Delta.SizeSquared() <=
			PickupRadiusSquared)
		{
			bFound = true;
			break;
		}
	}
	Pickups.Free();
	return bFound;
}

void AFortPlayerControllerAthena::ClientOnPawnDied(AFortPlayerControllerAthena* PlayerController, FFortPlayerDeathReport& DeathReport)
{
	if (!PlayerController)
		return ClientOnPawnDiedOG(PlayerController, DeathReport);

	// A DBNO notification is not a pawn replacement. The custom revive path
	// re-acknowledges this same pawn after clearing its downed state; if its
	// remembered identity is erased here, ServerAcknowledgePossession mistakes
	// that acknowledgement for a true death respawn and applies the configured
	// mid-zone spawn transform.
	const bool bIsDBNONotification =
		IsPawnDBNOForSpectating(PlayerController->Pawn);
	if (bIsDBNONotification)
	{
		// A knock is not an elimination. Keep the pawn identity and leave the
		// native DBNO notification to handle its UI/state without entering our
		// inventory, siphon, kill-credit, spectating, or respawn pipelines.
		SDK::DbgLog(
			"[DBNO] preserving same-pawn death notification "
			"controller=%p pawn=%p version=%.2f\n",
			(void*)PlayerController,
			(void*)PlayerController->Pawn,
			VersionInfo.FortniteVersion);
		return ClientOnPawnDiedOG(
			PlayerController,
			DeathReport);
	}

	// Remote-controlled projectiles use the pawn-death notification to end their
	// native possession. If the remembered character is still alive, this is the
	// missile dying, not the player: running our custom elimination path here
	// drops inventory and makes the native return look like a configured respawn.
	auto RemoteReturn = GRemoteControlReturnPawn.find(PlayerController);
	if (RemoteReturn != GRemoteControlReturnPawn.end() &&
		IsLiveRemoteControlReturnPawn(RemoteReturn->second))
	{
		SDK::DbgLog("[Possession] remote-control pawn death left to native controller=%p return=%p\n",
			(void*)PlayerController, (void*)RemoteReturn->second);
		return ClientOnPawnDiedOG(PlayerController, DeathReport);
	}
	GRemoteControlReturnPawn.erase(PlayerController);

	auto VehicleReturn =
		GVehiclePossessionReturnPawn.find(PlayerController);
	if (VehicleReturn != GVehiclePossessionReturnPawn.end() &&
		IsLiveRemoteControlReturnPawn(VehicleReturn->second))
	{
		SDK::DbgLog(
			"[Possession] vehicle pawn death left to native "
			"controller=%p return=%p\n",
			(void*)PlayerController, (void*)VehicleReturn->second);
		return ClientOnPawnDiedOG(PlayerController, DeathReport);
	}
	GVehiclePossessionReturnPawn.erase(PlayerController);
	GVehiclePossessionVehicle.erase(PlayerController);
	GTrackedVehicleLoadouts.erase(PlayerController);

	RestoreRespawnHiddenWeapon(PlayerController);
	GPendingRespawnLandingFinalization.erase(
		PlayerController);
	GRespawnSkydivingObserved.erase(
		PlayerController);
	GPendingLegacyAircraftLandingEquipment.erase(
		PlayerController);
	GLegacyAircraftSkydivingObserved.erase(
		PlayerController);

	// Only a permanent death can be followed by a replacement player pawn.
	// Preserve the identity across DBNO/revive so a same-pawn acknowledgement
	// remains a duplicate and cannot run respawn setup or teleport the player.
	if (!bIsDBNONotification)
		GLastAcknowledgedPawn.erase(PlayerController);

	auto World = UWorld::GetWorld();
	auto GameMode = World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
	auto GameState = GameMode ? (AFortGameStateAthena*)GameMode->GameState : nullptr;
	auto PlayerState = PlayerController->HasPlayerState() ? (AFortPlayerStateAthena*)PlayerController->PlayerState : nullptr;

	if (!GameMode || !GameState || !PlayerState)
		return ClientOnPawnDiedOG(PlayerController, DeathReport);

	const bool bPlayerAIVictim =
		MagnesiumPlayerAIIntegration::IsPlayerAIController(
			PlayerController);
	const bool bSpawnedCommandBotVictim =
		IsTrackedSpawnedBotController(PlayerController);
	auto ManagedBotEliminatedPawn =
		(AFortPlayerPawnAthena*)PlayerController->Pawn;
	auto ManagedBotInventory = PlayerController->WorldInventory;

	const bool bMatchWasLiveAtDeath =
		GUI::gsStatus.load(
			std::memory_order_acquire) == StartedMatch;
	bool bVictimWasAliveParticipant =
		IsControllerInAlivePlayers(
			GameMode, PlayerController);
	if (!bVictimWasAliveParticipant &&
		GameMode->HasAliveBots())
	{
		for (auto AliveBot : GameMode->AliveBots)
		{
			if (AliveBot == PlayerController)
			{
				bVictimWasAliveParticipant = true;
				break;
			}
		}
	}

	const bool bIsLateSeasonBotVictim =
		VersionInfo.FortniteVersion >= 17.0 &&
		VersionInfo.FortniteVersion < 19.0 &&
		((PlayerState->HasbIsABot() && PlayerState->bIsABot) ||
			MagnesiumPlayerAIIntegration::IsPlayerAIController(
				PlayerController));
	UFortWeaponItemDefinition* LateSeasonFinishingWeapon = nullptr;
	uint8 LateSeasonDeathCause = 0;

	bool bCalledNativeDeathEarly = false;
	const bool bIs172SpawnedBotVictim =
		VersionInfo.FortniteVersion == 1.72 &&
		IsTrackedSpawnedBotController(PlayerController);
	const bool bIsFinal172SpawnedBotDeath =
		bIs172SpawnedBotVictim &&
		(!PlayerController->Pawn || !PlayerController->Pawn->IsDBNO());
	const bool bCanAttempt172SpawnedBotRemoval =
		bIsFinal172SpawnedBotDeath &&
		G172SpawnedBotRemovalAttempts.insert(PlayerController).second;

	const bool bPawnNormallyDropsItems =
		PlayerController->Pawn &&
		(PlayerController->Pawn->HasbShouldDropItemsOnDeath()
			? PlayerController->Pawn->bShouldDropItemsOnDeath
			: true);
	const bool bIsNative1040LTM =
		IsCurrentNative1040LTM(GameState);
	const bool bDropNormalInventory =
		bPawnNormallyDropsItems &&
		!FConfiguration::bKeepInventory &&
		!bIsNative1040LTM;
	const bool bIsGetawayMatch =
		IsCurrentGetawayMatch(GameState);
	const bool bKeepInventoryApplies =
		FConfiguration::bKeepInventory &&
		(!bIsNative1040LTM || bIsGetawayMatch);
	const bool bIsAshtonMatch =
		IsCurrentAshtonMatch(GameState);
	const bool bWasAshtonLeader =
		bIsAshtonMatch &&
		FFortAthenaNativeLTMCompatibility::
			IsCurrentAshtonLeader(
				PlayerController);
	if (bWasAshtonLeader)
	{
		// Publish the vacancy before gadget removal raises the base passive's
		// death event. This blocks its stock killer-transfer/world-drop path;
		// the next completed stone owns succession instead.
		FFortAthenaNativeLTMCompatibility::
			HandleAshtonLeaderEliminated(
				PlayerController,
				false);
	}
	auto KillerPlayerState =
		DeathReport.HasKillerPlayerState()
			? (AFortPlayerStateAthena*)
				  DeathReport.KillerPlayerState
			: nullptr;
	auto KillerPawn =
		DeathReport.HasKillerPawn()
			? (AFortPlayerPawnAthena*)
				  DeathReport.KillerPawn
			: nullptr;
	if (!IsUsableDeathObject(KillerPlayerState))
		KillerPlayerState = nullptr;
	if (!IsUsableDeathObject(KillerPawn))
		KillerPawn = nullptr;
	auto KillerPlayerController =
		(KillerPlayerState &&
		 KillerPlayerState->HasOwner() &&
		 IsUsableDeathObject(KillerPlayerState->Owner))
			? KillerPlayerState->Owner
				  ->Cast<AFortPlayerControllerAthena>()
			: nullptr;
	// Capture legitimate killer ownership before the death pickup exists.
	// Carmine's forced auto-pickup must never turn a just-collected row into
	// a "preexisting" row that survives the post-native cleanup.
	const auto KillerCarmineArtifactsBefore =
		!bIsAshtonMatch
			? SnapshotCarmineGauntletArtifacts(
				  KillerPlayerController)
			: std::vector<FGuid>{};
	auto ManualCarmineDeadPawn =
		IsUsableDeathObject(PlayerController->Pawn)
			? PlayerController->Pawn
				->Cast<AFortPlayerPawnAthena>()
			: nullptr;
	FVector ManualCarmineDropLocation =
		ManualCarmineDeadPawn
			? ManualCarmineDeadPawn->K2_GetActorLocation()
			: FVector();
	ManualCarmineDropLocation.Z += 50.f;
	bool bManualCarminePickupSpawned =
		!bIsAshtonMatch &&
		DropManualCarmineGauntletOnDeath(
			PlayerController,
			IsUsableDeathObject(KillerPawn)
				? KillerPawn
				: ManualCarmineDeadPawn,
			ManualCarmineDropLocation);
	bool bPreserveStormElimination = false;

	auto CallNativePawnDied =
		[&]()
		{
			auto DeathPawn =
				IsUsableDeathObject(PlayerController->Pawn)
					? PlayerController->Pawn
						->Cast<AFortPlayerPawnAthena>()
					: nullptr;
			const bool bCanSuppressNativeDrops =
				bKeepInventoryApplies &&
				DeathPawn &&
				DeathPawn->HasbShouldDropItemsOnDeath();
			const bool bOriginalShouldDrop =
				bCanSuppressNativeDrops
					? DeathPawn->bShouldDropItemsOnDeath
					: false;
			std::vector<FGuid> RetainedItemGuids;
			if (bKeepInventoryApplies &&
				PlayerController->WorldInventory)
			{
				auto& Entries =
					PlayerController->WorldInventory
						->Inventory.ReplicatedEntries;
				RetainedItemGuids.reserve(Entries.Num());
				for (int32 Index = 0;
					Index < Entries.Num();
					++Index)
				{
					auto& Entry = Entries.Get(
						Index, FFortItemEntry::Size());
					if (bIsGetawayMatch &&
						IsGetawayJewelDefinition(
							Entry.ItemDefinition))
					{
						continue;
					}
					if (!CanDropInventoryItem(
							Entry.ItemDefinition))
					{
						continue;
					}
					RetainedItemGuids.push_back(
						Entry.ItemGuid);
				}
			}

			// The Getaway Jewel has already been spawned and removed by the
			// explicit exception below. Prevent native death mutators from
			// subsequently dropping ordinary retained rows. Other native 10.40
			// LTMs keep ownership of tier/token/objective inventory.
			if (bCanSuppressNativeDrops)
				DeathPawn->bShouldDropItemsOnDeath = false;
			AFortInventory::
				BeginNativeDeathInventoryRetention(
					PlayerController,
					RetainedItemGuids);

			const bool bRestoreNativeCheatAfterDeath =
				bPreserveStormElimination &&
				IsUsableDeathObject(GameState) &&
				GameState->HasbCheatRespawnEnabled() &&
				GameState->bCheatRespawnEnabled;
			if (bRestoreNativeCheatAfterDeath)
				GameState->bCheatRespawnEnabled = false;

			ClientOnPawnDiedOG(
				PlayerController, DeathReport);

			if (bRestoreNativeCheatAfterDeath &&
				IsUsableDeathObject(GameState) &&
				FConfiguration::bForceRespawns)
			{
				GameState->bCheatRespawnEnabled = true;
				GameState->ForceNetUpdate();
			}

			AFortInventory::
				EndNativeDeathInventoryRetention(
					PlayerController);
			if (bCanSuppressNativeDrops &&
				IsUsableDeathObject(DeathPawn))
			{
				bool OriginalShouldDrop =
					bOriginalShouldDrop;
				DeathPawn->bShouldDropItemsOnDeath =
					OriginalShouldDrop;
			}
		};

	// The exact 10.40 LTMs ship configured death/inventory mutators. Let those
	// decide which normal rows survive or drop so our generic elimination path
	// cannot duplicate or pre-empt Deep Fried/Arsenal progression. Getaway's
	// Jewel exception below remains authoritative and removes only the carried
	// objective representation before native death processing.
	// The Jewel is the transferable Getaway objective, not ordinary retained
	// inventory. It must leave the eliminated carrier even when Keep Inventory
	// is enabled (including Late Game). In that configuration no other entry is
	// selected by this exception.
	if (PlayerController->WorldInventory &&
		PlayerController->Pawn &&
		(bDropNormalInventory || bIsGetawayMatch))
	{
		int32 GetawayJewelIndex = -1;
		if (bIsGetawayMatch)
		{
			for (int32 Index = 0;
				Index < PlayerController->WorldInventory
					->Inventory.ReplicatedEntries.Num();
				++Index)
			{
				auto& Candidate =
					PlayerController->WorldInventory
						->Inventory.ReplicatedEntries.Get(
							Index, FFortItemEntry::Size());
				if (IsGetawayJewelDefinition(
						Candidate.ItemDefinition))
				{
					GetawayJewelIndex = Index;
					break;
				}
			}
		}

		std::vector<FGuid> DroppedItemGuids;
		for (int i = 0; i < PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num(); i++)
		{
			auto& entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());
			const bool bIsAnyGetawayJewel =
				bIsGetawayMatch &&
					entry.ItemDefinition
					? IsGetawayJewelDefinition(
						entry.ItemDefinition)
					: false;
			const bool bIsGetawayJewel =
				i == GetawayJewelIndex;
			const bool bShouldDropEntry =
				bIsGetawayJewel ||
				(bDropNormalInventory &&
					!bIsAnyGetawayJewel &&
					!IsCarmineGauntletArtifact(
						entry.ItemDefinition) &&
					CanDropInventoryItem(
						entry.ItemDefinition));

			if (bShouldDropEntry)
			{
				FVector DropLocation =
					PlayerController->Pawn->K2_GetActorLocation();
				if (bIsGetawayJewel)
					DropLocation.Z += 50.f;

				// The Jewel's B_Pickups_Bling class is a pickup *effect*, not
				// an AFortPickupAthena subclass. The normal pickup reads that
				// effect from Athena_Bling_Pack, so it must not be supplied as
				// SpawnPickup's actor-class override.
				auto Pickup = AFortInventory::SpawnPickup(
					DropLocation,
					entry,
					EFortPickupSourceTypeFlag::GetPlayer(),
					EFortPickupSpawnSource::GetPlayerElimination(),
					PlayerController->MyFortPawn,
					-1,
					true,
					true,
					true,
					nullptr);
				if (!Pickup && bIsGetawayJewel)
				{
					// Retry slightly above the death location if collision at
					// the pawn prevented the first spawn. Keep the inventory
					// row unless one of the attempts succeeds.
					DropLocation.Z += 50.f;
					Pickup = AFortInventory::SpawnPickup(
						DropLocation,
						entry,
						EFortPickupSourceTypeFlag::GetPlayer(),
						EFortPickupSpawnSource::
							GetPlayerElimination(),
						PlayerController->MyFortPawn,
						-1,
						true,
						true,
						true,
						nullptr);
				}
				if (Pickup)
				{
					DroppedItemGuids.push_back(
						entry.ItemGuid);
				}

				if (bIsGetawayJewel)
				{
					SDK::DbgLog(
						"[Heist] Jewel death drop "
						"controller=%p def=%s pickup=%p "
						"keepInventory=%d lateGame=%d\n",
						(void*)PlayerController,
						entry.ItemDefinition
							? entry.ItemDefinition->Name
								.ToString().c_str()
							: "null",
						(void*)Pickup,
						(int)FConfiguration::bKeepInventory,
						(int)FConfiguration::bLateGame);
				}
			}
		}

		// Core's DropAllItems removes every entry after spawning its pickup.
		// Magnesium previously spawned the world actors but left the entries in
		// the dead player's inventory, which was especially visible with the
		// persistent Infinity Gauntlet gadget.
		for (const FGuid& ItemGuid : DroppedItemGuids)
			PlayerController->WorldInventory->Remove(ItemGuid);
	}

	// Exclusive gadgets must never survive death, including keep-inventory
	// modes. Their native transformation otherwise carries into the next pawn.
	if (!bWasAshtonLeader)
		PurgeExclusiveGadgets(PlayerController);

	// PlayerAI victim diagnostic: kill credit for the killer flows from this
	// report - a missing killer here explains "no credit for killing AI".
	if (MagnesiumPlayerAIIntegration::IsPlayerAIController(PlayerController))
		AIDebugLogger::Log("Elimination", "AI death report: killerPS %d, killerPawn %d, killerCtrl %d",
			KillerPlayerState ? 1 : 0, KillerPawn ? 1 : 0,
			(KillerPawn && KillerPawn->Controller) ? 1 : 0);
	FGameplayTagContainer EmptyDeathTags{};
	auto& DeathTags = DeathReport.HasTags() ? DeathReport.Tags : EmptyDeathTags;
	const bool bConfirmedStormDeath =
		HasConfirmedStormDeathEvidence(
			DeathTags, DeathReport);
	bool bRespawnAllowed = IsRespawningAllowedForDeath(
		GameMode, GameState, PlayerController, PlayerState);

	if (VersionInfo.FortniteVersion > 1.8 || VersionInfo.EngineVersion >= 4.19)
	{
		if (PlayerState->HasPawnDeathLocation())
			PlayerState->PawnDeathLocation = PlayerController->Pawn ? PlayerController->Pawn->K2_GetActorLocation() : FVector();

		if (PlayerState->HasDeathInfo())
		{
			memset(&PlayerState->DeathInfo, 0, FDeathInfo::Size());
			PlayerState->DeathInfo.bDBNO = PlayerController->Pawn ? PlayerController->Pawn->IsDBNO() : false;
			if (FDeathInfo::HasKiller())
				PlayerState->DeathInfo.Killer = KillerPlayerState;
			if (FDeathInfo::HasDeathLocation())
				PlayerState->DeathInfo.DeathLocation = PlayerState->HasPawnDeathLocation() ? PlayerState->PawnDeathLocation : (PlayerController->Pawn ? PlayerController->Pawn->K2_GetActorLocation() : FVector());
			if (FDeathInfo::HasDeathTags())
			{
				// TArray assignment in the SDK is a shallow header copy. On
				// 15.30 this source belongs to the dying pawn, while DeathInfo
				// persists on PlayerState after that pawn is destroyed. Keeping
				// the copied headers therefore leaves DeathInfo pointing at
				// freed tag buffers. The fields were zero-initialized above;
				// leave them empty on 15.30 instead of publishing borrowed
				// storage.
				if (VersionInfo.FortniteVersion != 15.30)
					PlayerState->DeathInfo.DeathTags =
						/*DeathReport.Tags*/ PlayerController->Pawn
							? *(FGameplayTagContainer*)(
								__int64(&PlayerController->Pawn
									->MoveSoundStimulusBroadcastInterval) +
								(VersionInfo.FortniteVersion >= 11 &&
										VersionInfo.FortniteVersion < 18
									? 0x18
									: 0x10))
							: FGameplayTagContainer();
			}
			if (FDeathInfo::HasDeathClassSlot())
				PlayerState->DeathInfo.DeathClassSlot = -1;
			PlayerState->DeathInfo.DeathCause = ToDeathCause(PlayerController->Pawn, DeathTags, PlayerState->DeathInfo.bDBNO);
			//PlayerState->DeathInfo.Downer = KillerPlayerState;
			if (FDeathInfo::HasFinisherOrDowner())
				PlayerState->DeathInfo.FinisherOrDowner = KillerPlayerState ? KillerPlayerState : PlayerState;
			if (FDeathInfo::HasFinisherOrDownerTags())
			{
				if (VersionInfo.FortniteVersion != 15.30)
					PlayerState->DeathInfo.FinisherOrDownerTags =
						KillerPawn
							? KillerPawn->GameplayTags
							: (PlayerController->Pawn
								? PlayerController->Pawn
									->GameplayTags
								: FGameplayTagContainer{});
			}
			if (FDeathInfo::HasVictimTags())
			{
				if (VersionInfo.FortniteVersion != 15.30)
					PlayerState->DeathInfo.VictimTags =
						PlayerController->Pawn
							? PlayerController->Pawn
								->GameplayTags
							: FGameplayTagContainer{};
			}
			if (FDeathInfo::HasDistance())
				PlayerState->DeathInfo.Distance = PlayerController->Pawn ? (PlayerState->DeathInfo.DeathCause != /*EDeathCause::FallDamage*/ 1 ? (KillerPawn ? KillerPawn->GetDistanceTo(PlayerController->Pawn) : 0) : (PlayerController->MyFortPawn->HasLastFallDistance() ? PlayerController->MyFortPawn->LastFallDistance : 0)) : 0;
			if (FDeathInfo::HasbInitialized())
				PlayerState->DeathInfo.bInitialized = true;
			PlayerState->OnRep_DeathInfo();
		}

		// Wax/Bounty's native death callback decrements the authored life,
		// awards/drops tokens, updates the replicated leaderboard and resolves
		// its mutator-controlled win condition. It must run after DeathInfo is
		// complete but before respawn eligibility is sampled.
		FFortAthenaNativeLTMCompatibility::
			HandleWaxElimination(
				PlayerState,
				(AFortPlayerPawnAthena*)
					PlayerController->Pawn);

		bPreserveStormElimination =
			FConfiguration::bForceRespawns &&
			!FConfiguration::PermanentRespawn &&
			bConfirmedStormDeath &&
			PlayerState->HasDeathInfo() &&
			FDeathInfo::HasbInitialized() &&
			PlayerState->DeathInfo.bInitialized &&
			PlayerState->DeathInfo.DeathCause == 0;
		if (bPreserveStormElimination)
		{
			GStormRespawnBlockedControllers[
				PlayerController] =
				TWeakObjectPtr<AFortGameStateAthena>(
					GameState);
			InvalidateRespawnHandshake(PlayerState);
		}
		else
		{
			// A later non-storm elimination starts a fresh respawn decision for
			// this controller. Do not let a prior life or recycled pointer keep
			// the new handshake blocked.
			GStormRespawnBlockedControllers.erase(
				PlayerController);
		}
		bRespawnAllowed =
			IsRespawningAllowedForDeath(
				GameMode, GameState, PlayerController,
				PlayerState,
				bConfirmedStormDeath);

		// RespawnType 2 is "InfiniteRespawnExceptStorm". The global override is
		// intentionally broad for normal deaths, so preserve the GUI's separate
		// Storm Respawns switch after ToDeathCause has populated reliable death
		// data (OutsideSafeZone is the stable zero enum value).
		if (PlayerController->Pawn &&
			KillerPlayerState && KillerPawn &&
			KillerPawn->Controller &&
			KillerPawn->Controller != PlayerController &&
			FFortAthenaNativeLTMCompatibility::
				TryClaimArsenalElimination(
					PlayerState,
					(AFortPlayerPawnAthena*)
						PlayerController->Pawn))
		{
			if (KillerPlayerState->HasKillScore())
				KillerPlayerState->KillScore++;
			else
				KillerPlayerState->Kills++;
			KillerPlayerState->OnRep_Kills();
			if (KillerPlayerState->HasTeamKillScore())
			{
				KillerPlayerState->TeamKillScore++;
				KillerPlayerState->OnRep_TeamKillScore();
			}

			struct Test { AFortPlayerStateAthena* ps; uint8_t p[0x8]; };

			Test t{ PlayerState };
			KillerPlayerState->ClientReportKill(t);
			if (KillerPlayerState->HasTeamKillScore())
				KillerPlayerState->ClientReportTeamKill(KillerPlayerState->TeamKillScore);

			for (auto& Damager : PlayerController->Pawn->Damagers)
			{
				if (Damager.DamageCauser != KillerPlayerController && Damager.DamageCauser->IsA<AFortPlayerControllerAthena>())
				{
					FGameplayTagContainer TargetTags{};
					auto DamagerController = (AFortPlayerControllerAthena*)Damager.DamageCauser;

					auto Interface = (IGameplayTagAssetInterface*)PlayerController->Pawn->GetInterface(IGameplayTagAssetInterface::StaticClass());
					if (Interface)
					{
						auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface->Vft[0x2];
						GetOwnedGameplayTags(Interface, &TargetTags);
						//Interface->GetOwnedGameplayTags(&TargetTags);
					}

					DamagerController->GetQuestManager(1)->SendStatEvent(DamagerController, EFortQuestObjectiveStatEvent::GetKillContribution(), 1, false, PlayerController->Pawn, TargetTags);

					TargetTags.GameplayTags.Free();
					TargetTags.ParentTags.Free();
				}
			}

			FGameplayTagContainer TargetTags{};

			auto Interface = (IGameplayTagAssetInterface*)PlayerController->Pawn->GetInterface(IGameplayTagAssetInterface::StaticClass());
			if (Interface)
			{
				auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface->Vft[0x2];
				GetOwnedGameplayTags(Interface, &TargetTags);
				//Interface->GetOwnedGameplayTags(&TargetTags);
			}

				KillerPlayerController->GetQuestManager(1)->SendStatEvent(KillerPlayerController, EFortQuestObjectiveStatEvent::GetKill(), 1, false, PlayerController->Pawn, TargetTags);

				TargetTags.GameplayTags.Free();
				TargetTags.ParentTags.Free();

				// AwardAtElim is authored against the post-increment KillScore.
				// This block is reached only for a full credited elimination; DBNO
				// notifications returned before inventory/kill processing above.
				FFortAthenaNativeLTMCompatibility::
					HandleArsenalElimination(
						KillerPlayerController,
						KillerPlayerState,
						PlayerController,
						PlayerState,
						(AFortPlayerPawnAthena*)
							PlayerController->Pawn);
			}

		if (IsUsableDeathObject(KillerPlayerState) &&
			(GUI::IsArenaPlaylist() || GUI::IsTournamentPlaylist()) &&
			VersionInfo.FortniteVersion < 20.40) // crashes on 20.40, test other versions
		{
			KillerPlayerState->ClientReportTournamentStatUpdate();
		}

		if (!bRespawnAllowed && (PlayerController->Pawn ? !PlayerController->Pawn->IsDBNO() : true) && PlayerState->HasPlace())
		{
			PlayerState->Place = GameState->PlayersLeft;
			PlayerState->OnRep_Place();

			AFortWeapon* DamageCauser = ResolveDeathReportWeapon(DeathReport);

			auto DeadPawn = (AFortPlayerPawnAthena*)PlayerController->Pawn;
			auto DeadPlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;
			auto DeadInventory = PlayerController->WorldInventory;

			UNetDriver* Driver = static_cast<UNetDriver*>(UWorld::GetWorld()->NetDriver);

			auto FindConnectionByPlayerState = [&](AFortPlayerStateAthena* PS) -> UNetConnection*
				{
					if (!PS || !Driver)
						return nullptr;

					for (int i = 0; i < Driver->ClientConnections.Num(); i++)
					{
						auto Conn = Driver->ClientConnections[i];
						if (!Conn || !Conn->PlayerController)
							continue;

						if (Conn->PlayerController->PlayerState == PS)
							return Conn;
					}

					return nullptr;
				};

			float KillDistanceCm = KillerPawn && DeadPawn ? KillerPawn->GetDistanceTo(DeadPawn) : 0.f;
			int KillDistanceMeters = static_cast<int>(KillDistanceCm / 100.f);

			auto KillerConn = FindConnectionByPlayerState(KillerPlayerState);
			auto DeadConn = FindConnectionByPlayerState(DeadPlayerState);

			std::string KillerName = GUI::GetPlayerName(KillerPlayerState, KillerConn);
			std::string DeadName = GUI::GetPlayerName(DeadPlayerState, DeadConn);

			std::string Distance = std::to_string(KillDistanceMeters);

			FConfiguration::ElimKillerName = KillerName;
			FConfiguration::ElimEliminatedName = DeadName;
			FConfiguration::ElimDistance = Distance;
			FConfiguration::ElimStatusMessage = ":3";

			auto KillerWeapon = GetWeaponDataSafe(DamageCauser);

			FConfiguration::ElimWeaponName = "Unknown";

			if (VersionInfo.FortniteVersion < 16.00 && !FConfiguration::bUseWinLines)
			{
				DamageCauser = nullptr;
				KillerWeapon = nullptr;
			}
			else
			{
				std::string WeaponName;
				if (TryGetItemDefinitionDisplayName(KillerWeapon, WeaponName))
					FConfiguration::ElimWeaponName = WeaponName;
			}

			// Every FN19+ build exposes the same crown component, but not every
			// build reaches endgame through our RemoveFromAlivePlayers finder.
			// Snapshot and prepare before either native death path can create
			// the victory view model.
			if (VersionInfo.FortniteVersion >= 19.0 &&
				VersionInfo.FortniteVersion < 26.0 &&
				FConfiguration::bCrownSlomo)
			{
				SnapshotVictoryCrownOwnershipBeforeNativeDeath();
				auto ImpendingCrownWinner =
					ResolveImpendingVictoryWinnerBeforeNativeDeath(
						GameMode, GameState, PlayerController,
						KillerPlayerState != PlayerState
							? KillerPlayerController
							: nullptr);
				PrepareVictoryCrownRoyalRoyaleForWinningTeam(
					ImpendingCrownWinner);
			}

			if (RemoveFromAlivePlayers_)
			{
				auto DamageCauserWeaponData = GetWeaponDataSafe(DamageCauser);

				// The 2.50 client must begin its native pawn-death lifecycle
				// before removing the final victim transitions the match into
				// post-game. Otherwise its legacy elimination drone/beam can
				// remain attached to a death animation that never completes.
				if (VersionInfo.FortniteVersion == 2.50)
				{
					if (bPlayerAIVictim ||
						bSpawnedCommandBotVictim)
					{
						InvalidateRespawnHandshake(
							PlayerState);
					}
					if (bPlayerAIVictim)
					{
						PlayerAIManager::HandleControllerDeath(
							PlayerController,
							ManagedBotEliminatedPawn);
					}
					CallNativePawnDied();
					bCalledNativeDeathEarly = true;
				}

				((void (*)(AFortGameMode*, AFortPlayerControllerAthena*, AFortPlayerStateAthena*, AFortPlayerPawnAthena*, UFortItemDefinition*, uint8, char))RemoveFromAlivePlayers_)(GameMode, PlayerController, KillerPlayerState == PlayerState ? nullptr : KillerPlayerState, KillerPawn, DamageCauserWeaponData, PlayerState->HasDeathInfo() ? PlayerState->DeathInfo.DeathCause : 0, 0);

				if (VersionInfo.FortniteVersion == 2.50 &&
					IsTrackedSpawnedBotController(PlayerController))
				{
					ScheduleSpawnedBotCleanup(
						PlayerController, DeadPawn, DeadPlayerState,
						DeadInventory);
				}

				// Native can choose a surviving winner when the final opponent
				// dies to storm, fall damage, or another environmental cause.
				// Resolve that result here so crown state is ready before any
				// Magnesium victory notification below.
				if (VersionInfo.FortniteVersion >= 19.0 &&
					VersionInfo.FortniteVersion < 26.0 &&
					FConfiguration::bCrownSlomo &&
					GameMode->MatchState == FName(L"WaitingPostMatch"))
				{
					auto NativeCrownWinner =
						ResolveNativeVictoryWinner(
							GameMode, GameState,
							KillerPlayerState, PlayerController);
					ApplyVictoryCrownWinStateToWinningTeam(
						NativeCrownWinner);
				}
			}

			if (VersionInfo.FortniteVersion >= 15)
			{
				//static auto SpectatingName = FName(L"Spectating");
				//PlayerController->StateName = SpectatingName;
				//PlayerController->ClientGotoState(SpectatingName);
				if (PlayerController->Pawn && PlayerController->Pawn->CharacterMovement)
					PlayerController->Pawn->CharacterMovement->ProcessEvent(PlayerController->Pawn->CharacterMovement->GetFunction("DisableMovement"), nullptr);
			}

			if (FConfiguration::bIsCustomMap && FConfiguration::AutoEndGame)
			{
				auto CustomWinnerController =
					KillerPlayerState != PlayerState &&
					IsHumanVictoryController(KillerPlayerController)
						? KillerPlayerController
						: nullptr;
				if (!CustomWinnerController &&
					GameMode->MatchState == FName(L"WaitingPostMatch"))
				{
					CustomWinnerController =
						ResolveNativeVictoryWinner(
							GameMode, GameState,
							KillerPlayerState, PlayerController);
				}

				auto CustomWinnerState =
					CustomWinnerController
						? CustomWinnerController->PlayerState
							->Cast<AFortPlayerStateAthena>()
						: nullptr;
				if (!CustomWinnerState)
				{
					SDK::DbgLog(
						"[Elimination] custom AutoEndGame skipped: no verified human winner victim=%p killer=%p\n",
						(void*)PlayerController,
						(void*)KillerPlayerController);
				}
				else
				{
					auto CustomWinnerPawn =
						CustomWinnerController ==
							KillerPlayerController &&
							IsUsableDeathObject(KillerPawn)
								? KillerPawn
								: CustomWinnerController->MyFortPawn;
					auto CustomWinnerWeapon =
						CustomWinnerController ==
							KillerPlayerController
								? KillerWeapon
								: nullptr;

					GameState->WinningTeam =
						CustomWinnerState->TeamIndex;
					GameState->OnRep_WinningTeam();

					if (GameState->HasWinningPlayerState())
					{
						GameState->WinningPlayerState =
							CustomWinnerState;
						GameState->OnRep_WinningPlayerState();
					}
					GameState->ForceNetUpdate();

					GUI::gsStatus = Ended;

					PrepareVictoryCrownRoyalRoyaleForWinningTeam(
						CustomWinnerController);
					ApplyVictoryCrownWinStateToWinningTeam(
						CustomWinnerController);

					SendOrDeferVictoryNotifications(
						CustomWinnerController,
						CustomWinnerPawn,
						CustomWinnerWeapon,
						PlayerState->DeathInfo.DeathCause,
						true, true, true);
				}
			}

			if (KillerPlayerState &&
				KillerPlayerState != PlayerState &&
				IsHumanVictoryController(KillerPlayerController) &&
				KillerPlayerState->Place == 1)
			{
				/*if (PlayerState->Place == 1)
				{
					KillerPlayerState = PlayerState;
					KillerPawn = (AFortPlayerPawnAthena*)PlayerController->Pawn;
				}*/

				GUI::gsStatus = Ended;

				GameState->WinningTeam = KillerPlayerState->TeamIndex;
				GameState->OnRep_WinningTeam();

				if (GameState->HasWinningPlayerState())
				{
					GameState->WinningPlayerState = KillerPlayerState;
					GameState->OnRep_WinningPlayerState();
				}
				GameState->ForceNetUpdate();

				PrepareVictoryCrownRoyalRoyaleForWinningTeam(
					KillerPlayerController);
				ApplyVictoryCrownWinStateToWinningTeam(
					KillerPlayerController);

				if (FConfiguration::bUseWinLines)
				{
					if (VersionInfo.FortniteVersion >= 16.00)
					{
						SendOrDeferVictoryNotifications(
							KillerPlayerController,
							KillerPawn, KillerWeapon,
							PlayerState->DeathInfo.DeathCause,
							true, true, true);
					}
				}

				if (FConfiguration::bCancelVelocityOnWin)
				{
					if (KillerPawn && KillerPawn->CharacterMovement)
					{
						KillerPawn->CharacterMovement->Velocity = FVector{};
					}
				}
			}
		}

		if ((FConfiguration::bSiphon && FConfiguration::SiphonAmount > 0) &&
			PlayerController->Pawn && KillerPlayerState &&
			KillerPlayerState->AbilitySystemComponent &&
			KillerPlayerController &&
			KillerPlayerController->WorldInventory &&
			KillerPawn &&
			KillerPawn->Controller != PlayerController)
		{
			auto Health = KillerPawn->GetHealth();
			auto Shield = KillerPawn->GetShield();
			float RemainingSiphon =
				static_cast<float>(FConfiguration::SiphonAmount);

			const float MaxHealth = KillerPawn->GetMaxHealth();
			if (FPlatformMath::IsFinite(MaxHealth) && MaxHealth > 0.f)
			{
				if (!FPlatformMath::IsFinite(Health) || Health < 0.f)
					Health = 0.f;
				if (Health > MaxHealth)
					Health = MaxHealth;

				const float HealthGain =
					(std::min)(RemainingSiphon, MaxHealth - Health);
				Health += HealthGain;
				RemainingSiphon -= HealthGain;
			}
			else
			{
				// Do not invent a capacity when reflection is unavailable.
				RemainingSiphon = 0.f;
			}

			Shield = ClampShieldToInitializedPawnCapacity(
				KillerPawn, Shield);
			const float MaxShield = KillerPawn->GetMaxShield();
			if (RemainingSiphon > 0.f &&
				FPlatformMath::IsFinite(MaxShield) &&
				MaxShield > Shield)
			{
				Shield += (std::min)(
					RemainingSiphon, MaxShield - Shield);
			}

			KillerPawn->SetHealth(Health);
			KillerPawn->SetShield(Shield);

			const auto WoodItemData =
				UFortKismetLibrary::K2_GetResourceItemDefinition(
					EFortResourceType::Wood);
			const auto StoneItemData =
				UFortKismetLibrary::K2_GetResourceItemDefinition(
					EFortResourceType::Stone);
			const auto MetalItemData =
				UFortKismetLibrary::K2_GetResourceItemDefinition(
					EFortResourceType::Metal);

			auto GrantSiphonMaterial =
				[&](const UFortItemDefinition* Definition)
				{
					if (Definition)
					{
						KillerPlayerController->WorldInventory
							->GiveItemToSingleStack(
								Definition,
								FConfiguration::SiphonAmount,
								false);
					}
				};
			GrantSiphonMaterial(WoodItemData);
			GrantSiphonMaterial(StoneItemData);
			GrantSiphonMaterial(MetalItemData);

			switch (FConfiguration::SiphonAnimType)
			{
			case 0: // Default
			{
				auto Handle = KillerPlayerState->AbilitySystemComponent->MakeEffectContext();
				FGameplayTag Tag;
				static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
				Tag.TagName = Cue;
				auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
				memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
				KillerPlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
				KillerPlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
				free(PredictionKey);
				break;
			}
			case 1: // Slurp
			{
				if (UAbilitySystemComponent* AbilitySystemComponent = KillerPlayerState->AbilitySystemComponent)
				{
					static auto GameplayEffect = FindObject<UClass>(L"/Game/Athena/Items/Gameplay/SilkyBingo/GE_Athena_EnvSlurp_Grant_SilkyBingo.GE_Athena_EnvSlurp_Grant_SilkyBingo_C");
					if (GameplayEffect)
					{
						FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();
						Context.Instigator = KillerPlayerController;
						Context.Causer = KillerPawn;
						Context.AddSourceObject(KillerPawn);

						float HealthBeforeEffect = KillerPawn->GetHealth();
						float ShieldBeforeEffect = KillerPawn->GetShield();
						AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(GameplayEffect, 1.0f, Context);
						KillerPawn->SetHealth(HealthBeforeEffect);
						KillerPawn->SetShield(ShieldBeforeEffect);

						float AddedBonus = 5.f;
						float NewHealth = KillerPawn->GetHealth() - AddedBonus;
						float NewShield = KillerPawn->GetShield() - AddedBonus;
						// This effect is cosmetic here; the siphon amount was
						// already applied above. Some versions do not grant the
						// expected +5 before this compensation runs, so never
						// let the compensation itself damage or kill the player.
						if (!FPlatformMath::IsFinite(NewHealth) ||
							NewHealth < HealthBeforeEffect)
						{
							NewHealth = HealthBeforeEffect;
						}
						if (!FPlatformMath::IsFinite(NewShield) ||
							NewShield < ShieldBeforeEffect)
						{
							NewShield = ShieldBeforeEffect;
						}
						KillerPawn->SetHealth(NewHealth);
						KillerPawn->SetShield(NewShield);
					}
				}
				break;
			}
			case 2: // Bandage Bazooka
			{
				if (UAbilitySystemComponent* AbilitySystemComponent = KillerPlayerState->AbilitySystemComponent)
				{
					static auto GameplayEffect = FindObject<UClass>(L"/Game/Athena/Items/Gameplay/Lotus/Mustache/GE_Lotus_Mustache_Heal_Burst.GE_Lotus_Mustache_Heal_Burst_C");

					if (GameplayEffect)
					{
						FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

						Context.Instigator = KillerPlayerController;
						Context.Causer = KillerPawn;
						Context.AddSourceObject(KillerPawn);

						AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(GameplayEffect, 1.0f, Context);
					}
				}

				break;
			}
			case 3: // Orange Paint
			{
				if (UAbilitySystemComponent* AbilitySystemComponent = KillerPlayerState->AbilitySystemComponent)
				{
					static auto GameplayEffect = FindObject<UClass>(L"/Game/Athena/Items/Weapons/Prototype/Papaya/GE_Player_PaintDecal_Red.GE_Player_PaintDecal_Red_C");

					if (GameplayEffect)
					{
						FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

						Context.Instigator = KillerPlayerController;
						Context.Causer = KillerPawn;
						Context.AddSourceObject(KillerPawn);

						AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(GameplayEffect, 1.0f, Context);
					}
				}

				break;
			}
			case 4: // Purple Paint
			{
				if (UAbilitySystemComponent* AbilitySystemComponent = KillerPlayerState->AbilitySystemComponent)
				{
					static auto GameplayEffect = FindObject<UClass>(L"/Game/Athena/Items/Weapons/Prototype/Papaya/GE_Player_PaintDecal_Blue.GE_Player_PaintDecal_Blue_C");

					if (GameplayEffect)
					{
						FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

						Context.Instigator = KillerPlayerController;
						Context.Causer = KillerPawn;
						Context.AddSourceObject(KillerPawn);

						AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(GameplayEffect, 1.0f, Context);
					}
				}

				break;
			}
			case 5:
			{
				auto Handle = KillerPlayerState->AbilitySystemComponent->MakeEffectContext();
				FGameplayTag Tag;
				static auto Cue = FName(L"GameplayCue.Athena.Health.HealUsed");
				Tag.TagName = Cue;
				auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
				memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
				KillerPlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
				KillerPlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
				free(PredictionKey);
				break;
			}
			case 6:
			{
				if (UAbilitySystemComponent* AbilitySystemComponent = KillerPlayerState->AbilitySystemComponent)
				{
					static auto GameplayEffect = FindObject<UClass>(L"/FlipperGameplay/Items/HealSpray/GE_Athena_HealSpray_Heal.GE_Athena_HealSpray_Heal_C");

					if (GameplayEffect)
					{
						FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

						Context.Instigator = KillerPlayerController;
						Context.Causer = KillerPawn;
						Context.AddSourceObject(KillerPawn);

						AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(GameplayEffect, 1.0f, Context);
					}
				}

				break;
			}
			case 7:
			{
				if (UAbilitySystemComponent* AbilitySystemComponent = KillerPlayerState->AbilitySystemComponent)
				{
					static auto GameplayEffect = FindObject<UClass>(L"/Game/Athena/Items/Gameplay/Wumba/GE_WumbaUsed.GE_WumbaUsed_C");

					if (GameplayEffect)
					{
						FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

						Context.Instigator = KillerPlayerController;
						Context.Causer = KillerPawn;
						Context.AddSourceObject(KillerPawn);

						AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(GameplayEffect, 1.0f, Context);
					}
				}

				break;
			}
			}
		}

		if (GUI::IsArenaPlaylist())
		{
			int PlayerCount = GameMode->AlivePlayers.Num();
			auto AwardRequirement = PlayerCount == 50 || PlayerCount == 35 || PlayerCount == 30 || PlayerCount == 25 || PlayerCount == 20 || PlayerCount == 15 || PlayerCount == 10 || PlayerCount == 5 || PlayerCount == 3 || PlayerCount == 2 || PlayerCount == 1;

			int Points = 10;

			if (PlayerCount == 50)
				Points = 10;
			else if (PlayerCount == 35)
				Points = 10;
			else if (PlayerCount == 30)
				Points = 10;
			else if (PlayerCount == 25)
				Points = 15;
			else if (PlayerCount == 20)
				Points = 10;
			else if (PlayerCount == 15)
				Points = 10;
			else if (PlayerCount == 10)
				Points = 15;
			else if (PlayerCount == 5)
				Points = 15;
			else if (PlayerCount == 3)
				Points = 10;
			else if (PlayerCount == 2)
				Points = 25;
			else if (PlayerCount == 1)
				Points = 50;

			for (auto& Player : GameMode->AlivePlayers)
			{
				auto Controller = (AFortPlayerControllerAthena*)Player;

				if (AwardRequirement && !Controller->IsInRespawnCountdown())
					Controller->ClientReportTournamentPlacementPointsScored(PlayerCount, Points);
			}
		}

		if (GUI::IsTournamentPlaylist())
		{
			int PlayerCount = GameMode->AlivePlayers.Num();
			auto AwardRequirement = PlayerCount == 50 || PlayerCount == 35 || PlayerCount == 30 || PlayerCount == 25 || PlayerCount == 20 || PlayerCount == 15 || PlayerCount == 10 || PlayerCount == 5 || PlayerCount == 3 || PlayerCount == 2 || PlayerCount == 1;

			int Points = 1;

			if (PlayerCount == 75)
				Points = 1;
			else if (PlayerCount == 50)
				Points = 1;
			else if (PlayerCount == 40)
				Points = 1;
			else if (PlayerCount == 30)
				Points = 2;
			else if (PlayerCount == 20)
				Points = 2;
			else if (PlayerCount == 15)
				Points = 2;
			else if (PlayerCount == 10)
				Points = 3;
			else if (PlayerCount == 9)
				Points = 1;
			else if (PlayerCount == 8)
				Points = 1;
			else if (PlayerCount == 7)
				Points = 1;
			else if (PlayerCount == 6)
				Points = 1;
			else if (PlayerCount == 5)
				Points = 2;
			else if (PlayerCount == 4)
				Points = 1;
			else if (PlayerCount == 3)
				Points = 3;
			else if (PlayerCount == 2)
				Points = 4;
			else if (PlayerCount == 1)
				Points = 7;

			for (auto& Player : GameMode->AlivePlayers)
			{
				auto Controller = (AFortPlayerControllerAthena*)Player;

				if (AwardRequirement && !Controller->IsInRespawnCountdown())
					Controller->ClientReportTournamentPlacementPointsScored(PlayerCount, Points);
			}
		}
	}

	// Resolve the first target before native ClientOnPawnDied can tear down the
	// victim's pawn state. Old builds use the delayed SpectateOnDeath timer;
	// modern builds are repaired immediately after native death initialization.
	const bool bIsHumanVictim =
		!(PlayerState->HasbIsABot() && PlayerState->bIsABot) &&
		!MagnesiumPlayerAIIntegration::IsPlayerAIController(
			PlayerController);
	const bool bDeathSpectatingAllowed =
		!GameMode->HasbAllowSpectateAfterDeath() ||
		GameMode->bAllowSpectateAfterDeath;
	const bool bInLiveDeathPhase =
		GameState->HasGamePhase()
			? GameState->GamePhase > 2
			: GUI::gsStatus == StartedMatch;
	const bool bCanInitializeDeathSpectating =
		bIsHumanVictim &&
		!bRespawnAllowed &&
		(!PlayerController->Pawn ||
			!IsPawnDBNOForSpectating(PlayerController->Pawn)) &&
		bInLiveDeathPhase &&
		bDeathSpectatingAllowed &&
		GUI::gsStatus != Ended &&
		GameMode->MatchState != FName(L"WaitingPostMatch");

	TWeakObjectPtr<AFortPlayerPawnAthena>
		DeathSpectateTarget;
	TWeakObjectPtr<AFortPlayerPawnAthena>
		DeathSpectateDeadPawn(
			PlayerController->Pawn);
	if (bCanInitializeDeathSpectating)
	{
		DeathSpectateTarget =
			ResolveDeathSpectateTarget(
				GameMode, PlayerController,
				KillerPlayerController, KillerPawn);
		if (auto TargetPawn = DeathSpectateTarget.Get();
			TargetPawn &&
			PlayerController->HasPlayerToSpectateOnDeath())
		{
			// This property is only present on the human controller in legacy
			// layouts. In 15.30 it belongs to the AI controller, so it is a
			// useful seed when available but never a modern-path requirement.
			PlayerController->PlayerToSpectateOnDeath =
				TargetPawn;
		}
	}

	auto KismetSystemLibrary = UKismetSystemLibrary::GetDefaultObj();
	const bool bCanSeedLegacySpectating =
		UsesCoreLegacyDeathSpectating() &&
		DeathSpectateTarget.Get() &&
		PlayerController->HasPlayerToSpectateOnDeath() &&
		PlayerController->GetFunction("SpectateOnDeath") &&
		KismetSystemLibrary &&
		KismetSystemLibrary->GetFunction("K2_SetTimer");

	if (bCanSeedLegacySpectating)
	{
		auto SpectateTarget = DeathSpectateTarget.Get();
		if (SpectateTarget)
		{
			UKismetSystemLibrary::K2_SetTimer(
				PlayerController,
				FString(L"SpectateOnDeath"),
				5.f,
				false);
		}
		else
		{
			SDK::DbgLog(
				"[Spectating] no valid legacy death target "
				"controller=%p version=%.2f\n",
				(void*)PlayerController,
				VersionInfo.FortniteVersion);
		}
	}

	if (bIsLateSeasonBotVictim)
	{
		LateSeasonFinishingWeapon =
			GetWeaponDataSafe(ResolveDeathReportWeapon(DeathReport));
		if (PlayerState->HasDeathInfo())
			LateSeasonDeathCause = PlayerState->DeathInfo.DeathCause;
	}

	// The explicit GUI override is authoritative across modes, but native LTM
	// mutators do not all create the RespawnData/client-countdown handshake.
	// Queue a one-tick supervisor before returning to native death handling.
	// It observes and retires itself when native succeeds; only a missing
	// handshake reaches the bounded fallback. Getaway's Jewel was already
	// dropped and removed above, so this never repeats inventory processing.
	if (FConfiguration::bForceRespawns &&
		bRespawnAllowed &&
		GUI::gsStatus != Ended &&
		GameMode->MatchState != FName(L"WaitingPostMatch"))
	{
		auto DeadPawnForRespawn =
			(AFortPlayerPawnAthena*)PlayerController->Pawn;
		const FVector DeathLocation =
			IsUsableDeathObject(DeadPawnForRespawn)
				? DeadPawnForRespawn->K2_GetActorLocation()
				: (PlayerState->HasPawnDeathLocation()
					? PlayerState->PawnDeathLocation
					: FVector());
		const FRotator DeathRotation =
			IsUsableDeathObject(DeadPawnForRespawn)
				? DeadPawnForRespawn->K2_GetActorRotation()
				: FRotator();
		QueueForcedRespawnRepair(
			PlayerController, PlayerState,
			DeadPawnForRespawn,
			DeathLocation, DeathRotation);
	}

	if (bPlayerAIVictim || bSpawnedCommandBotVictim)
		InvalidateRespawnHandshake(PlayerState);

	// Command-spawned bots are not owned by PlayerAIManager, so retain their
	// actor graph until native death has emitted its effects and then tear it
	// down as one unit. The cleanup also catches any unauthorized replacement
	// pawn native respawn code managed to create.
	if (bSpawnedCommandBotVictim)
	{
		ScheduleSpawnedBotCleanup(
			PlayerController, ManagedBotEliminatedPawn,
			PlayerState, ManagedBotInventory);
	}

	if (bPlayerAIVictim)
	{
		PlayerAIManager::HandleControllerDeath(
			PlayerController, ManagedBotEliminatedPawn);
	}

	if (!bCalledNativeDeathEarly)
		CallNativePawnDied();

	// A solo host can eliminate themselves with nobody left to receive a
	// Victory Royale. Several legacy builds then show Match Stats to the dead
	// client but leave MatchState in progress forever. This edge is derived
	// from an actual permanent death—not an empty connection list—so ordinary
	// disconnects cannot close an Auto Host match.
	const bool bNoAlivePlayers =
		IsUsableDeathObject(GameMode) &&
		GameMode->HasAlivePlayers() &&
		GameMode->AlivePlayers.Num() == 0;
	const bool bNoAliveBots =
		IsUsableDeathObject(GameMode) &&
		(!GameMode->HasAliveBots() ||
			GameMode->AliveBots.Num() == 0);
	if (!bRespawnAllowed &&
		bMatchWasLiveAtDeath &&
		bVictimWasAliveParticipant &&
		GUI::gsStatus.load(
			std::memory_order_acquire) == StartedMatch &&
		bNoAlivePlayers &&
		bNoAliveBots &&
		GameMode->MatchState != FName(L"WaitingPostMatch"))
	{
		GUI::gsStatus.store(
			Ended, std::memory_order_release);
		SDK::DbgLog(
			"[MatchLifecycle] Permanent elimination left no alive participants; match ended without a winner\n");
	}

	if (bWasAshtonLeader)
	{
		// Native death mutators may restore the old row or publish a delayed
		// candidate. Run the cleanup again after native processing.
		FFortAthenaNativeLTMCompatibility::
			HandleAshtonLeaderEliminated(
				PlayerController,
				true);
	}

	// Native death handling must run first so the client has its death UI and
	// the server has removed the victim from the alive set. Unlike the legacy
	// PlayerToSpectateOnDeath field, these reflected spectator calls exist on
	// the human controller and perform the first target/state handoff.
	if (!UsesCoreLegacyDeathSpectating() &&
		bCanInitializeDeathSpectating &&
		IsUsableDeathObject(GameMode) &&
		GUI::gsStatus != Ended &&
		GameMode->MatchState != FName(L"WaitingPostMatch"))
	{
		auto TargetPawn = DeathSpectateTarget.Get();
		AFortPlayerControllerAthena* TargetController = nullptr;
		if (IsUsableDeathObject(TargetPawn) &&
			TargetPawn->HasController() &&
			IsUsableDeathObject(TargetPawn->Controller))
		{
			TargetController =
				TargetPawn->Controller
					->Cast<AFortPlayerControllerAthena>();
		}
		if (ResolveValidDeathSpectatePawn(
				GameMode, PlayerController,
				TargetController) != TargetPawn)
		{
			// Native death processing can invalidate or down the originally
			// selected pawn. Resolve again instead of cutting the client camera
			// to a stale target.
			TargetPawn = ResolveDeathSpectateTarget(
				GameMode, PlayerController,
				KillerPlayerController, KillerPawn);
		}

		if (IsUsableDeathObject(TargetPawn))
		{
			InitializeDeathSpectating(
				PlayerController, TargetPawn,
				DeathSpectateDeadPawn.Get());
		}
		else
		{
			SDK::DbgLog(
				"[Spectating] no valid first death target "
				"controller=%p version=%.2f\n",
				(void*)PlayerController,
				VersionInfo.FortniteVersion);
		}
	}

	// Some later builds finalize the winner in native ClientOnPawnDied rather
	// than in RemoveFromAlivePlayers. Apply the already-prepared state after
	// that path as well; this is idempotent when the earlier path handled it.
	if (VersionInfo.FortniteVersion >= 19.0 &&
		VersionInfo.FortniteVersion < 26.0 &&
		FConfiguration::bCrownSlomo &&
		IsUsableDeathObject(GameMode) &&
		IsUsableDeathObject(GameState) &&
		GameMode->MatchState == FName(L"WaitingPostMatch"))
	{
		auto NativeCrownWinner =
			ResolveNativeVictoryWinner(
				GameMode, GameState,
				KillerPlayerState, PlayerController);
		ApplyVictoryCrownWinStateToWinningTeam(
			NativeCrownWinner);
	}

	if (bIsLateSeasonBotVictim)
	{
		CompleteLateSeasonBotVictoryAfterNative(
			GameMode, GameState, PlayerController,
			KillerPawn, KillerPlayerController,
			LateSeasonFinishingWeapon, LateSeasonDeathCause);
	}

	if (bIs172SpawnedBotVictim)
	{
		bool bVictimRemoved =
			!IsControllerInAlivePlayers(GameMode, PlayerController);

		if (!bVictimRemoved && bCanAttempt172SpawnedBotRemoval)
		{
			bVictimRemoved =
				Remove172SpawnedBotFromAlivePlayersAfterNative(
					GameMode, GameState, PlayerController, KillerPawn,
					KillerPlayerController);
		}

		if (bVictimRemoved)
		{
			G172SpawnedBotRemovalAttempts.erase(PlayerController);
			Complete172SpawnedBotVictoryAfterNative(
				GameMode, GameState, PlayerController,
				KillerPlayerState, KillerPawn, KillerPlayerController);
		}
		else if (bIsFinal172SpawnedBotDeath)
		{
			SDK::DbgLog(
				"[Elimination] 1.72 spawnbot remains registered after native death controller=%p attempted=%d\n",
				(void*)PlayerController,
				bCanAttempt172SpawnedBotRemoval ? 1 : 0);
		}
	}

	if (!bIsAshtonMatch)
	{
		if (!bManualCarminePickupSpawned &&
			HasNearbyCarmineGauntletPickup(
				ManualCarmineDropLocation))
		{
			// PassiveSetup/native death may have spawned the transferable
			// pickup while also restoring the victim row. Reuse that world
			// pickup and limit the compatibility repair to stale inventory.
			bManualCarminePickupSpawned = true;
			SDK::DbgLog(
				"[Ashton1040] found native Carmine death "
				"pickup near controller=%p; suppressing retry\n",
				(void*)PlayerController);
		}
		if (!bManualCarminePickupSpawned)
		{
			// Native teardown can restore the gadget row. Retry against the
			// same death location, but never create a second pickup.
			bManualCarminePickupSpawned =
				DropManualCarmineGauntletOnDeath(
					PlayerController,
					IsUsableDeathObject(KillerPawn)
						? KillerPawn
						: ManualCarmineDeadPawn,
					ManualCarmineDropLocation);
		}

		// A successful pre-native drop can still be followed by a restored
		// victim row or a non-authored killer transfer. Keep the one world
		// pickup and remove only rows introduced by this death.
		const int32 VictimRowsRemoved =
			RemoveCarmineGauntletArtifacts(
				PlayerController);
		const int32 KillerRowsRemoved =
			RemoveCarmineGauntletArtifacts(
				KillerPlayerController,
				&KillerCarmineArtifactsBefore);
		if (VictimRowsRemoved > 0 ||
			KillerRowsRemoved > 0)
		{
			SDK::DbgLog(
				"[Ashton1040] cleaned post-death Carmine "
				"restoration victimRows=%d killerRows=%d "
				"pickupSpawned=%d\n",
				VictimRowsRemoved,
				KillerRowsRemoved,
				bManualCarminePickupSpawned ? 1 : 0);
		}
	}

	// Run once more after native death handling, which may restore persistent
	// gadget items as part of its pawn-replacement teardown.
	PurgeExclusiveGadgets(PlayerController);
}

void AFortPlayerControllerAthena::ServerClientIsReadyToRespawn(UObject* Context, FFrame& Stack)
{
	Stack.IncrementCode();

	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	auto World = UWorld::GetWorld();
	auto GameMode =
		World
			? (AFortGameMode*)World->AuthorityGameMode
			: nullptr;
	auto GameState =
		GameMode
			? (AFortGameStateAthena*)GameMode->GameState
			: nullptr;
	auto PlayerState =
		IsUsableDeathObject(PlayerController)
			? (AFortPlayerStateAthena*)
				PlayerController->PlayerState
			: nullptr;
	if (!IsUsableDeathObject(GameMode) ||
		!IsUsableDeathObject(GameState) ||
		!IsUsableDeathObject(PlayerState) ||
		!PlayerState->HasRespawnData())
	{
		return;
	}

	bool bStormRespawnBlocked = false;
	auto StormBlock =
		GStormRespawnBlockedControllers.find(
			PlayerController);
	if (StormBlock !=
		GStormRespawnBlockedControllers.end())
	{
		bStormRespawnBlocked =
			StormBlock->second.Get() == GameState;
		if (!bStormRespawnBlocked)
		{
			// Match objects can be recycled independently of controllers.
			// Discard a stale life-specific decision before consulting the
			// current match's policy.
			GStormRespawnBlockedControllers.erase(
				StormBlock);
		}
	}

	const bool bMatchLifecycleAllowsRespawn =
		GUI::gsStatus >= Joinable &&
		GUI::gsStatus < Ended &&
		GameMode->MatchState !=
			FName(L"WaitingPostMatch") &&
		(!GameState->HasGamePhase() ||
			GameState->GamePhase <
				(uint8)EAthenaGamePhase::EndGame);
	if (!bMatchLifecycleAllowsRespawn ||
		!IsRespawningAllowedForDeath(
			GameMode, GameState, PlayerController, PlayerState,
			bStormRespawnBlocked))
	{
		// Ready is a client RPC and may arrive after the user changed the
		// Infinite/Storm Respawns toggle, after objective elimination, or after
		// post-match began. Re-evaluate the live policy instead of trusting the
		// stale packet and invalidate all halves of its old handshake.
		InvalidateRespawnHandshake(PlayerState);
		return;
	}

	bool bWaxRespawnAllowed = false;
	const bool bWaxRespawnManaged =
		PlayerState &&
		FFortAthenaNativeLTMCompatibility::
			TryGetWaxRespawnAllowed(
				PlayerState,
				bWaxRespawnAllowed);
	if (bWaxRespawnManaged && !bWaxRespawnAllowed)
	{
		// A ready packet may already be queued when the final authored life is
		// consumed. Invalidate every side of the handshake so InfiniteRespawn
		// cannot recreate a permanently waxed player.
		InvalidateRespawnHandshake(PlayerState);
		return;
	}

	bool bDeepFriedRespawnAllowed = false;
	if (PlayerState &&
		FFortAthenaNativeLTMCompatibility::
			TryGetFoodFightRespawnAllowed(
				PlayerState,
				bDeepFriedRespawnAllowed) &&
		!bDeepFriedRespawnAllowed)
	{
		// A client-ready packet can already be queued when its mascot dies.
		// Invalidate the whole handshake so that packet cannot recreate the
		// pawn after the Barrier mutator has disabled this team's respawns.
		InvalidateRespawnHandshake(PlayerState);
		return;
	}

	bool bDiscoRespawnAllowed = false;
	if (PlayerState &&
		FFortAthenaNativeLTMCompatibility::
			TryGetDiscoRespawnAllowed(
				PlayerState,
				bDiscoRespawnAllowed) &&
		!bDiscoRespawnAllowed)
	{
		// Consume any ready packet already queued when the authored final
		// respawn window closed.
		InvalidateRespawnHandshake(PlayerState);
		return;
	}

	if (PlayerState->RespawnData.bRespawnDataAvailable && PlayerState->RespawnData.bServerIsReady)
	{
		// Consume the server half of this handshake before any spawn call. A
		// delayed duplicate client-ready packet must not be able to create a
		// second pawn from the same RespawnData.
		auto RespawnData = PlayerState->RespawnData;
		PlayerState->RespawnData.bRespawnDataAvailable =
			false;
		PlayerState->RespawnData.bServerIsReady = false;
		PlayerState->RespawnData.bClientIsReady = true;
		PlayerState->ForceNetUpdate();

		PurgeExclusiveGadgets(PlayerController);
		auto OldPawn = PlayerController->MyFortPawn;
		ResetLowerSeasonStormStateForRespawn(PlayerController, OldPawn, nullptr);

		FTransform SpawnTransform{};

		FQuat Rotation = RespawnData.RespawnRotation;
		SpawnTransform.Translation = RespawnData.RespawnLocation;
		SpawnTransform.Rotation = Rotation;

		FVector ConfiguredRespawnLocation{};
		if (!bWaxRespawnManaged &&
			TryGetConfiguredRespawnLocation(
				GameMode,
				ConfiguredRespawnLocation))
			SpawnTransform.Translation = ConfiguredRespawnLocation;

		auto Scale = FVector(1, 1, 1);
		SpawnTransform.Scale3D = Scale;

		auto NewPawn = GameMode->SpawnDefaultPawnAtTransform(PlayerController, SpawnTransform);
		if (!IsUsableDeathObject(NewPawn))
		{
			SDK::DbgLog(
				"[RespawnReady] pawn spawn failed "
				"controller=%p playerState=%p FN=%.2f\n",
				(void*)PlayerController,
				(void*)PlayerState,
				VersionInfo.FortniteVersion);
			return;
		}
		PlayerController->Possess(NewPawn);
		PlayerController->RespawnPlayerAfterDeath(true);

		NewPawn->SetHealth(100.f);

		// -315.373858 219.791659 452.150000 // button
		if (wcsstr(FConfiguration::Playlist, L"/Game/Gav/Levels/GM_1v1/Playlist_Arena_DefaultSolo_Respawn.Playlist_Arena_DefaultSolo_Respawn") && VersionInfo.FortniteVersion == 27.11)
		{
			NewPawn->K2_TeleportTo(FVector(-16.314775, 258.315735, 861.021480), FRotator(0.f, 0.f, 0.f));

			if (UAbilitySystemComponent* AbilitySystemComponent = PlayerState->AbilitySystemComponent)
			{
				static auto FallDamageGE = FindObject<UClass>(L"/Game/Athena/Items/Gameplay/Backpacks/Ashton/GE_AshtonPack_FallDamageImmune.GE_AshtonPack_FallDamageImmune_C");

				if (FallDamageGE)
				{
					FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

					Context.Instigator = PlayerController;
					Context.Causer = NewPawn;
					Context.AddSourceObject(NewPawn);

					AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(FallDamageGE, 1.0f, Context);
				}
			}

		}

		EnsurePawnGameplayAbilitiesInitialized(
			PlayerController, NewPawn);

		if (FConfiguration::bLateGame)
			ApplyLateGameSpawnShield(NewPawn);
		else
			NewPawn->SetShield(0.f);

		EnsureOneShotLowGravityVfx(PlayerController, NewPawn);
		NormalizeShieldAfterGameplayInitialization(NewPawn);

		ResetLowerSeasonStormStateForRespawn(PlayerController, OldPawn, NewPawn);
		// On the earliest clients, wait for the observed landing transition
		// before equipping. Performing the server equip here and again through
		// the legacy quickbar produces competing CurrentWeapon actors.
		if (!UsesEarlyAthenaLandingClientRefresh())
			RestoreEquipmentAfterRespawn(PlayerController);

		FFortAthenaNativeLTMCompatibility::
			HandleWaxPlayerReady(PlayerController);
		FFortAthenaNativeLTMCompatibility::
			HandleAshtonPlayerReady(PlayerController);
		if (AFortPlayerPawnAthena::
				HasMinimumHealthGodMode(PlayerController))
		{
			AFortPlayerPawnAthena::
				SetMinimumHealthGodMode(
					PlayerController, true);
		}
		GStormRespawnBlockedControllers.erase(
			PlayerController);
	}

	PlayerState->RespawnData.bClientIsReady = true;
}

struct FCustomCharacterParts
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FCustomCharacterParts);
};

void AFortPlayerControllerAthena::InternalPickup(FFortItemEntry* PickupEntry)
{
	if (!PickupEntry || !PickupEntry->ItemDefinition || !WorldInventory)
		return;
	auto PickupPawn = MyFortPawn
		? MyFortPawn
		: Pawn
			? Pawn->Cast<AFortPlayerPawnAthena>()
			: nullptr;
	if (FFortAthenaNativeLTMCompatibility::
			ShouldRejectAshtonPickup(
				PickupPawn,
				PickupEntry->ItemDefinition))
	{
		return;
	}
	if (FFortAthenaNativeLTMCompatibility::
			ShouldBlockAshtonGenericPickup(
				PickupPawn,
				PickupEntry->ItemDefinition))
	{
		return;
	}

	auto MaxStack = (int32)PickupEntry->ItemDefinition->GetMaxStackSize();
	int ItemCount = 0;


	if (!PickupEntry->ItemDefinition->HasbForceIntoOverflow() || !PickupEntry->ItemDefinition->bForceIntoOverflow)
		for (int i = 0; i < WorldInventory->Inventory.ReplicatedEntries.Num(); i++)
		{
			auto& Item = WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

			if (AFortInventory::IsPrimaryQuickbar(Item.ItemDefinition) && (!Item.ItemDefinition->HasbForceIntoOverflow() || !Item.ItemDefinition->bForceIntoOverflow))
			{
				// The visible backpack allocates one occupied slot per replicated
				// primary inventory entry. NumberOfSlotsToTake is not a reliable
				// capacity signal across supported builds and can overcount an
				// entry, making an open slot appear full while the pickaxe is held.
				ItemCount++;
			}
		}

	//printf("br: %d\n", ItemCount);
	FVector FinalLoc = Pawn ? Pawn->K2_GetActorLocation() : FVector();

	FVector ForwardVector = Pawn ? Pawn->GetActorForwardVector() : FVector();
	ForwardVector.Z = 0.0f;
	ForwardVector.Normalize();

	FinalLoc = FinalLoc + ForwardVector * 450.f;
	FinalLoc.Z += 50.f;

	const float RandomAngleVariation = ((float)rand() * 0.00109866634f) - 18.f;
	const float FinalAngle = RandomAngleVariation * 0.017453292519943295f;

	FinalLoc.X += cos(FinalAngle) * 100.f;
	FinalLoc.Y += sin(FinalAngle) * 100.f;

	auto _SendStat = [&](int Count)
		{
			FGameplayTagContainer TargetTags{};

			auto Interface = (IGameplayTagAssetInterface*)PickupEntry->ItemDefinition->GetInterface(IGameplayTagAssetInterface::StaticClass());
			if (Interface)
			{
				auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface->Vft[0x2];
				GetOwnedGameplayTags(Interface, &TargetTags);
				//Interface->GetOwnedGameplayTags(&TargetTags);
			}

			if (auto QuestManager = GetQuestManager(1))
				QuestManager->SendStatEvent(this, EFortQuestObjectiveStatEvent::GetCollect(), Count, true, (UObject*)PickupEntry->ItemDefinition, TargetTags);

			TargetTags.GameplayTags.Free();
			TargetTags.ParentTags.Free();
		};

	auto GiveOrSwap = [&]()
		{
			if (ItemCount >= 5 && AFortInventory::IsPrimaryQuickbar(PickupEntry->ItemDefinition))
			{
				if (!MyFortPawn || !MyFortPawn->CurrentWeapon)
					return;

				if (AFortInventory::IsPrimaryQuickbar(((AFortWeapon*)MyFortPawn->CurrentWeapon)->WeaponData))
				{
					auto itemEntry = WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
						{ return entry.ItemGuid == ((AFortWeapon*)MyFortPawn->CurrentWeapon)->ItemEntryGuid; }, FFortItemEntry::Size());

					if (!itemEntry)
						return;
					AFortInventory::SpawnPickup(Pawn->K2_GetActorLocation() + Pawn->GetActorForwardVector() * 70.f + FVector(0, 0, 50), *itemEntry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetUnset(), MyFortPawn, -1, true, true, true, nullptr, FinalLoc);
					WorldInventory->Remove(((AFortWeapon*)MyFortPawn->CurrentWeapon)->ItemEntryGuid);
					bool bForceFocusHandled = false;
					auto Item = WorldInventory->GiveItem(
						*PickupEntry,
						PickupEntry->Count,
						true,
						true,
						&bForceFocusHandled);
					if (!Item)
						return;
					if (!bForceFocusHandled)
					{
						ServerExecuteInventoryItem(
							Item->ItemEntry.ItemGuid);
						if (VersionInfo.FortniteVersion < 3)
						{
							auto& QuickBar = (AFortInventory::IsPrimaryQuickbar(Item->ItemEntry.ItemDefinition) || Item->ItemEntry.ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest()) ? QuickBars->PrimaryQuickBar : QuickBars->SecondaryQuickBar;
							int i = 0;
							for (i = 0; i < QuickBar.Slots.Num(); i++)
							{
								auto& Slot = QuickBar.Slots.Get(i, FQuickBarSlot::Size());

								for (auto& SlotItem : Slot.Items)
									if (SlotItem == Item->ItemEntry.ItemGuid)
									{
										QuickBars->ServerActivateSlotInternal(!(AFortInventory::IsPrimaryQuickbar(Item->ItemEntry.ItemDefinition) || Item->ItemEntry.ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest()), i, 0.f, true);
										break;
									}
							}
						}
						else
							ClientEquipItem(Item->ItemEntry.ItemGuid, true);
					}
				}
				else
				{
					AFortInventory::SpawnPickup(Pawn->K2_GetActorLocation() + Pawn->GetActorForwardVector() * 70.f + FVector(0, 0, 50), *PickupEntry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetUnset(), MyFortPawn, -1, true, true, true, nullptr, FinalLoc);
					return;
				}
			}
			else
				WorldInventory->GiveItem(*PickupEntry, PickupEntry->Count, true);

			_SendStat(PickupEntry->Count);
		};

	auto GiveOrSwapStack = [&](int32 OriginalCount)
		{
			if (PickupEntry->ItemDefinition->bAllowMultipleStacks && ItemCount < 5)
			{
				WorldInventory->GiveItem(*PickupEntry, OriginalCount - MaxStack, true);
				_SendStat(PickupEntry->Count);
			}
			else
				AFortInventory::SpawnPickup(Pawn->K2_GetActorLocation() + Pawn->GetActorForwardVector() * 70.f + FVector(0, 0, 50), *PickupEntry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetUnset(), MyFortPawn, OriginalCount - MaxStack, true, true, true, nullptr, FinalLoc);
		};

	if (MaxStack > 1)
	{
		auto item = WorldInventory->Inventory.ItemInstances.Search([PickupEntry, MaxStack](UFortWorldItem* entry)
			{ return entry->ItemEntry.ItemDefinition == PickupEntry->ItemDefinition && entry->ItemEntry.Count < MaxStack; });
		auto itemEntry = WorldInventory->Inventory.ReplicatedEntries.Search([PickupEntry, MaxStack](FFortItemEntry& entry)
			{ return entry.ItemDefinition == PickupEntry->ItemDefinition && entry.Count < MaxStack; }, FFortItemEntry::Size());

		if (item && *item)
		{
			bool bFound = false;
			/*for (int i = 0; i < itemEntry->StateValues.Num(); i++)
			{
				auto& StateValue = itemEntry->StateValues.Get(i, FFortItemEntryStateValue::Size());

				if (StateValue.StateType != 2)
					continue;

				bFound = true;
				StateValue.IntValue = 0;
				break;
			}*/

			auto TheRealOriginalCount = itemEntry->Count;
			if ((itemEntry->Count += PickupEntry->Count) > MaxStack)
			{
				auto OriginalCount = itemEntry->Count;
				itemEntry->Count = MaxStack;

				GiveOrSwapStack(OriginalCount);
			}

			// full proper
			for (int i = 0; i < itemEntry->StateValues.Num(); i++)
			{
				auto& StateValue = itemEntry->StateValues.Get(i, FFortItemEntryStateValue::Size());

				if (StateValue.StateType != 2)
					continue;

				StateValue.IntValue = 1;
				bFound = true;
				break;
			}

			if (!bFound)
			{
				auto Value = (FFortItemEntryStateValue*)malloc(FFortItemEntryStateValue::Size());
				memset((PBYTE)Value, 0, FFortItemEntryStateValue::Size());

				Value->IntValue = 1;
				Value->StateType = 2;
				itemEntry->StateValues.Add(*Value, FFortItemEntryStateValue::Size());

				free(Value);
			}

			auto Gained = itemEntry->Count - TheRealOriginalCount;

			_SendStat(Gained);
			(*item)->ItemEntry.Count = itemEntry->Count;
			WorldInventory->UpdateEntry(*itemEntry);
		}
		else
		{
			auto itemEntry2 = WorldInventory->Inventory.ReplicatedEntries.Search([PickupEntry, MaxStack](FFortItemEntry& entry)
				{ return entry.ItemDefinition == PickupEntry->ItemDefinition && entry.Count >= MaxStack; }, FFortItemEntry::Size());

			if (!itemEntry && itemEntry2 && !PickupEntry->ItemDefinition->bAllowMultipleStacks)
			{
				AFortInventory::SpawnPickup(Pawn->K2_GetActorLocation() + Pawn->GetActorForwardVector() * 70.f + FVector(0, 0, 50), *PickupEntry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetUnset(), MyFortPawn, PickupEntry->Count, true, true, true, nullptr, FinalLoc);
				return;
			}

			if (PickupEntry->Count > MaxStack)
			{
				auto OriginalCount = PickupEntry->Count;
				PickupEntry->Count = MaxStack;

				GiveOrSwapStack(OriginalCount);
			}

			GiveOrSwap();
		}
	}
	else
		GiveOrSwap();
}

std::unordered_map<std::string, std::vector<FVector>> Waypoints;
std::unordered_set<AFortPlayerControllerAthena*> MarkToTeleportPlayers;

static void RegenerateMinimumGodHealthAfterWaypoint(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* Pawn)
{
	if (!PlayerController ||
		!AFortPlayerPawnAthena::
			HasMinimumHealthGodMode(PlayerController))
	{
		return;
	}

	auto PlayerPawnClass =
		AFortPlayerPawnAthena::StaticClass();
	AFortPlayerPawnAthena* HealthPawn = nullptr;
	if (PlayerPawnClass &&
		PlayerController->HasMyFortPawn() &&
		PlayerController->MyFortPawn &&
		PlayerController->MyFortPawn->IsA(
			PlayerPawnClass))
	{
		HealthPawn = PlayerController->MyFortPawn;
	}
	else if (PlayerPawnClass && Pawn &&
		Pawn->IsA(PlayerPawnClass))
	{
		HealthPawn = Pawn;
	}
	if (!HealthPawn)
		return;

	const float MaxHealth = HealthPawn->GetMaxHealth();
	if (!FPlatformMath::IsFinite(MaxHealth) ||
		MaxHealth <= 0.f)
	{
		return;
	}

	HealthPawn->SetHealth(MaxHealth);
	HealthPawn->ForceNetUpdate();
}

static void (*ServerAddMapMarkerOG)(UObject*, FFrame&) = nullptr;
static int32 MarkerWorldPositionOffset = -2;

static void ServerAddMapMarker(UObject* Context, FFrame& Stack)
{
	auto MarkerComponent = Context;
	auto Owner = MarkerComponent->Outer;
	auto PlayerController = Owner ? Owner->Cast<AFortPlayerControllerAthena>() : nullptr;

	if (PlayerController && MarkToTeleportPlayers.count(PlayerController) && Stack.Locals)
	{
		if (MarkerWorldPositionOffset == -2)
		{
			auto MarkerRequestStruct = FindObject<UStruct>(L"/Script/FortniteGame.FortClientMarkerRequest");
			if (MarkerRequestStruct)
			{
				auto Prop = MarkerRequestStruct->GetProperty("WorldPosition");
				if (!Prop)
					Prop = MarkerRequestStruct->GetProperty("BasePosition");
				if (Prop)
					MarkerWorldPositionOffset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
				else
					MarkerWorldPositionOffset = -1;
			}
			else
				MarkerWorldPositionOffset = -1;
		}

		if (MarkerWorldPositionOffset >= 0)
		{
			FVector MarkerWorldPos = *(FVector*)(Stack.Locals + MarkerWorldPositionOffset);

			auto Pawn = PlayerController->Pawn;
			if (Pawn && (MarkerWorldPos.X != 0.f || MarkerWorldPos.Y != 0.f))
			{
				FVector TeleportLoc = UFortKismetLibrary::FindGroundLocationAt(UWorld::GetWorld(), nullptr, MarkerWorldPos, 10000.f, -10000.f, FName());
				if (TeleportLoc.X == 0.f && TeleportLoc.Y == 0.f && TeleportLoc.Z == 0.f)
					TeleportLoc = FVector(MarkerWorldPos.X, MarkerWorldPos.Y, MarkerWorldPos.Z + 500.f);
				else
					TeleportLoc.Z += 200.f;

				Pawn->K2_TeleportTo(TeleportLoc, Pawn->K2_GetActorRotation(), false, true);
				Pawn->CharacterMovement->Velocity = FVector{};
				PlayerController->ClientMessage(FString(L"Teleported to marker!"), FName(), 1.f);
			}
		}
	}

	if (ServerAddMapMarkerOG)
		return ServerAddMapMarkerOG(Context, Stack);
}

extern uint64_t ApplyCharacterCustomization;
extern uint64_t NotifyGameMemberAdded_;

int32 PlayerBotID = 0;

static std::string TrimPlayerCommandString(std::string Str)
{
	auto Start = Str.find_first_not_of(" \t");

	if (Start == std::string::npos)
		return "";

	auto End = Str.find_last_not_of(" \t");
	return Str.substr(Start, End - Start + 1);
}

static std::string NormalizePlayerCommandString(std::string Str)
{
	Str = TrimPlayerCommandString(Str);
	std::transform(Str.begin(), Str.end(), Str.begin(), [](unsigned char c) { return std::tolower(c); });
	return Str;
}

static int HexDigitValueForCommand(char Char)
{
	if (Char >= '0' && Char <= '9')
		return Char - '0';

	Char = static_cast<char>(std::tolower(static_cast<unsigned char>(Char)));

	if (Char >= 'a' && Char <= 'f')
		return 10 + (Char - 'a');

	return -1;
}

static std::string DecodePlayerCommandURLName(const std::string& Value)
{
	std::string Result;
	Result.reserve(Value.size());

	for (size_t i = 0; i < Value.size(); i++)
	{
		if (Value[i] == '+' )
		{
			Result += ' ';
			continue;
		}

		if (Value[i] == '%' && i + 2 < Value.size())
		{
			auto High = HexDigitValueForCommand(Value[i + 1]);
			auto Low = HexDigitValueForCommand(Value[i + 2]);

			if (High >= 0 && Low >= 0)
			{
				Result += static_cast<char>((High << 4) | Low);
				i += 2;
				continue;
			}
		}

		Result += Value[i];
	}

	return Result;
}

static void AddPlayerCommandNameCandidate(std::vector<std::string>& Candidates, const std::string& Name)
{
	auto TrimmedName = TrimPlayerCommandString(Name);

	if (TrimmedName.empty())
		return;

	auto NormalizedName = NormalizePlayerCommandString(TrimmedName);

	for (auto& ExistingName : Candidates)
	{
		if (NormalizePlayerCommandString(ExistingName) == NormalizedName)
			return;
	}

	Candidates.push_back(TrimmedName);
}

static std::string GetRequestURLPlayerNameForCommand(UNetConnection* Connection)
{
	auto RequestURL = GUI::GetRequestURL(Connection);

	if (!RequestURL || !RequestURL->Data || !RequestURL->NumElements)
		return "";

	auto RequestURLAllocated = RequestURL->ToString();
	std::string RequestURLStr(RequestURLAllocated.begin(), RequestURLAllocated.end());
	std::size_t pos = RequestURLStr.find("Name=");

	if (pos == std::string::npos)
		return "";

	std::size_t end_pos = RequestURLStr.find('?', pos);

	if (end_pos != std::string::npos)
		return RequestURLStr.substr(pos + 5, end_pos - pos - 5);

	return RequestURLStr.substr(pos + 5);
}

static std::vector<std::string> GetPlayerNameCandidatesForCommand(AFortPlayerControllerAthena* PlayerController, UNetConnection* Connection, int Index)
{
	std::vector<std::string> Candidates;
	auto PlayerState = PlayerController ? PlayerController->PlayerState : nullptr;

	auto RequestURLName = GetRequestURLPlayerNameForCommand(Connection);
	AddPlayerCommandNameCandidate(Candidates, RequestURLName);

	if (!RequestURLName.empty())
		AddPlayerCommandNameCandidate(Candidates, DecodePlayerCommandURLName(RequestURLName));

	if (PlayerState)
	{
		FString PSName = PlayerState->GetPlayerName();

		if (PSName.Data && PSName.NumElements)
		{
			auto PSNameString = PSName.ToString();
			AddPlayerCommandNameCandidate(Candidates, std::string(PSNameString.begin(), PSNameString.end()));
		}
	}

	AddPlayerCommandNameCandidate(Candidates, GUI::GetPlayerName(PlayerState, Connection));
	AddPlayerCommandNameCandidate(Candidates, GUI::GetPlayerNameFromConnection(Connection));
	AddPlayerCommandNameCandidate(Candidates, std::string("Player ") + std::to_string(Index + 1));

	return Candidates;
}

static std::string GetResolvedPlayerNameForCommand(AFortPlayerControllerAthena* PlayerController, UNetConnection* Connection, int Index)
{
	auto Candidates = GetPlayerNameCandidatesForCommand(PlayerController, Connection, Index);
	return Candidates.empty() ? std::string("Player ") + std::to_string(Index + 1) : Candidates[0];
}

static AFortPlayerControllerAthena* FindRealPlayerByExactNameForCommand(const std::string& Name, AFortPlayerControllerAthena* RequestingPlayer, std::string& MatchedName, bool& bAmbiguous, bool bAllowRequestingPlayer = false)
{
	bAmbiguous = false;
	MatchedName.clear();

	auto World = UWorld::GetWorld();

	if (!World || !World->NetDriver)
		return nullptr;

	auto Driver = (UNetDriver*)World->NetDriver;
	auto& ClientConnections = Driver->ClientConnections;
	auto NormalizedName = NormalizePlayerCommandString(Name);

	if (NormalizedName.empty())
		return nullptr;

	AFortPlayerControllerAthena* ExactMatch = nullptr;
	std::string ExactName;
	int ExactMatches = 0;

	for (int i = 0; i < ClientConnections.Num(); i++)
	{
		auto Connection = ClientConnections[i];

		if (!Connection || !Connection->PlayerController)
			continue;

		auto PC = (AFortPlayerControllerAthena*)Connection->PlayerController;

		if (!PC || (!bAllowRequestingPlayer && PC == RequestingPlayer) || !PC->Pawn)
			continue;

		auto PS = PC->PlayerState;

		if (PS && PS->HasbIsABot() && PS->bIsABot)
			continue;

		auto NameCandidates = GetPlayerNameCandidatesForCommand(PC, Connection, i);

		for (auto& PlayerName : NameCandidates)
		{
			auto NormalizedPlayerName = NormalizePlayerCommandString(PlayerName);

			if (NormalizedPlayerName == NormalizedName)
			{
				ExactMatch = PC;
				ExactName = GetResolvedPlayerNameForCommand(PC, Connection, i);
				ExactMatches++;
				break;
			}
		}
	}

	if (ExactMatches > 1)
	{
		bAmbiguous = true;
		return nullptr;
	}

	if (ExactMatches == 1)
	{
		MatchedName = ExactName;
		return ExactMatch;
	}

	return nullptr;
}

static FRotator MakeRotationFromDirection(FVector Direction)
{
	Direction = Direction.GetSafeNormal();

	if (Direction.IsZero())
		return FRotator(-90.f, 0.f, 0.f);

	constexpr double RadToDeg = 57.29577951308232;
	return FRotator(asin(Direction.Z) * RadToDeg, atan2(Direction.Y, Direction.X) * RadToDeg, 0.f);
}

template <typename T>
static bool SetReflectedProperty(UObject* Object, const char* PropertyName, const T& Value)
{
	if (!Object)
		return false;

	auto Prop = Object->GetProperty(PropertyName, GUESS_PROP_FLAGS(T));

	if (!Prop)
		return false;

	auto Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
	auto ElementSize = GetFromOffset<uint32>(Prop, Offsets::ElementSize);

	if (Offset == -1)
		return false;

	if constexpr (std::is_same_v<T, FVector>)
	{
		if (ElementSize != FVector::Size())
			return false;
	}
	else if (ElementSize != sizeof(T))
	{
		return false;
	}

	auto ValueCopy = Value;
	GetFromOffset<T>(Object, Offset) = ValueCopy;
	return true;
}

static bool SetReflectedBoolProperty(UObject* Object, const char* PropertyName, bool Value)
{
	if (!Object)
		return false;

	auto Prop = Object->GetProperty(PropertyName, 0x20000);

	if (!Prop)
		return false;

	auto Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
	auto FieldMask = Prop->GetFieldMask();

	if (Offset == -1 || FieldMask == 0)
		return false;

	if (Value)
		GetFromOffset<uint8>(Object, Offset) |= FieldMask;
	else
		GetFromOffset<uint8>(Object, Offset) &= ~FieldMask;

	return true;
}

static uint32 GetReflectedPropertyOffset(const UField* Prop)
{
	return Prop ? GetFromOffset<uint32>(Prop, Offsets::Offset_Internal) : -1;
}

static uint32 GetReflectedPropertyElementSize(const UField* Prop)
{
	return Prop ? GetFromOffset<uint32>(Prop, Offsets::ElementSize) : 0;
}

static std::string GetReflectedPropertyName(const UField* Prop)
{
	if (!Prop)
		return "";

	auto Name = VersionInfo.FortniteVersion >= 12.10 ? Prop->FField_GetName().ToSDKString() : Prop->GetName().ToSDKString();
	return std::string(Name.c_str());
}

static bool LooksLikeNukeTargetProperty(std::string Name)
{
	std::transform(Name.begin(), Name.end(), Name.begin(), [](unsigned char c) { return std::tolower(c); });

	static const char* Tokens[] =
	{
		"target",
		"destination",
		"impact",
		"goal",
		"homing",
		"seek",
		"lockon",
		"aim",
		"reticle",
		"crosshair",
		"final"
	};

	for (auto Token : Tokens)
	{
		if (Name.find(Token) != std::string::npos)
			return true;
	}

	return false;
}

static const UClass* FindActorClassByCommandArg(const std::string& ClassArg)
{
	auto TrimmedArg = TrimPlayerCommandString(ClassArg);

	if (TrimmedArg.empty())
		return nullptr;

	auto NormalizedArg = NormalizePlayerCommandString(TrimmedArg);

	auto TryFindClass = [](const std::string& Value) -> const UClass*
	{
		if (Value.empty())
			return nullptr;

		UEAllocatedWString WideValue(Value.begin(), Value.end());
		auto Class = FindObject<UClass>(WideValue.c_str());

		if (!Class)
			Class = FindClass(Value.c_str());

		return Class;
	};

	if (auto Class = TryFindClass(TrimmedArg))
		return Class;

	if (TrimmedArg.find('/') != std::string::npos)
	{
		auto DotIndex = TrimmedArg.rfind('.');

		if (DotIndex == std::string::npos)
		{
			auto LastSlashIndex = TrimmedArg.find_last_of('/');
			auto AssetName = LastSlashIndex == std::string::npos ? TrimmedArg : TrimmedArg.substr(LastSlashIndex + 1);

			if (auto Class = TryFindClass(TrimmedArg + "." + AssetName + "_C"))
				return Class;
		}
		else if (!NormalizedArg.ends_with("_c"))
		{
			if (auto Class = TryFindClass(TrimmedArg + "_C"))
				return Class;
		}
	}

	if (TrimmedArg.find('.') == std::string::npos)
	{
		if (auto Class = TryFindClass(TrimmedArg + "." + TrimmedArg))
			return Class;
	}

	auto ShortName = Misc::ObjectNames.find(NormalizedArg);

	if (ShortName != Misc::ObjectNames.end())
	{
		if (auto Class = TryFindClass(ShortName->second))
			return Class;
	}

	return nullptr;
}

// Resolves an item command argument (full path, object name, or short id) to its item definition.
static const UFortItemDefinition* FindItemDefinitionByCommandArg(const std::string& ItemArg)
{
	if (ItemArg.empty())
		return nullptr;

	auto ItemDefinition = FindObject<UFortItemDefinition>(UEAllocatedWString(ItemArg.begin(), ItemArg.end()));

	if (!ItemDefinition)
		ItemDefinition = TUObjectArray::FindObject<UFortItemDefinition>(ItemArg.c_str());

	if (!ItemDefinition)
	{
		// Preserve casing for full object names/paths above, but keep short IDs
		// case-insensitive just like the command name itself.
		auto NormalizedItemArg = NormalizePlayerCommandString(ItemArg);
		auto ShortNames = Misc::ItemNames.find(NormalizedItemArg.c_str());

		if (ShortNames != Misc::ItemNames.end())
		{
			std::string Value = ShortNames->second;

			if (Value == "logic_grappler") // stupid icl
			{
				if (VersionInfo.FortniteVersion >= 12.50)
					Value = "WID_Hook_Gun_Spytech_VR_Ore_T03";
				else if (VersionInfo.FortniteVersion >= 7.10 && VersionInfo.FortniteVersion < 12.50)
					Value = "WID_Hook_Gun_Slide";
				else
					Value = "WID_Hook_Gun_VR_Ore_T03";
			}

			ItemDefinition = TUObjectArray::FindObject<UFortItemDefinition>(Value.c_str());

			if (!ItemDefinition && Value.find('/') != std::string::npos)
			{
				if (auto Item = StaticLoadObject(UEAllocatedWString(Value.begin(), Value.end()).c_str(), UFortItemDefinition::StaticClass()))
					ItemDefinition = Item->Cast<UFortItemDefinition>();
			}
		}
	}

	return ItemDefinition;
}

// Zeroes gravity/velocity on a spawned pickup so it floats where it was spawned instead of falling.
static void DisablePickupGravity(AFortPickupAthena* Pickup)
{
	if (!Pickup || !Pickup->HasMovementComponent())
		return;

	auto MovementComponent = Pickup->MovementComponent;

	if (!MovementComponent)
		return;

	SetReflectedProperty<float>(MovementComponent, "ProjectileGravityScale", 0.f);
	SetReflectedProperty<FVector>(MovementComponent, "Velocity", FVector());
	SetReflectedProperty<float>(MovementComponent, "InitialSpeed", 0.f);
	SetReflectedProperty<float>(MovementComponent, "MaxSpeed", 0.f);
}

static const UClass* GetDefaultNukeProjectileClass()
{
	static const UClass* DefaultNukeProjectileClass = nullptr;

	if (!DefaultNukeProjectileClass)
		DefaultNukeProjectileClass = FindActorClassByCommandArg("/Game/Weapons/FORT_RocketLaunchers/Blueprints/B_Prj_Ranged_Rocket_Athena.B_Prj_Ranged_Rocket_Athena_C");

	return DefaultNukeProjectileClass;
}

static bool IsObjectShortCommandArg(const std::string& Arg)
{
	auto NormalizedArg = NormalizePlayerCommandString(Arg);
	return !NormalizedArg.empty() && Misc::ObjectNames.find(NormalizedArg) != Misc::ObjectNames.end();
}

static bool IsNukePlayerTargetKeyword(const std::string& Arg)
{
	auto NormalizedArg = NormalizePlayerCommandString(Arg);
	return NormalizedArg == "target" || NormalizedArg == "player";
}

static std::vector<std::string> SplitPlayerCommandArgs(const std::string& Args)
{
	std::vector<std::string> Tokens;
	std::stringstream Stream(Args);
	std::string Token;

	while (Stream >> Token)
		Tokens.push_back(Token);

	return Tokens;
}

static std::string JoinPlayerCommandArgs(const std::vector<std::string>& Tokens, size_t StartIndex)
{
	std::string Result;

	for (size_t i = StartIndex; i < Tokens.size(); i++)
	{
		if (!Result.empty())
			Result += " ";

		Result += Tokens[i];
	}

	return TrimPlayerCommandString(Result);
}

static bool TryParsePrefixedCommandFloat(const std::string& Arg, char Prefix, float& OutValue)
{
	auto TrimmedArg = TrimPlayerCommandString(Arg);

	if (TrimmedArg.size() < 2 || std::tolower(static_cast<unsigned char>(TrimmedArg[0])) != Prefix)
		return false;

	auto ValuePart = TrimmedArg.substr(1);
	size_t ParsedCount = 0;

	try
	{
		auto ParsedValue = std::stof(ValuePart, &ParsedCount);

		if (ParsedCount != ValuePart.size() || !std::isfinite(ParsedValue))
			return false;

		OutValue = ParsedValue;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

static bool TryParsePrefixedCommandVector(const std::string& Arg, char Prefix, FVector& OutValue)
{
	auto TrimmedArg = TrimPlayerCommandString(Arg);

	if (TrimmedArg.size() < 2 || std::tolower(static_cast<unsigned char>(TrimmedArg[0])) != Prefix)
		return false;

	auto ValuePart = TrimmedArg.substr(1);

	auto ParseOne = [](const std::string& S, float& Out) -> bool
	{
		if (S.empty())
			return false;

		size_t Cnt = 0;

		try
		{
			auto V = std::stof(S, &Cnt);

			if (Cnt != S.size() || !std::isfinite(V))
				return false;

			Out = V;
			return true;
		}
		catch (...)
		{
			return false;
		}
	};

	auto FirstComma = ValuePart.find(',');

	if (FirstComma == std::string::npos)
	{
		// uniform: "s2" -> (2, 2, 2)
		float V = 0.f;

		if (!ParseOne(ValuePart, V))
			return false;

		OutValue = FVector(V, V, V);
		return true;
	}

	// per-axis: "s1,1,5" -> (1, 1, 5)
	auto SecondComma = ValuePart.find(',', FirstComma + 1);

	if (SecondComma == std::string::npos || ValuePart.find(',', SecondComma + 1) != std::string::npos)
		return false;

	float X = 0.f, Y = 0.f, Z = 0.f;

	if (!ParseOne(ValuePart.substr(0, FirstComma), X) ||
		!ParseOne(ValuePart.substr(FirstComma + 1, SecondComma - FirstComma - 1), Y) ||
		!ParseOne(ValuePart.substr(SecondComma + 1), Z))
		return false;

	OutValue = FVector(X, Y, Z);
	return true;
}

static bool TryParseCommandFloat(const std::string& Arg, float& OutValue)
{
	auto TrimmedArg = TrimPlayerCommandString(Arg);

	if (TrimmedArg.empty())
		return false;

	size_t ParsedCount = 0;

	try
	{
		auto ParsedValue = std::stof(TrimmedArg, &ParsedCount);

		if (ParsedCount != TrimmedArg.size() || !std::isfinite(ParsedValue))
			return false;

		OutValue = ParsedValue;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

static bool TryParseCommandInt(const std::string& Arg, int& OutValue)
{
	auto TrimmedArg = TrimPlayerCommandString(Arg);

	if (TrimmedArg.empty())
		return false;

	size_t ParsedCount = 0;

	try
	{
		auto ParsedValue = std::stoi(TrimmedArg, &ParsedCount);

		if (ParsedCount != TrimmedArg.size())
			return false;

		OutValue = ParsedValue;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

static bool LooksLikeSizeOrHeightModifierArg(const std::string& Arg)
{
	auto TrimmedArg = TrimPlayerCommandString(Arg);

	if (TrimmedArg.size() < 2)
		return false;

	auto Prefix = static_cast<char>(std::tolower(static_cast<unsigned char>(TrimmedArg[0])));
	if (Prefix != 's' && Prefix != 'h')
		return false;

	auto ValueStart = TrimmedArg[1];
	return std::isdigit(static_cast<unsigned char>(ValueStart)) || ValueStart == '.' || ValueStart == '-' || ValueStart == '+';
}

static std::wstring FormatCommandFloatForMessage(float Value)
{
	std::ostringstream Stream;
	Stream << Value;

	auto Text = Stream.str();
	return std::wstring(Text.begin(), Text.end());
}

static std::wstring FormatCommandScaleForMessage(const FVector& Scale)
{
	if (Scale.X == Scale.Y && Scale.Y == Scale.Z)
		return FormatCommandFloatForMessage((float)Scale.X);

	return FormatCommandFloatForMessage((float)Scale.X) + L"," +
		FormatCommandFloatForMessage((float)Scale.Y) + L"," +
		FormatCommandFloatForMessage((float)Scale.Z);
}

static bool FunctionHasSingleCommandInputParam(UFunction* Function, uint32 ExpectedElementSize)
{
	if (!Function)
		return false;

	auto Params = Function->GetParams();
	int InputParamCount = 0;

	for (auto& Param : Params.NameOffsetMap)
	{
		if (((Param.PropertyFlags & 0x100) != 0 && (Param.PropertyFlags & 0x8000000) == 0) || (Param.PropertyFlags & 0x400) != 0)
			continue;

		InputParamCount++;

		if (Param.ElementSize != ExpectedElementSize)
			return false;
	}

	return InputParamCount == 1;
}

static bool FunctionHasNoCommandInputParams(UFunction* Function)
{
	if (!Function)
		return false;

	auto Params = Function->GetParams();

	for (auto& Param : Params.NameOffsetMap)
	{
		if (((Param.PropertyFlags & 0x100) != 0 && (Param.PropertyFlags & 0x8000000) == 0) || (Param.PropertyFlags & 0x400) != 0)
			continue;

		return false;
	}

	return true;
}

static bool TryCallNoParamCommandFunction(UObject* Object, const char* FunctionName)
{
	if (!Object)
		return false;

	auto Function = Object->GetFunction(FunctionName);

	if (!FunctionHasNoCommandInputParams(Function))
		return false;

	Object->Call<void>(Function);
	return true;
}

static bool TryCallVectorCommandFunction(UObject* Object, const char* FunctionName, const FVector& Value)
{
	if (!Object)
		return false;

	auto Function = Object->GetFunction(FunctionName);

	if (!FunctionHasSingleCommandInputParam(Function, FVector::Size()))
		return false;

	auto ValueCopy = Value;
	Object->Call<void>(Function, ValueCopy);
	return true;
}

static bool SetReflectedTransformScaleProperty(UObject* Object, const char* PropertyName, const FVector& Scale)
{
	if (!Object)
		return false;

	auto Prop = Object->GetProperty(PropertyName);

	if (!Prop)
		return false;

	auto Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
	auto ElementSize = GetFromOffset<uint32>(Prop, Offsets::ElementSize);

	if (Offset == -1 || ElementSize != FTransform::Size())
		return false;

	auto Transform = GetFromOffset<FTransform>(Object, Offset);
	auto ScaleCopy = Scale;
	Transform.Scale3D = ScaleCopy;
	GetFromOffset<FTransform>(Object, Offset) = Transform;
	return true;
}

static bool GetReflectedFloatForCommand(UObject* Object, const char* PropertyName, float& OutValue)
{
	if (!Object)
		return false;

	auto Prop = Object->GetProperty(PropertyName);

	if (!Prop)
		return false;

	auto Offset = GetFromOffset<uint32>(Prop, Offsets::Offset_Internal);
	auto ElementSize = GetFromOffset<uint32>(Prop, Offsets::ElementSize);

	if (Offset == -1 || ElementSize != sizeof(float))
		return false;

	OutValue = GetFromOffset<float>(Object, Offset);
	return true;
}

static void RefreshScaledComponentForCommand(UObject* Component)
{
	if (!Component)
		return;

	if (auto UpdateComponentToWorldFn = Component->GetFunction("UpdateComponentToWorld"))
		Component->Call<void>(UpdateComponentToWorldFn);

	if (auto UpdateBoundsFn = Component->GetFunction("UpdateBounds"))
		Component->Call<void>(UpdateBoundsFn);

	if (auto MarkRenderTransformDirtyFn = Component->GetFunction("MarkRenderTransformDirty"))
		Component->Call<void>(MarkRenderTransformDirtyFn);

	if (auto MarkRenderDynamicDataDirtyFn = Component->GetFunction("MarkRenderDynamicDataDirty"))
		Component->Call<void>(MarkRenderDynamicDataDirtyFn);

	if (auto MarkRenderStateDirtyFn = Component->GetFunction("MarkRenderStateDirty"))
		Component->Call<void>(MarkRenderStateDirtyFn);

	TryCallNoParamCommandFunction(Component, "RecreateRenderState_Concurrent");
	TryCallNoParamCommandFunction(Component, "ReregisterComponent");
}

static bool SetComponentScaleForCommand(UObject* Component, const FVector& Scale)
{
	if (!Component)
		return false;

	bool bApplied = false;

	bApplied = TryCallVectorCommandFunction(Component, "SetWorldScale3D", Scale) || bApplied;
	bApplied = TryCallVectorCommandFunction(Component, "K2_SetWorldScale3D", Scale) || bApplied;
	bApplied = TryCallVectorCommandFunction(Component, "SetRelativeScale3D", Scale) || bApplied;
	bApplied = TryCallVectorCommandFunction(Component, "K2_SetRelativeScale3D", Scale) || bApplied;

	bApplied = SetReflectedProperty<FVector>(Component, "RelativeScale3D", Scale) || bApplied;
	bApplied = SetReflectedProperty<FVector>(Component, "WorldScale3D", Scale) || bApplied;
	bApplied = SetReflectedTransformScaleProperty(Component, "ComponentToWorld", Scale) || bApplied;
	bApplied = SetReflectedTransformScaleProperty(Component, "RelativeTransform", Scale) || bApplied;

	if (bApplied)
		RefreshScaledComponentForCommand(Component);

	return bApplied;
}

static bool SetActorScaleForCommand(AActor* Actor, const FVector& Scale)
{
	if (!Actor)
		return false;

	bool bApplied = false;

	bApplied = TryCallVectorCommandFunction(Actor, "K2_SetActorScale3D", Scale) || bApplied;
	bApplied = TryCallVectorCommandFunction(Actor, "SetActorScale3D", Scale) || bApplied;
	bApplied = SetReflectedProperty<FVector>(Actor, "DrawScale3D", Scale) || bApplied;
	float DrawScaleValue = (float)Scale.X;
	bApplied = SetReflectedProperty<float>(Actor, "DrawScale", DrawScaleValue) || bApplied;

	auto Transform = Actor->GetTransform();
	auto ScaleCopy = Scale;
	Transform.Scale3D = ScaleCopy;

	if (auto SetTransformFn = Actor->GetFunction("SetTransform"))
	{
		if (FunctionHasSingleCommandInputParam(SetTransformFn, FTransform::Size()))
		{
			Actor->Call<void>(SetTransformFn, Transform);
			bApplied = true;
		}
	}

	if (Actor->HasRootComponent() && Actor->RootComponent)
		bApplied = SetComponentScaleForCommand(Actor->RootComponent, Scale) || bApplied;

	if (auto Pawn = Actor->Cast<AFortPlayerPawnAthena>())
	{
		if (Pawn->HasMesh() && Pawn->Mesh)
			bApplied = SetComponentScaleForCommand(Pawn->Mesh, Scale) || bApplied;

		static auto CapsuleComponentClass = FindClass("CapsuleComponent");
		auto CapsuleComponent = CapsuleComponentClass ? Pawn->GetComponentByClass(CapsuleComponentClass) : nullptr;

		if (CapsuleComponent)
			bApplied = SetComponentScaleForCommand(CapsuleComponent, Scale) || bApplied;
	}

	Actor->FlushNetDormancy();
	Actor->ForceNetUpdate();
	return bApplied;
}

static bool SetActorScaleForCommand(AActor* Actor, float ScaleValue)
{
	return SetActorScaleForCommand(Actor, FVector(ScaleValue, ScaleValue, ScaleValue));
}

static bool TryGetCrosshairGroundLocationForCommand(AFortPlayerControllerAthena* PlayerController, FVector& OutLocation)
{
	if (!PlayerController)
		return false;

	FVector ViewLoc;
	FRotator ViewRot;

	if (PlayerController->Pawn)
	{
		ViewLoc = PlayerController->Pawn->K2_GetActorLocation();
		ViewLoc.Z += 80.f;
		ViewRot = PlayerController->GetControlRotation();
	}
	else
	{
		AFortPlayerControllerAthena::GetPlayerViewPoint(PlayerController, ViewLoc, ViewRot);
	}

	FVector ViewDir = ViewRot.Vector().GetSafeNormal();

	if (ViewDir.IsZero())
		return false;

	auto IsInvalidGroundLocation = [](const FVector& Location, const FVector& Reference)
	{
		return Location.X == 0.f && Location.Y == 0.f && (std::abs(Reference.X) > 100.f || std::abs(Reference.Y) > 100.f);
	};

	constexpr float StepSize = 500.f;
	constexpr float MaxDist = 50000.f;
	constexpr float GroundHitTolerance = 50.f;
	float PreviousRayHeightOverGround = (std::numeric_limits<float>::max)();

	for (float Dist = StepSize; Dist <= MaxDist; Dist += StepSize)
	{
		FVector SamplePoint = FVector(
			ViewLoc.X + ViewDir.X * Dist,
			ViewLoc.Y + ViewDir.Y * Dist,
			ViewLoc.Z + ViewDir.Z * Dist
		);

		FVector GroundLoc = UFortKismetLibrary::FindGroundLocationAt(UWorld::GetWorld(), nullptr, SamplePoint, 10000.f, -10000.f, FName());

		if (IsInvalidGroundLocation(GroundLoc, SamplePoint))
			continue;

		auto RayHeightOverGround = SamplePoint.Z - GroundLoc.Z;

		if (std::abs(RayHeightOverGround) <= GroundHitTolerance || RayHeightOverGround <= 0.f || (PreviousRayHeightOverGround > 0.f && RayHeightOverGround <= 0.f))
		{
			OutLocation = GroundLoc;
			return true;
		}

		PreviousRayHeightOverGround = RayHeightOverGround;
	}

	FVector EndPoint = FVector(
		ViewLoc.X + ViewDir.X * MaxDist,
		ViewLoc.Y + ViewDir.Y * MaxDist,
		ViewLoc.Z + ViewDir.Z * MaxDist
	);

	OutLocation = UFortKismetLibrary::FindGroundLocationAt(UWorld::GetWorld(), nullptr, EndPoint, 10000.f, -10000.f, FName());

	if (IsInvalidGroundLocation(OutLocation, EndPoint))
		OutLocation = FVector(EndPoint.X, EndPoint.Y, ViewLoc.Z);

	return true;
}

static bool IsGameplayEffectClassForCommand(const UClass* Class)
{
	auto GameplayEffectClass = UGameplayEffect::StaticClass();

	if (!Class || !GameplayEffectClass)
		return false;

	int SuperGuard = 0;

	for (const UStruct* CurrentClass = Class;
		CurrentClass && SuperGuard++ < 4096;
		CurrentClass = CurrentClass->GetSuper())
	{
		if (CurrentClass == GameplayEffectClass)
			return true;
	}

	return false;
}

struct FLoadedGameplayEffectCommandEntry
{
	TWeakObjectPtr<UClass> GameplayEffectClass;
	std::string EffectName;
	std::string EffectClassName;
	std::string NormalizedEffectName;
	std::string NormalizedEffectClassName;
	std::string NormalizedShortClassName;
};

struct FLoadedGameplayEffectCommandCatalog
{
	TWeakObjectPtr<UWorld> World;
	std::vector<FLoadedGameplayEffectCommandEntry> Entries;
	std::vector<FLoadedGameplayEffectCommandEntry> PendingEntries;
	std::vector<TWeakObjectPtr<AFortPlayerControllerAthena>> Requesters;
	int32 NextObjectIndex = 0;
	int32 ObjectCount = 0;
	bool bBuilding = false;
	bool bHasCompletedCatalog = false;
};

static FLoadedGameplayEffectCommandCatalog GLoadedGameplayEffectCommandCatalog;

struct FObservedGameplayEffectCommandEntry
{
	TWeakObjectPtr<UClass> GameplayEffectClass;
	FName EffectName;
	FName EffectClassName;
};

struct FObservedGameplayEffectCommandCache
{
	TWeakObjectPtr<UWorld> World;
	std::vector<FObservedGameplayEffectCommandEntry> Entries;
};

static FObservedGameplayEffectCommandCache
	GObservedGameplayEffectCommandCache;
static constexpr size_t MaxObservedGameplayEffectCommandEntries = 256;

static void ResetObservedGameplayEffectCommandCacheForWorld(
	UWorld* CurrentWorld)
{
	auto& Cache = GObservedGameplayEffectCommandCache;

	if (Cache.World.Get() == CurrentWorld)
		return;

	Cache = FObservedGameplayEffectCommandCache();

	if (CurrentWorld)
	{
		Cache.World = TWeakObjectPtr<UWorld>(CurrentWorld);
		Cache.Entries.reserve(
			MaxObservedGameplayEffectCommandEntries);
	}
}

static void RememberObservedGameplayEffectClassForCommand(
	const FName& EffectName,
	const FName& EffectClassName,
	UClass* GameplayEffectClass)
{
	auto CurrentWorld = UWorld::GetWorld();
	ResetObservedGameplayEffectCommandCacheForWorld(CurrentWorld);

	if (!CurrentWorld ||
		!IsGameplayEffectClassForCommand(GameplayEffectClass))
	{
		return;
	}

	auto& Entries = GObservedGameplayEffectCommandCache.Entries;

	for (size_t Index = 0; Index < Entries.size(); Index++)
	{
		if (Entries[Index].GameplayEffectClass.Get() !=
			GameplayEffectClass)
		{
			continue;
		}

		FObservedGameplayEffectCommandEntry RefreshedEntry;
		RefreshedEntry.GameplayEffectClass =
			TWeakObjectPtr<UClass>(GameplayEffectClass);
		RefreshedEntry.EffectName = EffectName;
		RefreshedEntry.EffectClassName = EffectClassName;

		Entries.erase(Entries.begin() + Index);
		Entries.emplace_back(std::move(RefreshedEntry));
		return;
	}

	Entries.erase(
		std::remove_if(
			Entries.begin(),
			Entries.end(),
			[](const FObservedGameplayEffectCommandEntry& Entry)
			{
				return !Entry.GameplayEffectClass.Get();
			}),
		Entries.end());

	if (Entries.size() >= MaxObservedGameplayEffectCommandEntries)
		Entries.erase(Entries.begin());

	FObservedGameplayEffectCommandEntry NewEntry;
	NewEntry.GameplayEffectClass =
		TWeakObjectPtr<UClass>(GameplayEffectClass);
	NewEntry.EffectName = EffectName;
	NewEntry.EffectClassName = EffectClassName;
	Entries.emplace_back(std::move(NewEntry));
}

static bool TryFindObservedGameplayEffectClassForCommand(
	const std::string& NormalizedArg,
	const UClass*& OutGameplayEffectClass)
{
	OutGameplayEffectClass = nullptr;
	auto CurrentWorld = UWorld::GetWorld();
	ResetObservedGameplayEffectCommandCacheForWorld(CurrentWorld);

	if (!CurrentWorld || NormalizedArg.empty())
		return false;

	auto& Entries = GObservedGameplayEffectCommandCache.Entries;
	bool bFoundMatch = false;

	for (auto Entry = Entries.rbegin();
		Entry != Entries.rend(); ++Entry)
	{
		auto GameplayEffectClass = Entry->GameplayEffectClass.Get();

		if (!IsGameplayEffectClassForCommand(GameplayEffectClass))
			continue;

		auto NormalizedEffectName =
			NormalizePlayerCommandString(
				Entry->EffectName.ToString().c_str());
		auto NormalizedEffectClassName =
			NormalizePlayerCommandString(
				Entry->EffectClassName.ToString().c_str());
		auto NormalizedShortClassName =
			NormalizedEffectClassName;

		if (NormalizedShortClassName.ends_with("_c"))
		{
			NormalizedShortClassName.resize(
				NormalizedShortClassName.size() - 2);
		}

		if (NormalizedArg == NormalizedEffectName ||
			NormalizedArg == NormalizedEffectClassName ||
			NormalizedArg == NormalizedShortClassName)
		{
			if (bFoundMatch &&
				OutGameplayEffectClass != GameplayEffectClass)
			{
				// Two packages can contain the same short class name. Never
				// guess in that case; a full path or dump index is unambiguous.
				OutGameplayEffectClass = nullptr;
				return true;
			}

			bFoundMatch = true;
			OutGameplayEffectClass = GameplayEffectClass;
		}
	}

	return bFoundMatch;
}

static const std::vector<FLoadedGameplayEffectCommandEntry>*
GetLoadedGameplayEffectCatalogForCommand()
{
	auto CurrentWorld = UWorld::GetWorld();

	if (!CurrentWorld ||
		!GLoadedGameplayEffectCommandCatalog.bHasCompletedCatalog ||
		GLoadedGameplayEffectCommandCatalog.World.Get() != CurrentWorld)
	{
		return nullptr;
	}

	return &GLoadedGameplayEffectCommandCatalog.Entries;
}

static void AddLoadedGameplayEffectCatalogRequester(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!PlayerController)
		return;

	auto& Requesters = GLoadedGameplayEffectCommandCatalog.Requesters;

	for (auto It = Requesters.begin(); It != Requesters.end();)
	{
		auto ExistingController = It->Get();

		if (!ExistingController)
		{
			It = Requesters.erase(It);
			continue;
		}

		if (ExistingController == PlayerController)
			return;

		++It;
	}

	Requesters.emplace_back(PlayerController);
}

static bool WriteLoadedGameplayEffectCatalogForCommand(
	const std::vector<FLoadedGameplayEffectCommandEntry>& Entries)
{
	std::ostringstream Buffer;
	Buffer << "Generated by Magnesium\n";
	Buffer << "Loaded Gameplay Effects: " << Entries.size() << "\n\n";

	for (size_t Index = 0; Index < Entries.size(); Index++)
	{
		auto& Entry = Entries[Index];
		Buffer << "[" << Index << "] " << Entry.EffectName;

		if (!Entry.EffectClassName.empty())
			Buffer << " (" << Entry.EffectClassName << ")";

		Buffer << "\n";
	}

	auto Contents = Buffer.str();
	std::ofstream Output(
		"DumpedGameplayEffects.txt",
		std::ios::out | std::ios::binary | std::ios::trunc);

	if (!Output.is_open())
		return false;

	Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
	Output.close();
	return Output.good();
}

static void CompleteLoadedGameplayEffectCatalogForCommand()
{
	auto& Catalog = GLoadedGameplayEffectCommandCatalog;
	Catalog.Entries = std::move(Catalog.PendingEntries);
	Catalog.PendingEntries.clear();
	Catalog.bBuilding = false;
	Catalog.bHasCompletedCatalog = true;

	const bool bWroteFile =
		WriteLoadedGameplayEffectCatalogForCommand(Catalog.Entries);
	const auto EffectCount = Catalog.Entries.size();
	std::wstring CompletionMessage =
		L"Gameplay Effect catalog complete: " +
		std::to_wstring(EffectCount) +
		L" loaded effect(s). ";

	if (bWroteFile)
	{
		CompletionMessage +=
			L"Wrote DumpedGameplayEffects.txt; applyge indexes are ready.";
	}
	else
	{
		CompletionMessage +=
			L"Applyge indexes are ready, but DumpedGameplayEffects.txt could not be written.";
	}

	for (auto& Requester : Catalog.Requesters)
	{
		auto PlayerController = Requester.Get();

		if (PlayerController)
		{
			PlayerController->ClientMessage(
				FString(CompletionMessage.c_str()),
				FName(),
				1.f);
		}
	}

	Catalog.Requesters.clear();
}

static void StartLoadedGameplayEffectCatalogForCommand(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!PlayerController)
		return;

	auto CurrentWorld = UWorld::GetWorld();
	auto GameplayEffectClass = UGameplayEffect::StaticClass();
	const int32 ObjectCount = SDK::TUObjectArray::Num();

	if (!CurrentWorld || !GameplayEffectClass ||
		ObjectCount <= 0 || ObjectCount > 50000000)
	{
		PlayerController->ClientMessage(
			FString(L"Gameplay Effect catalog could not start on this version."),
			FName(),
			1.f);
		return;
	}

	auto& Catalog = GLoadedGameplayEffectCommandCatalog;

	if (Catalog.World.Get() != CurrentWorld)
	{
		Catalog = FLoadedGameplayEffectCommandCatalog();
		Catalog.World = TWeakObjectPtr<UWorld>(CurrentWorld);
	}

	AddLoadedGameplayEffectCatalogRequester(PlayerController);

	if (Catalog.bBuilding)
	{
		PlayerController->ClientMessage(
			FString(L"Gameplay Effect catalog scan is already running; completion will be reported here."),
			FName(),
			1.f);
		return;
	}

	Catalog.PendingEntries.clear();
	Catalog.PendingEntries.reserve(4096);
	Catalog.NextObjectIndex = 0;
	Catalog.ObjectCount = ObjectCount;
	Catalog.bBuilding = true;

	PlayerController->ClientMessage(
		FString(L"Started the non-blocking Gameplay Effect catalog scan."),
		FName(),
		1.f);
}

static void TickLoadedGameplayEffectCatalogForCommand()
{
	auto& Catalog = GLoadedGameplayEffectCommandCatalog;

	if (!Catalog.bBuilding)
		return;

	auto CurrentWorld = UWorld::GetWorld();

	if (!CurrentWorld || Catalog.World.Get() != CurrentWorld)
	{
		Catalog = FLoadedGameplayEffectCommandCatalog();
		return;
	}

	auto GameplayEffectClass = UGameplayEffect::StaticClass();

	if (!GameplayEffectClass)
	{
		Catalog.PendingEntries.clear();
		Catalog.ObjectCount = Catalog.NextObjectIndex;
		CompleteLoadedGameplayEffectCatalogForCommand();
		return;
	}

	constexpr int32 MaxObjectsPerTick = 2048;
	constexpr auto TimeBudget = std::chrono::microseconds(1000);
	const auto TickStart = std::chrono::steady_clock::now();
	const int32 SkipFlags = Offsets::bEncryptedObjects
		? 0x10200000
		: 0x20;
	int32 ObjectsProcessed = 0;

	while (Catalog.NextObjectIndex < Catalog.ObjectCount &&
		ObjectsProcessed < MaxObjectsPerTick)
	{
		if (ObjectsProcessed > 0 &&
			(ObjectsProcessed & 0x3f) == 0 &&
			std::chrono::steady_clock::now() - TickStart >= TimeBudget)
		{
			break;
		}

		const int32 ObjectIndex = Catalog.NextObjectIndex++;
		ObjectsProcessed++;

		auto Item = SDK::TUObjectArray::GetItemByIndex(ObjectIndex);

		if (!Item || (Item->GetFlags() & SkipFlags) != 0)
			continue;

		auto Object = Item->GetObject();

		if (!Object || !SDK::MemReadable((void*)Object, 0x40) ||
			!Object->Class || !SDK::MemReadable(Object->Class, 0x40) ||
			!Object->IsA(GameplayEffectClass))
		{
			continue;
		}

		FLoadedGameplayEffectCommandEntry Entry;
		Entry.GameplayEffectClass =
			TWeakObjectPtr<UClass>(Object->Class);
		Entry.EffectName = Object->Name.ToString().c_str();
		Entry.EffectClassName =
			Object->Class->GetName().ToString().c_str();
		Entry.NormalizedEffectName =
			NormalizePlayerCommandString(Entry.EffectName);
		Entry.NormalizedEffectClassName =
			NormalizePlayerCommandString(Entry.EffectClassName);
		Entry.NormalizedShortClassName =
			Entry.NormalizedEffectClassName;

		if (Entry.NormalizedShortClassName.ends_with("_c"))
		{
			Entry.NormalizedShortClassName.resize(
				Entry.NormalizedShortClassName.size() - 2);
		}

		Catalog.PendingEntries.emplace_back(std::move(Entry));
	}

	if (Catalog.NextObjectIndex >= Catalog.ObjectCount)
		CompleteLoadedGameplayEffectCatalogForCommand();
}

// Resolves the forms people commonly copy from dump output:
//   Default__GE_Foo_C, GE_Foo_C, and GE_Foo.
// Full object paths continue to work, including shortened blueprint paths such
// as /Game/Effects/GE_Foo and /Game/Effects/GE_Foo.GE_Foo.
static const UClass* FindGameplayEffectClassByCommandArg(const std::string& EffectArg)
{
	auto TrimmedArg = TrimPlayerCommandString(EffectArg);

	if (TrimmedArg.empty())
		return nullptr;

	auto NormalizedArg = NormalizePlayerCommandString(TrimmedArg);

	auto TryFindClassByPath = [](const std::string& Value) -> const UClass*
	{
		if (Value.empty() ||
			(Value.find('/') == std::string::npos &&
				Value.find('.') == std::string::npos))
		{
			return nullptr;
		}

		UEAllocatedWString WideValue(Value.begin(), Value.end());
		auto Class = FindObject<UClass>(WideValue.c_str());
		return IsGameplayEffectClassForCommand(Class) ? Class : nullptr;
	};

	if (TrimmedArg.find('/') != std::string::npos)
	{
		auto DotIndex = TrimmedArg.rfind('.');

		if (DotIndex == std::string::npos)
		{
			auto LastSlashIndex = TrimmedArg.find_last_of('/');
			auto AssetName = LastSlashIndex == std::string::npos
				? TrimmedArg
				: TrimmedArg.substr(LastSlashIndex + 1);

			if (auto Class = TryFindClassByPath(TrimmedArg + "." + AssetName + "_C"))
				return Class;

			// Keep the raw path as a fallback, but only after the canonical
			// generated-class path so an incomplete package path is not sent
			// to StaticLoadObject first.
			if (auto Class = TryFindClassByPath(TrimmedArg))
				return Class;
		}
		else if (NormalizedArg.ends_with("_c") || NormalizedArg.starts_with("/script/"))
		{
			if (auto Class = TryFindClassByPath(TrimmedArg))
				return Class;
		}
		else
		{
			if (auto Class = TryFindClassByPath(TrimmedArg + "_C"))
				return Class;

			if (auto Class = TryFindClassByPath(TrimmedArg))
				return Class;
		}
	}
	else if (TrimmedArg.find('.') != std::string::npos)
	{
		if (auto Class = TryFindClassByPath(TrimmedArg))
			return Class;

		if (!NormalizedArg.ends_with("_c"))
			if (auto Class = TryFindClassByPath(TrimmedArg + "_C"))
				return Class;
	}

	const UClass* ObservedGameplayEffectClass = nullptr;
	if (TryFindObservedGameplayEffectClassForCommand(
		NormalizedArg, ObservedGameplayEffectClass))
		return ObservedGameplayEffectClass;

	auto GameplayEffects = GetLoadedGameplayEffectCatalogForCommand();

	if (!GameplayEffects)
		return nullptr;

	// The catalog snapshots normalized names while it is built, so resolving a
	// short name never has to rescan GObjects or repeatedly convert FNames.
	for (auto& Effect : *GameplayEffects)
	{
		if (NormalizedArg != Effect.NormalizedEffectName &&
			NormalizedArg != Effect.NormalizedEffectClassName &&
			NormalizedArg != Effect.NormalizedShortClassName)
		{
			continue;
		}

		auto EffectClass = Effect.GameplayEffectClass.Get();
		return IsGameplayEffectClassForCommand(EffectClass)
			? EffectClass
			: nullptr;
	}

	return nullptr;
}

struct FRemoveGameplayEffectsCommandResult
{
	int InitialEffectCount = 0;
	int RemainingEffectCount = 0;
	bool bRemovalApiAvailable = true;
};

static FRemoveGameplayEffectsCommandResult RemoveAllGameplayEffectsForCommand(
	UAbilitySystemComponent* AbilitySystemComponent)
{
	FRemoveGameplayEffectsCommandResult Result;

	if (!AbilitySystemComponent)
	{
		Result.bRemovalApiAvailable = false;
		return Result;
	}

	auto& Effects =
		AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal;
	const int InitialEffectCount = Effects.Num();
	Result.InitialEffectCount = InitialEffectCount;
	Result.RemainingEffectCount = InitialEffectCount;

	if (InitialEffectCount == 0)
		return Result;

	if (InitialEffectCount < 0 || InitialEffectCount > 100000)
	{
		Result.bRemovalApiAvailable = false;
		return Result;
	}

	auto RemoveActiveEffectFn =
		AbilitySystemComponent->GetFunction("RemoveActiveGameplayEffect");

	if (!RemoveActiveEffectFn)
	{
		Result.bRemovalApiAvailable = false;
		return Result;
	}

	std::vector<FActiveGameplayEffectHandle> Handles;
	Handles.reserve(InitialEffectCount);

	for (int Index = 0; Index < InitialEffectCount; Index++)
	{
		auto& Effect = Effects.Get(Index, FActiveGameplayEffect::Size());
		auto Handle =
			*(FActiveGameplayEffectHandle*)((char*)&Effect + 0xc);

		if (Handle.Handle > 0)
			Handles.push_back(Handle);
	}

	// Always use the GAS removal API so attributes, tags, cues, delegates, and
	// replication are unwound with the effect. Clearing GameplayEffects_Internal
	// directly leaves those systems in a corrupt half-removed state.
	for (auto& Handle : Handles)
	{
		AbilitySystemComponent->Call<bool>(
			RemoveActiveEffectFn,
			Handle,
			-1);
	}

	const int RemainingEffectCount = Effects.Num();

	if (RemainingEffectCount < 0 || RemainingEffectCount > 100000)
		Result.bRemovalApiAvailable = false;
	else
		Result.RemainingEffectCount = RemainingEffectCount;

	return Result;
}

static std::wstring GetRemoveGameplayEffectsCommandMessage(
	const FRemoveGameplayEffectsCommandResult& Result)
{
	if (!Result.bRemovalApiAvailable)
		return L"Gameplay Effect removal is unavailable on this version.";

	if (Result.InitialEffectCount == 0)
		return L"No active Gameplay Effects to remove.";

	if (Result.RemainingEffectCount == 0)
	{
		return L"Removed " +
			std::to_wstring(Result.InitialEffectCount) +
			L" active Gameplay Effect(s)!";
	}

	const int RemovedEffectCount =
		Result.InitialEffectCount > Result.RemainingEffectCount
			? Result.InitialEffectCount - Result.RemainingEffectCount
			: 0;

	if (RemovedEffectCount > 0)
	{
		return L"Removed " +
			std::to_wstring(RemovedEffectCount) +
			L" active Gameplay Effect(s), but " +
			std::to_wstring(Result.RemainingEffectCount) +
			L" could not be removed.";
	}

	return L"Could not remove the active Gameplay Effects.";
}

struct FGameplayEffectOutputEntry
{
	TWeakObjectPtr<UClass> GameplayEffectClass;
	FName EffectName;
	FName EffectClassName;
	bool bHasLevel = false;
	float Level = 0.f;
	bool bHasStackCount = false;
	int32 StackCount = 0;
};

enum class EGameplayEffectOutputChange : uint8
{
	Applied,
	Updated,
	Removed
};

struct FGameplayEffectOutputChange
{
	EGameplayEffectOutputChange Change;
	int32 Handle = 0;
	FGameplayEffectOutputEntry Entry;
};

struct FGameplayEffectOutputState
{
	TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;
	std::unordered_map<int32, FGameplayEffectOutputEntry> ActiveEffects;
	std::vector<FGameplayEffectOutputChange> PendingChanges;
	float SecondsUntilPoll = 0.f;
	int32 SuppressedChangeCount = 0;
	int32 ConsecutiveSnapshotFailures = 0;
	bool bHasBaseline = false;
};

static constexpr int32 GameplayEffectOutputMaxSnapshotEffects = 1024;
static constexpr size_t GameplayEffectOutputMaxPendingChanges = 32;
static constexpr size_t GameplayEffectOutputMaxLinesPerMessage = 8;
static constexpr size_t GameplayEffectOutputMaxMessageCharacters = 1024;
static constexpr float GameplayEffectOutputPollIntervalSeconds = 0.1f;
static constexpr int32 GameplayEffectOutputMaxSnapshotFailures = 3;

static std::unordered_map<AFortPlayerControllerAthena*, FGameplayEffectOutputState>
	GGameplayEffectOutputStates;

static void SendGameplayEffectOutputMessage(
	AFortPlayerControllerAthena* PlayerController,
	const wchar_t* Message)
{
	if (!PlayerController || !Message || !*Message)
		return;

	FString ClientMessageText(Message);
	PlayerController->ClientMessage(
		ClientMessageText,
		FName(),
		1.f);
	ClientMessageText.Free();
}

static UAbilitySystemComponent* GetGameplayEffectOutputAbilitySystem(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!PlayerController ||
		!PlayerController->PlayerState ||
		!SDK::MemReadable(
			PlayerController->PlayerState, sizeof(UObject)))
		return nullptr;

	auto PlayerState =
		PlayerController->PlayerState->Cast<AFortPlayerStateAthena>();

	if (!PlayerState || !PlayerState->HasAbilitySystemComponent())
		return nullptr;

	return PlayerState->AbilitySystemComponent;
}

template <typename T>
static int32 ResolveGameplayEffectOutputStructPropertyOffset(
	const UStruct* Struct,
	const char* PropertyName,
	uint64 PropertyFlags,
	int32 StructSize)
{
	if (!Struct || StructSize < static_cast<int32>(sizeof(T)))
		return -1;

	const uint32 Offset = Struct->GetOffset(PropertyName, PropertyFlags);

	if (Offset == static_cast<uint32>(-1) ||
		Offset > static_cast<uint32>(
			StructSize - static_cast<int32>(sizeof(T))))
	{
		return -1;
	}

	return static_cast<int32>(Offset);
}

static bool CaptureGameplayEffectOutputSnapshot(
	UAbilitySystemComponent* AbilitySystemComponent,
	std::unordered_map<int32, FGameplayEffectOutputEntry>& Snapshot)
{
	Snapshot.clear();

	if (!AbilitySystemComponent ||
		!SDK::MemReadable(AbilitySystemComponent, sizeof(UObject)) ||
		!AbilitySystemComponent->HasActiveGameplayEffects())
	{
		return false;
	}

	auto ActiveGameplayEffectsStruct =
		FActiveGameplayEffectsContainer::StaticStruct();
	auto ActiveGameplayEffectStruct =
		FActiveGameplayEffect::StaticStruct();
	auto GameplayEffectSpecStruct =
		FGameplayEffectSpec::StaticStruct();

	if (!ActiveGameplayEffectsStruct ||
		!ActiveGameplayEffectStruct ||
		!GameplayEffectSpecStruct ||
		!FActiveGameplayEffectsContainer::HasGameplayEffects_Internal() ||
		!FActiveGameplayEffect::HasSpec() ||
		!FGameplayEffectSpec::HasDef())
	{
		return false;
	}

	auto& ActiveGameplayEffects =
		AbilitySystemComponent->ActiveGameplayEffects;

	const int32 ActiveGameplayEffectsSize =
		FActiveGameplayEffectsContainer::Size();
	const int32 EffectsOffset =
		FActiveGameplayEffectsContainer::GameplayEffects_Internal__Offset;

	if (ActiveGameplayEffectsSize < static_cast<int32>(
			sizeof(TArray<FActiveGameplayEffect>)) ||
		EffectsOffset < 0 ||
		EffectsOffset > ActiveGameplayEffectsSize -
			static_cast<int32>(sizeof(TArray<FActiveGameplayEffect>)))
	{
		return false;
	}

	auto& Effects = ActiveGameplayEffects.GameplayEffects_Internal;

	if (!SDK::MemReadable(&Effects, sizeof(Effects)))
		return false;

	const int EffectCount = Effects.Num();
	const int EffectCapacity = Effects.Max();
	const int32 ActiveEffectSize = FActiveGameplayEffect::Size();
	const int32 GameplayEffectSpecSize = FGameplayEffectSpec::Size();
	const int32 GameplayEffectSpecOffset =
		FActiveGameplayEffect::Spec__Offset;
	const int32 GameplayEffectDefinitionOffset =
		FGameplayEffectSpec::Def__Offset;

	if (EffectCount < 0 ||
		EffectCount > GameplayEffectOutputMaxSnapshotEffects ||
		EffectCapacity < EffectCount ||
		EffectCapacity > 100000 ||
		ActiveEffectSize < 0x14 ||
		ActiveEffectSize > 0x1000 ||
		GameplayEffectSpecSize < static_cast<int32>(sizeof(void*)) ||
		GameplayEffectSpecSize > ActiveEffectSize ||
		GameplayEffectSpecOffset < 0 ||
		GameplayEffectSpecOffset >
			ActiveEffectSize - GameplayEffectSpecSize ||
		GameplayEffectDefinitionOffset < 0 ||
		GameplayEffectDefinitionOffset >
			GameplayEffectSpecSize - static_cast<int32>(sizeof(void*)))
	{
		return false;
	}

	if (EffectCount == 0)
		return true;

	const size_t EffectsByteCount =
		static_cast<size_t>(EffectCount) *
		static_cast<size_t>(ActiveEffectSize);

	if (!Effects.GetData() ||
		!SDK::MemReadable(Effects.GetData(), EffectsByteCount))
	{
		return false;
	}

	Snapshot.reserve(EffectCount);
	const bool bCanReadLevel = FGameplayEffectSpec::HasLevel();
	static const int32 StackCountOffset =
		ResolveGameplayEffectOutputStructPropertyOffset<int32>(
			FGameplayEffectSpec::StaticStruct(),
			"StackCount",
			0x80,
			FGameplayEffectSpec::Size());

	for (int Index = 0; Index < EffectCount; Index++)
	{
		auto& Effect = Effects.Get(Index, ActiveEffectSize);
		auto EffectDefinition = Effect.Spec.Def;

		if (!EffectDefinition ||
			!SDK::MemReadable(EffectDefinition, sizeof(UObject)))
			continue;

		auto Handle = *(FActiveGameplayEffectHandle*)((char*)&Effect + 0xc);

		if (Handle.Handle <= 0)
			continue;

		FGameplayEffectOutputEntry Entry;
		Entry.EffectName = EffectDefinition->Name;

		if (EffectDefinition->Class &&
			SDK::MemReadable(EffectDefinition->Class, sizeof(UObject)))
		{
			Entry.GameplayEffectClass =
				TWeakObjectPtr<UClass>(EffectDefinition->Class);
			Entry.EffectClassName = EffectDefinition->Class->Name;
		}

		if (bCanReadLevel &&
			FGameplayEffectSpec::Level__Offset >= 0 &&
			FGameplayEffectSpec::Level__Offset <=
				GameplayEffectSpecSize - static_cast<int32>(sizeof(float)))
		{
			Entry.bHasLevel = true;
			Entry.Level = Effect.Spec.Level;
		}

		if (StackCountOffset >= 0)
		{
			Entry.bHasStackCount = true;
			Entry.StackCount =
				GetFromOffset<int32>(&Effect.Spec, StackCountOffset);
		}

		Snapshot.insert_or_assign(Handle.Handle, std::move(Entry));
	}

	return true;
}

static const wchar_t* GetGameplayEffectOutputChangeName(
	EGameplayEffectOutputChange Change)
{
	switch (Change)
	{
	case EGameplayEffectOutputChange::Applied:
		return L"APPLIED";
	case EGameplayEffectOutputChange::Updated:
		return L"UPDATED";
	case EGameplayEffectOutputChange::Removed:
		return L"REMOVED";
	default:
		return L"UNKNOWN";
	}
}

static std::wstring FormatGameplayEffectOutputChange(
	EGameplayEffectOutputChange Change,
	int32 Handle,
	const FGameplayEffectOutputEntry& Entry)
{
	auto EffectName = Entry.EffectName.IsValid()
		? Entry.EffectName.ToWString()
		: UEAllocatedWString(L"unknown");
	auto EffectClassName = Entry.EffectClassName.IsValid()
		? Entry.EffectClassName.ToWString()
		: UEAllocatedWString(L"unknown");

	wchar_t HeaderBuffer[96];
	swprintf_s(
		HeaderBuffer,
		std::size(HeaderBuffer),
		L"[OutputGE] %ls handle=%d effect=",
		GetGameplayEffectOutputChangeName(Change),
		Handle);

	std::wstring Line = HeaderBuffer;
	Line.append(EffectName.c_str());
	Line.append(L" class=");
	Line.append(EffectClassName.c_str());

	wchar_t DetailBuffer[64];

	if (Entry.bHasLevel)
	{
		swprintf_s(
			DetailBuffer,
			std::size(DetailBuffer),
			L" level=%.3f",
			Entry.Level);
		Line.append(DetailBuffer);
	}

	if (Entry.bHasStackCount)
	{
		swprintf_s(
			DetailBuffer,
			std::size(DetailBuffer),
			L" stacks=%d",
			Entry.StackCount);
		Line.append(DetailBuffer);
	}

	static constexpr size_t MaxLineCharacters = 384;
	if (Line.size() > MaxLineCharacters)
	{
		Line.resize(MaxLineCharacters - 3);
		Line.append(L"...");
	}

	return Line;
}

static void QueueGameplayEffectOutputChange(
	FGameplayEffectOutputState& State,
	EGameplayEffectOutputChange Change,
	int32 Handle,
	const FGameplayEffectOutputEntry& Entry)
{
	RememberObservedGameplayEffectClassForCommand(
		Entry.EffectName,
		Entry.EffectClassName,
		Entry.GameplayEffectClass.Get());

	for (auto Existing = State.PendingChanges.rbegin();
		Existing != State.PendingChanges.rend(); ++Existing)
	{
		if (Existing->Handle != Handle)
			continue;

		if (Existing->Change == EGameplayEffectOutputChange::Applied &&
			Change == EGameplayEffectOutputChange::Updated)
		{
			Existing->Entry = Entry;
			return;
		}

		if (Existing->Change == EGameplayEffectOutputChange::Updated &&
			Change == EGameplayEffectOutputChange::Updated)
		{
			Existing->Entry = Entry;
			return;
		}

		if (Existing->Change == EGameplayEffectOutputChange::Updated &&
			Change == EGameplayEffectOutputChange::Removed)
		{
			Existing->Change = Change;
			Existing->Entry = Entry;
			return;
		}

		break;
	}

	if (State.PendingChanges.size() >=
		GameplayEffectOutputMaxPendingChanges)
	{
		if (Change == EGameplayEffectOutputChange::Applied)
		{
			auto Replace = std::find_if(
				State.PendingChanges.rbegin(),
				State.PendingChanges.rend(),
				[](const FGameplayEffectOutputChange& PendingChange)
				{
					return PendingChange.Change !=
						EGameplayEffectOutputChange::Applied;
				});

			if (Replace != State.PendingChanges.rend())
			{
				*Replace = {Change, Handle, Entry};
				State.SuppressedChangeCount++;
				return;
			}
		}

		State.SuppressedChangeCount++;
		return;
	}

	State.PendingChanges.push_back({Change, Handle, Entry});
}

static void FlushGameplayEffectOutputChanges(
	AFortPlayerControllerAthena* PlayerController,
	FGameplayEffectOutputState& State)
{
	if (!PlayerController)
		return;

	std::wstring Message;
	size_t EmittedLineCount = 0;
	int32 SuppressedChangeCount = State.SuppressedChangeCount;

	const EGameplayEffectOutputChange ChangeOrder[] =
	{
		EGameplayEffectOutputChange::Applied,
		EGameplayEffectOutputChange::Updated,
		EGameplayEffectOutputChange::Removed
	};

	for (auto ChangeType : ChangeOrder)
	{
		for (const auto& PendingChange : State.PendingChanges)
		{
			if (PendingChange.Change != ChangeType)
				continue;

			if (EmittedLineCount >=
				GameplayEffectOutputMaxLinesPerMessage)
			{
				SuppressedChangeCount++;
				continue;
			}

			auto Line = FormatGameplayEffectOutputChange(
				PendingChange.Change,
				PendingChange.Handle,
				PendingChange.Entry);
			const size_t SeparatorLength = Message.empty() ? 0 : 1;
			static constexpr size_t SuppressionSummaryReserve = 72;

			if (Message.size() + SeparatorLength + Line.size() >
					GameplayEffectOutputMaxMessageCharacters -
						SuppressionSummaryReserve)
			{
				SuppressedChangeCount++;
				continue;
			}

			if (!Message.empty())
				Message.push_back(L'\n');

			Message.append(Line);
			EmittedLineCount++;
		}
	}

	if (SuppressedChangeCount > 0)
	{
		wchar_t SummaryBuffer[72];
		swprintf_s(
			SummaryBuffer,
			std::size(SummaryBuffer),
			L"[OutputGE] %d additional change(s) suppressed.",
			SuppressedChangeCount);

		if (!Message.empty())
			Message.push_back(L'\n');

		Message.append(SummaryBuffer);
	}

	State.PendingChanges.clear();
	State.SuppressedChangeCount = 0;

	if (!Message.empty())
		SendGameplayEffectOutputMessage(
			PlayerController, Message.c_str());
}

static bool HasGameplayEffectOutputFloatChanged(
	bool bPreviousHasValue,
	float PreviousValue,
	bool bCurrentHasValue,
	float CurrentValue)
{
	if (bPreviousHasValue != bCurrentHasValue)
		return true;

	if (!bCurrentHasValue)
		return false;

	const bool bPreviousValueFinite =
		std::isfinite((double)PreviousValue);
	const bool bCurrentValueFinite =
		std::isfinite((double)CurrentValue);

	if (bPreviousValueFinite != bCurrentValueFinite)
		return true;

	return bCurrentValueFinite &&
		std::abs(CurrentValue - PreviousValue) > 0.001f;
}

static bool HasGameplayEffectOutputEntryChanged(
	const FGameplayEffectOutputEntry& Previous,
	const FGameplayEffectOutputEntry& Current)
{
	if (Previous.EffectName != Current.EffectName ||
		Previous.EffectClassName != Current.EffectClassName ||
		Previous.bHasStackCount != Current.bHasStackCount ||
		(Previous.bHasStackCount &&
			Previous.StackCount != Current.StackCount))
	{
		return true;
	}

	if (HasGameplayEffectOutputFloatChanged(
			Previous.bHasLevel,
			Previous.Level,
			Current.bHasLevel,
			Current.Level))
	{
		return true;
	}

	return false;
}

static bool ToggleGameplayEffectOutputForCommand(
	AFortPlayerControllerAthena* PlayerController)
{
	auto Existing = GGameplayEffectOutputStates.find(PlayerController);

	if (Existing != GGameplayEffectOutputStates.end())
	{
		// A raw address can be reused after travel. Only treat this as the same
		// toggle when its weak object identity still resolves to the requester.
		if (Existing->second.Controller.Get() == PlayerController)
		{
			GGameplayEffectOutputStates.erase(Existing);
			return false;
		}

		GGameplayEffectOutputStates.erase(Existing);
	}

	FGameplayEffectOutputState State;
	State.Controller =
		TWeakObjectPtr<AFortPlayerControllerAthena>(PlayerController);
	State.World = TWeakObjectPtr<UWorld>(UWorld::GetWorld());

	auto AbilitySystemComponent =
		GetGameplayEffectOutputAbilitySystem(PlayerController);

	if (AbilitySystemComponent)
	{
		std::unordered_map<int32, FGameplayEffectOutputEntry> Baseline;

		if (CaptureGameplayEffectOutputSnapshot(
			AbilitySystemComponent, Baseline))
		{
			State.AbilitySystemComponent =
				TWeakObjectPtr<UAbilitySystemComponent>(
					AbilitySystemComponent);
			State.ActiveEffects = std::move(Baseline);
			State.bHasBaseline = true;
		}
	}

	GGameplayEffectOutputStates.emplace(PlayerController, std::move(State));
	return true;
}

static void TickGameplayEffectOutputForCommand(float DeltaSeconds)
{
	if (GGameplayEffectOutputStates.empty())
		return;

	auto CurrentWorld = UWorld::GetWorld();
	const float SafeDeltaSeconds =
		std::isfinite((double)DeltaSeconds) && DeltaSeconds > 0.f
			? (std::min)(DeltaSeconds, 1.f)
			: 0.f;

	for (auto It = GGameplayEffectOutputStates.begin();
		It != GGameplayEffectOutputStates.end();)
	{
		auto& State = It->second;
		auto PlayerController = State.Controller.Get();

		if (!PlayerController ||
			PlayerController != It->first ||
			State.World.Get() != CurrentWorld)
		{
			It = GGameplayEffectOutputStates.erase(It);
			continue;
		}

		State.SecondsUntilPoll -= SafeDeltaSeconds;

		if (State.SecondsUntilPoll > 0.f)
		{
			++It;
			continue;
		}

		State.SecondsUntilPoll =
			GameplayEffectOutputPollIntervalSeconds;

		auto AbilitySystemComponent =
			GetGameplayEffectOutputAbilitySystem(PlayerController);

		if (!AbilitySystemComponent)
		{
			State.AbilitySystemComponent =
				TWeakObjectPtr<UAbilitySystemComponent>();
			State.ActiveEffects.clear();
			State.PendingChanges.clear();
			State.SuppressedChangeCount = 0;
			State.ConsecutiveSnapshotFailures = 0;
			State.bHasBaseline = false;
			++It;
			continue;
		}

		if (!State.bHasBaseline ||
			State.AbilitySystemComponent.Get() != AbilitySystemComponent)
		{
			std::unordered_map<int32, FGameplayEffectOutputEntry> Baseline;

			if (CaptureGameplayEffectOutputSnapshot(
				AbilitySystemComponent, Baseline))
			{
				State.AbilitySystemComponent =
					TWeakObjectPtr<UAbilitySystemComponent>(
						AbilitySystemComponent);
				State.ActiveEffects = std::move(Baseline);
				State.PendingChanges.clear();
				State.SuppressedChangeCount = 0;
				State.ConsecutiveSnapshotFailures = 0;
				State.bHasBaseline = true;
			}
			else
			{
				State.ConsecutiveSnapshotFailures++;
			}

			if (State.ConsecutiveSnapshotFailures >=
				GameplayEffectOutputMaxSnapshotFailures)
			{
				SendGameplayEffectOutputMessage(
					PlayerController,
					L"Active Gameplay Effect output disabled because this version's effect data could not be read safely.");
				It = GGameplayEffectOutputStates.erase(It);
				continue;
			}

			++It;
			continue;
		}

		std::unordered_map<int32, FGameplayEffectOutputEntry>
			CurrentEffects;

		if (!CaptureGameplayEffectOutputSnapshot(
			AbilitySystemComponent, CurrentEffects))
		{
			State.ConsecutiveSnapshotFailures++;

			if (State.ConsecutiveSnapshotFailures >=
				GameplayEffectOutputMaxSnapshotFailures)
			{
				SendGameplayEffectOutputMessage(
					PlayerController,
					L"Active Gameplay Effect output disabled because this version's effect data could not be read safely.");
				It = GGameplayEffectOutputStates.erase(It);
				continue;
			}

			++It;
			continue;
		}

		State.ConsecutiveSnapshotFailures = 0;

		for (auto& [Handle, CurrentEntry] : CurrentEffects)
		{
			auto Previous = State.ActiveEffects.find(Handle);

			if (Previous == State.ActiveEffects.end())
			{
				QueueGameplayEffectOutputChange(
					State,
					EGameplayEffectOutputChange::Applied,
					Handle,
					CurrentEntry);
			}
			else if (HasGameplayEffectOutputEntryChanged(
				Previous->second, CurrentEntry))
			{
				QueueGameplayEffectOutputChange(
					State,
					EGameplayEffectOutputChange::Updated,
					Handle,
					CurrentEntry);
			}
		}

		for (auto& [Handle, PreviousEntry] : State.ActiveEffects)
		{
			if (!CurrentEffects.contains(Handle))
			{
				QueueGameplayEffectOutputChange(
					State,
					EGameplayEffectOutputChange::Removed,
					Handle,
					PreviousEntry);
			}
		}

		State.ActiveEffects = std::move(CurrentEffects);
		FlushGameplayEffectOutputChanges(
			PlayerController, State);

		++It;
	}
}

// Collects every loaded emote definition. Used by "emoteall" when no specific
// emote was given, so the random pick is drawn from whatever the build has
// actually loaded instead of a hardcoded list.
static std::vector<UAthenaDanceItemDefinition*> GatherLoadedEmotesForCommand()
{
	std::vector<UAthenaDanceItemDefinition*> Emotes;

	auto DanceClass = UAthenaDanceItemDefinition::StaticClass();

	if (!DanceClass)
		return Emotes;

	for (int i = 0; i < TUObjectArray::Num(); i++)
	{
		auto Object = const_cast<UObject*>(TUObjectArray::GetObjectByIndex(i));

		if (!Object || !Object->IsA(DanceClass))
			continue;

		Emotes.push_back((UAthenaDanceItemDefinition*)Object);
	}

	return Emotes;
}

// Resolves an emote by short name (EID_Foo) or full path, matching the lookup
// order "botemote" already uses.
static UAthenaDanceItemDefinition* FindEmoteByCommandArg(const std::string& Arg)
{
	if (Arg.empty())
		return nullptr;

	auto Wide = UEAllocatedWString(Arg.begin(), Arg.end());

	auto Emote = const_cast<UAthenaDanceItemDefinition*>(FindObject<UAthenaDanceItemDefinition>(Wide.c_str()));

	if (!Emote)
		Emote = const_cast<UAthenaDanceItemDefinition*>(TUObjectArray::FindObject<UAthenaDanceItemDefinition>(Arg.c_str()));

	if (!Emote)
	{
		UEAllocatedWString FullPath = L"/Game/Athena/Items/Cosmetics/Dances/" + Wide + L"." + Wide;
		Emote = const_cast<UAthenaDanceItemDefinition*>(FindObject<UAthenaDanceItemDefinition>(FullPath.c_str()));
	}

	return Emote;
}

// Finds a player (real or bot) by case-insensitive player-name substring.
// Reports ambiguity rather than silently picking the first match.
// bAllowRequester lets a command match the caller as well. possess needs it -
// "possess cipher" when you ARE cipher has to find you, and an exact match on
// your own name must win over a partial match on "cipher2".
static AFortPlayerControllerAthena* FindPlayerByNameSubstringForCommand(AFortGameMode* GameMode, const std::string& Name, AFortPlayerControllerAthena* Requester, std::string& OutMatchedName, bool& bOutAmbiguous, bool bAllowRequester = false)
{
	bOutAmbiguous = false;

	if (!GameMode || Name.empty())
		return nullptr;

	auto Needle = NormalizePlayerCommandString(Name);

	AFortPlayerControllerAthena* ExactMatch = nullptr;
	std::string ExactMatchName;
	std::vector<std::pair<AFortPlayerControllerAthena*, std::string>> PartialMatches;

	for (auto& Player : GameMode->AlivePlayers)
	{
		auto PC = (AFortPlayerControllerAthena*)Player;

		if (!PC || (!bAllowRequester && PC == Requester) || !PC->PlayerState)
			continue;

		auto PlayerState = (AFortPlayerStateAthena*)PC->PlayerState;
		std::string PlayerName = PlayerState->GetPlayerName().ToString().c_str();

		if (PlayerName.empty())
			continue;

		auto Haystack = NormalizePlayerCommandString(PlayerName);

		if (Haystack == Needle)
		{
			ExactMatch = PC;
			ExactMatchName = PlayerName;
			break;
		}

		if (Haystack.find(Needle) != std::string::npos)
			PartialMatches.push_back({ PC, PlayerName });
	}

	if (ExactMatch)
	{
		OutMatchedName = ExactMatchName;
		return ExactMatch;
	}

	if (PartialMatches.size() == 1)
	{
		OutMatchedName = PartialMatches[0].second;
		return PartialMatches[0].first;
	}

	if (PartialMatches.size() > 1)
		bOutAmbiguous = true;

	return nullptr;
}

// Player pawns only - deliberately NOT the generic "Pawn" class.
//
// Enumerating every Pawn sweeps up vehicles, AI and spectator pawns whose
// classes do not have the properties AFortPlayerPawnAthena declares. That
// matters more than it looks: DEFINE_PROP caches its resolved offset in a
// static keyed on the declaring class, so the FIRST object it is read from
// decides the offset for every later read. Reading ->Controller off a
// foreign pawn class resolves to -1 and poisons the cache permanently,
// after which every access lands at base + 0xFFFFFFFF.
//
// The index printed by "dumppawns" is the one "possess <index>" takes, so
// both must enumerate identically.
static void GatherAllPawnsForCommand(TArray<AActor*>& OutPawns)
{
	static auto PawnClass = FindClass("Pawn");

	if (!PawnClass)
		return;

	Utils::GetAll<AActor>(PawnClass, OutPawns);
}

// Guard for every AFortPlayerPawnAthena-declared property read below.
//
// This matters more than it looks: DEFINE_PROP caches its resolved offset in a
// static keyed on the DECLARING class, and the first object it is read from
// decides that offset forever. Reading ->Controller or ->PlayerState off a
// vehicle or AI pawn resolves to -1 and poisons the cache permanently, after
// which every later access lands at base + 0xFFFFFFFF. That was the possess
// crash. The list can therefore contain any pawn, as long as nothing reads a
// player-pawn property without checking this first.
static bool IsFortPlayerPawnForCommand(AActor* Actor)
{
	return Actor && Actor->IsA(AFortPlayerPawnAthena::StaticClass());
}

// True only for real player controllers. Object/AI pawns carry an AI controller
// (or none), and treating those as AFortPlayerControllerAthena - calling the
// spectate/view-target path on them - is not safe. The Controller property
// itself is a base APawn field, so reading it off any pawn resolves to the same
// offset and does not poison the cache the way a Fortnite-specific field would.
static bool IsPlayerControllerForCommand(AActor* Controller)
{
	static auto PlayerControllerClass = FindClass("PlayerController");
	return Controller && PlayerControllerClass && Controller->IsA(PlayerControllerClass);
}

// Matches on the pawn's object name (what dumppawns prints), unlike the
// player-name matching used by swap - possess is a debugging tool and object
// names are what you actually have in front of you.
static AActor* FindPawnByNameSubstringForCommand(const std::string& Name, std::string& OutMatchedName, bool& bOutAmbiguous)
{
	bOutAmbiguous = false;

	if (Name.empty())
		return nullptr;

	auto Needle = NormalizePlayerCommandString(Name);

	TArray<AActor*> Pawns;
	GatherAllPawnsForCommand(Pawns);

	AActor* ExactMatch = nullptr;
	std::string ExactMatchName;
	std::vector<std::pair<AActor*, std::string>> PartialMatches;

	for (auto& Pawn : Pawns)
	{
		if (!Pawn)
			continue;

		std::string PawnName = Pawn->Name.ToString().c_str();
		auto Haystack = NormalizePlayerCommandString(PawnName);

		if (Haystack == Needle)
		{
			ExactMatch = Pawn;
			ExactMatchName = PawnName;
			break;
		}

		if (Haystack.find(Needle) != std::string::npos)
			PartialMatches.push_back({ Pawn, PawnName });
	}

	Pawns.Free();

	if (ExactMatch)
	{
		OutMatchedName = ExactMatchName;
		return ExactMatch;
	}

	if (PartialMatches.size() == 1)
	{
		OutMatchedName = PartialMatches[0].second;
		return PartialMatches[0].first;
	}

	if (PartialMatches.size() > 1)
		bOutAmbiguous = true;

	return nullptr;
}

// Forces a (possibly remote) client's camera onto an actor via the client RPC.
//
// SetViewTargetWithBlend acts on the server-side camera manager and does not
// reliably push the change down to a remote client. ClientSetViewTarget is a
// client RPC: called on the server against a controller owned by a client
// connection, it is delivered to that client and runs there, which is what
// actually moves their camera.
//
// The params are [AActor* A][FViewTargetTransitionParams]. A zeroed transition
// struct means BlendTime 0 - an instant cut. ProcessEvent copies only the
// function's real ParmsSize, so an over-sized zeroed buffer is safe.
static bool ClientForceViewTarget(
	AFortPlayerControllerAthena* PC, AActor* Target)
{
	if (!PC || !Target)
		return false;

	auto Fn = PC->GetFunction("ClientSetViewTarget");

	if (!Fn)
		return false;

	const auto ReflectedParams = Fn->GetParamsNamed();
	// FN32 relocates/encrypts UStruct::PropertiesSize and FProperty element
	// sizes. Parameter names and decrypted offsets remain usable, while the
	// reflected total size does not; mirror UObject::Call's fixed safe buffer.
	const bool bFN32ReflectionLayout =
		VersionInfo.FortniteVersion >= 32.00;
	const uint32 ParamsSize =
		bFN32ReflectionLayout
			? 0x1000
			: ReflectedParams.Size > 0
			? ReflectedParams.Size
			: 0x40;
	if (ParamsSize < sizeof(AActor*) ||
		ParamsSize > 0x1000)
	{
		return false;
	}

	std::vector<uint8> Params(ParamsSize, 0);
	int32 TargetOffset = -1;
	for (const auto& Parameter :
		ReflectedParams.NameOffsetMap)
	{
		if (Parameter.Name == "ReturnValue")
			continue;

		const bool bKnownTargetName =
			Parameter.Name == "A" ||
			Parameter.Name == "NewViewTarget" ||
			Parameter.Name == "ViewTarget" ||
			Parameter.Name == "Target" ||
			Parameter.Name.find("ViewTarget") !=
				UEAllocatedString::npos;
		if (bKnownTargetName &&
			Parameter.Offset + sizeof(AActor*) <=
				Params.size())
		{
			TargetOffset =
				static_cast<int32>(Parameter.Offset);
			break;
		}
	}

	// Legacy layouts consistently place the actor at offset zero, but prefer
	// reflection and use this only when parameter names are unavailable.
	if (TargetOffset < 0 &&
		!bFN32ReflectionLayout)
	{
		for (const auto& Parameter :
			ReflectedParams.NameOffsetMap)
		{
			if (Parameter.Name != "ReturnValue" &&
				Parameter.ElementSize == sizeof(AActor*) &&
				Parameter.Offset + sizeof(AActor*) <=
					Params.size())
			{
				TargetOffset =
					static_cast<int32>(Parameter.Offset);
				break;
			}
		}
	}
	if (TargetOffset < 0 &&
		ReflectedParams.NameOffsetMap.empty())
	{
		TargetOffset = 0;
	}
	if (TargetOffset < 0)
		return false;

	memcpy(
		Params.data() + TargetOffset,
		&Target, sizeof(Target));

	static std::unordered_set<UFunction*>
		LoggedClientSetViewTargetFunctions;
	if (LoggedClientSetViewTargetFunctions.insert(Fn).second)
	{
		SDK::DbgLog(
			"[Spectating] ClientSetViewTarget params "
			"size=0x%x targetOffset=0x%x count=%zu FN=%.2f\n",
			(unsigned)ParamsSize,
			(unsigned)TargetOffset,
			ReflectedParams.NameOffsetMap.size(),
			VersionInfo.FortniteVersion);
	}

	PC->ProcessEvent(Fn, Params.data());
	return true;
}

// Cleans up the state a freshly possessed pawn is left in.
//
//   Float instead of walk: possession lands the pawn in a non-walking movement
//   mode (leftover skydive/glide, or a disabled-movement state). Forcing
//   MOVE_Walking and clearing the flags puts it back on the ground - which also
//   tends to un-block weapon fire, since firing is suppressed while airborne in
//   a cheat-fly state.
//
//   Zero health bar: re-initializing the ability system does not repopulate the
//   pawn's health/shield attributes, so the bar reads empty. Set them so the
//   body is actually alive and playable.
static void FinalizePossessedPawnForCommand(AFortPlayerControllerAthena* PC, AFortPlayerPawnAthena* Pawn)
{
	(void)PC;

	if (!Pawn)
		return;

	// PUPPET MODE finalize - kept deliberately minimal. Earlier versions of this
	// also rebound the pawn's PlayerState (with a native OnRep_PlayerState) and
	// forced health/shield, but those native calls faulted on early builds where
	// the reflected functions/attributes are laid out differently - that was the
	// possess crash. Movement is all a puppet needs, so that is all this does.

	// Put it on the ground so it walks instead of floating. Guard the UFunction:
	// on a build where it is absent, the multi-arg param marshalling must not run.
	if (Pawn->CharacterMovement && Pawn->CharacterMovement->GetFunction("SetMovementMode"))
	{
		Pawn->CharacterMovement->bCheatFlying = false;
		Pawn->CharacterMovement->SetMovementMode(EMovementMode::MOVE_Walking, 0);
	}

	// Keep the puppet alive. SetHealth is the known-good path (health writes work
	// on these builds); shield is intentionally left alone - it goes through the
	// early-build fallback and is not needed for a movement puppet.
	Pawn->SetHealth(100.f);

	Pawn->ForceNetUpdate();
}

// Turns a player into a spectator locked onto the pawn we took from them, so
// they watch it move under our control.
//
// UnPossess is unavoidable here: while a controller still possesses a pawn, its
// OWN client keeps predicting that pawn locally, and with no input the pawn
// just freezes in place on their screen - it never follows what we do. Freeing
// it makes the pawn a normal replicated actor that animates for them.
//
// The catch UnPossess brings is a stranded camera (view target goes null), so
// SetViewTargetWithBlend then pins their camera onto the pawn. That explicit
// view target is the part the earlier PlayerToSpectateOnDeath-only attempt was
// missing.
static void MakeControllerSpectatePawnForCommand(AFortPlayerControllerAthena* PC, AFortPlayerPawnAthena* Pawn)
{
	if (!PC)
		return;

	PC->UnPossess();

	if (PC->HasPlayerToSpectateOnDeath())
		PC->PlayerToSpectateOnDeath = (AActor*)Pawn;

	PC->ClientGotoState(FName(L"Spectating"));

	// (target, blend time 0, VTBlend_Linear, exp 0, don't lock outgoing)
	PC->SetViewTargetWithBlend((AActor*)Pawn, 0.f, (uint8_t)0, 0.f, false);

	// The one that actually moves a remote client's camera.
	ClientForceViewTarget(PC, (AActor*)Pawn);

	PC->ForceNetUpdate();
}

// Re-equips the pickaxe so the controller has a working held item on its new
// pawn. The inventory lives on the CONTROLLER, not the pawn, so after any
// possession change the held weapon still points at the old body and cannot be
// used. This is the same equip path the "size" command uses after it
// re-possesses - ClientActivateSlot alone does not restore it.
static void ReEquipAfterPossessForCommand(AFortPlayerControllerAthena* PC)
{
	if (!PC || !PC->WorldInventory)
		return;

	auto PickaxeEntry = PC->WorldInventory->Inventory.ReplicatedEntries.Search([](FFortItemEntry& Entry)
		{ return Entry.ItemDefinition && Entry.ItemDefinition->IsA<UFortWeaponMeleeItemDefinition>(); }, FFortItemEntry::Size());

	if (!PickaxeEntry)
		return;

	PC->ServerExecuteInventoryItem(PickaxeEntry->ItemGuid);
	PC->ClientEquipItem(PickaxeEntry->ItemGuid, true);
}

// Puts a controller back in control by SPAWNING A FRESH PAWN, not by
// re-possessing the old one.
//
// Re-attaching to a pawn you previously left never fully re-binds movement and
// input on the client: you cannot jump, cannot use items, and your inventory
// still looks like it is driving the pawn you came from. The "size" command
// hits exactly the same problem and sidesteps it by swapping in a new pawn -
// which is why running "size 1" clears the symptoms. So do what size does.
//
// Deliberately does NOT touch the inventory: WorldInventory lives on the
// controller and survives the pawn swap by itself, and Remove/GiveItem here
// hard-crashes on some builds (see the note in the "size" command).
//
// Returns the new pawn, or nullptr if the spawn failed - in which case the old
// pawn is left untouched rather than destroyed.
static AFortPlayerPawnAthena* RespawnControllerOnFreshPawnForCommand(AFortGameMode* GameMode, AFortPlayerControllerAthena* PC, AFortPlayerPawnAthena* OldPawn)
{
	if (!GameMode || !PC || !OldPawn)
		return nullptr;

	FTransform SpawnTransform = OldPawn->GetTransform();

	// Preserve health/shield so a possession round-trip is not a free heal.
	float SavedHealth = OldPawn->GetHealth();
	float SavedShield = OldPawn->GetShield();

	auto NewPawn = GameMode->SpawnDefaultPawnAtTransform(PC, SpawnTransform);

	if (!NewPawn)
		return nullptr;

	SkipNextPossessRespawn(PC);
	PC->Possess(NewPawn);
	PC->MyFortPawn = NewPawn;

	NewPawn->SetHealth(SavedHealth);
	NewPawn->SetShield(SavedShield);

	OldPawn->K2_DestroyActor();

	// If they were spectating (a displaced owner getting their body back), pull
	// them back into play and put the camera on the new pawn - possessing alone
	// does not clear the spectator view target we set earlier.
	PC->ClientGotoState(FName(L"Playing"));
	PC->SetViewTargetWithBlend((AActor*)NewPawn, 0.f, (uint8_t)0, 0.f, false);
	ClientForceViewTarget(PC, (AActor*)NewPawn);

	ReEquipAfterPossessForCommand(PC);

	PC->OnRep_Pawn();
	PC->ForceNetUpdate();
	NewPawn->ForceNetUpdate();

	return NewPawn;
}

static void SetMatchingNukeTargetVectorProperties(UObject* Object, const FVector& TargetLocation)
{
	if (!Object || !Object->Class)
		return;

	for (const UStruct* Clss = Object->Class; Clss; Clss = Clss->GetSuper())
	{
		auto Prop = VersionInfo.FortniteVersion >= 12.10 ? Clss->GetChildProperties() : Clss->GetChildren();

		for (; Prop; Prop = VersionInfo.FortniteVersion >= 12.10 ? Prop->FField_GetNext() : Prop->GetNext())
		{
			if (GetReflectedPropertyElementSize(Prop) != FVector::Size())
				continue;

			if (!LooksLikeNukeTargetProperty(GetReflectedPropertyName(Prop)))
				continue;

			auto Offset = GetReflectedPropertyOffset(Prop);

			if (Offset == -1)
				continue;

			auto TargetLocationCopy = TargetLocation;
			GetFromOffset<FVector>(Object, Offset) = TargetLocationCopy;
		}
	}
}

static bool FunctionHasSingleNukeInputParam(UFunction* Function, uint32 ExpectedElementSize)
{
	if (!Function)
		return false;

	auto Params = Function->GetParams();
	int InputParamCount = 0;

	for (auto& Param : Params.NameOffsetMap)
	{
		if (((Param.PropertyFlags & 0x100) != 0 && (Param.PropertyFlags & 0x8000000) == 0) || (Param.PropertyFlags & 0x400) != 0)
			continue;

		InputParamCount++;

		if (Param.ElementSize != ExpectedElementSize)
			return false;
	}

	return InputParamCount == 1;
}

template <typename T>
static bool TryCallNukeTargetFunction(UObject* Object, const char* FunctionName, const T& Value)
{
	if (!Object)
		return false;

	auto Function = Object->GetFunction(FunctionName);
	uint32 ExpectedElementSize = std::is_same_v<T, FVector> ? FVector::Size() : sizeof(T);

	if (!FunctionHasSingleNukeInputParam(Function, ExpectedElementSize))
		return false;

	auto ValueCopy = Value;
	Object->Call<void>(Function, ValueCopy);
	return true;
}

static void TryCallNukeTargetFunctions(AActor* Rocket, AFortPlayerPawnAthena* TargetPawn, const FVector& TargetLocation)
{
	if (!Rocket)
		return;

	static const char* VectorFunctions[] =
	{
		"SetTargetLocation",
		"SetTargetPosition",
		"SetDestination",
		"SetDestinationLocation",
		"SetImpactLocation",
		"SetGoalLocation",
		"SetAimLocation",
		"SetHomingTargetLocation",
		"SetTarget"
	};

	for (auto FunctionName : VectorFunctions)
		TryCallNukeTargetFunction<FVector>(Rocket, FunctionName, TargetLocation);

	if (!TargetPawn)
		return;

	static const char* ActorFunctions[] =
	{
		"SetTargetActor",
		"SetTargetPawn",
		"SetHomingTarget",
		"SetLockOnTarget",
		"SetSeekTarget",
		"SetTarget"
	};

	for (auto FunctionName : ActorFunctions)
		TryCallNukeTargetFunction<AActor*>(Rocket, FunctionName, TargetPawn);
}

static constexpr float NukeRocketPlayerDamage = 25.f;
static constexpr float NukeRocketEnvironmentDamage = 1000.f;
static constexpr float NukeRocketDamageRadius = 650.f;

static bool TryCallFloatCommandFunction(UObject* Object, const char* FunctionName, float Value)
{
	if (!Object)
		return false;

	auto Function = Object->GetFunction(FunctionName);

	if (!FunctionHasSingleCommandInputParam(Function, sizeof(float)))
		return false;

	Object->Call<void>(Function, Value);
	return true;
}

static bool TryApplySummonHealth(AActor* Actor, float Health)
{
	if (!Actor)
		return false;

	if (auto Vehicle = Actor->Cast<AFortAthenaVehicle>())
	{
		if (Vehicle->HealthSet)
		{
			auto& VehicleHealth = Vehicle->HealthSet->Health;
			VehicleHealth.CurrentValue = Health;
			VehicleHealth.BaseValue = Health;
			VehicleHealth.UnclampedCurrentValue = Health;
			VehicleHealth.UnclampedBaseValue = Health;
			Vehicle->OnRep_HealthSet();

			if (Health <= 0.f)
				Vehicle->DestroyVehicle();

			return true;
		}
	}

	return TryCallFloatCommandFunction(Actor, "SetHealth", Health);
}

static void ConfigureNukeRocketDamageProperties(AActor* Rocket, bool bDamageEnabled)
{
	if (!Rocket)
		return;

	auto PlayerDamage = bDamageEnabled ? NukeRocketPlayerDamage : 0.f;
	auto EnvironmentDamage = bDamageEnabled ? NukeRocketEnvironmentDamage : 0.f;

	SetReflectedProperty<float>(Rocket, "Damage", PlayerDamage);
	SetReflectedProperty<float>(Rocket, "BaseDamage", PlayerDamage);
	SetReflectedProperty<float>(Rocket, "MaxDamage", PlayerDamage);
	SetReflectedProperty<float>(Rocket, "PlayerDamage", PlayerDamage);
	SetReflectedProperty<float>(Rocket, "DamageRadius", NukeRocketDamageRadius);
	SetReflectedProperty<float>(Rocket, "ExplosionRadius", NukeRocketDamageRadius);
	SetReflectedProperty<float>(Rocket, "OuterExplosionRadius", NukeRocketDamageRadius);
	SetReflectedProperty<float>(Rocket, "InnerExplosionRadius", NukeRocketDamageRadius * 0.5f);
	SetReflectedProperty<float>(Rocket, "EnvDamage", EnvironmentDamage);
	SetReflectedProperty<float>(Rocket, "EnvironmentalDamage", EnvironmentDamage);
	SetReflectedProperty<float>(Rocket, "BuildingDamage", EnvironmentDamage);

	SetReflectedBoolProperty(Rocket, "bCanDamage", bDamageEnabled);
	SetReflectedBoolProperty(Rocket, "bCanDamagePlayers", bDamageEnabled);
	SetReflectedBoolProperty(Rocket, "bDamagePlayers", bDamageEnabled);
	SetReflectedBoolProperty(Rocket, "bCanDamageEnvironment", bDamageEnabled);
	SetReflectedBoolProperty(Rocket, "bDamageEnvironment", bDamageEnabled);
	SetReflectedBoolProperty(Rocket, "bDamageBuildings", bDamageEnabled);
	SetReflectedBoolProperty(Rocket, "bExplodeOnImpact", false);
}

static void ConfigureNukeRocket(AActor* Rocket, AFortPlayerPawnAthena* TargetPawn, const FVector& TargetLocation, const FVector& Velocity, bool bDamageEnabled)
{
	if (!Rocket)
		return;

	if (Rocket->HasbReplicates())
		Rocket->bReplicates = true;
	if (Rocket->HasbAlwaysRelevant())
		Rocket->bAlwaysRelevant = true;
	if (Rocket->HasNetUpdateFrequency())
		Rocket->NetUpdateFrequency = 100.f;

	ConfigureNukeRocketDamageProperties(Rocket, bDamageEnabled);

	SetReflectedProperty<FVector>(Rocket, "Velocity", Velocity);
	SetReflectedProperty<FVector>(Rocket, "InitialVelocity", Velocity);
	SetReflectedProperty<FVector>(Rocket, "LaunchVelocity", Velocity);
	SetReflectedProperty<FVector>(Rocket, "TargetLocation", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "TargetPosition", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "Destination", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "InitialTargetLocation", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "ImpactLocation", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "GoalLocation", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "AimLocation", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "HomingTargetLocation", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "LockOnLocation", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "SeekLocation", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "ReticleTargetLocation", TargetLocation);
	SetReflectedProperty<FVector>(Rocket, "CrosshairTargetLocation", TargetLocation);
	SetMatchingNukeTargetVectorProperties(Rocket, TargetLocation);

	if (TargetPawn)
	{
		SetReflectedProperty<AActor*>(Rocket, "Target", TargetPawn);
		SetReflectedProperty<AActor*>(Rocket, "CurrentTarget", TargetPawn);
		SetReflectedProperty<AActor*>(Rocket, "CachedTarget", TargetPawn);
		SetReflectedProperty<AFortPlayerPawnAthena*>(Rocket, "TargetPawn", TargetPawn);
		SetReflectedProperty<AActor*>(Rocket, "TargetActor", TargetPawn);
		SetReflectedProperty<AActor*>(Rocket, "HomingTarget", TargetPawn);
		SetReflectedProperty<AActor*>(Rocket, "LockOnTarget", TargetPawn);
		SetReflectedProperty<AActor*>(Rocket, "LockedOnTarget", TargetPawn);
		SetReflectedProperty<AActor*>(Rocket, "SeekTarget", TargetPawn);

		if (TargetPawn->HasRootComponent() && TargetPawn->RootComponent)
			SetReflectedProperty<UActorComponent*>(Rocket, "HomingTargetComponent", TargetPawn->RootComponent);
	}

	TryCallNukeTargetFunctions(Rocket, TargetPawn, TargetLocation);

	if (auto SetActorTickEnabledFn = Rocket->GetFunction("SetActorTickEnabled"))
	{
		bool bEnabled = false;

		if (FunctionHasSingleNukeInputParam(SetActorTickEnabledFn, sizeof(bool)))
			Rocket->Call<void>(SetActorTickEnabledFn, bEnabled);
	}

	static auto ProjectileMovementClass = FindClass("ProjectileMovementComponent");
	auto ProjectileMovement = ProjectileMovementClass ? Rocket->GetComponentByClass(ProjectileMovementClass) : nullptr;

	if (ProjectileMovement)
	{
		auto Speed = static_cast<float>(Velocity.Magnitude());

		SetReflectedProperty<FVector>(ProjectileMovement, "Velocity", Velocity);
		SetReflectedProperty<float>(ProjectileMovement, "InitialSpeed", Speed);
		SetReflectedProperty<float>(ProjectileMovement, "MaxSpeed", Speed);
		SetReflectedProperty<float>(ProjectileMovement, "ProjectileGravityScale", 0.f);
		SetReflectedProperty<float>(ProjectileMovement, "HomingAccelerationMagnitude", 0.f);
		SetReflectedBoolProperty(ProjectileMovement, "bRotationFollowsVelocity", true);
		SetReflectedBoolProperty(ProjectileMovement, "bIsHomingProjectile", false);

		SetReflectedProperty<UActorComponent*>(ProjectileMovement, "HomingTargetComponent", nullptr);

		if (auto SetVelocityInLocalSpaceFn = ProjectileMovement->GetFunction("SetVelocityInLocalSpace"))
			ProjectileMovement->Call<void>(SetVelocityInLocalSpaceFn, FVector(Speed, 0.f, 0.f));
	}

	if (Rocket->HasRootComponent() && Rocket->RootComponent)
	{
		if (auto SetPhysicsLinearVelocityFn = Rocket->RootComponent->GetFunction("SetPhysicsLinearVelocity"))
			Rocket->RootComponent->Call<void>(SetPhysicsLinearVelocityFn, Velocity, false, FName(0));
	}

	Rocket->SetLifeSpan(20.f);
	Rocket->ForceNetUpdate();
}

struct FGuidedNukeRocket
{
	AActor* Rocket;
	AActor* InstigatorPawn;
	AFortPlayerControllerAthena* InstigatorController;
	AFortPlayerPawnAthena* TargetPawn;
	FVector TargetLocation;
	float LifeSeconds;
	float Speed;
	bool bDamageEnabled;
	int EnvironmentDamageContextId;
};

static std::vector<FGuidedNukeRocket> GuidedNukeRockets;

struct FNukeEnvironmentDamageContext
{
	int Id;
	int RocketRefs;
	std::vector<ABuildingSMActor*> Buildings;
};

static std::vector<FNukeEnvironmentDamageContext> NukeEnvironmentDamageContexts;
static int NextNukeEnvironmentDamageContextId = 1;

static FNukeEnvironmentDamageContext* FindNukeEnvironmentDamageContext(int ContextId)
{
	if (ContextId <= 0)
		return nullptr;

	for (auto& Context : NukeEnvironmentDamageContexts)
	{
		if (Context.Id == ContextId)
			return &Context;
	}

	return nullptr;
}

static void AddNukeEnvironmentDamageContextRef(int ContextId)
{
	auto Context = FindNukeEnvironmentDamageContext(ContextId);

	if (Context)
		Context->RocketRefs++;
}

static void RemoveUnusedNukeEnvironmentDamageContext(int ContextId)
{
	if (ContextId <= 0)
		return;

	for (size_t i = 0; i < NukeEnvironmentDamageContexts.size(); i++)
	{
		if (NukeEnvironmentDamageContexts[i].Id == ContextId && NukeEnvironmentDamageContexts[i].RocketRefs <= 0)
		{
			NukeEnvironmentDamageContexts.erase(NukeEnvironmentDamageContexts.begin() + i);
			return;
		}
	}
}

static void ReleaseNukeEnvironmentDamageContext(int ContextId)
{
	if (ContextId <= 0)
		return;

	for (size_t i = 0; i < NukeEnvironmentDamageContexts.size(); i++)
	{
		if (NukeEnvironmentDamageContexts[i].Id != ContextId)
			continue;

		NukeEnvironmentDamageContexts[i].RocketRefs--;

		if (NukeEnvironmentDamageContexts[i].RocketRefs <= 0)
			NukeEnvironmentDamageContexts.erase(NukeEnvironmentDamageContexts.begin() + i);

		return;
	}
}

static int CreateNukeEnvironmentDamageContext(const FVector& Center, double Radius)
{
	if (Radius <= 0.0)
		return 0;

	FNukeEnvironmentDamageContext Context{};
	Context.Id = NextNukeEnvironmentDamageContextId++;
	Context.RocketRefs = 0;

	if (NextNukeEnvironmentDamageContextId <= 0)
		NextNukeEnvironmentDamageContextId = 1;

	auto RadiusSquared = Radius * Radius;
	TArray<ABuildingSMActor*> Buildings;
	Utils::GetAll<ABuildingSMActor>(Buildings);

	Context.Buildings.reserve(Buildings.Num());

	for (auto Building : Buildings)
	{
		if (!IsUsableDeathObject(Building) || (Building->HasbDestroyed() && Building->bDestroyed))
			continue;

		if ((Building->K2_GetActorLocation() - Center).SizeSquared() <= RadiusSquared)
			Context.Buildings.push_back(Building);
	}

	Buildings.Free();

	if (Context.Buildings.empty())
		return 0;

	NukeEnvironmentDamageContexts.push_back(std::move(Context));
	return NukeEnvironmentDamageContexts.back().Id;
}

static void RegisterGuidedNukeRocket(AActor* Rocket, AFortPlayerControllerAthena* InstigatorController, AFortPlayerPawnAthena* TargetPawn, const FVector& TargetLocation, float Speed, bool bDamageEnabled, int EnvironmentDamageContextId)
{
	if (!Rocket)
		return;

	AddNukeEnvironmentDamageContextRef(EnvironmentDamageContextId);
	GuidedNukeRockets.push_back({ Rocket, InstigatorController ? InstigatorController->Pawn : nullptr, InstigatorController, TargetPawn, TargetLocation, 8.f, Speed, bDamageEnabled, EnvironmentDamageContextId });
}

static void RemoveGuidedNukeRocketAt(size_t Index)
{
	if (Index >= GuidedNukeRockets.size())
		return;

	ReleaseNukeEnvironmentDamageContext(GuidedNukeRockets[Index].EnvironmentDamageContextId);
	GuidedNukeRockets.erase(GuidedNukeRockets.begin() + Index);
}

static bool IsGuidedNukeObjectValid(const UObject* Object)
{
	return IsUsableDeathObject(Object) && Object->Class;
}

static void TryForceKillPawnForNuke(AFortPlayerPawnAthena* Pawn, AFortPlayerControllerAthena* InstigatorController, AActor* DamageCauser)
{
	if (!Pawn)
		return;

	auto ForceKillFn = Pawn->GetFunction("ForceKill");

	if (ForceKillFn)
	{
		FGameplayTag DeathTag{};
		Pawn->Call<void>(ForceKillFn, DeathTag, InstigatorController, DamageCauser);
		return;
	}

	Pawn->SetHealth(0.f);
	Pawn->ForceNetUpdate();
}

static void ApplyNukePlayerDamageAtLocation(const FVector& ImpactLocation, AActor* InstigatorPawn, AFortPlayerControllerAthena* InstigatorController, AActor* DamageCauser)
{
	TArray<AFortPlayerPawnAthena*> Pawns;
	Utils::GetAll<AFortPlayerPawnAthena>(Pawns);

	for (auto Pawn : Pawns)
	{
		if (!Pawn)
			continue;

		const float InitialHealth = Pawn->GetHealth();
		if (!FPlatformMath::IsFinite(InitialHealth) ||
			InitialHealth <= 0.f ||
			IsPawnDBNOForSpectating(Pawn) ||
			(Pawn->HasbIsDying() && Pawn->bIsDying) ||
			(Pawn->HasbPlayedDying() &&
				Pawn->bPlayedDying) ||
			(Pawn->HasbIsHiddenForDeath() &&
				Pawn->bIsHiddenForDeath))
		{
			continue;
		}

		if (Pawn->HasbCanBeDamaged() && !Pawn->bCanBeDamaged)
			continue;
		if (AFortPlayerPawnAthena::
				HasFullHealthGodMode(Pawn))
		{
			continue;
		}

		if (Pawn->K2_GetActorLocation().GetDistanceTo(ImpactLocation) > NukeRocketDamageRadius)
			continue;

		auto RemainingDamage = NukeRocketPlayerDamage;
		auto Shield = Pawn->GetShield();

		if (Shield > 0.f)
		{
			auto ShieldDamage = Shield < RemainingDamage ? Shield : RemainingDamage;
			Pawn->SetShield(Shield - ShieldDamage);
			RemainingDamage -= ShieldDamage;
		}

		if (RemainingDamage > 0.f)
		{
			auto Health = Pawn->GetHealth();

			if (Health <= RemainingDamage)
			{
				if (AFortPlayerPawnAthena::
						HasMinimumHealthGodMode(Pawn))
				{
					Pawn->SetHealth(1.f);
				}
				else
				{
					TryForceKillPawnForNuke(
						Pawn, InstigatorController,
						DamageCauser);
				}
			}
			else
				Pawn->SetHealth(Health - RemainingDamage);
		}

		Pawn->ForceNetUpdate();
	}

	Pawns.Free();
}

static void ApplyNukeEnvironmentDamageToBuilding(ABuildingSMActor* Building, float Damage)
{
	if (!Building || Damage <= 0.f)
		return;

	auto RemainingHealth = Building->GetHealth() - Damage;

	if (RemainingHealth <= 0.f)
		Building->SilentDie(true);
	else if (TryCallFloatCommandFunction(Building, "SetHealth", RemainingHealth))
		Building->ForceNetUpdate();
	else
		Building->K2_DestroyActor();
}

static void ApplyNukeEnvironmentDamageAtLocation(const FVector& ImpactLocation, int EnvironmentDamageContextId)
{
	auto Context = FindNukeEnvironmentDamageContext(EnvironmentDamageContextId);

	if (!Context)
		return;

	const auto RadiusSquared = static_cast<double>(NukeRocketDamageRadius) * static_cast<double>(NukeRocketDamageRadius);

	for (auto Building : Context->Buildings)
	{
		if (!IsUsableDeathObject(Building) || (Building->HasbDestroyed() && Building->bDestroyed))
			continue;

		if ((Building->K2_GetActorLocation() - ImpactLocation).SizeSquared() > RadiusSquared)
			continue;

		ApplyNukeEnvironmentDamageToBuilding(Building, NukeRocketEnvironmentDamage);
	}
}

static void ApplyNukeImpactDamage(const FVector& ImpactLocation, AActor* InstigatorPawn, AFortPlayerControllerAthena* InstigatorController, AActor* DamageCauser, int EnvironmentDamageContextId)
{
	ApplyNukePlayerDamageAtLocation(ImpactLocation, InstigatorPawn, InstigatorController, DamageCauser);
	ApplyNukeEnvironmentDamageAtLocation(ImpactLocation, EnvironmentDamageContextId);
}

static bool ApplyGuidedNukeRocket(AActor* Rocket, AActor* InstigatorPawn, AFortPlayerControllerAthena* InstigatorController, AFortPlayerPawnAthena* TargetPawn, const FVector& StaticTargetLocation, float Speed, float DeltaSeconds, bool bDamageEnabled, int EnvironmentDamageContextId)
{
	auto TargetLocation = StaticTargetLocation;

	if (TargetPawn)
	{
		TargetLocation = TargetPawn->K2_GetActorLocation();
		TargetLocation.Z += 80.f;
	}

	auto RocketLocation = Rocket->K2_GetActorLocation();
	auto Direction = (TargetLocation - RocketLocation).GetSafeNormal();

	if (Direction.IsZero())
		Direction = FVector(0.f, 0.f, -1.f);

	auto Velocity = Direction * Speed;
	auto Rotation = MakeRotationFromDirection(Direction);

	Rocket->K2_SetActorRotation(Rotation, false);

	auto Step = (DeltaSeconds > 0.f ? DeltaSeconds : 0.f) * Speed;
	auto Distance = RocketLocation.GetDistanceTo(TargetLocation);
	bool bImpacted = Distance <= (Step > 0.f ? Step : 100.f);

	if (Distance > 0.f && Step > 0.f)
	{
		auto NextLocation = Distance <= Step ? TargetLocation : RocketLocation + (Direction * Step);
		Rocket->K2_SetActorLocation(NextLocation, false, nullptr, true);
	}

	SetReflectedProperty<FVector>(Rocket, "Velocity", Velocity);
	SetReflectedProperty<FVector>(Rocket, "InitialVelocity", Velocity);
	SetReflectedProperty<FVector>(Rocket, "LaunchVelocity", Velocity);
	SetMatchingNukeTargetVectorProperties(Rocket, TargetLocation);
	TryCallNukeTargetFunctions(Rocket, TargetPawn, TargetLocation);

	static auto ProjectileMovementClass = FindClass("ProjectileMovementComponent");
	auto ProjectileMovement = ProjectileMovementClass ? Rocket->GetComponentByClass(ProjectileMovementClass) : nullptr;

	if (ProjectileMovement)
	{
		SetReflectedProperty<FVector>(ProjectileMovement, "Velocity", Velocity);
		SetReflectedProperty<float>(ProjectileMovement, "InitialSpeed", Speed);
		SetReflectedProperty<float>(ProjectileMovement, "MaxSpeed", Speed);
		SetReflectedProperty<float>(ProjectileMovement, "ProjectileGravityScale", 0.f);
		SetReflectedProperty<float>(ProjectileMovement, "HomingAccelerationMagnitude", 0.f);
		SetReflectedBoolProperty(ProjectileMovement, "bRotationFollowsVelocity", true);
		SetReflectedBoolProperty(ProjectileMovement, "bIsHomingProjectile", false);
		SetReflectedProperty<UActorComponent*>(ProjectileMovement, "HomingTargetComponent", nullptr);
	}

	if (Rocket->HasRootComponent() && Rocket->RootComponent)
	{
		if (auto SetPhysicsLinearVelocityFn = Rocket->RootComponent->GetFunction("SetPhysicsLinearVelocity"))
			Rocket->RootComponent->Call<void>(SetPhysicsLinearVelocityFn, Velocity, false, FName(0));
	}

	Rocket->ForceNetUpdate();

	if (bImpacted)
	{
		if (bDamageEnabled)
			ApplyNukeImpactDamage(TargetLocation, InstigatorPawn, InstigatorController, Rocket, EnvironmentDamageContextId);

		return true;
	}

	return false;
}

void AFortPlayerControllerAthena::TickNukeRockets(float DeltaSeconds)
{
	RefreshSpawnedBotTrackingWorld();
	TickForcedRespawnRepairs(DeltaSeconds);
	TickDeathSpectateCameraHandoffs(DeltaSeconds);
	TickOneShotLowGravityVfxRetries(DeltaSeconds);
	TickLoadedGameplayEffectCatalogForCommand();
	TickGameplayEffectOutputForCommand(DeltaSeconds);

	for (int CleanupIndex =
		static_cast<int>(GPendingSpawnedBotCleanup.size()) - 1;
		CleanupIndex >= 0; --CleanupIndex)
	{
		auto& Cleanup = GPendingSpawnedBotCleanup[CleanupIndex];
		Cleanup.RemainingSeconds -= DeltaSeconds;
		if (Cleanup.RemainingSeconds > 0.f)
			continue;

		auto PlayerController = Cleanup.Controller.Get();
		auto ControllerIdentity = Cleanup.ControllerIdentity;
		auto Pawn = Cleanup.Pawn.Get();
		auto PlayerState = Cleanup.PlayerState.Get();
		auto Inventory = Cleanup.Inventory.Get();
		AFortPlayerPawnAthena* ReplacementPawn = nullptr;
		AFortPlayerPawnAthena* SecondaryReplacementPawn = nullptr;
		AFortInventory* ReplacementInventory = nullptr;
		AFortPlayerStateAthena* ReplacementPlayerState = nullptr;
		bool bRemovedFromRoster = false;

		if (PlayerController)
		{
			auto CaptureReplacementPawn =
				[Pawn, &ReplacementPawn,
				 &SecondaryReplacementPawn](AActor* Candidate)
				{
					if (!IsUsableDeathObject(Candidate) ||
						Candidate == Pawn ||
						!Candidate->IsA(
							AFortPlayerPawnAthena::StaticClass()))
					{
						return;
					}

					auto CandidatePawn =
						(AFortPlayerPawnAthena*)Candidate;
					if (!ReplacementPawn)
						ReplacementPawn = CandidatePawn;
					else if (CandidatePawn != ReplacementPawn)
						SecondaryReplacementPawn = CandidatePawn;
				};
			CaptureReplacementPawn(PlayerController->Pawn);
			if (PlayerController->MyFortPawn !=
				PlayerController->Pawn)
			{
				CaptureReplacementPawn(
					PlayerController->MyFortPawn);
			}

			if (IsUsableDeathObject(
					PlayerController->WorldInventory))
			{
				ReplacementInventory =
					PlayerController->WorldInventory;
			}
			if (IsUsableDeathObject(
					PlayerController->PlayerState))
			{
				ReplacementPlayerState =
					(AFortPlayerStateAthena*)
						PlayerController->PlayerState;
			}

			auto World = UWorld::GetWorld();
			auto GameMode =
				World
					? (AFortGameMode*)World->AuthorityGameMode
					: nullptr;
			if (IsUsableDeathObject(GameMode))
			{
				auto RemoveController =
					[PlayerController, &bRemovedFromRoster](
						TArray<AActor*>& Roster)
					{
						for (int Index = Roster.Num() - 1;
							Index >= 0; --Index)
						{
							if (Roster[Index] == PlayerController)
							{
								Roster.Remove(Index);
								bRemovedFromRoster = true;
							}
						}
					};
				RemoveController(GameMode->AlivePlayers);
				if (GameMode->HasAliveBots())
					RemoveController(GameMode->AliveBots);
			}

			GPendingRespawnLandingFinalization.erase(PlayerController);
			GRespawnSkydivingObserved.erase(PlayerController);
			GPendingLegacyAircraftLandingEquipment.erase(PlayerController);
			GLegacyAircraftSkydivingObserved.erase(PlayerController);
			GRespawnHiddenWeapons.erase(PlayerController);
			GLastAcknowledgedPawn.erase(PlayerController);
			GRemoteControlReturnPawn.erase(PlayerController);
			GVehiclePossessionReturnPawn.erase(PlayerController);
			GVehiclePossessionVehicle.erase(PlayerController);
			GTrackedVehicleLoadouts.erase(PlayerController);
			GPendingLateGameAircraftLoadout.erase(PlayerController);
			PlayersInitialized.erase(PlayerController);

			for (int PendingIndex =
				(int)GPendingForcedRespawnRepairs.size() - 1;
				PendingIndex >= 0; --PendingIndex)
			{
				if (GPendingForcedRespawnRepairs[PendingIndex]
						.PlayerController.Get() == PlayerController)
				{
					GPendingForcedRespawnRepairs.erase(
						GPendingForcedRespawnRepairs.begin() +
							PendingIndex);
				}
			}
		}

		// A weak actor reference can already be null if native death destroyed
		// the controller synchronously. Erasing containers by the retained
		// identity is pointer-only and prevents a recycled address from being
		// mistaken for a managed bot in a later match.
		UnregisterTrackedSpawnedBotController(ControllerIdentity);
		G172SpawnedBotRemovalAttempts.erase(ControllerIdentity);

		// Destroy the synthetic actor graph together after native death has had
		// time to emit its elimination effects. If native respawn produced a
		// second pawn/inventory/state, destroy those current objects as well.
		if (ReplacementPawn != Pawn &&
			IsUsableDeathObject(ReplacementPawn))
		{
			ReplacementPawn->K2_DestroyActor();
		}
		if (SecondaryReplacementPawn != Pawn &&
			SecondaryReplacementPawn != ReplacementPawn &&
			IsUsableDeathObject(SecondaryReplacementPawn))
		{
			SecondaryReplacementPawn->K2_DestroyActor();
		}
		if (IsUsableDeathObject(Pawn))
			Pawn->K2_DestroyActor();
		if (ReplacementInventory != Inventory &&
			IsUsableDeathObject(ReplacementInventory))
		{
			ReplacementInventory->K2_DestroyActor();
		}
		if (IsUsableDeathObject(Inventory))
			Inventory->K2_DestroyActor();
		if (ReplacementPlayerState != PlayerState &&
			IsUsableDeathObject(ReplacementPlayerState))
		{
			ReplacementPlayerState->K2_DestroyActor();
		}
		if (IsUsableDeathObject(PlayerState))
			PlayerState->K2_DestroyActor();
		if (IsUsableDeathObject(PlayerController))
			PlayerController->K2_DestroyActor();

		if (bRemovedFromRoster)
			VersionFeatureAdapter::SyncPlayersLeft(true);

		SDK::DbgLog("[Elimination] completed terminal spawnbot cleanup controller=%p pawn=%p replacementPawn=%p secondaryPawn=%p playerState=%p inventory=%p rosterRemoved=%d\n",
			(void*)PlayerController, (void*)Pawn,
			(void*)ReplacementPawn,
			(void*)SecondaryReplacementPawn,
			(void*)PlayerState, (void*)Inventory,
			bRemovedFromRoster ? 1 : 0);
		GPendingSpawnedBotCleanup.erase(
			GPendingSpawnedBotCleanup.begin() + CleanupIndex);
	}

	if (UsesEarlyAthenaLandingClientRefresh() &&
		!GPendingLegacyAircraftLandingEquipment.empty())
	{
		std::vector<std::pair<AFortPlayerControllerAthena*, AFortPlayerPawnAthena*>>
			LandedAircraftPlayers;
		std::vector<AFortPlayerControllerAthena*> InvalidAircraftPlayers;
		for (auto PlayerController : GPendingLegacyAircraftLandingEquipment)
		{
			if (!IsUsableDeathObject(PlayerController))
			{
				InvalidAircraftPlayers.push_back(PlayerController);
				continue;
			}

			auto Pawn = PlayerController->MyFortPawn;
			if (!IsUsableDeathObject(Pawn))
			{
				InvalidAircraftPlayers.push_back(PlayerController);
				continue;
			}

			const bool bIsStillSkydiving =
				(Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) ||
				(Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus);
			if (bIsStillSkydiving)
			{
				GLegacyAircraftSkydivingObserved.insert(PlayerController);
				continue;
			}

			if (GLegacyAircraftSkydivingObserved.find(PlayerController) !=
				GLegacyAircraftSkydivingObserved.end())
			{
				LandedAircraftPlayers.emplace_back(PlayerController, Pawn);
			}
		}

		for (auto PlayerController : InvalidAircraftPlayers)
		{
			GPendingLegacyAircraftLandingEquipment.erase(PlayerController);
			GLegacyAircraftSkydivingObserved.erase(PlayerController);
		}

		for (auto& [PlayerController, Pawn] : LandedAircraftPlayers)
		{
			GPendingLegacyAircraftLandingEquipment.erase(PlayerController);
			GLegacyAircraftSkydivingObserved.erase(PlayerController);
			RestoreEquipmentAfterRespawn(PlayerController, true);
			Pawn->ForceNetUpdate();
			PlayerController->ForceNetUpdate();
			SDK::DbgLog("[Equipment] polled early aircraft landing version=%.2f controller=%p pawn=%p weapon=%p\n",
				VersionInfo.FortniteVersion, (void*)PlayerController, (void*)Pawn,
				(void*)GetPawnCurrentWeaponSafe(Pawn));
		}
	}

	// EndSkydiving is not reflected/hookable on legacy Season 4 builds. Observe
	// the replicated skydive state here so their cosmetic weapon hide and normal
	// post-landing respawn finalization are always released. This loop is dormant
	// unless at least one controller is in the custom respawn glide.
	static const bool bNeedsLegacyLandingPoll =
		AFortPlayerPawnAthena::GetDefaultObj()->GetFunction("EndSkydiving") == nullptr;
	if ((bNeedsLegacyLandingPoll || UsesEarlyAthenaLandingClientRefresh()) &&
		!GPendingRespawnLandingFinalization.empty())
	{
		std::vector<std::pair<AFortPlayerControllerAthena*, AFortPlayerPawnAthena*>> LandedRespawns;
		for (auto PlayerController : GPendingRespawnLandingFinalization)
		{
			if (!IsUsableDeathObject(PlayerController))
				continue;

			auto Pawn = PlayerController->MyFortPawn;
			if (!IsUsableDeathObject(Pawn))
				continue;

			const bool bIsStillSkydiving =
				(Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) ||
				(Pawn->HasbIsSkydivingFromBus() && Pawn->bIsSkydivingFromBus);
			if (bIsStillSkydiving)
			{
				GRespawnSkydivingObserved.insert(PlayerController);
				continue;
			}

			if (GRespawnSkydivingObserved.find(PlayerController) !=
				GRespawnSkydivingObserved.end())
			{
				LandedRespawns.emplace_back(PlayerController, Pawn);
			}
		}

		for (auto& [PlayerController, Pawn] : LandedRespawns)
		{
			// Do not synthesize the whole EndSkydiving lifecycle on builds where
			// the callback does not exist. Only release this cosmetic visibility
			// state and its pending marker; the normal respawn setup already ran.
			GPendingRespawnLandingFinalization.erase(PlayerController);
			GRespawnSkydivingObserved.erase(PlayerController);
			if (UsesEarlyAthenaLandingClientRefresh())
				RestoreEquipmentAfterRespawn(PlayerController, true);
			else
				RestoreRespawnHiddenWeapon(PlayerController);

			auto CurrentWeapon = GetPawnCurrentWeaponSafe(Pawn);
			if (CurrentWeapon)
			{
				CurrentWeapon->SetActorHiddenInGame(false);
				CurrentWeapon->ForceNetUpdate();
			}
			Pawn->ForceNetUpdate();
			PlayerController->ForceNetUpdate();
			SDK::DbgLog("[Respawn] legacy landing restored weapon controller=%p pawn=%p\n",
				(void*)PlayerController, (void*)Pawn);
		}
	}

	if (GuidedNukeRockets.empty())
		return;

	for (int i = static_cast<int>(GuidedNukeRockets.size()) - 1; i >= 0; i--)
	{
		auto& GuidedRocket = GuidedNukeRockets[i];

		if (!IsGuidedNukeObjectValid(GuidedRocket.Rocket) || (GuidedRocket.TargetPawn && !IsGuidedNukeObjectValid(GuidedRocket.TargetPawn)))
		{
			RemoveGuidedNukeRocketAt(static_cast<size_t>(i));
			continue;
		}

		GuidedRocket.LifeSeconds -= DeltaSeconds;

		if (GuidedRocket.LifeSeconds <= 0.f || (GuidedRocket.Rocket->HasbActorIsBeingDestroyed() && GuidedRocket.Rocket->bActorIsBeingDestroyed))
		{
			RemoveGuidedNukeRocketAt(static_cast<size_t>(i));
			continue;
		}

		if (ApplyGuidedNukeRocket(GuidedRocket.Rocket, GuidedRocket.InstigatorPawn, GuidedRocket.InstigatorController, GuidedRocket.TargetPawn, GuidedRocket.TargetLocation, GuidedRocket.Speed, DeltaSeconds, GuidedRocket.bDamageEnabled, GuidedRocket.EnvironmentDamageContextId))
			RemoveGuidedNukeRocketAt(static_cast<size_t>(i));
	}
}

int AFortPlayerControllerAthena::TeleportAllPlayersTo(AFortPlayerControllerAthena* TargetPlayer, bool bSendMessage)
{
	if (!TargetPlayer || !TargetPlayer->Pawn)
		return 0;

	UObject* NetDriver = UWorld::GetWorld()->NetDriver;

	if (!NetDriver)
	{
		if (bSendMessage)
			TargetPlayer->ClientMessage(FString(L"NetDriver not found!"), FName(), 1.f);
		return 0;
	}

	UNetDriver* Driver = static_cast<UNetDriver*>(NetDriver);
	auto& ClientConnections = Driver->ClientConnections;

	if (ClientConnections.Num() <= 0)
	{
		if (bSendMessage)
			TargetPlayer->ClientMessage(FString(L"No players found!"), FName(), 1.f);
		return 0;
	}

	std::vector<AFortPlayerControllerAthena*> TargetPlayers;
	TargetPlayers.reserve(ClientConnections.Num());

	for (int i = 0; i < ClientConnections.Num(); i++)
	{
		UNetConnection* Connection = ClientConnections[i];

		if (!Connection)
			continue;

		auto PC = (AFortPlayerControllerAthena*)Connection->PlayerController;

		if (!PC || PC == TargetPlayer || !PC->Pawn)
			continue;

		auto PS = PC->PlayerState;

		if (PS && PS->HasbIsABot() && PS->bIsABot)
			continue;

		TargetPlayers.push_back(PC);
	}

	int TargetCount = (int)TargetPlayers.size();

	if (TargetCount <= 0)
	{
		if (bSendMessage)
			TargetPlayer->ClientMessage(FString(L"No real players found!"), FName(), 1.f);
		return 0;
	}

	auto TargetPawn = TargetPlayer->Pawn;
	auto TargetLocation = TargetPawn->K2_GetActorLocation();
	auto TargetRotation = TargetPawn->K2_GetActorRotation();
	int TeleportCount = 0;

	for (int i = 0; i < TargetCount; i++)
	{
		auto PC = TargetPlayers[i];
		auto Pawn = PC->Pawn;

		if (!Pawn)
			continue;

		int Ring = i / 8;
		int Slot = i % 8;
		int RemainingPlayers = TargetCount - (Ring * 8);
		int PlayersOnRing = RemainingPlayers < 8 ? RemainingPlayers : 8;
		float Radius = 350.f + (Ring * 250.f);
		float AngleStep = 6.2831853071795864769f / PlayersOnRing;
		float Angle = ((float)TargetRotation.Yaw * 0.017453292519943295f) + (Slot * AngleStep);

		FVector TeleportLoc = TargetLocation;
		TeleportLoc.X += cos(Angle) * Radius;
		TeleportLoc.Y += sin(Angle) * Radius;
		TeleportLoc.Z += 200.f;

		Pawn->K2_TeleportTo(TeleportLoc, TargetRotation, false, true);
		if (Pawn->CharacterMovement)
			Pawn->CharacterMovement->Velocity = FVector{};
		TeleportCount++;
	}

	if (bSendMessage)
	{
		auto msg = L"Teleported " + std::to_wstring(TeleportCount) + L" player(s) around you!";
		TargetPlayer->ClientMessage(FString(msg.c_str()), FName(), 1.f);
	}

	return TeleportCount;
}

static bool HasLoopbackSavedAddressForCheatCommands(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!IsUsableDeathObject(PlayerController) ||
		!PlayerController->HasPlayerState() ||
		!IsUsableDeathObject(PlayerController->PlayerState))
	{
		return false;
	}

	auto PlayerState = PlayerController->PlayerState;
	const uint32 AddressOffset =
		PlayerState->GetOffset("SavedNetworkAddress");
	if (AddressOffset == static_cast<uint32>(-1) ||
		!IsUsableDeathObject(PlayerState->Class))
	{
		return false;
	}

	const int32 ObjectSize =
		PlayerState->Class->GetPropertiesSize();
	if (ObjectSize < static_cast<int32>(sizeof(FString)) ||
		AddressOffset > static_cast<uint32>(
			ObjectSize - sizeof(FString)))
	{
		return false;
	}

	const auto AddressField =
		reinterpret_cast<const FString*>(
			reinterpret_cast<const uint8*>(PlayerState) +
				AddressOffset);
	if (!SDK::MemReadable(AddressField, sizeof(FString)))
		return false;

	const int32 Length = AddressField->Num();
	if (Length <= 1 || Length > 128 ||
		AddressField->Max() < Length ||
		!AddressField->CStr() ||
		!SDK::MemReadable(
			AddressField->CStr(),
			static_cast<size_t>(Length) *
				sizeof(wchar_t)) ||
		AddressField->CStr()[Length - 1] != L'\0')
	{
		return false;
	}

	const auto Address = AddressField->ToString();
	return Address == "127.0.0.1" ||
		Address.starts_with("127.0.0.1:") ||
		Address == "::1" ||
		Address.starts_with("[::1]:") ||
		Address == "::ffff:127.0.0.1" ||
		Address.starts_with("[::ffff:127.0.0.1]:");
}

static bool IsCheatCommandHost(
	AFortPlayerControllerAthena* PlayerController)
{
	if (!IsUsableDeathObject(PlayerController))
		return false;

	auto World = UWorld::GetWorld();
	if (IsUsableDeathObject(World) &&
		World->HasOwningGameInstance())
	{
		auto GameInstance = World->OwningGameInstance;
		if (IsUsableDeathObject(GameInstance) &&
			GameInstance->HasLocalPlayers())
		{
			const auto& LocalPlayers =
				GameInstance->LocalPlayers;
			const int32 Count = LocalPlayers.Num();
			if (Count > 0)
			{
				if (Count > 8 ||
					LocalPlayers.Max() < Count ||
					!SDK::MemReadable(
						LocalPlayers.GetData(),
						static_cast<size_t>(Count) *
							sizeof(ULocalPlayer*)))
				{
					return false;
				}

				for (int32 Index = 0;
					Index < Count; ++Index)
				{
					auto LocalPlayer =
						LocalPlayers[Index];
					if (IsUsableDeathObject(LocalPlayer) &&
						LocalPlayer->PlayerController ==
							PlayerController)
					{
						return true;
					}
				}

				// A retained local-player identity is authoritative. Do not
				// let another controller qualify through a loopback proxy.
				return false;
			}
		}
	}

	// Older hosted builds remove the local player before travel. Their
	// authoritative host reconnects through the local loopback socket.
	return HasLoopbackSavedAddressForCheatCommands(
		PlayerController);
}

void AFortPlayerControllerAthena::ServerCheat(UObject* Context, FFrame& Stack)
{
	FString Msg;
	Stack.StepCompiledIn(&Msg);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	auto originalCommand = Msg.ToString();
	// This optional, fixed-format telemetry channel is negotiated per client.
	// Consume it before cheat authorization and never expose it as a command.
	if (PlayerLoadout::HandleBridgeMessage(
			PlayerController,
			std::string(originalCommand.c_str())))
	{
		return;
	}
	// ServerCheat can arrive while seamless travel is replacing the world.
	// Ordinary commands have no valid authority target in that window.
	auto World = UWorld::GetWorld();
	if (!World || !SDK::MemReadable(World, sizeof(UWorld)) ||
		!World->AuthorityGameMode)
	{
		return;
	}

	auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
	if (!SDK::MemReadable(GameMode, sizeof(AFortGameMode)) ||
		!GameMode->GameState)
	{
		return;
	}
	auto GameState = (AFortGameStateAthena*)GameMode->GameState;

	auto fullCommand = originalCommand;

	std::transform(fullCommand.begin(), fullCommand.end(), fullCommand.begin(),
		[](unsigned char c) { return std::tolower(c); });

	std::vector<UEAllocatedString> args;

	size_t pos = 0, lastPos = 0;
	while ((pos = fullCommand.find(' ', lastPos)) != std::string::npos)
	{
		args.push_back(fullCommand.substr(lastPos, pos - lastPos));

		lastPos = pos + 1;
	}

	args.push_back(fullCommand.substr(lastPos));

	if (args.size() == 0)
	{
	_help:
		PlayerController->ClientMessage(FString(LR"(
cheat startaircraft - Starts the battle bus
cheat resumesafezone - Resumes the storm
cheat pausesafezone - Pauses the storm
cheat skipsafezone - Skips to the next safe zone
cheat startshrinksafezone - Starts shrinking the safe zone
cheat infiniteammo - Toggles infinite ammo
cheat infinitemats - Toggles infinite materials
cheat serversendmessage - Broadcasts a message to all clients in the game
cheat fly <speed> - Toggles fly mode (with optional speed value)
cheat ghost <speed> - Toggles no-clip flying (with optional speed value)
cheat gravity <scale> - Sets the gravity scale
cheat changename <name> - Changes your player name
cheat keepinventory - Toggles keeping inventory on death
cheat spawnactor/summon <class/path> [s<size> | s<X>,<Y>,<Z>] [h<meters>] - Spawns an actor near your location
cheat destroyall <class/path> - Destroys all actors of a class
cheat deltarget - Destroys the actor your crosshair is aiming at
cheat resetbuilds <radius> - Resets player builds, all of them without a radius
cheat sethealth <amount> - Sets your pawn's health (0-100)
cheat setshield <amount> - Sets your pawn's shield (0-100)
cheat setmaxhealth <amount> - Sets your pawn's maximum health
cheat setmaxshield <amount> - Sets your pawn's maximum shield
cheat regen - Regenerates health and shield to the maximum value
cheat regenall - Regenerates health and shield for all players
cheat setkills - Sets your kill count
cheat setarenapoints - Sets your arena points : Use a negative number to take away points
cheat demospeed <speed> - Sets the speed of the server
cheat god [min | check] - Toggles full god mode or a 1-health minimum
cheat godall - Toggles god mode for all players
cheat speed <scale> - Sets the player's movement speed
cheat size <scale> | size <X> <Y> <Z> - Resizes your pawn (uniform or per-axis)
cheat timeofday <hour> - Sets the time of day (0-24)
cheat pausetimeofday - Pauses/Unpauses the time of day
cheat spawnbot <count> <weapon> <s[size] | s[X,Y,Z]> [X Y Z] - Spawns a player bot at your or a specified location (WIP)
cheat tpbot - Teleports the player bot to your location
cheat delbot - Removes every spawned player bot (PlayerAI is left alone)
cheat dumppawns - Lists every player pawn with its index and owner
cheat dumpge - Builds an indexed gameplay-effect catalog and writes DumpedGameplayEffects.txt
cheat applyge <index | effect name/path | remove> - Applies a gameplay effect, or removes all active effects
cheat outputge - Toggles batched UE-console output for active Gameplay Effect changes on your player
cheat possess <player name | index | pawn name | reset> - Puppet a pawn (move it around, owner watches), reset returns you to your own
cheat tpall - Teleports all real players around your location
cheat botemote - Plays the 'Accolades' emote to the player bot
cheat emoteall <emote> - Makes everyone emote, a random one each without an emote
cheat startevent - Starts the event for the current version
cheat getlocation - Copies your current location to the clipboard
cheat setrespawnpoint - Sets your respawn point to a specified location
cheat tpto <exact player name> - Teleports you next to a real player
cheat swap <player name> - Swaps places with a player or bot (partial name)
cheat tp | tp <X> <Y> <Z> - Teleports to where your crosshair is aiming, or to a location
cheat launch <X> <Y> <Z> - Launches the player
cheat savewaypoint - Saves your current location as a waypoint
cheat waypoint <name> - Loads a saved waypoint
cheat skydive - Toggles skydiving
cheat mark - Toggles teleporting to placed map markers
cheat togglepersonalvehicle - Toggles the personal vehicle
cheat giveitem <WID/path> <Count = 1> - Gives you an item
cheat givetoall <WID/path> - Gives an item to all connected players
cheat giveall - Gives you all ammo, mats, and traps
cheat givetraps - Gives you all available traps
cheat giveammo - Gives you 999 of every ammo type
cheat givemats - Gives you 500 of each material
cheat spawnpickup <WID/path> <Count = 1> [X Y Z] - Spawns a pickup at your player's or specified location
cheat lootrain <Count = 20> <Radius = 600> <TierGroup = chest> - Rains chest, floor, rare, or custom tier-group loot around you
cheat clearinventory - Clears your inventory of all items that are droppable
cheat delitem - Removes the item you currently have equipped
cheat spawn <class/path> <s[size] | s[X,Y,Z]> <h[meters]> - Spawns an actor at your location
cheat shortcmds <items/objects> - Lists all short names for cheat give/spawn
)"), FName(), 1);
	}
	else
	{
		auto& command = args[0];
		std::transform(command.begin(), command.end(), command.begin(), tolower);

		const bool bHostCheatOverride =
			GUI::GetSelectedPlaylist() !=
				static_cast<int>(Playlist::OnlyUp) &&
			IsCheatCommandHost(PlayerController);
		const bool bCanUseCheatCommands =
			FConfiguration::bEnableCheats.load(
				std::memory_order_acquire) ||
			bHostCheatOverride;

		if (bCanUseCheatCommands)
		{
			if (command == "startaircraft")
			{
				if (VersionInfo.FortniteVersion < 11.00 &&
					GUI::gsStatus == Joinable)
				{
					// Chapter 1's native StartAircraftPhase is protected by
					// the same authoritative warmup policy as the GUI button.
					// Route the documented cheat through that synchronized
					// ten-second release instead of bypassing or being blocked
					// by the legacy phase gate.
					FConfiguration::bStartBusRequested.store(
						true, std::memory_order_release);
					PlayerController->ClientMessage(
						FString(L"Aircraft will start in 10 seconds."),
						FName(), 1.f);
				}
				else if (UFortGameStateComponent_BattleRoyaleGamePhaseLogic::GetDefaultObj())
				{
					auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

					GamePhaseLogic->StartAircraftPhase();
					PlayerController->ClientMessage(FString(L"Started the aircraft!"), FName(), 1.f);
				}
				else
				{
					UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startaircraft"), nullptr);
					PlayerController->ClientMessage(FString(L"Started the aircraft!"), FName(), 1.f);
				}
			}
			else if (command == "resumesafezone")
			{
				UFortGameStateComponent_BattleRoyaleGamePhaseLogic::SetSafeZonePaused(false);
				PlayerController->ClientMessage(FString(L"Resumed the safe zone."), FName(), 1.f);
			}
			else if (command == "pausesafezone")
			{
				UFortGameStateComponent_BattleRoyaleGamePhaseLogic::SetSafeZonePaused(true);
				PlayerController->ClientMessage(FString(L"Paused the safe zone."), FName(), 1.f);
			}
			else if (command == "skipsafezone")
			{
				if (GameMode->HasSafeZoneIndicator())
				{
					if (GameMode->SafeZoneIndicator)
					{
						GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
						GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.05f;
					}
				}
				else
				{
					auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

					if (GamePhaseLogic->SafeZoneIndicator)
					{
						GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
						GamePhaseLogic->SafeZoneIndicator->SafeZoneFinishShrinkTime = GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.05f;
					}
				}

				PlayerController->ClientMessage(FString(L"Currently skipping the zone."), FName(), 1.f);
				//UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"skipsafezone"), nullptr);
			}
			else if (command == "startshrinksafezone")
			{
				auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
				if (GameMode->HasSafeZoneIndicator())
				{
					if (GameMode->SafeZoneIndicator)
						GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
				}
				else
				{
					auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

					if (GamePhaseLogic->SafeZoneIndicator)
						GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
				}

				PlayerController->ClientMessage(FString(L"Started shrinking the zone."), FName(), 1.f);

				//UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startshrinksafezone"), nullptr);
			}
			else if (command == "dumpitems")
			{
				std::stringstream ss;

				ss << "Generated by Erbium (https://github.com/plooshi/Erbium)\n";
				char version[6];

				sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);
				ss << "Fortnite Version: " << version << "\n\n";

				auto RarityEnum = EFortRarity::StaticEnum();
				for (int i = 0; i < TUObjectArray::Num(); i++)
				{
					auto Object = TUObjectArray::GetObjectByIndex(i);
					if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA<UFortWorldItemDefinition>())
						continue;
					auto Item = (UFortWorldItemDefinition*)Object;

					FString Name = UKismetTextLibrary::Conv_TextToString(Item->HasDisplayName() ? Item->DisplayName : Item->ItemName);

					ss << "- " << UKismetSystemLibrary::GetPathName(Item).ToString() << "\n";
					ss << "-     Name: " << (Name.GetData() ? Name.ToString() : "None") << "\n";

					auto Names = *(TArray<TPair<FName, int64>>*)(__int64(RarityEnum) + 0x40);

					for (int i = 0; i < Names.Num(); i++)
					{
						auto& Pair = Names[i];
						auto& Name = Pair.Key();
						auto& Value = Pair.Value();

						if (Value == Item->Rarity)
						{
							auto str = Name.ToString();
							auto colcolIdx = str.find_last_of("::");

							auto RealName = colcolIdx == -1 ? str : str.substr(colcolIdx + 1);

							ss << "-     Rarity: " << RealName << "\n";
						}
					}
				}

				std::ofstream of("DumpedItems.txt", std::ios::trunc);

				of << ss.str();
				of.close();

				PlayerController->ClientMessage(FString(L"Dumped all available items! Head to your Win64/Binaries folder to find the .txt file!"), FName(), 1.f);
			}
			else if (command == "dumpplaylist" || command == "dumpplaylists")
			{
				std::stringstream ss;

				ss << "Generated by Erbium (https://github.com/plooshi/Erbium)\n";
				char version[6];

				sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);
				ss << "Fortnite Version: " << version << "\n\n";

				auto RarityEnum = EFortRarity::StaticEnum();
				for (int i = 0; i < TUObjectArray::Num(); i++)
				{
					auto Object = TUObjectArray::GetObjectByIndex(i);
					if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA<UFortPlaylistAthena>())
						continue;
					auto Playlist = (UFortPlaylistAthena*)Object;

					FString Name = UKismetTextLibrary::Conv_TextToString(Playlist->UIDisplayName);

					ss << "- " << UKismetSystemLibrary::GetPathName(Playlist).ToString() << "\n";
					ss << "-     Name: " << (Name.GetData() ? Name.ToString() : "None") << "\n";
					if (Playlist->HasMaxPlayers())
						ss << "-     Max Players: " << std::to_string(Playlist->MaxPlayers) << "\n";
					if (Playlist->HasMaxSquadSize())
						ss << "-     Squad Size: " << std::to_string(Playlist->MaxSquadSize) << "\n";
				}

				std::ofstream of("DumpedPlaylists.txt", std::ios::trunc);

				of << ss.str();
				of.close();

				PlayerController->ClientMessage(FString(L"Dumped all available playlists! Head to your Win64/Binaries folder to find the .txt file!"), FName(), 1.f);
			}
			else if (command == "suicide")
			{
				PlayerController->ServerSuicide();
				PlayerController->ClientMessage(FString(L"Killed player!"), FName(), 1.f);
			}
			else if (command == "infiniteammo")
				FConfiguration::bInfiniteAmmo.store(
					!FConfiguration::bInfiniteAmmo.load());
			else if (command == "infinitemats")
				FConfiguration::bInfiniteMats.store(
					!FConfiguration::bInfiniteMats.load());
			else if (command == "demospeed")
			{
				if (args.size() != 2)
				{
					PlayerController->ClientMessage(FString(L"Wrong number of arguments!"), FName(), 1.f);
					return;
				}

				auto ws = L"demospeed " + UEAllocatedWString(args[1].begin(), args[1].end());

				UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(ws.c_str()), nullptr);
				PlayerController->ClientMessage(FString(L"Modified the server's demospeed!"), FName(), 1.f);
			}
			else if (command == "god")
			{
				auto Pawn = PlayerController->MyFortPawn
					? PlayerController->MyFortPawn
					: PlayerController->Pawn;
				if (!Pawn)
				{
					PlayerController->ClientMessage(
						FString(L"No player pawn is available."),
						FName(), 1.f);
					return;
				}

				bool bUseMinimum = false;
				bool bCheckOnly = false;
				if (args.size() > 1)
				{
					std::string Mode = args[1].c_str();
					std::transform(
						Mode.begin(), Mode.end(), Mode.begin(),
						tolower);
					bUseMinimum =
						Mode == "min" || Mode == "minimum";
					bCheckOnly =
						Mode == "check" || Mode == "c";
				}

				UFortHealthSet* HealthSet = nullptr;
				FFortGameplayAttributeData* Health = nullptr;
				if (Pawn->HasHealthSet() &&
					Pawn->HealthSet &&
					Pawn->HealthSet->HasHealth() &&
					FFortGameplayAttributeData::StaticStruct() &&
					FFortGameplayAttributeData::HasMinimum())
				{
					HealthSet = Pawn->HealthSet;
					Health = &HealthSet->Health;
				}

				const float MaxHealth = Pawn->GetMaxHealth();
				const float MaxShield = Pawn->GetMaxShield();
				auto HasFullGodMode = [&]()
				{
					const bool bDamageDisabled =
						Pawn->HasbCanBeDamaged() &&
						!Pawn->bCanBeDamaged;
					const bool bMaximumFloor =
						Health &&
						FPlatformMath::IsFinite(MaxHealth) &&
						MaxHealth > 1.f &&
						FPlatformMath::IsFinite(
							Health->Minimum) &&
						std::abs(
							Health->Minimum - MaxHealth) <=
							0.01f;
					return bDamageDisabled || bMaximumFloor;
				};
				auto RefillAndPlayCue = [&]()
				{
					if (FPlatformMath::IsFinite(MaxHealth) &&
						MaxHealth > 0.f)
					{
						Pawn->SetHealth(MaxHealth);
					}
					if (FPlatformMath::IsFinite(MaxShield) &&
						MaxShield >= 0.f)
					{
						Pawn->SetShield(MaxShield);
					}

					auto PlayerState =
						PlayerController->PlayerState;
					if (!PlayerState ||
						!PlayerState
							->HasAbilitySystemComponent() ||
						!PlayerState->AbilitySystemComponent)
					{
						return;
					}

					auto AbilitySystem =
						PlayerState->AbilitySystemComponent;
					auto Handle =
						AbilitySystem->MakeEffectContext();
					FGameplayTag Tag{};
					static auto Cue = FName(
						L"GameplayCue.Shield.PotionConsumed");
					Tag.TagName = Cue;
					auto PredictionKey =
						(FPredictionKey*)malloc(
							FPredictionKey::Size());
					if (!PredictionKey)
						return;
					memset(
						(PBYTE)PredictionKey, 0,
						FPredictionKey::Size());
					AbilitySystem
						->NetMulticast_InvokeGameplayCueAdded(
							Tag, *PredictionKey, Handle);
					AbilitySystem
						->NetMulticast_InvokeGameplayCueExecuted(
							Tag, *PredictionKey, Handle);
					free(PredictionKey);
				};

				const bool bTrackedMinimum =
					AFortPlayerPawnAthena::
						HasMinimumHealthGodMode(
							PlayerController);
				if (bCheckOnly)
				{
					if (bTrackedMinimum)
					{
						PlayerController->ClientMessage(
							FString(
								L"Minimum-health god mode is "
								L"enabled (damage stops at 1 HP)."),
							FName(), 1.f);
					}
					else if (HasFullGodMode())
					{
						PlayerController->ClientMessage(
							FString(
								L"Full god mode is enabled."),
							FName(), 1.f);
					}
					else
					{
						PlayerController->ClientMessage(
							FString(L"God mode is disabled."),
							FName(), 1.f);
					}
					return;
				}

				if (bUseMinimum)
				{
					if (bTrackedMinimum)
					{
						AFortPlayerPawnAthena::
							SetMinimumHealthGodMode(
								PlayerController, false);
						PlayerController->ClientMessage(
							FString(
								L"Minimum-health god mode "
								L"disabled."),
							FName(), 1.f);
						return;
					}

					const float CurrentHealth =
						Pawn->GetHealth();
					if (!FPlatformMath::IsFinite(
							CurrentHealth) ||
						CurrentHealth <= 0.f ||
						IsPawnDBNOForSpectating(Pawn) ||
						(Pawn->HasbIsDying() &&
							Pawn->bIsDying) ||
						(Pawn->HasbPlayedDying() &&
							Pawn->bPlayedDying) ||
						(Pawn->HasbIsHiddenForDeath() &&
							Pawn->bIsHiddenForDeath))
					{
						PlayerController->ClientMessage(
							FString(
								L"Minimum-health god mode "
								L"can only be enabled while "
								L"alive."),
							FName(), 1.f);
						return;
					}

					if (!Health)
					{
						PlayerController->ClientMessage(
							FString(
								L"This build does not expose "
								L"a safe minimum-health "
								L"attribute."),
							FName(), 1.f);
						return;
					}

					// Switching from full god must make the pawn damageable.
					// The one-health floor, rather than immunity, now prevents
					// the lethal transition.
					if (Pawn->HasbCanBeDamaged() &&
						!Pawn->bCanBeDamaged)
					{
						Pawn->bCanBeDamaged = true;
					}
					if (FPlatformMath::IsFinite(
							Health->Minimum) &&
						FPlatformMath::IsFinite(MaxHealth) &&
						MaxHealth > 1.f &&
						std::abs(
							Health->Minimum - MaxHealth) <=
							0.01f)
					{
						Health->Minimum = 0.f;
					}

					if (!AFortPlayerPawnAthena::
							SetMinimumHealthGodMode(
								PlayerController, true))
					{
						PlayerController->ClientMessage(
							FString(
								L"Minimum-health god mode "
								L"could not attach to this "
								L"pawn yet."),
							FName(), 1.f);
						return;
					}

					RefillAndPlayCue();
					Pawn->ForceNetUpdate();
					PlayerController->ClientMessage(
						FString(
							L"Minimum-health god mode enabled: "
							L"damage is active and health stops "
							L"at 1 HP."),
						FName(), 1.f);
					return;
				}

				// Plain "god" switches minimum mode to full mode in one
				// command instead of first leaving the player unprotected.
				const bool bSwitchingFromMinimum =
					bTrackedMinimum;
				if (bTrackedMinimum)
				{
					AFortPlayerPawnAthena::
						SetMinimumHealthGodMode(
							PlayerController, false);
				}

				const bool bEnableFull =
					bSwitchingFromMinimum ||
					!HasFullGodMode();
				bool bApplied = false;
				if (VersionInfo.FortniteVersion >= 21 &&
					Pawn->HasbCanBeDamaged())
				{
					Pawn->bCanBeDamaged = !bEnableFull;
					bApplied = true;
				}
				else if (Health)
				{
					Health->Minimum =
						bEnableFull ? MaxHealth : 0.f;
					bApplied = true;
				}
				else if (Pawn->HasbCanBeDamaged())
				{
					Pawn->bCanBeDamaged = !bEnableFull;
					bApplied = true;
				}

				if (!bApplied)
				{
					PlayerController->ClientMessage(
						FString(
							L"This build exposes no safe god "
							L"mode mechanism."),
						FName(), 1.f);
					return;
				}

				if (bEnableFull)
					RefillAndPlayCue();
				Pawn->ForceNetUpdate();
				PlayerController->ClientMessage(
					FString(
						bEnableFull
							? L"Full god mode enabled."
							: L"God mode disabled."),
					FName(), 1.f);
			}
			else if (command == "godall")
			{
				UObject* NetDriver = UWorld::GetWorld()->NetDriver;

				if (!NetDriver)
				{
					PlayerController->ClientMessage(FString(L"NetDriver not found!"), FName(), 1.f);
					return;
				}

				UNetDriver* Driver = static_cast<UNetDriver*>(NetDriver);
				auto& ClientConnections = Driver->ClientConnections;

				if (ClientConnections.Num() <= 0)
				{
					PlayerController->ClientMessage(FString(L"No players found!"), FName(), 1.f);
					return;
				}

				int GodCount = 0;

				for (int i = 0; i < ClientConnections.Num(); i++)
				{
					UNetConnection* Connection = ClientConnections[i];

					if (!Connection)
						continue;

					auto PC = (AFortPlayerControllerAthena*)Connection->PlayerController;

					if (!PC || !PC->Pawn || !PC->MyFortPawn)
						continue;

					AFortPlayerPawnAthena::
						SetMinimumHealthGodMode(PC, false);

					auto Pawn = PC->Pawn;
					auto PS = PC->PlayerState;

					float MaxHealth = Pawn->GetMaxHealth();
					float MaxShield = Pawn->GetMaxShield();

					if (VersionInfo.FortniteVersion >= 21)
					{
						Pawn->bCanBeDamaged ^= 1;

						if (Pawn->bCanBeDamaged == 0)
						{
							Pawn->SetHealth(MaxHealth);
							Pawn->SetShield(MaxShield);

							if (PS && PS->AbilitySystemComponent)
							{
								auto Handle = PS->AbilitySystemComponent->MakeEffectContext();
								FGameplayTag Tag;
								static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
								Tag.TagName = Cue;
								auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
								memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
								PS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
								PS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
								free(PredictionKey);
							}
						}
					}
					else
					{
						auto HealthSet =
							PC->MyFortPawn->HasHealthSet()
								? PC->MyFortPawn->HealthSet
								: nullptr;
						if (!HealthSet ||
							!HealthSet->HasHealth() ||
							!FFortGameplayAttributeData::
								StaticStruct() ||
							!FFortGameplayAttributeData::
								HasMinimum())
						{
							continue;
						}

						auto& Health = HealthSet->Health;
						float MinValue = Pawn->GetMaxHealth();

						if (Health.Minimum != MinValue)
						{
							Health.Minimum = MinValue;
							Pawn->ForceNetUpdate();

							Pawn->SetHealth(MaxHealth);
							Pawn->SetShield(MaxShield);

							if (PS && PS->AbilitySystemComponent)
							{
								auto Handle = PS->AbilitySystemComponent->MakeEffectContext();
								FGameplayTag Tag;
								static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
								Tag.TagName = Cue;
								auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
								memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
								PS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
								PS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
								free(PredictionKey);
							}
						}
						else
						{
							Health.Minimum = 0.f;
							Pawn->ForceNetUpdate();
						}
					}

					GodCount++;
				}

				auto msg = L"Toggled god mode for " + std::to_wstring(GodCount) + L" player(s)!";
				PlayerController->ClientMessage(FString(msg.c_str()), FName(), 1.f);
			}
			else if (command == "speed")
			{
				float Speed = 1.0f;

				if (args.size() > 1)
				{
					try { Speed = std::stof(std::string(args[1])); }
					catch (...)
					{
						PlayerController->ClientMessage(FString(L"Invalid speed value"), FName(), 1.f);
						return;
					}
				}

				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn to set speed"), FName(), 1.f);
					return;
				}

				static auto SetMovementSpeedFn = Pawn->GetFunction("SetMovementSpeed");

				if (!SetMovementSpeedFn)
					SetMovementSpeedFn = Pawn->GetFunction("SetMovementSpeedMultiplier");

				if (!SetMovementSpeedFn)
					return;

				Pawn->ProcessEvent(SetMovementSpeedFn, &Speed);
				PlayerController->ClientMessage(FString(L"Set player speed!"), FName(), 1.f);
			}
			else if (command == "size")
			{
				double X = 1., Y = 1., Z = 1.;

				if (args.size() == 2)
				{
					// Uniform scale on every axis: "size 5"
					X = Y = Z = strtod(args[1].c_str(), nullptr);
				}
				else if (args.size() == 4)
				{
					// Per-axis scale: "size 1 1 5"
					X = strtod(args[1].c_str(), nullptr);
					Y = strtod(args[2].c_str(), nullptr);
					Z = strtod(args[3].c_str(), nullptr);
				}
				else
				{
					PlayerController->ClientMessage(FString(L"Usage: size <scale>  or  size <X> <Y> <Z>"), FName(), 1.f);
					return;
				}

				auto OldPawn = (AFortPlayerPawnAthena*)PlayerController->Pawn;

				if (!OldPawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn to resize!"), FName(), 1.f);
					return;
				}

				// IMPORTANT: do NOT touch the inventory (Remove/GiveItem) here. On this build that reliably
				// hard-crashes the game. The WorldInventory lives on the controller and survives the pawn
				// swap on its own, so we just respawn a scaled pawn and re-equip the pickaxe.

				// Build the spawn transform: current location + rotation, new scale, and raise the (bigger)
				// pawn by the change in capsule half-height so its feet stay on the ground.
				FTransform SpawnTransform = OldPawn->GetTransform();
				FVector OldScaleVec = SpawnTransform.Scale3D;
				float OldScaleZ = (float)OldScaleVec.Z;
				if (OldScaleZ == 0.f)
					OldScaleZ = 1.f;

				float BaseHalfHeight = 0.f;
				static auto CapsuleComponentClass = FindClass("CapsuleComponent");
				if (auto CapsuleComponent = CapsuleComponentClass ? OldPawn->GetComponentByClass(CapsuleComponentClass) : nullptr)
					GetReflectedFloatForCommand(CapsuleComponent, "CapsuleHalfHeight", BaseHalfHeight);

				auto Scale = FVector(X, Y, Z);
				SpawnTransform.Scale3D = Scale;
				if (BaseHalfHeight > 0.f)
					SpawnTransform.Translation.Z += BaseHalfHeight * ((float)Z - OldScaleZ);

				// Preserve health/shield so resizing is not a free heal.
				float SavedHealth = OldPawn->GetHealth();
				float SavedShield = OldPawn->GetShield();

				auto NewPawn = GameMode->SpawnDefaultPawnAtTransform(PlayerController, SpawnTransform);

				if (!NewPawn)
				{
					PlayerController->ClientMessage(FString(L"Failed to spawn resized pawn!"), FName(), 1.f);
					return;
				}

				// Mark this possess as a resize so the possession handler does not teleport us to a
				// respawn point / leave us in a cannot-use-items respawn state.
				SkipNextPossessRespawn(PlayerController);
				PlayerController->Possess(NewPawn);
				PlayerController->MyFortPawn = NewPawn;

				NewPawn->SetHealth(SavedHealth);
				NewPawn->SetShield(SavedShield);

				// Remove the old body so it does not linger as a ghost (safe without inventory edits).
				OldPawn->K2_DestroyActor();

				// Re-equip the pickaxe (equip is safe; only Remove/GiveItem crash). The rest of your items
				// are still in the inventory - switch weapon slots to them.
				if (PlayerController->WorldInventory)
				{
					auto PickaxeEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([](FFortItemEntry& Entry)
						{ return Entry.ItemDefinition && Entry.ItemDefinition->IsA<UFortWeaponMeleeItemDefinition>(); }, FFortItemEntry::Size());

					if (PickaxeEntry)
					{
						PlayerController->ServerExecuteInventoryItem(PickaxeEntry->ItemGuid);
						PlayerController->ClientEquipItem(PickaxeEntry->ItemGuid, true);
					}
				}

				PlayerController->ClientMessage(FString(L"Set size!"), FName(), 1.f);
			}
			else if (command == "randomize")
			{
				if (args.size() > 1)
				{
					if (args[1].contains("versionized") || args[1].contains("v"))
					{
						const bool bPreviousVersionized =
							FConfiguration::bUseVersionizedLoadout;
						const bool bPreviousCustom =
							FConfiguration::bUseCustomLoadout;
						FConfiguration::bUseVersionizedLoadout = true;
						FConfiguration::bUseCustomLoadout = false;
						LateGame::EquipLoadout(PlayerController);
						FConfiguration::bUseVersionizedLoadout =
							bPreviousVersionized;
						FConfiguration::bUseCustomLoadout =
							bPreviousCustom;
						PlayerController->ClientMessage(FString(L"Randomized LateGame loadout!"), FName(), 1.f);
						return;
					}
					else if (args[1].contains("oneshot") || args[1].contains("os"))
					{
						IsOneShot() == true;
						LateGame::EquipLoadout(PlayerController);
						IsOneShot() == false;
						PlayerController->ClientMessage(FString(L"Randomized LateGame loadout!"), FName(), 1.f);
						return;
					}
					else if (args[1].contains("customloadout") || args[1].contains("cl"))
					{
						if (!FConfiguration::Primary.IsEmpty() || !FConfiguration::Secondary.IsEmpty() || !FConfiguration::Tertiary.IsEmpty() || !FConfiguration::Quaternary.IsEmpty() || !FConfiguration::Quinary.IsEmpty())
						{
							if (FConfiguration::PrimaryAmount != 0 || FConfiguration::SecondaryAmount != 0 || FConfiguration::TertiaryAmount != 0 || FConfiguration::QuaternaryAmount != 0 || FConfiguration::QuinaryAmount != 0 || FConfiguration::TrapsAmount != 0)
							{
								const bool bPreviousVersionized =
									FConfiguration::bUseVersionizedLoadout;
								const bool bPreviousCustom =
									FConfiguration::bUseCustomLoadout;
								FConfiguration::bUseVersionizedLoadout =
									false;
								FConfiguration::bUseCustomLoadout = true;
								LateGame::EquipLoadout(PlayerController);
								FConfiguration::bUseVersionizedLoadout =
									bPreviousVersionized;
								FConfiguration::bUseCustomLoadout =
									bPreviousCustom;
								PlayerController->ClientMessage(FString(L"Randomized LateGame loadout!"), FName(), 1.f);
								return;
							}
							else
							{
								PlayerController->ClientMessage(FString(L"One or more of your slots amount is 0!"), FName(), 1.f);
								return;
							}
						}
						else
						{
							PlayerController->ClientMessage(FString(L"Could not find a custom loadout."), FName(), 1.f);
							return;
						}
					}
					else if (args[1].contains("loadout") || args[1].contains("l"))
					{
						LateGame::EquipLoadout(PlayerController);
						PlayerController->ClientMessage(FString(L"Randomized LateGame loadout!"), FName(), 1.f);
						return;
					}
				}

				LateGame::EquipLoadout(PlayerController);
				PlayerController->ClientMessage(FString(L"Randomized LateGame loadout!"), FName(), 1.f);
			}
			else if (command == "revive" || command == "res")
			{
				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"Could not find a pawn!"), FName(), 1.f);
					return;
				}

				if (Pawn->IsDBNO())
				{
					const bool bReviveStarted =
						AFortPlayerPawnAthena::
							ReviveFromDBNOCompat(
								Pawn,
								(AController*)
									PlayerController);
					PlayerController->ClientMessage(
						FString(
							bReviveStarted
								? L"Player revived!"
								: L"Could not revive player."),
						FName(),
						1.f);
				}
			}
			else if (command == "setrespawnpoint")
			{
				if (args.size() != 4)
				{
					PlayerController->ClientMessage(FString(L"Wrong number of arguments!"), FName(), 1.f);
					return;
				}

				static auto IsRespawningAllowedFunc = GameState->GetFunction("IsRespawningAllowed");

				bool bRespawnAllowed = false;

				if (!IsRespawningAllowedFunc)
				{
					auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;

					// respawn except storm needs to be fixed
					bRespawnAllowed = Playlist ? Playlist->RespawnType > 0 : false;
				}
				else
					bRespawnAllowed = GameState->Call<bool>(IsRespawningAllowedFunc, PlayerController->PlayerState);

				if (bRespawnAllowed)
				{
					double X = 0., Y = 0., Z = 0.;

					X = strtod(args[1].c_str(), nullptr);
					Y = strtod(args[2].c_str(), nullptr);
					Z = strtod(args[3].c_str(), nullptr);

					FConfiguration::CustomRespawnPoint = FVector((float)X, (float)Y, (float)Z);
					FConfiguration::HasCustomRespawnPoint = true;
					PlayerController->ClientMessage(FString(L"Set the respawn point!"), FName(), 1.f);
				}
				else
				{
					PlayerController->ClientMessage(FString(L"You are not on a respawning playlist!"), FName(), 1.f);
					return;
				}
			}
			else if (command == "clearinventory" || command == "wipeinventory" || command == "clearinv" || command == "clearall" || command == "wipeall")
			{
				std::vector<std::pair<FGuid, int>> GuidsAndCountsToRemove;
				auto& ItemInstances = PlayerController->WorldInventory->Inventory.ItemInstances;

				for (int i = 0; i < ItemInstances.Num(); ++i)
				{
					auto ItemInstance = ItemInstances[i];
					auto& ItemEntry = ItemInstance->GetItemEntry();
					const auto ItemDefinition = ItemEntry.ItemDefinition;

					if (ItemDefinition->HasbCanBeDropped() ? ItemDefinition->bCanBeDropped : (ItemDefinition->GetPickupComponent() ? ItemDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false))
					{
						GuidsAndCountsToRemove.push_back({ ItemEntry.ItemGuid, ItemEntry.Count });
					}
				}

				for (auto& [Guid, Count] : GuidsAndCountsToRemove)
				{
					PlayerController->WorldInventory->Remove(Guid);
				}

				PlayerController->ClientMessage(FString(L"Cleared the inventory!"), FName(), 1.f);
			}
			else if (command == "gravity")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please specify the amount you'd like to modify the gravity!"), FName(), 1.f);
					return;
				}

				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
					return;

				float Multiplier = 1.0f;

				try
				{
					std::string argStr(args[1].begin(), args[1].end());
					Multiplier = std::stof(argStr);
				}
				catch (...)
				{
					PlayerController->ClientMessage(FString(L"Invalid multiplier!"), FName(), 1.f);
					return;
				}

				Pawn->SetGravityMultiplier(Multiplier);

				PlayerController->ClientMessage(FString(L"Gravity multiplier set!"), FName(), 1.f);
			}
			else if (command == "regen")
			{
				auto Pawn = PlayerController->Pawn;
				auto PlayerState = PlayerController->PlayerState;

				float MaxHealth = Pawn->GetMaxHealth();
				float MaxShield = Pawn->GetMaxShield();

				if (Pawn)
				{
					Pawn->SetHealth(MaxHealth);
					Pawn->SetShield(MaxShield);

					auto Handle = PlayerState->AbilitySystemComponent->MakeEffectContext();
					FGameplayTag Tag;
					static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
					Tag.TagName = Cue;
					auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
					memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
					PlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
					PlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
					free(PredictionKey);

					PlayerController->ClientMessage(FString(L"Regenerated the player's health!"), FName(), 1.f);
				}
			}
			else if (command == "regenall")
			{
				UObject* NetDriver = UWorld::GetWorld()->NetDriver;

				if (!NetDriver)
				{
					PlayerController->ClientMessage(FString(L"NetDriver not found!"), FName(), 1.f);
					return;
				}

				UNetDriver* Driver = static_cast<UNetDriver*>(NetDriver);
				auto& ClientConnections = Driver->ClientConnections;

				if (ClientConnections.Num() <= 0)
				{
					PlayerController->ClientMessage(FString(L"No players found!"), FName(), 1.f);
					return;
				}

				int RegenCount = 0;

				for (int i = 0; i < ClientConnections.Num(); i++)
				{
					UNetConnection* Connection = ClientConnections[i];

					if (!Connection)
						continue;

					auto PC = (AFortPlayerControllerAthena*)Connection->PlayerController;

					if (!PC || !PC->Pawn)
						continue;

					auto Pawn = PC->Pawn;
					auto PS = PC->PlayerState;

					float MaxHealth = Pawn->GetMaxHealth();
					float MaxShield = Pawn->GetMaxShield();

					Pawn->SetHealth(MaxHealth);
					Pawn->SetShield(MaxShield);

					if (PS && PS->AbilitySystemComponent)
					{
						auto Handle = PS->AbilitySystemComponent->MakeEffectContext();
						FGameplayTag Tag;
						static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
						Tag.TagName = Cue;
						auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
						memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
						PS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
						PS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
						free(PredictionKey);
					}

					RegenCount++;
				}

				auto msg = L"Regenerated health and shield for " + std::to_wstring(RegenCount) + L" player(s)!";
				PlayerController->ClientMessage(FString(msg.c_str()), FName(), 1.f);
			}
			else if (command == "changename" || command == "name")
			{
				const auto CommandEnd =
					originalCommand.find_first_of(" \t");
				const auto NameStart =
					CommandEnd == std::string::npos
						? std::string::npos
						: originalCommand.find_first_not_of(
							" \t", CommandEnd);
				const auto NameEnd =
					originalCommand.find_last_not_of(" \t");
				if (NameStart == std::string::npos ||
					NameEnd == std::string::npos ||
					NameEnd < NameStart)
				{
					PlayerController->ClientMessage(FString(L"Please include a phrase/name you'd want to change to!"), FName(), 1);
					return;
				}

				// ServerCheat keeps a lower-cased copy for case-insensitive
				// command dispatch. Read the display name from the untouched
				// input so "cheat name Hello World" stays "Hello World".
				std::string nameStr =
					std::string(
						originalCommand.data() + NameStart,
						NameEnd - NameStart + 1);

				if (nameStr.empty())
				{
					PlayerController->ClientMessage(FString(L"Invalid name!"), FName(), 1);
					return;
				}

				std::wstring nameW(nameStr.begin(), nameStr.end());
				FString NewName = FString(nameW.c_str());

				if (!PlayerController)
					return;

				PlayerController->ServerChangeName(NewName);

				PlayerController->ClientMessage(FString(L"Changed the player's name!"), FName(), 1.f);
			}
			else if (command == "pausetimeofday" || command == "pausetime" || command == "pt")
			{
				auto World = UWorld::GetWorld();
				float CurrentSpeed = 1.f;
				const bool bHasCurrentSpeed =
					UFortKismetLibrary::
						GetTimeOfDaySpeedCompat(
							World, CurrentSpeed);
				const bool bIsPaused =
					(bHasCurrentSpeed &&
						std::fabs(CurrentSpeed) <=
							std::numeric_limits<float>::
								epsilon()) ||
					(!bHasCurrentSpeed &&
						FConfiguration::bAutoPauseTODM.load(
							std::memory_order_acquire));
				const float NewSpeed =
					bIsPaused ? 1.f : 0.f;

				// Auto Pause owns the manager every server tick. Turning it
				// off here prevents an explicit unpause command from being
				// immediately overwritten by the Trickshot policy.
				if (bIsPaused &&
					FConfiguration::bAutoPauseTODM.load(
						std::memory_order_acquire))
				{
					FConfiguration::bAutoPauseTODM.store(
						false,
						std::memory_order_release);
				}

				const bool bChanged =
					UFortKismetLibrary::
						SetTimeOfDaySpeedCompat(
							World, NewSpeed);
				if (!bChanged)
				{
					PlayerController->ClientMessage(
						FString(
							L"Time-of-day manager is not ready!"),
						FName(), 1.f);
					return;
				}

				if (bIsPaused)
					PlayerController->ClientMessage(FString(L"Unpaused time of day!"), FName(), 1.f);
				else
					PlayerController->ClientMessage(FString(L"Paused time of day!"), FName(), 1.f);
			}
			else if (command == "sethealth" || command == "health")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please choose an amount to set your health to!"), FName(), 1.f);
					return;
				}

				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn!"), FName(), 1.f);
					return;
				}

				float Health = 100.f;

				try { Health = std::stof(std::string(args[1])); }
				catch (...) {}

				Pawn->SetHealth(Health);
				PlayerController->ClientMessage(FString(L"Set pawn's health!"), FName(), 1.f);
			}
			else if (command == "setshield" || command == "shield")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please choose an amount to set your shield to!"), FName(), 1.f);
					return;
				}

				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn!"), FName(), 1.f);
					return;
				}

				float Shield = 100.f;

				try { Shield = std::stof(std::string(args[1])); }
				catch (...) {}

				Pawn->SetShield(Shield);
				PlayerController->ClientMessage(FString(L"Set pawn's shield!"), FName(), 1.f);
			}
			else if (command == "setmaxhealth" || command == "maxhealth")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please choose an amount to set your max health to!"), FName(), 1.f);
					return;
				}

				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn!"), FName(), 1.f);
					return;
				}

				float Health = 100.f;

				try { Health = std::stof(std::string(args[1])); }
				catch (...) {}

				Pawn->SetMaxHealth(Health);
				PlayerController->ClientMessage(FString(L"Set pawn's max health!"), FName(), 1.f);
			}
			else if (command == "setmaxshield" || command == "maxshield")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please choose an amount to set your max shield to!"), FName(), 1.f);
					return;
				}

				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn!"), FName(), 1.f);
					return;
				}

				float Shield = 100.f;

				try { Shield = std::stof(std::string(args[1])); }
				catch (...) {}

				Pawn->SetMaxShield(Shield);
				PlayerController->ClientMessage(FString(L"Set pawn's max shield!"), FName(), 1.f);
			}
			else if (command == "effect")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please provide a custom GameplayCue."), FName(), 1.f);
					return;
				}

				auto PlayerState = PlayerController->PlayerState;

				auto Handle = PlayerState->AbilitySystemComponent->MakeEffectContext();
				FGameplayTag Tag;

				std::wstring WideCue(args[1].begin(), args[1].end());

				FName Cue(WideCue.c_str());
				Tag.TagName = Cue;

				auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
				memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());

				PlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
				PlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
				PlayerController->ClientMessage(FString(L"Applied effect!"), FName(), 1.f);

				free(PredictionKey);
			}
			else if (command == "applyge" || command == "givege")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat applyge <index | effect name/path | remove>. See cheat dumpge for indexes."), FName(), 1.f);
					return;
				}

				auto PlayerState = PlayerController->PlayerState;
				auto Pawn = PlayerController->Pawn;

				if (!PlayerState ||
					!PlayerState->AbilitySystemComponent)
				{
					PlayerController->ClientMessage(
						FString(L"No Ability System Component was found for your player."),
						FName(),
						1.f);
					return;
				}

				if (UAbilitySystemComponent* AbilitySystemComponent = PlayerState->AbilitySystemComponent)
				{
					const UClass* GEClass = nullptr;
					int GameplayEffectIndex = 0;
					std::string GameplayEffectArg = args[1].c_str();

					if (NormalizePlayerCommandString(GameplayEffectArg) ==
						"remove")
					{
						const auto RemovalResult =
							RemoveAllGameplayEffectsForCommand(
								AbilitySystemComponent);
						auto Message =
							GetRemoveGameplayEffectsCommandMessage(
								RemovalResult);

						PlayerController->ClientMessage(
							FString(Message.c_str()),
							FName(),
							1.f);
						return;
					}

					if (TryParseCommandInt(GameplayEffectArg, GameplayEffectIndex))
					{
						auto GameplayEffects =
							GetLoadedGameplayEffectCatalogForCommand();

						if (!GameplayEffects)
						{
							PlayerController->ClientMessage(FString(L"No completed Gameplay Effect catalog is available. Run cheat dumpge and wait for its completion message."), FName(), 1.f);
							return;
						}

						if (GameplayEffects->empty())
						{
							PlayerController->ClientMessage(FString(L"The completed Gameplay Effect catalog is empty. Run cheat dumpge again after more assets have loaded."), FName(), 1.f);
							return;
						}

						if (GameplayEffectIndex < 0 || GameplayEffectIndex >= static_cast<int>(GameplayEffects->size()))
						{
							wchar_t wmsg[112];
							swprintf_s(wmsg, 112, L"Invalid index. Use 0 to %d from DumpedGameplayEffects.txt.", static_cast<int>(GameplayEffects->size()) - 1);
							PlayerController->ClientMessage(FString(wmsg), FName(), 1.f);
							return;
						}

						auto& GameplayEffect =
							(*GameplayEffects)[GameplayEffectIndex];
						GEClass =
							GameplayEffect.GameplayEffectClass.Get();
					}
					else
					{
						GEClass = FindGameplayEffectClassByCommandArg(GameplayEffectArg);
					}

					if (!IsGameplayEffectClassForCommand(GEClass))
					{
						PlayerController->ClientMessage(FString(L"Could not find that Gameplay Effect. Trigger it while outputge is enabled, rerun cheat dumpge after it loads, or use a full path."), FName(), 1.f);
						return;
					}

					FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

					Context.Instigator = PlayerController;
					Context.Causer = Pawn;
					Context.AddSourceObject(Pawn);

					auto AppliedHandle =
						AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(
							GEClass,
							1.0f,
							Context);
					const bool bGameplayEffectApplied =
						AppliedHandle.bPassedFiltersAndWasExecuted;

					PlayerController->ClientMessage(
						FString(bGameplayEffectApplied
							? L"Applied Gameplay Effect!"
							: L"That Gameplay Effect was rejected by the Ability System."),
						FName(),
						1.f);
				}
			}
			else if (command == "removege")
			{
				auto PlayerState = PlayerController->PlayerState;
				auto AbilitySystemComponent =
					PlayerState
						? PlayerState->AbilitySystemComponent
						: nullptr;

				if (!AbilitySystemComponent)
				{
					PlayerController->ClientMessage(
						FString(L"No Ability System Component was found for your player."),
						FName(),
						1.f);
					return;
				}

				const auto RemovalResult =
					RemoveAllGameplayEffectsForCommand(
						AbilitySystemComponent);
				auto Message =
					GetRemoveGameplayEffectsCommandMessage(
						RemovalResult);

				PlayerController->ClientMessage(
					FString(Message.c_str()),
					FName(),
					1.f);
			}
			else if (command == "outputge")
			{
				const bool bEnabled =
					ToggleGameplayEffectOutputForCommand(PlayerController);

				SendGameplayEffectOutputMessage(
					PlayerController,
					bEnabled
						? L"Active Gameplay Effect output enabled.\nWatching future active duration/infinite effects on your player; instant or very brief effects may not appear. Output is sampled, batched, and capped for stability."
						: L"Active Gameplay Effect console output disabled!");
			}
			else if (command == "cipher")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please input a website link to open!"), FName(), 1.f);
					return;
				}

				FString URL = args[1];

				/*if (!URL.StartsWith(L"http://") && !URL.StartsWith(L"https://"))
				{
					PlayerController->ClientMessage(FString(L"Input must start with https://"), FName(), 1.f);
					return;
				}*/

				int AmountToOpen = 1;

				if (args.size() >= 3)
				{
					try { AmountToOpen = std::stoi(args[2].c_str(), nullptr); }
					catch (...) {}
				}

				if (AmountToOpen <= 0)
					AmountToOpen = 1;

				if (AmountToOpen > 20)
					AmountToOpen = 20;

				AmountToOpen = FMath::Clamp(AmountToOpen, 1, 10);

				for (int32 i = 0; i < AmountToOpen; ++i)
				{
					bool bSuccess = Memcury::Util::OpenURL(std::wstring(*URL));

					if (!bSuccess)
					{
						PlayerController->ClientMessage(FString(L"ShellExecute failed!"), FName(), 1.f);
					}
				}

				PlayerController->ClientMessage(FString(L"Opened link!"), FName(), 1.f);
			}
			else if (command == "setkills" || command == "kills")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please choose an amount to set your kills to!"), FName(), 1.f);
					return;
				}

				auto PlayerState = PlayerController->PlayerState;
				int Count = 1;

				if (args.size() >= 2)
				{
					try { Count = std::stoi(args[1].c_str(), nullptr); }
					catch (...) {}
				}

				if (PlayerState->HasKillScore())
				{
					PlayerState->KillScore = Count;
				}
				else
				{
					PlayerState->Kills = Count;
				}

				PlayerState->OnRep_Kills();
				PlayerController->ClientMessage(FString(L"Set kills!"), FName(), 1.f);
			}
			else if (command == "setpoints" || command == "setarenapoints")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please choose an amount to set your points to!"), FName(), 1.f);
					return;
				}

				int AlivePlayers = GameMode->AlivePlayers.Num();

				int Points = 1;

				if (args.size() >= 2)
				{
					try { Points = std::stoi(args[1].c_str(), nullptr); }
					catch (...) {}
				}

				if (GUI::IsArenaPlaylist() || GUI::IsTournamentPlaylist())
				{
					PlayerController->ClientReportTournamentPlacementPointsScored(AlivePlayers, Points);

					std::wstring PointsStr = std::to_wstring(Points) + L"!";
					FString Message = FString((L"Set your arena points to " + PointsStr + L"\n").c_str());
					PlayerController->ClientMessage(Message, FName(), 1.f);
					PlayerController->ClientMessage(FString(L"Use a negative number to take away points!"), FName(), 1.f);
				}
				else
				{
					PlayerController->ClientMessage(FString(L"Please choose an amount to set your kills to!"), FName(), 1.f);
					PlayerController->ClientMessage(FString(L"Use a negative number to take away points!"), FName(), 1.f);
					return;
				}
			}
			else if (command == "keepinv" || command == "keepinventory")
			{
				FConfiguration::bKeepInventory.store(
					!FConfiguration::bKeepInventory.load());
				PlayerController->ClientMessage(FString(L"Toggled keep inventory!"), FName(), 1.f);
			}
			else if (command == "destroyall")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Wrong number of arguments!"), FName(), 1.f);
					return;
				}

				TArray<AActor*> Actors;

				auto ActorClass = FindObject<UClass>(UEAllocatedWString(args[1].begin(), args[1].end()).c_str());

				if (!ActorClass)
					ActorClass = FindClass(args[1].c_str());

				if (args[1].contains("pickup"))
					ActorClass = AFortPickupAthena::StaticClass();

				if (!ActorClass)
				{
					auto ShortNames = Misc::ObjectNames.find(args[1].c_str());

					if (ShortNames != Misc::ObjectNames.end())
						ActorClass = FindObject<UClass>(UEAllocatedWString(ShortNames->second.begin(), ShortNames->second.end()).c_str());
				}

				if (args[1].contains("volume"))
				{
					UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"destroyall volume"), nullptr);
					PlayerController->ClientMessage(FString(L"Destroyed the barrier!"), FName(), 1.f);
					return;
				}

				if (!ActorClass)
				{
					PlayerController->ClientMessage(FString(L"Invalid class path!"), FName(), 1.f);
					return;
				}

				//UGameplayStatics::GetAllActorsOfClass(UWorld::GetWorld(), ActorClass, &Actors); // if this crashes then do it other way around
				Utils::GetAll<AActor>(ActorClass, Actors);

				for (auto Actor : Actors)
				{
					if (Actor)
					{
						Actor->K2_DestroyActor();
					}
					else
					{
						PlayerController->ClientMessage(FString(L"Could not find actor!"), FName(), 1.f);
						return;
					}
				}

				PlayerController->ClientMessage(FString(L"Destroyed all specified actors!"), FName(), 1.f);
				Actors.Free();
			}
			else if (command == "fly")
			{
				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn found!"), FName(), 1.f);
					return;
				}

				Pawn->SetActorEnableCollision(true);

				if (Pawn->CharacterMovement)
				{
					Pawn->CharacterMovement->bCheatFlying = !Pawn->CharacterMovement->bCheatFlying;

					if (Pawn->CharacterMovement->bCheatFlying)
					{
						Pawn->CharacterMovement->SetMovementMode(EMovementMode::MOVE_Flying, 0);
						PlayerController->ClientMessage(FString(L"Enabled flying!"), FName(), 1.f);

						if (args.size() == 2)
						{
							try {
								float Speed = std::stof(std::string(args[1]));
								static auto SetMovementSpeedFn = Pawn->GetFunction("SetMovementSpeed");
								if (!SetMovementSpeedFn)
									SetMovementSpeedFn = Pawn->GetFunction("SetMovementSpeedMultiplier");
								if (SetMovementSpeedFn)
									Pawn->ProcessEvent(SetMovementSpeedFn, &Speed);
							} catch (...) {}
						}
					}
					else
					{
						Pawn->CharacterMovement->SetMovementMode(EMovementMode::MOVE_Walking, 0);

						float ResetSpeed = 1.0f;
						static auto SetMovementSpeedFn2 = Pawn->GetFunction("SetMovementSpeed");
						if (!SetMovementSpeedFn2)
							SetMovementSpeedFn2 = Pawn->GetFunction("SetMovementSpeedMultiplier");
						if (SetMovementSpeedFn2)
							Pawn->ProcessEvent(SetMovementSpeedFn2, &ResetSpeed);

						PlayerController->ClientMessage(FString(L"Disabled flying"), FName(), 1.f);
					}
				}
			}
			else if (command == "ghost")
			{
				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn found!"), FName(), 1.f);
					return;
				}

				Pawn->SetActorEnableCollision(!Pawn->bActorEnableCollision);

				if (Pawn->CharacterMovement)
				{
					Pawn->CharacterMovement->bCheatFlying = !Pawn->CharacterMovement->bCheatFlying;

					if (Pawn->CharacterMovement->bCheatFlying)
					{
						Pawn->CharacterMovement->SetMovementMode(EMovementMode::MOVE_Flying, 0);

						if (args.size() == 2)
						{
							try {
								float Speed = std::stof(std::string(args[1]));
								static auto SetMovementSpeedFn = Pawn->GetFunction("SetMovementSpeed");
								if (!SetMovementSpeedFn)
									SetMovementSpeedFn = Pawn->GetFunction("SetMovementSpeedMultiplier");
								if (SetMovementSpeedFn)
									Pawn->ProcessEvent(SetMovementSpeedFn, &Speed);
							} catch (...) {}
						}
					}
					else
					{
						Pawn->CharacterMovement->SetMovementMode(EMovementMode::MOVE_Walking, 0);

						float ResetSpeed = 1.0f;
						static auto SetMovementSpeedFn3 = Pawn->GetFunction("SetMovementSpeed");
						if (!SetMovementSpeedFn3)
							SetMovementSpeedFn3 = Pawn->GetFunction("SetMovementSpeedMultiplier");
						if (SetMovementSpeedFn3)
							Pawn->ProcessEvent(SetMovementSpeedFn3, &ResetSpeed);
					}
				}
			}
			else if (command == "flyspeed")
			{
				if (args.size() != 2)
				{
					PlayerController->ClientMessage(FString(L"Wrong number of arguments!"), FName(), 1.f);
					return;
				}

				int Index = 1;

				try
				{
					Index = std::stoi(args[1].c_str(), nullptr);
				}
				catch (...)
				{
				}

				PlayerController->FlyingModifierIndex = Index;
				PlayerController->OnRep_FlyingModifierIndex();
			}
			else if (command == "timeofday" || command == "time" || command == "t")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Wrong number of arguments!"), FName(), 1.f);
					return;

				}

				float NewTOD = 0.f;
				bool bParsedTime = false;
				try
				{
					NewTOD =
						std::stof(std::string(args[1]));
					bParsedTime =
						std::isfinite(NewTOD);
				}
				catch (...)
				{
				}
				if (!bParsedTime)
				{
					PlayerController->ClientMessage(
						FString(
							L"Please enter a valid time from 0 to 24!"),
						FName(), 1.f);
					return;
				}

				NewTOD = std::fmod(NewTOD, 24.f);
				if (NewTOD < 0.f)
					NewTOD += 24.f;

				const bool bAutoPause =
					FConfiguration::bAutoPauseTODM.load(
						std::memory_order_acquire);
				// Keep the command and slider as one source of truth so the
				// policy cannot snap this command back 0.5s later, and a
				// later Auto Pause enable starts at the last commanded time.
				FConfiguration::TODMTime.store(
					NewTOD,
					std::memory_order_release);

				auto World = UWorld::GetWorld();
				const bool bTimeChanged =
					UFortKismetLibrary::
						SetTimeOfDayCompat(
							World, NewTOD);
				const bool bPauseChanged =
					!bAutoPause ||
					UFortKismetLibrary::
						SetTimeOfDaySpeedCompat(
							World, 0.f);
				if (!bTimeChanged ||
					!bPauseChanged)
				{
					PlayerController->ClientMessage(
						FString(
							L"Time-of-day manager is not ready; the selected time was saved."),
						FName(), 1.f);
					return;
				}

				PlayerController->ClientMessage(FString(L"Set time of day!"), FName(), 1.f);
			}
			else if (command == "spawnbot" || command == "bot")
			{
				if (!PlayerController->Pawn)
					return;

				auto CallerController = PlayerController;
				int Count = 1;
				std::string WeaponArg = "";
				FVector BotScale = FVector(1.f, 1.f, 1.f);
				bool HasLocation = false;
				FVector SpawnLocation{};

				for (size_t ArgIndex = 1; ArgIndex < args.size(); ArgIndex++)
				{
					auto CurrentArg = std::string(args[ArgIndex].c_str());
					float ParsedScale = 0.f;

					if (TryParsePrefixedCommandVector(CurrentArg, 's', BotScale))
						continue;

					if (LooksLikeSizeOrHeightModifierArg(CurrentArg))
					{
						PlayerController->ClientMessage(FString(L"Invalid bot size. Use s2 for 2x, or s1,1,5 for per-axis (X,Y,Z)."), FName(), 1.f);
						return;
					}

					if (!HasLocation && ArgIndex + 2 < args.size())
					{
						float X = 0.f;
						float Y = 0.f;
						float Z = 0.f;

						std::string YArg = std::string(args[ArgIndex + 1].c_str());
						std::string ZArg = std::string(args[ArgIndex + 2].c_str());

						if (TryParseCommandFloat(CurrentArg, X) && TryParseCommandFloat(YArg, Y) && TryParseCommandFloat(ZArg, Z))
						{
							SpawnLocation = FVector(X, Y, Z);
							HasLocation = true;
							ArgIndex += 2;
							continue;
						}
					}

					int ParsedCount = 0;

					if (TryParseCommandInt(CurrentArg, ParsedCount))
					{
						Count = ParsedCount;
						continue;
					}

					if (CurrentArg.find('.') != std::string::npos && TryParseCommandFloat(CurrentArg, ParsedScale))
					{
						BotScale = FVector(ParsedScale, ParsedScale, ParsedScale);
						continue;
					}

					if (WeaponArg.empty())
					{
						WeaponArg = CurrentArg;
						continue;
					}

					PlayerController->ClientMessage(FString(L"Invalid spawnbot argument. Use count, weapon, s2 or s1,1,5 size, or X Y Z location."), FName(), 1.f);
					return;
				}

				for (int i = 0; i < Count; i++)
				{
					auto Transform = PlayerController->Pawn->GetTransform();
					Transform.Scale3D = BotScale;

					if (HasLocation)
						Transform.Translation = SpawnLocation;

					auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
					auto GameState = GameMode->GameState;
					auto Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(GameMode->DefaultPawnClass, Transform);
					auto PC = (AFortPlayerControllerAthena*)UWorld::SpawnActor(FindObject<UClass>(L"/Game/Athena/Athena_PlayerController.Athena_PlayerController_C"), Transform);
					//auto PlayerState = PlayerController->PlayerState;

					if (!PC || !Pawn)
					{
						if (Pawn)
							Pawn->K2_DestroyActor();
						if (PC)
							PC->K2_DestroyActor();
						continue;
					}

					PC->Possess(Pawn);
					PC->MyFortPawn = Pawn; // dont't ask, crashes on 27+

					auto PlayerState = (AFortPlayerStateAthena*)UWorld::SpawnActor(AFortPlayerStateAthena::StaticClass(), Transform);

					if (!PlayerState)
					{
						Pawn->K2_DestroyActor();
						PC->K2_DestroyActor();
						continue;
					}

					PlayerState->SetOwner(PC);

					PC->PlayerState = PlayerState;
					PC->OnRep_PlayerState();

					Pawn->PlayerState = PlayerState;
					Pawn->OnRep_PlayerState();

					Pawn->SetMaxHealth(FConfiguration::BotHealth);
					Pawn->SetHealth(FConfiguration::BotHealth);
					Pawn->SetMaxShield(FConfiguration::BotShield);
					Pawn->SetShield(FConfiguration::BotShield);

					Pawn->bReplicates = true;
					Pawn->bAlwaysRelevant = true;
					Pawn->NetCullDistanceSquared = FLT_MAX;
					Pawn->SetNetDormancy(ENetDormancy::DORM_Never);
					Pawn->NetUpdateFrequency = 100.f;
					Pawn->MinNetUpdateFrequency = 100.f;

					PlayerState->TeamIndex = AFortGameMode::PickTeam(GameMode, 0, PC);
					if (PlayerState->HasSquadId())
						PlayerState->SquadId = PlayerState->TeamIndex - 3;
					if (PlayerState->HasbIsABot())
						PlayerState->bIsABot = true;

					if (GameState->HasGameMemberInfoArray())
					{
						auto Member = (FGameMemberInfo*)malloc(FGameMemberInfo::Size());
						if (Member)
						{
							memset((PBYTE)Member, 0, FGameMemberInfo::Size());
							Member->MostRecentArrayReplicationKey = -1;
							Member->ReplicationID = -1;
							Member->ReplicationKey = -1;
							Member->TeamIndex = PlayerState->TeamIndex;
							Member->SquadId = PlayerState->SquadId;
							Member->MemberUniqueId = PlayerState->UniqueId;

							GameState->GameMemberInfoArray.Members.Add(
								*Member, FGameMemberInfo::Size());
							GameState->GameMemberInfoArray.MarkItemDirty(*Member);

							auto NotifyGameMemberAdded = (void(*)(AFortGameStateAthena*, uint8_t, uint8_t, FUniqueNetIdRepl*)) NotifyGameMemberAdded_;
							if (NotifyGameMemberAdded)
								NotifyGameMemberAdded(
									GameState,
									Member->SquadId,
									Member->TeamIndex,
									&Member->MemberUniqueId);

							free(Member);
						}
					}

					for (auto& AbilitySet : AFortGameMode::AbilitySets)
						PlayerState->AbilitySystemComponent->GiveAbilitySet(AbilitySet);

					EnsureOneShotLowGravityVfx(PC, Pawn);

					if (!PC->WorldInventory)
					{
						PC->WorldInventory = (AFortInventory*)UWorld::SpawnActor(AFortInventory::StaticClass(), Transform);

						if (PC->WorldInventory)
						{
							PC->WorldInventory->SetOwner(PC);
							PC->WorldInventory->InventoryType = 0;
						}
					}

					PC->bHasInitializedWorldInventory = true;

					if (VersionInfo.FortniteVersion == 1.72)
					{
						// Unlike a real connection, a synthetic controller never
						// reaches possession acknowledgement, where this legacy
						// alive flag is normally initialized.
						if (PC->HasbMarkedAlive())
							PC->bMarkedAlive = true;
						if (PC->HasbClientNotifiedOfPawnDied())
							PC->bClientNotifiedOfPawnDied = false;
						G172SpawnedBotRemovalAttempts.erase(PC);
					}

					GameState->PlayersLeft++;
					GameState->OnRep_PlayersLeft();

					GameMode->AlivePlayers.Add(PC);
					RegisterTrackedSpawnedBotController(PC);

					// The bot is now eligible to become Infinite Render's viewpoint.
					// Wake its replicated state immediately so existing clients can
					// open/update the actor channel on the next replication pass.
					Pawn->FlushNetDormancy();
					Pawn->ForceNetUpdate();
					PlayerState->ForceNetUpdate();

					static auto Commando = FindObject(L"/Game/Athena/Heroes/HID_001_Athena_Commando_F.HID_001_Athena_Commando_F", nullptr);
					auto BotHero = Commando;
					if (!BotHero)
					{
						// Evaluate the fallback lazily. HID_Commando_Athena_01 is
						// absent on 2.50, and attempting to resolve it despite
						// HID_001 being valid produces a misleading load failure.
						static auto CommandoFallback = FindObject(L"/Game/Athena/Heroes/HID_Commando_Athena_01.HID_Commando_Athena_01", nullptr);
						BotHero = CommandoFallback;
					}
					PlayerState->HeroType = BotHero;

					static auto Head = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");
					static auto Body = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01");
					static auto Backpack = FindObject<UObject>(L"/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack");

					static auto CharacterPartsOffset = PlayerState->GetOffset("CharacterParts", 0x100000);

					if (CharacterPartsOffset == -1)
					{
						static auto CharacterPartsOff = PlayerState->GetOffset("CharacterParts");
						if (CharacterPartsOff == -1)
							CharacterPartsOff = PlayerState->GetOffset("LocalCharacterParts");
						auto& CharacterParts = GetFromOffset<const UObject * [0x6]>(PlayerState, CharacterPartsOff);

						CharacterParts[0] = Head;
						CharacterParts[1] = Body;
						CharacterParts[3] = Backpack;
					}
					else
					{
						static auto CharacterPartsOff = PlayerState->GetOffset("CharacterParts");
						auto& CustomCharacterParts = GetFromOffset<FCustomCharacterParts>(PlayerState, CharacterPartsOff);
						static auto PartsOffset = FCustomCharacterParts::StaticStruct()->GetOffset("Parts");
						auto& CharacterParts = GetFromOffset<const UObject * [0x6]>(&CustomCharacterParts, PartsOffset);

						CharacterParts[0] = Head;
						CharacterParts[1] = Body;
						CharacterParts[3] = Backpack;
					}

					if (VersionInfo.FortniteVersion == 2.50)
					{
						// Synthetic controllers have no Athena profile or HeroId,
						// so 2.50's profile-driven customization always fails and
						// replaces these valid parts with an incomplete fallback.
						// Choose and replicate the explicit parts directly.
						if (auto OnRepHeroType = PlayerState->GetFunction("OnRep_HeroType"))
							PlayerState->ProcessEvent(OnRepHeroType, nullptr);

						if (Head)
							Pawn->ServerChoosePart(0, Head);
						if (Body)
							Pawn->ServerChoosePart(1, Body);
						if (Backpack)
							Pawn->ServerChoosePart(3, Backpack);

						if (auto OnRepCharacterParts = PlayerState->GetFunction("OnRep_CharacterParts"))
							PlayerState->ProcessEvent(OnRepCharacterParts, nullptr);

						// The first notification happened before this synthetic
						// PlayerState had any hero/parts. Notify the pawn again
						// after the complete cosmetic state has been populated.
						Pawn->OnRep_PlayerState();

						Pawn->ForceNetUpdate();
						PlayerState->ForceNetUpdate();
						SDK::DbgLog("[SpawnBot] applied direct 2.50 character parts controller=%p pawn=%p hero=%p head=%p body=%p\n",
							(void*)PC, (void*)Pawn, (void*)BotHero,
							(void*)Head, (void*)Body);
					}
					else if (ApplyCharacterCustomization)
						((void (*)(AActor*, AFortPlayerPawnAthena*)) ApplyCharacterCustomization)(PlayerState, Pawn);

					SetActorScaleForCommand(Pawn, BotScale);

					PlayerBotID++;

					std::string BaseName = FConfiguration::BotName;
					std::string FinalName;

					if (FConfiguration::UseCustomBotNames)
					{
						if (FString::EndsWithSpace(BaseName))
						{
							BaseName.pop_back();

							FinalName = BaseName + " (#" + std::to_string(PlayerBotID) + ")";
						}
						else
						{
							FinalName = BaseName;
						}
					}
					else
					{
						int RandomNumber = 200 + (std::rand() % 151);

						char Buffer[4];
						snprintf(Buffer, sizeof(Buffer), "%03d", RandomNumber);

						FinalName = "Anonymous [" + std::string(Buffer) + "]";
					}

					std::wstring WideName(FinalName.begin(), FinalName.end());

					FString BotName = FString(WideName.c_str());

					if (std::floor(VersionInfo.FortniteVersion) < 9)
					{
						PC->ServerChangeName(BotName);
					}
					else
					{
						GameMode->ChangeName(PC, BotName, true);
					}

					PlayerState->OnRep_PlayerName();

					static auto DefaultPickaxe = FindObject<UFortItemDefinition>(L"/Game/Athena/Items/Weapons/WID_Harvest_Pickaxe_Athena_C_T01.WID_Harvest_Pickaxe_Athena_C_T01");

					if (DefaultPickaxe)
						PC->WorldInventory->GiveItem(DefaultPickaxe);

					static auto SmartItemDefClass = FindClass("FortSmartBuildingItemDefinition");

					for (int i = 0; i < GameMode->StartingItems.Num(); i++)
					{
						auto& StartingItem = GameMode->StartingItems.Get(i, FItemAndCount::Size());
						if (StartingItem.Count && StartingItem.Item && (!SmartItemDefClass || !StartingItem.Item->IsA(SmartItemDefClass)))
							PC->WorldInventory->GiveItem(StartingItem.Item, StartingItem.Count);
					}

					UFortItemDefinition* CustomWeapon = nullptr;

					if (!WeaponArg.empty())
					{
						CustomWeapon = const_cast<UFortItemDefinition*>(FindObject<UFortItemDefinition>(UEAllocatedWString(WeaponArg.begin(), WeaponArg.end())));

						if (!CustomWeapon)
							CustomWeapon = const_cast<UFortItemDefinition*>(TUObjectArray::FindObject<UFortItemDefinition>(WeaponArg.c_str()));

						if (!CustomWeapon)
						{
							std::string DoubleNamed = WeaponArg + "." + WeaponArg;
							CustomWeapon = const_cast<UFortItemDefinition*>(TUObjectArray::FindObject<UFortItemDefinition>(DoubleNamed.c_str()));
						}

						if (!CustomWeapon)
						{
							auto ShortNames = Misc::ItemNames.find(WeaponArg);

							if (ShortNames != Misc::ItemNames.end())
							{
								std::string Value = ShortNames->second;
								CustomWeapon = const_cast<UFortItemDefinition*>(TUObjectArray::FindObject<UFortItemDefinition>(Value.c_str()));

								if (!CustomWeapon && Value.find('/') != std::string::npos)
								{
									auto Item = StaticLoadObject(UEAllocatedWString(Value.begin(), Value.end()).c_str(), UFortItemDefinition::StaticClass());

									if (Item)
										CustomWeapon = Item->Cast<UFortItemDefinition>();
								}
							}
						}

						if (CustomWeapon)
							PC->WorldInventory->GiveItem(CustomWeapon, 1);
						else
							CallerController->ClientMessage(FString(L"Failed to find item! Try passing it as a path or check your spelling & casing"), FName(), 1.f);
					}

					// A manually spawned controller has no owning connection, so
					// 1.7.2 and 2.50 never perform the native quickbar selection
					// that real players receive. Equip its server weapon actor
					// explicitly; observers receive CurrentWeapon through pawn
					// replication.
					const bool bCanEquipSpawnedBot =
						VersionInfo.FortniteVersion > 3.00 ||
						UsesTrackedLegacySpawnedBotLifecycle();
					if (bCanEquipSpawnedBot && PC->WorldInventory &&
						PC->WorldInventory->Inventory.ReplicatedEntries.Num() > 0)
					{
						FFortItemEntry* PickaxeEntry = nullptr;
						FFortItemEntry* CustomWeaponEntry = nullptr;

						for (int e = 0; e < PC->WorldInventory->Inventory.ReplicatedEntries.Num(); e++)
						{
							auto* Entry = (FFortItemEntry*)((PBYTE)PC->WorldInventory->Inventory.ReplicatedEntries.GetData() + (e * FFortItemEntry::Size()));

							if (!Entry || !Entry->ItemDefinition)
								continue;

							if (!PickaxeEntry && Entry->ItemDefinition->IsA<UFortWeaponMeleeItemDefinition>())
								PickaxeEntry = Entry;

							if (CustomWeapon && !CustomWeaponEntry && Entry->ItemDefinition == CustomWeapon)
								CustomWeaponEntry = Entry;
						}

						FFortItemEntry* EntryToEquip = CustomWeaponEntry ? CustomWeaponEntry : PickaxeEntry;

						if (EntryToEquip && EntryToEquip->ItemDefinition)
						{
							PC->ServerExecuteInventoryItem(EntryToEquip->ItemGuid);
							if (VersionInfo.FortniteVersion > 3.00)
								PC->ClientEquipItem(EntryToEquip->ItemGuid, true);

							Pawn->ForceNetUpdate();
							PC->ForceNetUpdate();
							SDK::DbgLog("[SpawnBot] equipped controller=%p pawn=%p entry=%p weapon=%p version=%.2f\n",
								(void*)PC, (void*)Pawn, (void*)EntryToEquip,
								(void*)GetPawnCurrentWeaponSafe(Pawn),
								VersionInfo.FortniteVersion);
						}
					}

					auto Message = L"Spawned a player bot! (s" + FormatCommandScaleForMessage(BotScale) +
						(HasLocation
							? L" @ " + FormatCommandFloatForMessage((float)SpawnLocation.X) + L" " + FormatCommandFloatForMessage((float)SpawnLocation.Y) + L" " + FormatCommandFloatForMessage((float)SpawnLocation.Z)
							: std::wstring()) +
						L")";
					CallerController->ClientMessage(FString(Message.c_str()), FName(), 1.f);
				}
			}
			else if (command == "tpbot" || command == "tpbots")
			{
				for (auto& Player : GameMode->AlivePlayers)
				{
					auto Controller = (AFortPlayerControllerAthena*)Player;

					if (Player && Player != PlayerController)
					{
						auto Controller = (AFortPlayerControllerAthena*)Player;
						auto BotPawn = Controller->Pawn;
						auto BotPS = Controller->PlayerState;
						auto PlayerPawn = PlayerController->Pawn;

						if (BotPawn && PlayerPawn)
						{
							if (BotPS->bIsABot)
							{
								auto ActorLocation = PlayerPawn->K2_GetActorLocation();
								ActorLocation.Z += 200.f;

								BotPawn->K2_TeleportTo(ActorLocation, PlayerPawn->K2_GetActorRotation(), false, true);
							}
						}
					}
				}
			}
			else if (command == "tpall")
			{
				TeleportAllPlayersTo(PlayerController);
			}
			else if (command == "tpto")
			{
				auto PlayerPawn = PlayerController->Pawn;

				if (!PlayerPawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn!"), FName(), 1.f);
					return;
				}

				auto PlayerNameStart = fullCommand.find(' ');

				if (PlayerNameStart == std::string::npos)
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat tpto <exact player name>"), FName(), 1.f);
					return;
				}

				std::string PlayerNameInput = fullCommand.substr(PlayerNameStart + 1).c_str();
				auto PlayerName = TrimPlayerCommandString(PlayerNameInput);

				if (PlayerName.empty())
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat tpto <exact player name>"), FName(), 1.f);
					return;
				}

				std::string MatchedName;
				bool bAmbiguous = false;
				auto TargetPC = FindRealPlayerByExactNameForCommand(PlayerName, PlayerController, MatchedName, bAmbiguous);

				if (bAmbiguous)
				{
					PlayerController->ClientMessage(FString(L"Multiple real players have that exact name."), FName(), 1.f);
					return;
				}

				if (!TargetPC || !TargetPC->Pawn)
				{
					PlayerController->ClientMessage(FString(L"Could not find a real player with that exact name."), FName(), 1.f);
					return;
				}

				auto TargetPawn = TargetPC->Pawn;
				auto SideVector = TargetPawn->GetActorRightVector();
				SideVector.Z = 0.0f;

				if (SideVector.IsZero())
					SideVector = FVector(1.f, 0.f, 0.f);
				else
					SideVector.Normalize();

				auto TeleportLoc = TargetPawn->K2_GetActorLocation() + (SideVector * 250.f);
				TeleportLoc.Z += 50.f;

				PlayerPawn->K2_TeleportTo(TeleportLoc, TargetPawn->K2_GetActorRotation(), false, true);
				if (PlayerPawn->CharacterMovement)
					PlayerPawn->CharacterMovement->Velocity = FVector{};

				PlayerController->ClientMessage(FString(L"Teleported next to player!"), FName(), 1.f);
			}
			else if (command == "troll")
			{
				PlayerController->ClientMessage(FString(LR"(
cheat nuke <projectile/path> <s[size]> <h[meters]> <nodmg> <player name> - Spawns 100 projectiles and targets them to a player or crosshair
)"), FName(), 1.f);
			}
			else if (command == "nuke")
			{
				auto NukeArgsStart = fullCommand.find(' ');
				auto NukeArgs = NukeArgsStart == std::string::npos
					? std::string()
					: TrimPlayerCommandString(originalCommand.substr(NukeArgsStart + 1).c_str());

				auto ProjectileClass = GetDefaultNukeProjectileClass();
				constexpr float DefaultNukeScale = 1.f;
				constexpr float DefaultNukeHeightMeters = 65.f;
				auto NukeScale = DefaultNukeScale;
				auto NukeHeightMeters = DefaultNukeHeightMeters;
				bool bNukeDamageEnabled = true;
				auto NukeTokens = SplitPlayerCommandArgs(NukeArgs);
				size_t PlayerNameStartIndex = 0;
				bool bExplicitPlayerTarget = false;

				if (!NukeTokens.empty())
				{
					auto ProjectileArg = TrimPlayerCommandString(NukeTokens[0]);
					auto NormalizedProjectileArg = NormalizePlayerCommandString(ProjectileArg);
					bool bExplicitProjectileArg = ProjectileArg.find('/') != std::string::npos || ProjectileArg.find('.') != std::string::npos || NormalizedProjectileArg.ends_with("_c");
					bool bShortProjectileArg = IsObjectShortCommandArg(ProjectileArg);

					if (IsNukePlayerTargetKeyword(ProjectileArg))
					{
						bExplicitPlayerTarget = true;
						PlayerNameStartIndex = 1;
					}
					else if (NukeTokens.size() > 1 || bExplicitProjectileArg || bShortProjectileArg)
					{
						auto CandidateProjectileClass = FindActorClassByCommandArg(ProjectileArg);

						if (CandidateProjectileClass)
						{
							ProjectileClass = CandidateProjectileClass;
							PlayerNameStartIndex = 1;
						}
						else if (bExplicitProjectileArg || bShortProjectileArg)
						{
							PlayerController->ClientMessage(FString(L"Failed to find projectile class. Try a class path, generated class path, or short object name."), FName(), 1.f);
							return;
						}
					}
				}

				while (PlayerNameStartIndex < NukeTokens.size())
				{
					auto ModifierArg = TrimPlayerCommandString(NukeTokens[PlayerNameStartIndex]);
					auto NormalizedModifierArg = NormalizePlayerCommandString(ModifierArg);
					float ModifierValue = 0.f;

					if (IsNukePlayerTargetKeyword(ModifierArg))
					{
						bExplicitPlayerTarget = true;
						PlayerNameStartIndex++;
						break;
					}

					if (NormalizedModifierArg == "nodmg" || NormalizedModifierArg == "nodamage" || NormalizedModifierArg == "no-damage")
					{
						bNukeDamageEnabled = false;
						PlayerNameStartIndex++;
						continue;
					}

					if (NormalizedModifierArg == "dmg" || NormalizedModifierArg == "damage")
					{
						bNukeDamageEnabled = true;
						PlayerNameStartIndex++;
						continue;
					}

					if (TryParsePrefixedCommandFloat(ModifierArg, 's', ModifierValue))
					{
						NukeScale = ModifierValue;
						PlayerNameStartIndex++;
						continue;
					}

					if (TryParsePrefixedCommandFloat(ModifierArg, 'h', ModifierValue))
					{
						NukeHeightMeters = ModifierValue;
						PlayerNameStartIndex++;
						continue;
					}

					if (LooksLikeSizeOrHeightModifierArg(ModifierArg))
					{
						PlayerController->ClientMessage(FString(L"Invalid nuke modifier. Use s2 for 2x size, h100 for 100 meters high, or nodmg to disable damage."), FName(), 1.f);
						return;
					}

					break;
				}

				auto PlayerName = JoinPlayerCommandArgs(NukeTokens, PlayerNameStartIndex);

				if (bExplicitPlayerTarget && PlayerName.empty())
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat nuke [projectile/path] [s2] [h100] [nodmg] <exact player name>"), FName(), 1.f);
					return;
				}

				if (!ProjectileClass)
				{
					PlayerController->ClientMessage(FString(L"Failed to find default Ostrich rocket projectile class."), FName(), 1.f);
					return;
				}

				std::string MatchedName;
				bool bAmbiguous = false;
				AFortPlayerControllerAthena* TargetPC = nullptr;
				AFortPlayerPawnAthena* TargetPawn = nullptr;
				FVector TargetLocation{};
				FVector AimLocation{};
				bool bUsingCrosshairTarget = PlayerName.empty();

				if (bUsingCrosshairTarget)
				{
					if (!TryGetCrosshairGroundLocationForCommand(PlayerController, TargetLocation))
					{
						PlayerController->ClientMessage(FString(L"Could not find a crosshair target."), FName(), 1.f);
						return;
					}

					AimLocation = TargetLocation;
				}
				else
				{
					TargetPC = FindRealPlayerByExactNameForCommand(PlayerName, PlayerController, MatchedName, bAmbiguous, true);

					if (bAmbiguous)
					{
						PlayerController->ClientMessage(FString(L"Multiple real players have that exact name."), FName(), 1.f);
						return;
					}

					if (!TargetPC || !TargetPC->Pawn)
					{
						PlayerController->ClientMessage(FString(L"Could not find a real player with that exact name."), FName(), 1.f);
						return;
					}

					TargetPawn = TargetPC->Pawn;
					TargetLocation = TargetPawn->K2_GetActorLocation();
					AimLocation = TargetLocation;
					AimLocation.Z += 80.f;
				}

				constexpr int RocketCount = 100;
				constexpr double GoldenAngle = 2.39996322972865332;
				constexpr double DiskRadius = 1800.0;
				constexpr double RocketSpeed = 5500.0;
				auto SpawnHeight = static_cast<double>(NukeHeightMeters) * 100.0;
				auto EffectiveDiskRadius = DiskRadius * static_cast<double>(NukeScale > 1.f ? NukeScale : 1.f);
				auto EnvironmentContextRadius = EffectiveDiskRadius + static_cast<double>(NukeRocketDamageRadius) + 1000.0;
				auto EnvironmentDamageContextId = bNukeDamageEnabled ? CreateNukeEnvironmentDamageContext(TargetLocation, EnvironmentContextRadius) : 0;
				int SpawnedRockets = 0;

				for (int i = 0; i < RocketCount; i++)
				{
					double NormalizedIndex = RocketCount > 1 ? static_cast<double>(i) / static_cast<double>(RocketCount - 1) : 0.0;
					double Radius = EffectiveDiskRadius * sqrt(NormalizedIndex);
					double Angle = GoldenAngle * i;

					FVector SpawnLocation(
						TargetLocation.X + (cos(Angle) * Radius),
						TargetLocation.Y + (sin(Angle) * Radius),
						TargetLocation.Z + SpawnHeight
					);

					auto Direction = (AimLocation - SpawnLocation).GetSafeNormal();

					if (Direction.IsZero())
						Direction = FVector(0.f, 0.f, -1.f);

					auto SpawnRotation = MakeRotationFromDirection(Direction);
					auto SpawnScale = FVector(NukeScale, NukeScale, NukeScale);
					auto SpawnTransform = FTransform(SpawnLocation, SpawnRotation.Quaternion(), SpawnScale);
					auto Rocket = UWorld::SpawnActor(ProjectileClass, SpawnTransform, PlayerController);

					if (!Rocket)
						continue;

					if (Rocket->HasInstigator() && PlayerController->Pawn)
						Rocket->Instigator = PlayerController->Pawn;

					ConfigureNukeRocket(Rocket, TargetPawn, AimLocation, Direction * RocketSpeed, bNukeDamageEnabled);
					RegisterGuidedNukeRocket(Rocket, PlayerController, TargetPawn, AimLocation, static_cast<float>(RocketSpeed), bNukeDamageEnabled, EnvironmentDamageContextId);
					SpawnedRockets++;
				}

				RemoveUnusedNukeEnvironmentDamageContext(EnvironmentDamageContextId);

				auto TargetText = bUsingCrosshairTarget ? std::wstring(L"your crosshair") : std::wstring(MatchedName.begin(), MatchedName.end());
				auto Message = L"Spawned " + std::to_wstring(SpawnedRockets) + L" projectiles above " + TargetText +
					L" (s" + FormatCommandFloatForMessage(NukeScale) + L" h" + FormatCommandFloatForMessage(NukeHeightMeters) + L"m dmg " + (bNukeDamageEnabled ? std::wstring(L"on") : std::wstring(L"off")) + L")!";
				PlayerController->ClientMessage(FString(Message.c_str()), FName(), 1.f);
			}
			else if (command == "botemote")
			{
				const UAthenaDanceItemDefinition* Emote = nullptr;

				if (args.size() > 1 && !args[1].empty())
				{
					Emote = FindObject<UAthenaDanceItemDefinition>(UEAllocatedWString(args[1].begin(), args[1].end()));

					if (!Emote)
						Emote = TUObjectArray::FindObject<UAthenaDanceItemDefinition>(args[1].c_str());
				}

				if (!Emote)
					Emote = FindObject<UAthenaDanceItemDefinition>(L"/Game/Athena/Items/Cosmetics/Dances/EID_Accolades.EID_Accolades");

				if (!Emote)
				{
					PlayerController->ClientMessage(FString(L"Failed to find emote."), FName(), 1.f);
					return;
				}

				for (auto& Player : GameMode->AlivePlayers)
				{
					auto BotController = (AFortPlayerControllerAthena*)Player;

					if (!BotController || BotController == PlayerController)
						continue;

					auto BotPS = (AFortPlayerStateAthena*)BotController->PlayerState;
					auto BotPawn = BotController->MyFortPawn;

					if (!BotPS || !BotPawn || !BotPS->bIsABot)
						continue;

					BotController->OnRep_PlayerState();
					BotController->OnRep_Pawn();

					PlayEmoteInternal(BotController, const_cast<UAthenaDanceItemDefinition*>(Emote));
					PlayerController->ClientMessage(FString(L"Bot is now emoting!"), FName(), 1.f);
				}
			}
			else if (command == "skin" || command == "applycid")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please provide a CID!"), FName(), 1.f);
					return;
				}

				auto Pawn = PlayerController->Pawn;
				auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

				if (!Pawn || !PlayerState)
					return;

				auto CID = UEAllocatedWString(args[1].begin(), args[1].end());

				auto Character = FindObject<UAthenaCharacterItemDefinition>(CID.c_str());

				if (!Character)
				{
					UEAllocatedWString FullPath = L"/Game/Athena/Items/Cosmetics/Characters/" + CID + L"." + CID;
					Character = FindObject<UAthenaCharacterItemDefinition>(FullPath.c_str());
				}

				if (!Character || !Character->HeroDefinition)
				{
					PlayerController->ClientMessage(FString(L"Failed to find Character CID. Check your spelling!"), FName(), 1.f);
					return;
				}

				PlayerState->HeroType = Character->HeroDefinition;

				static auto GenderOffset = PlayerState->GetOffset("Gender");

				if (GenderOffset != -1)
					*(EFortCustomGender*)(__int64(PlayerState) + GenderOffset) = Character->Gender;

				std::unordered_map<uint8_t, UObject*> NewParts;

				for (auto& SoftSpec : Character->HeroDefinition->Specializations)
				{
					auto Specialization = SoftSpec.Get();

					if (Specialization)
					{
						for (auto& PartSoft : Specialization->CharacterParts)
						{
							auto Part = PartSoft.Get();

							if (Part)
								NewParts[Part->CharacterPartType] = const_cast<UCustomCharacterPart*>(Part);
						}
					}
				}

				if (NewParts.size() == 0)
				{
					PlayerController->ClientMessage(FString(L"No parts discovered for this skin."), FName(), 1.f);
					return;
				}

				for (auto& [PartType, Part] : NewParts)
					Pawn->ServerChoosePart(PartType, Part); // why does this not work

				UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization(PlayerState);

				if (ApplyCharacterCustomization)
					((void (*)(AActor*, AActor*))ApplyCharacterCustomization)(PlayerState, Pawn);

				Pawn->ForceNetUpdate();
				PlayerState->ForceNetUpdate();

				PlayerController->ClientMessage(FString(L"Applied CID!"), FName(), 1.f);
			}
			else if (command == "startevent")
			{
				Events::StartEvent();
				PlayerController->ClientMessage(FString(L"Event started!"), FName(), 1);
			}
			else if (command == "bugitgo" || command == "tp")
			{
				if (args.size() == 1)
				{
					auto Pawn = PlayerController->Pawn;
					if (!Pawn)
					{
						PlayerController->ClientMessage(FString(L"No pawn!"), FName(), 1.f);
						return;
					}

					static auto TeleportFn = const_cast<UFunction*>(FindObject<UFunction>(L"/Script/Engine.CheatManager.Teleport"));
					static auto CheatManagerClass = FindClass("CheatManager");

					if (!TeleportFn)
					{
						PlayerController->ClientMessage(FString(L"Teleport function not found on this version."), FName(), 1.f);
						return;
					}

					auto CheatManagerOffset = PlayerController->GetOffset("CheatManager");
					UObject* CheatManager = CheatManagerOffset != -1 ? *(UObject**)(__int64(PlayerController) + CheatManagerOffset) : nullptr;

					if (!CheatManager && CheatManagerClass)
					{
						CheatManager = UGameplayStatics::SpawnObject(CheatManagerClass, PlayerController);
						if (CheatManager && CheatManagerOffset != -1)
							*(UObject**)(__int64(PlayerController) + CheatManagerOffset) = CheatManager;
					}

					if (!CheatManager)
					{
						PlayerController->ClientMessage(FString(L"Failed to create cheat manager!"), FName(), 1.f);
						return;
					}

					auto LocBefore = Pawn->K2_GetActorLocation();
					CheatManager->ProcessEvent(TeleportFn, nullptr);
					auto LocAfter = Pawn->K2_GetActorLocation();

					bool bMoved = (LocBefore.X != LocAfter.X || LocBefore.Y != LocAfter.Y || LocBefore.Z != LocAfter.Z);
					bool bInvalid = (LocAfter.X == 0.f && LocAfter.Y == 0.f && LocAfter.Z == 0.f);

					if (!bMoved || bInvalid)
					{
						FVector TeleportLoc{};

						if (!TryGetCrosshairGroundLocationForCommand(PlayerController, TeleportLoc))
						{
							PlayerController->ClientMessage(FString(L"Could not find a crosshair target."), FName(), 1.f);
							return;
						}

						TeleportLoc.Z += 200.f;
						Pawn->K2_TeleportTo(TeleportLoc, Pawn->K2_GetActorRotation(), false, true);
						Pawn->CharacterMovement->Velocity = FVector{};
						PlayerController->ClientMessage(FString(L"Teleported to crosshair! (fallback)"), FName(), 1.f);
					}
					else
					{
						Pawn->CharacterMovement->Velocity = FVector{};
						PlayerController->ClientMessage(FString(L"Teleported to crosshair!"), FName(), 1.f);
					}
				}
				else if (args.size() == 4)
				{
					double X = strtod(args[1].c_str(), nullptr);
					double Y = strtod(args[2].c_str(), nullptr);
					double Z = strtod(args[3].c_str(), nullptr);

					if (PlayerController->Pawn)
					{
						PlayerController->Pawn->K2_SetActorLocation(FVector(X, Y, Z), false, nullptr, true);
						PlayerController->ClientMessage(FString(L"Teleported to location!"), FName(), 1.f);
					}
				}
				else
					PlayerController->ClientMessage(FString(L"Usage: cheat tp OR cheat tp <X> <Y> <Z>"), FName(), 1.f);
			}
			else if (command == "launch" || command == "launchpawn" || command == "l")
			{
				float X = 0.0f, Y = 0.0f, Z = 0.0f;

				int Arguments = args.size();

				try
				{
					if (Arguments == 3 && args[2].size() > 0 && !isdigit(args[2][0]))
					{
						float Mag = std::stof(args[1].c_str());
						std::string Dir = args[2].c_str();

						std::transform(Dir.begin(), Dir.end(), Dir.begin(), ::toupper);

						if (Dir == "N" || Dir == "NORTH")
						{
							X = Mag; Z = Mag;
						}
						else if (Dir == "S" || Dir == "SOUTH")
						{
							X = -Mag; Z = Mag;
						}
						else if (Dir == "E" || Dir == "EAST")
						{
							Y = Mag; Z = Mag;
						}
						else if (Dir == "W" || Dir == "WEST")
						{
							Y = -Mag; Z = Mag;
						}
						else if (Dir == "NE" || Dir == "NORTHEAST")
						{
							X = Mag; Y = Mag; Z = Mag;
						}
						else if (Dir == "NW" || Dir == "NORTHWEST")
						{
							X = Mag; Y = -Mag; Z = Mag;
						}
						else if (Dir == "SE" || Dir == "SOUTHEAST")
						{
							X = -Mag; Y = Mag; Z = Mag;
						}
						else if (Dir == "SW" || Dir == "SOUTHWEST")
						{
							X = -Mag; Y = -Mag; Z = Mag;
						}
						else if (Dir == "U" || Dir == "UP")
						{
							Z = Mag;
						}
						else if (Dir == "D" || Dir == "DOWN")
						{
							Z = -Mag;
						}
						else
						{
							PlayerController->ClientMessage(FString(L"Invalid direction. Use N, S, E, W, NE, NW, SE, SW, U, or D."), FName(), 1.f);
							return;
						}
					}
					else if (Arguments == 2)
					{
						Z = std::stof(args[1].c_str());
					}
					else if (Arguments == 3)
					{
						X = std::stof(args[1].c_str());
						Z = std::stof(args[2].c_str());
					}
					else if (Arguments == 4)
					{
						X = std::stof(args[1].c_str());
						Y = std::stof(args[2].c_str());
						Z = std::stof(args[3].c_str());
					}
				}
				catch (...)
				{
					PlayerController->ClientMessage(FString(L"Invalid input. Please provide numeric values."), FName(), 1.f);
					return;
				}

				if (PlayerController->Pawn)
				{
					PlayerController->Pawn->LaunchCharacterJump(FVector(X, Y, Z), false, nullptr, true);
					PlayerController->ClientMessage(FString(L"Launched player!"), FName(), 1.f);
				}
			}
			else if (command == "velocity" || command == "v")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please provide a velocity vector."), FName(), 1.f);
					return;
				}

				FVector CurrentVelocity = PlayerController->Pawn->CharacterMovement->Velocity;
				float VelocityMultiplier = std::stof(std::string(args[1]));

				if (PlayerController->Pawn && PlayerController->Pawn->CharacterMovement)
				{
					PlayerController->Pawn->CharacterMovement->Velocity = FVector(CurrentVelocity * VelocityMultiplier);
					PlayerController->ClientMessage(FString(L"Set player velocity!"), FName(), 1.f);
				}
			}
			else if (command == "savewaypoint" || command == "s")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please provide a phrase to save the waypoint to."), FName(), 1.f);
					return;
				}

				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"Couldn't find a pawn!"), FName(), 1.f);
					return;
				}

				FVector PawnLocation(Pawn->K2_GetActorLocation().X, Pawn->K2_GetActorLocation().Y, Pawn->K2_GetActorLocation().Z);

				if (PawnLocation.X == 0.0f && PawnLocation.Y == 0.0f && PawnLocation.Z == 0.0f)
				{
					PlayerController->ClientMessage(FString(L"Failed to save a waypoint."), FName(), 1.f);
					return;
				}

				std::string Phrase = args[1].c_str();

				auto It = Waypoints.find(Phrase);

				if (It != Waypoints.end())
				{
					if (args.size() >= 3 && (args[2] == "override" || args[2] == "o"))
					{
						It->second.clear();
						It->second.push_back(PawnLocation);

						PlayerController->ClientMessage(FString(L"Waypoint overridden successfully!"), FName(), 1.f);
					}
					else
					{
						PlayerController->ClientMessage(FString(L"A waypoint with this phrase already exists! Use 'waypoint {phrase} override' to override it."), FName(), 1);
					}
				}
				else
				{
					std::vector<FVector> Locations;
					Locations.push_back(PawnLocation);
					Waypoints[Phrase] = Locations;

					PlayerController->ClientMessage(FString(L"Waypoint saved! Use \" cheat waypoint (phrase) \" to teleport to that location!"), FName(), 1);
				}
			}
			else if (command == "waypoint" || command == "w")
			{
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please provide a waypoint phrase to teleport to."), FName(), 1.f);
					return;
				}

				std::string Phrase = args[1].c_str();

				auto It = Waypoints.find(Phrase);

				if (It == Waypoints.end() || It->second.empty())
				{
					PlayerController->ClientMessage(FString(L"A saved waypoint with this phrase was not found!"), FName(), 1.f);
					return;
				}

				const auto& WaypointList = It->second;

				if (args.size() >= 3 && (args[2] == "previous" || args[2] == "p"))
				{
					if (WaypointList.size() < 2)
					{
						PlayerController->ClientMessage(FString(L"No previous waypoint available for this phrase!"), FName(), 1.f);
						return;
					}

					FVector Destination = Waypoints[Phrase][Waypoints[Phrase].size() - 2];

					auto Pawn = PlayerController->Pawn;

					if (Pawn)
					{
						Pawn->K2_TeleportTo(Destination, Pawn->K2_GetActorRotation(), false, true);
						Pawn->CharacterMovement->Velocity = FVector{};
						RegenerateMinimumGodHealthAfterWaypoint(
							PlayerController, Pawn);
						PlayerController->ClientMessage(FString(L"Teleported to previous waypoint!"), FName(), 1.f);
					}
					else
					{
						PlayerController->ClientMessage(FString(L"Couldn't find a pawn to teleport!"), FName(), 1.f);
					}
				}
				else
				{
					FVector Destination = WaypointList.back();

					if (Destination.X == 0.0f && Destination.Y == 0.0f && Destination.Z == 0.0f)
					{
						PlayerController->ClientMessage(FString(L"Waypoint is invalid (0, 0, 0)! Aborting teleport."), FName(), 1.f);
						return;
					}

					auto Pawn = PlayerController->Pawn;

					if (Pawn)
					{
						Pawn->K2_TeleportTo(Destination, Pawn->K2_GetActorRotation(), false, true);
						Pawn->CharacterMovement->Velocity = FVector{};
						RegenerateMinimumGodHealthAfterWaypoint(
							PlayerController, Pawn);
						PlayerController->ClientMessage(FString(L"Teleported to waypoint!"), FName(), 1.f);
					}
					else
					{
						PlayerController->ClientMessage(FString(L"Couldn't find a pawn to teleport!"), FName(), 1.f);
					}
				}
			}
			else if (command == "giveall" || (command == "give" && args.size() == 2 && args[1] == "all"))
			{
				auto Pawn = PlayerController->Pawn;
				if (!Pawn)
					return;

				auto GiveItems = [&](const char* Name, int Count) {
					auto ItemDef = TUObjectArray::FindObject<UFortItemDefinition>(Name);
					if (!ItemDef)
						return;

					FVector FinalLoc = Pawn->K2_GetActorLocation();
					FVector ForwardVector = Pawn->GetActorForwardVector();
					ForwardVector.Z = 0.0f;
					ForwardVector.Normalize();
					FinalLoc = FinalLoc + ForwardVector * 450.f;
					FinalLoc.Z += 50.f;

					auto Pickup = AFortInventory::SpawnPickup(FinalLoc, ItemDef, Count, -1, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetUnset(), Pawn);
					if (Pickup)
						Pawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
				};

				const char* AmmoNames[] = {
					"AmmoDataRockets", "AthenaAmmoDataBulletsHeavy", "AthenaAmmoDataBulletsLight",
					"AthenaAmmoDataBulletsMedium", "AthenaAmmoDataEnergyCell", "AthenaAmmoDataHooks",
					"AthenaAmmoDataShells", "AthenaOakCash", "AmmoDataBulletsHeavy", "AmmoDataBulletsLight",
					"AmmoDataBulletsMedium", "AmmoDataEnergyCell", "AmmoDataExplosive", "AmmoDataShells",
					"AmmoDataBotTurret", "AmmoDataBottleRockets", "AmmoDataFragGrenades", "AmmoDataFragments",
					"AmmoDataGlowTorch", "AmmoDataM80", "AmmoDataProximityMines", "AmmoDataProximityMine",
					"AmmoDataSmokeBomb", "AmmoDataThrowingStar", "AmmoInfiniteEnergy", "AmmoInfinitePossessor",
					"AmmoTrollData",
				};
				for (auto& Name : AmmoNames) GiveItems(Name, 999);

				const char* MatNames[] = { "WoodItemData", "StoneItemData", "MetalItemData" };
				for (auto& Name : MatNames) GiveItems(Name, 500);

				const char* TrapNames[] = {
					"TID_Context_BouncePad_Athena", "TID_Ceiling_BouncePad_Athena_R_T01",
					"TID_Floor_Player_Launch_Pad_Athena", "TID_Context_Freeze_Athena",
					"TID_Floor_Player_Campfire_Athena", "TID_Floor_MountedTurret_Athena",
					"TID_ContextTrap_Athena", "TID_PoisonDartTrap_Context",
					"TID_Floor_Player_Jump_Pad_Free_Direction_Athena", "TID_Floor_Player_Jump_Pad_Athena",
					"TID_Context_SpeedBoost", "TID_ZippyTroutTrap_Context",
					"TID_Ceiling_Goop_VR_T01", "TID_Context_Reinforced_Athena",
				};
				for (auto& Name : TrapNames) GiveItems(Name, 6);

				PlayerController->ClientMessage(FString(L"Gave all ammo, mats, and traps!"), FName(), 1.f);
			}
			else if (command == "giveammo" || (command == "give" && args.size() == 2 && args[1] == "ammo"))
			{
				auto Pawn = PlayerController->Pawn;
				if (!Pawn)
					return;

				const char* AmmoNames[] = {
					"AmmoDataRockets",
					"AthenaAmmoDataBulletsHeavy",
					"AthenaAmmoDataBulletsLight",
					"AthenaAmmoDataBulletsMedium",
					"AthenaAmmoDataEnergyCell",
					"AthenaAmmoDataHooks",
					"AthenaAmmoDataShells",
					"AthenaOakCash",
					"AmmoDataBulletsHeavy",
					"AmmoDataBulletsLight",
					"AmmoDataBulletsMedium",
					"AmmoDataEnergyCell",
					"AmmoDataExplosive",
					"AmmoDataShells",
					"AmmoDataBotTurret",
					"AmmoDataBottleRockets",
					"AmmoDataFragGrenades",
					"AmmoDataFragments",
					"AmmoDataGlowTorch",
					"AmmoDataM80",
					"AmmoDataProximityMines",
					"AmmoDataProximityMine",
					"AmmoDataSmokeBomb",
					"AmmoDataThrowingStar",
					"AmmoInfiniteEnergy",
					"AmmoInfinitePossessor",
					"AmmoTrollData",
				};

				int GivenCount = 0;
				for (auto& AmmoName : AmmoNames)
				{
					auto ItemDef = TUObjectArray::FindObject<UFortItemDefinition>(AmmoName);
					if (!ItemDef)
						continue;

					FVector FinalLoc = Pawn->K2_GetActorLocation();
					FVector ForwardVector = Pawn->GetActorForwardVector();
					ForwardVector.Z = 0.0f;
					ForwardVector.Normalize();
					FinalLoc = FinalLoc + ForwardVector * 450.f;
					FinalLoc.Z += 50.f;

					auto Pickup = AFortInventory::SpawnPickup(FinalLoc, ItemDef, 999, -1, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetUnset(), Pawn);
					if (Pickup)
					{
						Pawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
						GivenCount++;
					}
				}

				wchar_t wmsg[64];
				swprintf_s(wmsg, 64, L"Gave %d ammo types!", GivenCount);
				PlayerController->ClientMessage(FString(wmsg), FName(), 1.f);
			}
			else if (command == "givemats" || (command == "give" && args.size() == 2 && args[1] == "mats"))
			{
				auto Pawn = PlayerController->Pawn;
				if (!Pawn)
					return;

				const char* MatNames[] = { "WoodItemData", "StoneItemData", "MetalItemData" };

				for (auto& MatName : MatNames)
				{
					auto ItemDef = TUObjectArray::FindObject<UFortItemDefinition>(MatName);
					if (!ItemDef)
						continue;

					FVector FinalLoc = Pawn->K2_GetActorLocation();
					FVector ForwardVector = Pawn->GetActorForwardVector();
					ForwardVector.Z = 0.0f;
					ForwardVector.Normalize();
					FinalLoc = FinalLoc + ForwardVector * 450.f;
					FinalLoc.Z += 50.f;

					auto Pickup = AFortInventory::SpawnPickup(FinalLoc, ItemDef, 500, -1, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetUnset(), Pawn);
					if (Pickup)
						Pawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
				}

				PlayerController->ClientMessage(FString(L"Gave 500 of each material!"), FName(), 1.f);
			}
			else if (command == "givetraps" || (command == "give" && args.size() == 2 && args[1] == "traps"))
			{
				auto Pawn = PlayerController->Pawn;
				if (!Pawn)
					return;

				const char* TrapNames[] = {
					"TID_Context_BouncePad_Athena",
					"TID_Ceiling_BouncePad_Athena_R_T01",
					"TID_Floor_Player_Launch_Pad_Athena",
					"TID_Context_Freeze_Athena",
					"TID_Floor_Player_Campfire_Athena",
					"TID_Floor_MountedTurret_Athena",
					"TID_ContextTrap_Athena",
					"TID_PoisonDartTrap_Context",
					"TID_Floor_Player_Jump_Pad_Free_Direction_Athena",
					"TID_Floor_Player_Jump_Pad_Athena",
					"TID_Context_SpeedBoost",
					"TID_ZippyTroutTrap_Context",
					"TID_Ceiling_Goop_VR_T01",
					"TID_Context_Reinforced_Athena",
				};

				int GivenCount = 0;
				for (auto& TrapName : TrapNames)
				{
					auto ItemDef = TUObjectArray::FindObject<UFortItemDefinition>(TrapName);
					if (!ItemDef)
						continue;

					FVector FinalLoc = Pawn->K2_GetActorLocation();
					FVector ForwardVector = Pawn->GetActorForwardVector();
					ForwardVector.Z = 0.0f;
					ForwardVector.Normalize();
					FinalLoc = FinalLoc + ForwardVector * 450.f;
					FinalLoc.Z += 50.f;

					auto Pickup = AFortInventory::SpawnPickup(FinalLoc, ItemDef, 6, -1, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetUnset(), Pawn);
					if (Pickup)
					{
						Pawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
						GivenCount++;
					}
				}

				wchar_t wmsg[64];
				swprintf_s(wmsg, 64, L"Gave %d traps!", GivenCount);
				PlayerController->ClientMessage(FString(wmsg), FName(), 1.f);
			}
			else if (command == "givetoall")
			{
				auto GiveToAllArgsStart =
					originalCommand.find_first_of(" \t");
				auto GiveToAllArgs =
					GiveToAllArgsStart == std::string::npos
						? std::string()
						: TrimPlayerCommandString(
							originalCommand.substr(
								GiveToAllArgsStart + 1).c_str());
				auto GiveToAllTokens =
					SplitPlayerCommandArgs(GiveToAllArgs);

				if (GiveToAllTokens.size() != 1)
				{
					PlayerController->ClientMessage(
						FString(L"Usage: cheat givetoall <WID/path>"),
						FName(), 1.f);
					return;
				}

				// Use the untouched token so case-sensitive WID/object names and
				// full paths resolve correctly.
				auto ItemDefinition =
					FindItemDefinitionByCommandArg(
						GiveToAllTokens[0]);
				if (!ItemDefinition)
				{
					PlayerController->ClientMessage(
						FString(L"Failed to find item! Try passing it as a path or check your spelling & casing"),
						FName(), 1.f);
					return;
				}

				int32 Count =
					ItemDefinition->IsA(
						UFortContextTrapItemDefinition::StaticClass()) ||
					ItemDefinition->IsA(
						UFortTrapItemDefinition::StaticClass())
						? 6
						: ItemDefinition->GetMaxStackSize();
				Count = std::max<int32>(Count, 1);

				std::vector<AFortPlayerControllerAthena*> Targets;
				std::unordered_set<AFortPlayerControllerAthena*> SeenTargets;
				auto AddTarget =
					[&](AFortPlayerControllerAthena* TargetPC)
					{
						if (!IsUsableDeathObject(TargetPC) ||
							!SeenTargets.insert(TargetPC).second)
						{
							return;
						}

						auto PlayerState = TargetPC->PlayerState;
						if (IsUsableDeathObject(PlayerState) &&
							PlayerState->HasbIsABot() &&
							PlayerState->bIsABot)
						{
							return;
						}

						Targets.push_back(TargetPC);
					};

				// ClientConnections does not always contain the listen-server
				// controller, so seed the roster with the command's caller.
				AddTarget(PlayerController);
				if (IsUsableDeathObject(World->NetDriver))
				{
					auto Driver = (UNetDriver*)World->NetDriver;
					for (int32 Index = 0;
						Index < Driver->ClientConnections.Num();
						Index++)
					{
						auto Connection =
							Driver->ClientConnections[Index];
						if (!IsUsableDeathObject(Connection) ||
							!IsUsableDeathObject(
								Connection->PlayerController))
						{
							continue;
						}

						AddTarget(
							Connection->PlayerController->Cast<
								AFortPlayerControllerAthena>());
					}
				}

				int GivenCount = 0;
				auto PlayerPawnClass =
					AFortPlayerPawnAthena::StaticClass();
				for (auto TargetPC : Targets)
				{
					auto IsOwnedPlayerPawn =
						[&](AFortPlayerPawnAthena* Candidate)
						{
							return PlayerPawnClass &&
								IsUsableDeathObject(Candidate) &&
								Candidate->IsA(PlayerPawnClass) &&
								Candidate->Controller == TargetPC;
						};
					auto TargetPawn = TargetPC->MyFortPawn;
					if (!IsOwnedPlayerPawn(TargetPawn))
						TargetPawn = TargetPC->Pawn;

					auto TargetInventory = TargetPC->WorldInventory;
					if (!IsOwnedPlayerPawn(TargetPawn) ||
						!IsUsableDeathObject(TargetInventory))
					{
						continue;
					}

					auto FinalLoc = TargetPawn->K2_GetActorLocation();
					auto ForwardVector =
						TargetPawn->GetActorForwardVector();
					ForwardVector.Z = 0.f;
					ForwardVector.Normalize();
					FinalLoc = FinalLoc + ForwardVector * 450.f;
					FinalLoc.Z += 50.f;

					const float RandomAngleVariation =
						((float)rand() * 0.00109866634f) - 18.f;
					const float FinalAngle =
						RandomAngleVariation * 0.017453292519943295f;
					FinalLoc.X += cos(FinalAngle) * 100.f;
					FinalLoc.Y += sin(FinalAngle) * 100.f;

					auto Pickup = AFortInventory::SpawnPickup(
						FinalLoc, ItemDefinition, Count, -1,
						EFortPickupSourceTypeFlag::GetOther(),
						EFortPickupSpawnSource::GetUnset(),
						TargetPawn);
					if (!Pickup)
						continue;

					TargetPawn->ServerHandlePickup(
						Pickup,
						Pickup->PickupLocationData.FlyTime,
						FVector(), true);
					GivenCount++;
				}

				auto Message = L"Gave item to " +
					std::to_wstring(GivenCount) + L" player(s)!";
				PlayerController->ClientMessage(
					FString(Message.c_str()), FName(), 1.f);
			}
			else if (command == "giveitem" || command == "give")
			{
				if (args.size() != 2 && args.size() != 3)
				{
					PlayerController->ClientMessage(FString(L"Wrong number of arguments!"), FName(), 1.f);
					return;
				}

				auto ItemDefinition = FindItemDefinitionByCommandArg(args[1].c_str());

				if (!ItemDefinition)
					return PlayerController->ClientMessage(FString(L"Failed to find item! Try passing it as a path or check your spelling & casing"), FName(), 1);

				int32 Count = 1;

				if (ItemDefinition->IsA(UFortContextTrapItemDefinition::StaticClass()) || ItemDefinition->IsA(UFortTrapItemDefinition::StaticClass()))
					Count = 6;
				else
					Count = ItemDefinition->GetMaxStackSize();

				if (args.size() == 3)
					Count = strtol(args[2].c_str(), nullptr, 10);

				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
					return;

				FVector FinalLoc = Pawn ? Pawn->K2_GetActorLocation() : FVector();

				FVector ForwardVector = Pawn ? Pawn->GetActorForwardVector() : FVector();
				ForwardVector.Z = 0.0f;
				ForwardVector.Normalize();

				FinalLoc = FinalLoc + ForwardVector * 450.f;
				FinalLoc.Z += 50.f;

				const float RandomAngleVariation = ((float)rand() * 0.00109866634f) - 18.f;
				const float FinalAngle = RandomAngleVariation * 0.017453292519943295f;

				FinalLoc.X += cos(FinalAngle) * 100.f;
				FinalLoc.Y += sin(FinalAngle) * 100.f;

				auto Pickup = AFortInventory::SpawnPickup(FinalLoc, ItemDefinition, Count, -1, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetUnset(), Pawn);

				if (Pawn && Pickup)
				{
					Pawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
					PlayerController->ClientMessage(FString(L"Gave item!"), FName(), 1.f);
				}
				else
				{
					PlayerController->ClientMessage(FString(L"Failed to give item (no pawn or pickup)."), FName(), 1.f);
				}
			}
			else if (command == "spawnpickup")
			{
				auto SpawnPickupArgsStart = originalCommand.find(' ');

				if (SpawnPickupArgsStart == std::string::npos)
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat spawnpickup <WID/path> [count] [X Y Z]"), FName(), 1.f);
					return;
				}

				auto SpawnPickupArgs = TrimPlayerCommandString(originalCommand.substr(SpawnPickupArgsStart + 1).c_str());
				auto SpawnPickupTokens = SplitPlayerCommandArgs(SpawnPickupArgs);

				if (SpawnPickupTokens.empty())
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat spawnpickup <WID/path> [count] [X Y Z]"), FName(), 1.f);
					return;
				}

				auto ItemArg = SpawnPickupTokens[0];
				auto ItemDefinition = FindItemDefinitionByCommandArg(ItemArg);

				if (!ItemDefinition)
					return PlayerController->ClientMessage(FString(L"Failed to find item! Try passing it as a path or check your spelling & casing"), FName(), 1);

				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn found!"), FName(), 1.f);
					return;
				}

				long Count = 1;
				bool HasLocation = false;
				FVector SpawnLocation{};

				if (SpawnPickupTokens.size() == 2)
				{
					int ParsedCount = 0;

					if (!TryParseCommandInt(SpawnPickupTokens[1], ParsedCount))
					{
						PlayerController->ClientMessage(FString(L"Usage: cheat spawnpickup <WID/path> [count] [X Y Z]"), FName(), 1.f);
						return;
					}

					Count = ParsedCount;
				}
				else if (SpawnPickupTokens.size() == 4 || SpawnPickupTokens.size() == 5)
				{
					size_t LocationStartIndex = 1;

					if (SpawnPickupTokens.size() == 5)
					{
						int ParsedCount = 0;

						if (!TryParseCommandInt(SpawnPickupTokens[1], ParsedCount))
						{
							PlayerController->ClientMessage(FString(L"Usage: cheat spawnpickup <WID/path> [count] [X Y Z]"), FName(), 1.f);
							return;
						}

						Count = ParsedCount;
						LocationStartIndex = 2;
					}

					float X = 0.f;
					float Y = 0.f;
					float Z = 0.f;

					if (!TryParseCommandFloat(SpawnPickupTokens[LocationStartIndex], X) ||
						!TryParseCommandFloat(SpawnPickupTokens[LocationStartIndex + 1], Y) ||
						!TryParseCommandFloat(SpawnPickupTokens[LocationStartIndex + 2], Z))
					{
						PlayerController->ClientMessage(FString(L"Usage: cheat spawnpickup <WID/path> [count] [X Y Z]"), FName(), 1.f);
						return;
					}

					SpawnLocation = FVector(X, Y, Z);
					HasLocation = true;
				}
				else if (SpawnPickupTokens.size() > 5)
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat spawnpickup <WID/path> [count] [X Y Z]"), FName(), 1.f);
					return;
				}
				else if (SpawnPickupTokens.size() != 1)
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat spawnpickup <WID/path> [count] [X Y Z]"), FName(), 1.f);
					return;
				}

				if (!HasLocation)
					SpawnLocation = Pawn->K2_GetActorLocation();

				if (auto SpawnedPickup = AFortInventory::SpawnPickup(SpawnLocation, ItemDefinition, Count, -1, EFortPickupSourceTypeFlag::GetTossed(), EFortPickupSpawnSource::GetUnset(), Pawn))
				{
					DisablePickupGravity(SpawnedPickup);
					PlayerController->ClientMessage(FString(L"Spawned pickup!"), FName(), 1.f);
				}
				else
					PlayerController->ClientMessage(FString(L"Failed to spawn pickup!"), FName(), 1.f);
			}
			else if (command == "getloc" || command == "a" || command == "getlocation")
			{
				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn found!"), FName(), 1.f);
					return;
				}

				auto Location = Pawn->K2_GetActorLocation();

				if (Location.X == 0.f && Location.Y == 0.f && Location.Z == 0.f)
				{
					PlayerController->ClientMessage(FString(L"Location is (0,0,0)! Cannot provide location."), FName(), 1.f);
					return;
				}

				Memcury::Util::CopyToClipboard(std::to_string(Location.X) + " " + std::to_string(Location.Y) + " " + std::to_string(Location.Z));
				PlayerController->ClientMessage(FString(L"Copied player location to clipboard!"), FName(), 1.f);
			}
			else if (command == "spawnactor" || command == "summon" || command == "spawn")
			{
				if (!PlayerController->Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn found!"), FName(), 1.f);
					return;
				}

				auto SummonArgsStart = originalCommand.find(' ');

				if (SummonArgsStart == std::string::npos)
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat summon <class/path> [X Y Z] [count] [s<size> | s<X,Y,Z>] [h<meters>] [hp<health>] [direction] [p<pitch>]"), FName(), 1.f);
					return;
				}

				auto SummonArgs = TrimPlayerCommandString(originalCommand.substr(SummonArgsStart + 1).c_str());
				auto SummonTokens = SplitPlayerCommandArgs(SummonArgs);

				if (SummonTokens.empty())
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat summon <class/path> [X Y Z] [count] [s<size> | s<X,Y,Z>] [h<meters>] [hp<health>] [direction] [p<pitch>]"), FName(), 1.f);
					return;
				}

				auto Class = FindActorClassByCommandArg(SummonTokens[0]);

				if (!Class)
				{
					return PlayerController->ClientMessage(FString(L"Failed to find class! Try passing it as a path or check your spelling & casing"), FName(), 1);
				}

				bool HasLocation = false;
				bool HasExplicitHeight = false;
				bool HasExplicitHealth = false;
				auto Loc = PlayerController->Pawn->K2_GetActorLocation();

				auto Rotation = PlayerController->Pawn->K2_GetActorRotation();
				int Count = 1;
				constexpr float DefaultSummonScale = 1.f;
				constexpr float DefaultSummonHeightMeters = 2.f;
				FVector SummonScale = FVector(DefaultSummonScale, DefaultSummonScale, DefaultSummonScale);
				auto SummonHeightMeters = DefaultSummonHeightMeters;
				auto SummonHealth = 0.f;

				std::map<std::wstring, float> DirectionToYaw = {
					{L"N", 0.0f}, {L"E", 90.0f}, {L"S", 180.0f}, {L"W", 270.0f},
					{L"NE", 45.0f}, {L"SE", 135.0f}, {L"SW", 225.0f}, {L"NW", 315.0f}
				};

				for (size_t i = 1; i < SummonTokens.size(); ++i)
				{
					auto CurrentArg = TrimPlayerCommandString(SummonTokens[i]);
					float ModifierValue = 0.f;

					if (TryParsePrefixedCommandVector(CurrentArg, 's', SummonScale))
						continue;

					if (CurrentArg.size() > 2 &&
						std::tolower(static_cast<unsigned char>(CurrentArg[0])) == 'h' &&
						std::tolower(static_cast<unsigned char>(CurrentArg[1])) == 'p')
					{
						if (TryParseCommandFloat(CurrentArg.substr(2), ModifierValue))
						{
							SummonHealth = ModifierValue;
							HasExplicitHealth = true;
							continue;
						}

						PlayerController->ClientMessage(FString(L"Invalid summon health. Use hp1 for 1 health or hp100 for 100 health."), FName(), 1.f);
						return;
					}

					if (TryParsePrefixedCommandFloat(CurrentArg, 'h', ModifierValue))
					{
						SummonHeightMeters = ModifierValue;
						HasExplicitHeight = true;
						continue;
					}

					if (LooksLikeSizeOrHeightModifierArg(CurrentArg))
					{
						PlayerController->ClientMessage(FString(L"Invalid summon modifier. Use s2 (or s1,1,5 per-axis) for size or h100 for height."), FName(), 1.f);
						return;
					}

					if (!HasLocation && i + 2 < SummonTokens.size())
					{
						float X = 0.f;
						float Y = 0.f;
						float Z = 0.f;

						if (TryParseCommandFloat(SummonTokens[i], X) && TryParseCommandFloat(SummonTokens[i + 1], Y) && TryParseCommandFloat(SummonTokens[i + 2], Z))
						{
							Loc = FVector(X, Y, Z);
							HasLocation = true;
							i += 2;
							continue;
						}
					}

					int PossibleCount = 0;

					if (TryParseCommandInt(CurrentArg, PossibleCount))
					{
						Count = PossibleCount;
						continue;
					}

					if (CurrentArg.size() > 1 && (std::tolower(static_cast<unsigned char>(CurrentArg[0])) == 'p' || std::tolower(static_cast<unsigned char>(CurrentArg[0])) == 'r'))
					{
						float PitchValue = 0.f;

						if (TryParseCommandFloat(CurrentArg.substr(1), PitchValue))
						{
							Rotation.Pitch = PitchValue;
							continue;
						}
					}

					std::wstring WideArg(CurrentArg.begin(), CurrentArg.end());
					std::transform(WideArg.begin(), WideArg.end(), WideArg.begin(), ::towupper);
					auto it = DirectionToYaw.find(WideArg);

					if (it != DirectionToYaw.end())
					{
						Rotation.Yaw = it->second;
						continue;
					}

					PlayerController->ClientMessage(FString(L"Invalid summon argument. Use count, X Y Z, s2, h100, hp100, p45/r45, or a compass direction."), FName(), 1.f);
					return;
				}

				auto AppliedSummonHeightMeters = (!HasLocation || HasExplicitHeight) ? SummonHeightMeters : 0.f;
				Loc.Z += AppliedSummonHeightMeters * 100.f;

				if (Count < 1)
				{
					PlayerController->ClientMessage(FString(L"Summon count must be at least 1."), FName(), 1.f);
					return;
				}

				int AmountSpawned = 0;

				for (int i = 0; i < Count; i++)
				{
					FQuat SpawnQuat = FRotator(Rotation.Pitch, Rotation.Yaw, Rotation.Roll).Quaternion();
					auto SpawnScale = SummonScale;
					FTransform SpawnTransform(Loc, SpawnQuat, SpawnScale);
					auto Actor = UWorld::SpawnActor(Class, SpawnTransform);

					if (!Actor)
						continue;

					if (auto Car = Actor->Cast<AFortDagwoodVehicle>())
					{
						FortVehicleMods::RegisterSpawnedVehicle(Car);
						Car->SetFuel(100.f);
					}

					if (HasExplicitHealth)
						TryApplySummonHealth(Actor, SummonHealth);

					Actor->ForceNetUpdate();
					AmountSpawned++;
				}

				auto Message = L"Spawned " + std::to_wstring(AmountSpawned) + L" actor(s)! (s" + FormatCommandScaleForMessage(SummonScale) +
					L" h" + FormatCommandFloatForMessage(AppliedSummonHeightMeters) + L"m" +
					(HasExplicitHealth ? std::wstring(L" hp") + FormatCommandFloatForMessage(SummonHealth) : std::wstring()) + L")";
				PlayerController->ClientMessage(FString(Message.c_str()), FName(), 1.f);
			}
			else if (command == "skydive")
			{
				auto Pawn = PlayerController->Pawn;
				if (!Pawn)
					return;

				static bool bInVortex = false;
				bInVortex ^= 1;

				Pawn->SetInVortex(bInVortex);
				PlayerController->ClientMessage(FString(L"Toggled skydiving!"), FName(), 1.f);
			}
			else if (command == "mark" || command == "marker" || command == "marktp" || command == "marktoteleport")
			{
				if (!ServerAddMapMarkerOG)
				{
					PlayerController->ClientMessage(FString(L"Marker teleport is not available on this version."), FName(), 1.f);
				}
				else if (MarkToTeleportPlayers.count(PlayerController))
				{
					MarkToTeleportPlayers.erase(PlayerController);
					PlayerController->ClientMessage(FString(L"Marker teleporting is now off."), FName(), 1.f);
				}
				else
				{
					MarkToTeleportPlayers.insert(PlayerController);
					PlayerController->ClientMessage(FString(L"Marker teleporting is now on. Place a map marker to teleport."), FName(), 1.f);
				}
			}
			else if (command == "crazy" || command == "serversendmessage" || command == "clientsendconfirmationscreen")
			{
				bool bClientQuitAfterMessage = false;

				auto World = UWorld::GetWorld();

				if (!World)
				{
					PlayerController->ClientMessage(FString(L"No UWorld!"), FName(), 1.f);
					return;
				}

				UObject* NetDriver = World->NetDriver;

				if (!NetDriver)
				{
					PlayerController->ClientMessage(FString(L"No NetDriver!"), FName(), 1.f);
					return;
				}

				UNetDriver* Driver = static_cast<UNetDriver*>(NetDriver);

				auto& ClientConnections = Driver->ClientConnections;

				if (ClientConnections.Num() <= 0)
				{
					PlayerController->ClientMessage(FString(L"no players found dawg how tf did you send this"), FName(), 1.f);
					return;
				}

				std::string Str;

				for (size_t i = 1; i < args.size(); i++)
				{
					Str += std::string(args[i].begin(), args[i].end());
					if (i + 1 < args.size())
						Str += " ";
				}

				if (Str.empty())
				{
					PlayerController->ClientMessage(FString(L"Empty message!"), FName(), 1.f);
					return;
				}

				std::wstring WideStr(Str.begin(), Str.end());
				FString UnrealString = FString(WideStr.c_str());

				FText MessageText = FText::FromString(UnrealString);

				int SentCount = 0;

				for (int i = 0; i < ClientConnections.Num(); i++)
				{
					UNetConnection* Connection = ClientConnections[i];

					if (!Connection)
						continue;

					auto PC = Connection->PlayerController;

					if (!PC)
						continue;

					if (bCanUseCheatCommands)
					{
						PC->ClientSendConfirmationMessage(MessageText);
						SentCount++;

						PlayerController->ClientMessage(FString(L"Broadcasted message to all players!"), FName(), 1); // it was trying to say this was crashing so i temporarily commented it out
					}
				}
			}
			else if (command == "applywrap" || command == "wrap")
			{
				std::wstring WrapName(args[1].begin(), args[1].end());

				std::wstring FullPath = L"/Game/Athena/Items/Cosmetics/ItemWraps/" + WrapName + L"." + WrapName;

				auto ItemWrap = FindObject<UAthenaItemWrapDefinition>(FullPath.c_str());

				if (!ItemWrap)
				{
					PlayerController->ClientMessage(FString(L"Wrap not found!"), FName(), 1.f);
					return;
				}

				TArray<FGuid> AllWeaponGuids;
				auto& ItemInstances = PlayerController->WorldInventory->Inventory.ItemInstances;

				for (int i = 0; i < ItemInstances.Num(); ++i)
				{
					auto ItemInstance = ItemInstances[i];
					auto& ItemEntry = ItemInstance->GetItemEntry();
					const auto ItemDefinition = ItemEntry.ItemDefinition;

					if (ItemDefinition->HasbCanBeDropped() ? ItemDefinition->bCanBeDropped : (ItemDefinition->GetPickupComponent() ? ItemDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false))
					{
						AllWeaponGuids.Add(ItemEntry.ItemGuid);
					}
				}

				if (AllWeaponGuids.Num() > 0)
				{
					PlayerController->ServerApplyOverrideWrapToItem(AllWeaponGuids, ItemWrap);
				}
				else
				{
					PlayerController->ClientMessage(FString(L"Could not find applicable weapons."), FName(), 1.f);
					return;
				}

				PlayerController->ClientMessage(FString(L"Applying wrap to all weapons!"), FName(), 1.f);
			}
			else if (command == "wraptest")
			{
				auto ItemWrap = FindObject<UAthenaItemWrapDefinition>(L"/Game/Athena/Items/Cosmetics/ItemWraps/Wrap_258_Celestial.Wrap_258_Celestial");

				if (!ItemWrap)
				{
					PlayerController->ClientMessage(FString(L"could not find wrap :c"), FName(), 1.f);
					return;
				}

				auto PickaxeInstance = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
					{
						return entry.ItemDefinition->Cast<UFortWeaponMeleeItemDefinition>();
					}, FFortItemEntry::Size());

				PlayerController->ServerApplyOverrideWrapToItem(PickaxeInstance->ItemGuid, ItemWrap);
				PlayerController->ClientMessage(FString(L"so sigma bro plz apply"), FName(), 1.f);
			}
			else if (command == "resetbuilds" || command == "reset")
			{
				float ResetRadius = 0.f;
				bool bHasResetRadius = args.size() >= 2 && TryParseCommandFloat(std::string(args[1].c_str()), ResetRadius) && ResetRadius > 0.f;
				FVector ResetCenter{};

				if (bHasResetRadius)
				{
					if (!PlayerController->Pawn)
					{
						PlayerController->ClientMessage(FString(L"No pawn found!"), FName(), 1.f);
						return;
					}

					ResetCenter = PlayerController->Pawn->K2_GetActorLocation();
				}

				TArray<ABuildingSMActor*> Builds;
				Utils::GetAll<ABuildingSMActor>(Builds);

				int ResetCount = 0;

				for (auto& Build : Builds)
				{
					if (!Build || !Build->bPlayerPlaced)
						continue;

					if (bHasResetRadius)
					{
						auto BuildLocation = Build->K2_GetActorLocation();
						auto DX = BuildLocation.X - ResetCenter.X;
						auto DY = BuildLocation.Y - ResetCenter.Y;
						auto DZ = BuildLocation.Z - ResetCenter.Z;

						if ((DX * DX) + (DY * DY) + (DZ * DZ) > (ResetRadius * ResetRadius))
							continue;
					}

					Build->SilentDie(true);
					ResetCount++;
				}

				Builds.Free();

				wchar_t wmsg[96];

				if (bHasResetRadius)
					swprintf_s(wmsg, 96, L"Reset %d builds within %.0f units!", ResetCount, ResetRadius);
				else
					swprintf_s(wmsg, 96, L"Reset %d builds!", ResetCount);

				PlayerController->ClientMessage(FString(wmsg), FName(), 1.f);
			}
			else if (command == "lootrain")
			{
				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn found!"), FName(), 1.f);
					return;
				}

				// Parsed from originalCommand because tier group names are
				// case-sensitive and fullCommand has been lowercased.
				auto LootRainArgsStart = originalCommand.find(' ');
				auto LootRainTokens = LootRainArgsStart == std::string::npos
					? std::vector<std::string>()
					: SplitPlayerCommandArgs(TrimPlayerCommandString(originalCommand.substr(LootRainArgsStart + 1).c_str()));

				int Count = 20;
				float Radius = 600.f;
				std::string TierGroupArg;

				if (LootRainTokens.size() >= 1 && !TryParseCommandInt(LootRainTokens[0], Count))
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat lootrain [count] [radius] [tiergroup]"), FName(), 1.f);
					return;
				}

				if (LootRainTokens.size() >= 2 && !TryParseCommandFloat(LootRainTokens[1], Radius))
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat lootrain [count] [radius] [tiergroup]"), FName(), 1.f);
					return;
				}

				if (LootRainTokens.size() >= 3)
					TierGroupArg = TrimPlayerCommandString(LootRainTokens[2]);

				if (LootRainTokens.size() > 3)
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat lootrain [count] [radius] [tiergroup]"), FName(), 1.f);
					return;
				}

				Count = std::clamp(Count, 1, 200);
				Radius = std::clamp(Radius, 100.f, 20000.f);

				static auto Loot_AthenaTreasure = FName(L"Loot_AthenaTreasure");
				static auto Loot_AthenaFloorLoot = FName(L"Loot_AthenaFloorLoot");
				static auto Loot_ApolloTreasure_Rare = FName(L"Loot_ApolloTreasure_Rare");

				FName TierGroup = Loot_AthenaTreasure;

				if (!TierGroupArg.empty())
				{
					auto TierGroupAlias = NormalizePlayerCommandString(TierGroupArg);

					if (TierGroupAlias == "chest")
						TierGroup = Loot_AthenaTreasure;
					else if (TierGroupAlias == "floor")
						TierGroup = Loot_AthenaFloorLoot;
					else if (TierGroupAlias == "rare")
						TierGroup = Loot_ApolloTreasure_Rare;
					else
					{
						auto TierGroupWide = UEAllocatedWString(TierGroupArg.begin(), TierGroupArg.end());
						TierGroup = FName(TierGroupWide.c_str());
					}
				}

				auto Center = Pawn->K2_GetActorLocation();
				int Spawned = 0;

				// Each ChooseLootForContainer call returns one container's worth
				// of drops, so roll until the requested count is reached. The
				// roll cap stops an empty/invalid tier group from spinning.
				for (int Roll = 0; Spawned < Count && Roll < Count * 4; Roll++)
				{
					TArray<FFortItemEntry*> LootDrops{};
					UFortLootPackage::ChooseLootForContainer(LootDrops, TierGroup);

					if (LootDrops.Num() == 0)
					{
						LootDrops.Free();
						break;
					}

					for (auto& LootDrop : LootDrops)
					{
						if (LootDrop && Spawned < Count)
						{
							auto Angle = ((float)rand() / (float)RAND_MAX) * 6.2831853f;
							auto Dist = ((float)rand() / (float)RAND_MAX) * Radius;

							FVector GroundLoc(
								Center.X + (cosf(Angle) * Dist),
								Center.Y + (sinf(Angle) * Dist),
								Center.Z
							);

							FVector AirLoc(
								GroundLoc.X,
								GroundLoc.Y,
								GroundLoc.Z + 400.f + (((float)rand() / (float)RAND_MAX) * 400.f)
							);

							// Spawned from the air with GroundLoc as the final
							// location so the pickups arc down like real drops.
							if (AFortInventory::SpawnPickup(AirLoc, *LootDrop, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetUnset(), nullptr, -1, true, true, true, nullptr, GroundLoc))
								Spawned++;
						}

						free(LootDrop);
					}

					LootDrops.Free();
				}

				if (Spawned > 0)
				{
					wchar_t wmsg[128];
					swprintf_s(wmsg, 128, L"Rained %d items!", Spawned);
					PlayerController->ClientMessage(FString(wmsg), FName(), 1.f);
				}
				else
					PlayerController->ClientMessage(FString(L"Found no loot for that tier group. Use chest, floor, rare, or a full tier-group name."), FName(), 1.f);
			}
			else if (command == "deltarget")
			{
				static auto DestroyTargetFn = const_cast<UFunction*>(FindObject<UFunction>(L"/Script/Engine.CheatManager.DestroyTarget"));
				static auto CheatManagerClass = FindClass("CheatManager");

				if (!DestroyTargetFn)
				{
					PlayerController->ClientMessage(FString(L"DestroyTarget function not found on this version."), FName(), 1.f);
					return;
				}

				auto CheatManagerOffset = PlayerController->GetOffset("CheatManager");
				UObject* CheatManager = CheatManagerOffset != -1 ? *(UObject**)(__int64(PlayerController) + CheatManagerOffset) : nullptr;

				if (!CheatManager && CheatManagerClass)
				{
					CheatManager = UGameplayStatics::SpawnObject(CheatManagerClass, PlayerController);

					if (CheatManager && CheatManagerOffset != -1)
						*(UObject**)(__int64(PlayerController) + CheatManagerOffset) = CheatManager;
				}

				if (!CheatManager)
				{
					PlayerController->ClientMessage(FString(L"Failed to create cheat manager!"), FName(), 1.f);
					return;
				}

				CheatManager->ProcessEvent(DestroyTargetFn, nullptr);
				PlayerController->ClientMessage(FString(L"Destroyed target."), FName(), 1.f);
			}
			else if (command == "delitem")
			{
				if (!PlayerController->WorldInventory)
				{
					PlayerController->ClientMessage(FString(L"No inventory found!"), FName(), 1.f);
					return;
				}

				auto Pawn = PlayerController->MyFortPawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn found!"), FName(), 1.f);
					return;
				}

				auto CurrentWeapon = (AFortWeapon*)Pawn->CurrentWeapon;

				if (!CurrentWeapon)
				{
					PlayerController->ClientMessage(FString(L"No equipped item to remove."), FName(), 1.f);
					return;
				}

				PlayerController->WorldInventory->Remove(CurrentWeapon->ItemEntryGuid);
				PlayerController->ClientMessage(FString(L"Removed the equipped item."), FName(), 1.f);
			}
			else if (command == "emoteall")
			{
				// Case preserved from originalCommand so EID names resolve.
				auto EmoteAllArgsStart = originalCommand.find(' ');
				auto EmoteArg = EmoteAllArgsStart == std::string::npos
					? std::string()
					: TrimPlayerCommandString(originalCommand.substr(EmoteAllArgsStart + 1).c_str());

				UAthenaDanceItemDefinition* ChosenEmote = nullptr;
				std::vector<UAthenaDanceItemDefinition*> EmotePool;

				if (!EmoteArg.empty())
				{
					ChosenEmote = FindEmoteByCommandArg(EmoteArg);

					if (!ChosenEmote)
					{
						PlayerController->ClientMessage(FString(L"Failed to find that emote. Try an EID name or a full path."), FName(), 1.f);
						return;
					}
				}
				else
				{
					EmotePool = GatherLoadedEmotesForCommand();

					if (EmotePool.empty())
					{
						PlayerController->ClientMessage(FString(L"No emotes are loaded!"), FName(), 1.f);
						return;
					}
				}

				int Dancing = 0;

				for (auto& Player : GameMode->AlivePlayers)
				{
					auto TargetPC = (AFortPlayerControllerAthena*)Player;

					if (!TargetPC || !TargetPC->MyFortPawn)
						continue;

					// Re-rolled per player when no emote was specified, so
					// everyone gets a different one.
					auto Emote = ChosenEmote ? ChosenEmote : EmotePool[rand() % EmotePool.size()];

					PlayEmoteInternal(TargetPC, Emote);
					Dancing++;
				}

				wchar_t wmsg[96];
				swprintf_s(wmsg, 96, L"%d players are emoting!", Dancing);
				PlayerController->ClientMessage(FString(wmsg), FName(), 1.f);
			}
			else if (command == "swap")
			{
				auto Pawn = PlayerController->Pawn;

				if (!Pawn)
				{
					PlayerController->ClientMessage(FString(L"No pawn found!"), FName(), 1.f);
					return;
				}

				auto SwapArgsStart = originalCommand.find(' ');

				if (SwapArgsStart == std::string::npos)
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat swap <player name>"), FName(), 1.f);
					return;
				}

				auto SwapName = TrimPlayerCommandString(originalCommand.substr(SwapArgsStart + 1).c_str());

				if (SwapName.empty())
				{
					PlayerController->ClientMessage(FString(L"Usage: cheat swap <player name>"), FName(), 1.f);
					return;
				}

				std::string MatchedName;
				bool bAmbiguous = false;
				auto TargetPC = FindPlayerByNameSubstringForCommand(GameMode, SwapName, PlayerController, MatchedName, bAmbiguous);

				if (bAmbiguous)
				{
					PlayerController->ClientMessage(FString(L"Multiple players match that name. Be more specific."), FName(), 1.f);
					return;
				}

				if (!TargetPC || !TargetPC->Pawn)
				{
					PlayerController->ClientMessage(FString(L"Could not find a player with that name."), FName(), 1.f);
					return;
				}

				auto TargetPawn = TargetPC->Pawn;

				auto MyLocation = Pawn->K2_GetActorLocation();
				auto TheirLocation = TargetPawn->K2_GetActorLocation();

				Pawn->K2_TeleportTo(TheirLocation, Pawn->K2_GetActorRotation());
				TargetPawn->K2_TeleportTo(MyLocation, TargetPawn->K2_GetActorRotation());

				auto Message = L"Swapped places with " + std::wstring(MatchedName.begin(), MatchedName.end()) + L"!";
				PlayerController->ClientMessage(FString(Message.c_str()), FName(), 1.f);
			}
			else if (command == "togglepersonalvehicle")
			{
				static auto TogglePersonalVehicleFn = PlayerController->GetFunction("TogglePersonalVehicle");

				if (!TogglePersonalVehicleFn)
				{
					PlayerController->ClientMessage(FString(L"Personal vehicles are not supported on this version."), FName(), 1.f);
					return;
				}

				bool bIsPersonalVehicleActive = false;
				auto IsActiveFn = PlayerController->GetFunction("IsPersonalVehicleActive");

				if (IsActiveFn)
					bIsPersonalVehicleActive = PlayerController->Call<bool>(IsActiveFn);

				bool bNewState = !bIsPersonalVehicleActive;
				PlayerController->Call<void>(TogglePersonalVehicleFn, bNewState);

				PlayerController->ClientMessage(FString(bNewState ? L"Personal vehicle enabled!" : L"Personal vehicle disabled!"), FName(), 1.f);
			}
			else if (command == "dumppawns")
			{
				TArray<AActor*> Pawns;
				GatherAllPawnsForCommand(Pawns);

				wchar_t wcount[64];
				swprintf_s(wcount, 64, L"Found %d pawns:", Pawns.Num());
				PlayerController->ClientMessage(FString(wcount), FName(), 1.f);

				for (int i = 0; i < Pawns.Num(); i++)
				{
					auto IndexedPawn = Pawns[i];

					if (!IndexedPawn)
						continue;

					std::string Line = "[" + std::to_string(i) + "] " + std::string(IndexedPawn->Name.ToString().c_str());

					// The player name is what you will usually want to type, so
					// lead with it. Guarded - see IsFortPlayerPawnForCommand.
					if (IsFortPlayerPawnForCommand(IndexedPawn))
					{
						auto FortPawn = (AFortPlayerPawnAthena*)IndexedPawn;

						if (FortPawn->PlayerState)
						{
							std::string OwnerName = ((AFortPlayerStateAthena*)FortPawn->PlayerState)->GetPlayerName().ToString().c_str();

							if (!OwnerName.empty())
								Line += " \"" + OwnerName + "\"";
						}
					}

					if (IndexedPawn->Class)
						Line += " (" + std::string(IndexedPawn->Class->GetName().ToString().c_str()) + ")";

					if (IndexedPawn == PlayerController->Pawn)
						Line += " <- you";

					auto Message = std::wstring(Line.begin(), Line.end());
					PlayerController->ClientMessage(FString(Message.c_str()), FName(), 1.f);
				}

				Pawns.Free();
			}
			else if (command == "dumpge")
			{
				StartLoadedGameplayEffectCatalogForCommand(PlayerController);
			}
			else if (command == "possess")
			{
				// Remembered so "possess" with no argument gets you back out
				// again - otherwise taking over a bot is a one-way trip.
				static std::unordered_map<AFortPlayerControllerAthena*, AActor*> OriginalPawns;

				// Pawn -> the controller we took it from, so it can be handed
				// back whenever we leave it, however we leave it.
				static std::unordered_map<AActor*, AFortPlayerControllerAthena*> DisplacedOwners;

				auto CurrentPawn = (AActor*)PlayerController->Pawn;

				auto PossessArgsStart = originalCommand.find(' ');
				auto PossessArg = PossessArgsStart == std::string::npos
					? std::string()
					: TrimPlayerCommandString(originalCommand.substr(PossessArgsStart + 1).c_str());

				if (NormalizePlayerCommandString(PossessArg) == "reset")
					PossessArg.clear();

				AActor* TargetPawn = nullptr;
				std::string MatchedName;

				if (PossessArg.empty())
				{
					auto It = OriginalPawns.find(PlayerController);

					if (It == OriginalPawns.end() || !It->second)
					{
						PlayerController->ClientMessage(FString(L"Usage: cheat possess <player name | index | pawn name>. With no argument it returns you to your own pawn."), FName(), 1.f);
						return;
					}

					// The pawn we left behind may have been destroyed since, so
					// only trust the pointer if it is still in the world.
					TArray<AActor*> Pawns;
					GatherAllPawnsForCommand(Pawns);

					bool bStillAlive = false;

					for (auto& Pawn : Pawns)
					{
						if (Pawn == It->second)
						{
							bStillAlive = true;
							break;
						}
					}

					Pawns.Free();

					if (!bStillAlive)
					{
						OriginalPawns.erase(It);
						PlayerController->ClientMessage(FString(L"Your original pawn no longer exists."), FName(), 1.f);
						return;
					}

					TargetPawn = It->second;
					MatchedName = TargetPawn->Name.ToString().c_str();
				}
				else
				{
					int PawnIndex = 0;

					if (TryParseCommandInt(PossessArg, PawnIndex))
					{
						TArray<AActor*> Pawns;
						GatherAllPawnsForCommand(Pawns);

						if (PawnIndex < 0 || PawnIndex >= Pawns.Num())
						{
							wchar_t wmsg[96];
							swprintf_s(wmsg, 96, L"Invalid index. Use 0 to %d, see cheat dumppawns.", Pawns.Num() - 1);
							Pawns.Free();
							PlayerController->ClientMessage(FString(wmsg), FName(), 1.f);
							return;
						}

						TargetPawn = Pawns[PawnIndex];
						Pawns.Free();

						if (TargetPawn)
							MatchedName = TargetPawn->Name.ToString().c_str();
					}
					else
					{
						// Player name first - "possess cipher" is what you
						// actually want to type. Falls back to the pawn's
						// object name so dumppawns output still works.
						bool bAmbiguous = false;
						auto TargetPC = FindPlayerByNameSubstringForCommand(GameMode, PossessArg, PlayerController, MatchedName, bAmbiguous, true);

						if (bAmbiguous)
						{
							PlayerController->ClientMessage(FString(L"Multiple players match that name. Be more specific."), FName(), 1.f);
							return;
						}

						if (TargetPC)
							TargetPawn = (AActor*)TargetPC->Pawn;

						if (!TargetPawn)
						{
							TargetPawn = FindPawnByNameSubstringForCommand(PossessArg, MatchedName, bAmbiguous);

							if (bAmbiguous)
							{
								PlayerController->ClientMessage(FString(L"Multiple pawns match that name. Be more specific, or use an index from cheat dumppawns."), FName(), 1.f);
								return;
							}
						}
					}
				}

				if (!TargetPawn)
				{
					PlayerController->ClientMessage(FString(L"Could not find that player or pawn."), FName(), 1.f);
					return;
				}

				if (TargetPawn == CurrentPawn)
				{
					PlayerController->ClientMessage(FString(L"You are already possessing that pawn."), FName(), 1.f);
					return;
				}

				// Objects (vehicles, AI, wildlife) are pawns too and can be
				// possessed - only the Fortnite-specific steps below are gated on
				// this, so their properties are never touched. See
				// IsFortPlayerPawnForCommand for why that guard matters.
				bool bTargetIsFortPawn =
					IsFortPlayerPawnForCommand(TargetPawn) &&
					!IsRemoteControlledPawn(TargetPawn) &&
					!IsNativeVehiclePossessionPawn(TargetPawn);

				// Only remember the pawn we started on, not each hop, so
				// repeated possessions still return to the real one.
				if (CurrentPawn && OriginalPawns.find(PlayerController) == OriginalPawns.end())
					OriginalPawns[PlayerController] = CurrentPawn;

				auto Original = OriginalPawns.find(PlayerController);
				bool bReturningHome = Original != OriginalPawns.end() && TargetPawn == Original->second;

				// Give back whatever we are currently borrowing before moving on.
				// This runs no matter HOW we leave - reset, an index, or a name -
				// so stepping back onto your own pawn also restores the player you
				// were riding. Release ourselves first so the pawn is never
				// attached to two controllers at once.
				auto Borrowed = CurrentPawn ? DisplacedOwners.find(CurrentPawn) : DisplacedOwners.end();

				if (Borrowed != DisplacedOwners.end())
				{
					auto ReturnedTo = Borrowed->second;
					DisplacedOwners.erase(Borrowed);

					std::string ReturnedName = CurrentPawn->Name.ToString().c_str();

					if (ReturnedTo && ReturnedTo->PlayerState)
					{
						std::string PS = ((AFortPlayerStateAthena*)ReturnedTo->PlayerState)->GetPlayerName().ToString().c_str();

						if (!PS.empty())
							ReturnedName = PS;
					}

					PlayerController->UnPossess();

					// Fresh pawn rather than a re-possess, or they come back
					// unable to jump or use items - see the helper.
					RespawnControllerOnFreshPawnForCommand(GameMode, ReturnedTo, (AFortPlayerPawnAthena*)CurrentPawn);

					auto Back = L"Gave " + std::wstring(ReturnedName.begin(), ReturnedName.end()) + L" back control.";
					PlayerController->ClientMessage(FString(Back.c_str()), FName(), 1.f);
				}

				if (bReturningHome)
				{
					// Coming back to our own body. Re-possessing the pawn we left
					// leaves us unable to jump or use items, so swap in a fresh
					// one at the same spot the way "size" does.
					auto NewPawn = RespawnControllerOnFreshPawnForCommand(GameMode, PlayerController, (AFortPlayerPawnAthena*)TargetPawn);

					if (!NewPawn)
					{
						PlayerController->ClientMessage(FString(L"Failed to spawn your pawn!"), FName(), 1.f);
						return;
					}

					OriginalPawns.erase(PlayerController);
					PlayerController->ClientMessage(FString(L"Back on your own pawn!"), FName(), 1.f);
					return;
				}

				// Controller is a base APawn field, safe to read off any pawn.
				auto ExistingController = ((AFortPlayerPawnAthena*)TargetPawn)->Controller;

				if (ExistingController && ExistingController != (AActor*)PlayerController)
				{
					if (IsPlayerControllerForCommand(ExistingController))
					{
						// A real player - turn them into a spectator locked on
						// their pawn so it moves for them under our control rather
						// than freezing. Must happen BEFORE we possess so the pawn
						// is never attached to two controllers at once.
						auto DisplacedPC = (AFortPlayerControllerAthena*)ExistingController;
						DisplacedOwners[TargetPawn] = DisplacedPC;
						MakeControllerSpectatePawnForCommand(DisplacedPC, (AFortPlayerPawnAthena*)TargetPawn);

						auto WarnName = std::wstring(MatchedName.begin(), MatchedName.end());
						PlayerController->ClientMessage(FString((WarnName + L" is now watching you.").c_str()), FName(), 1.f);
					}
					else
					{
						// An AI controller (or similar). Detach it so the possess
						// is clean, but do not run any of the player-only spectate
						// logic on it. UnPossess is a base AController function.
						((AFortPlayerControllerAthena*)ExistingController)->UnPossess();
					}
				}

				// Taking a live pawn has to be a real possess - swapping in a
				// fresh pawn would defeat the point. Skip the possession handler's
				// respawn teleport the way "size" does, or we land in a
				// just-respawned state where items cannot be used.
				SkipNextPossessRespawn(PlayerController);
				PlayerController->Possess(TargetPawn);

				// Only touch Fortnite-specific state for actual player pawns.
				// Pointing MyFortPawn at a vehicle would feed a non-player pawn
				// into systems that assume it is one; re-equipping a weapon onto
				// an object is meaningless.
				if (bTargetIsFortPawn)
				{
					PlayerController->MyFortPawn = (AFortPlayerPawnAthena*)TargetPawn;

					// PUPPET MODE: this drives another player's body around for
					// movement/emotes only. We deliberately do NOT rebind abilities
					// or re-equip weapons - taking over a live pawn's ability and
					// weapon systems fought the engine's ownership assumptions and
					// never became reliable. Keeping it to movement is the stable
					// subset.
					//
					// Put it on the ground and give it health so the puppet does
					// not float or die. Done now AND again from the possession-ack:
					// the ack fires after this command on a client round-trip and
					// re-runs native setup that resets both, so the in-ack pass is
					// the one that sticks (this covers the no-ack case).
					FinalizePossessedPawnForCommand(PlayerController, (AFortPlayerPawnAthena*)TargetPawn);
					GFinalizePossessTakeover.insert(PlayerController);
				}

				PlayerController->OnRep_Pawn();
				PlayerController->ForceNetUpdate();
				TargetPawn->ForceNetUpdate();

				auto Message = L"Possessing " + std::wstring(MatchedName.begin(), MatchedName.end()) + (bTargetIsFortPawn ? L" (puppet - movement only)!" : L" (object)!");
				PlayerController->ClientMessage(FString(Message.c_str()), FName(), 1.f);
			}
			else if (command == "delbot")
			{
				std::vector<AFortPlayerControllerAthena*> BotsToRemove;

				for (auto& Player : GameMode->AlivePlayers)
				{
					auto BotPC = (AFortPlayerControllerAthena*)Player;

					if (!BotPC || BotPC == PlayerController || !BotPC->PlayerState)
						continue;

					auto BotPS = (AFortPlayerStateAthena*)BotPC->PlayerState;

					if (!BotPS->HasbIsABot() || !BotPS->bIsABot)
						continue;

					// PlayerAI entities also carry bIsABot. They are owned by
					// PlayerAI (which does its own alive-count bookkeeping), so
					// leave them alone - delbot is for "spawnbot" bots only.
					if (PlayerAIManager::FindByController(BotPC))
						continue;

					BotsToRemove.push_back(BotPC);
				}

				int Removed = 0;

				for (auto& BotPC : BotsToRemove)
				{
					UnregisterTrackedSpawnedBotController(BotPC);
					G172SpawnedBotRemovalAttempts.erase(BotPC);

					// "spawnbot" creates a pawn, a controller and a player
					// state, so all three go.
					if (BotPC->MyFortPawn)
						BotPC->MyFortPawn->K2_DestroyActor();
					else if (BotPC->Pawn)
						BotPC->Pawn->K2_DestroyActor();

					if (BotPC->PlayerState)
						BotPC->PlayerState->K2_DestroyActor();

					BotPC->K2_DestroyActor();
					Removed++;
				}

				wchar_t wmsg[96];
				swprintf_s(wmsg, 96, L"Removed %d bots!", Removed);
				PlayerController->ClientMessage(FString(wmsg), FName(), 1.f);
			}
			else if (command == "shortcmds" || command == "shortcommands")
			{
				std::string category = args.size() >= 2 ? std::string(args[1].c_str()) : "";
				std::transform(category.begin(), category.end(), category.begin(), tolower);

				if (category.empty() || category == "items")
				{
					std::vector<std::string> names;
					names.reserve(Misc::ItemNames.size());
					for (auto& pair : Misc::ItemNames)
						names.push_back(pair.first);
					std::sort(names.begin(), names.end());

					PlayerController->ClientMessage(FString(L"Short commands for 'cheat give <name>':"), FName(), 1.f);
					for (auto& name : names)
						PlayerController->ClientMessage(FString(std::wstring(name.begin(), name.end()).c_str()), FName(), 1.f);
				}

				if (category.empty())
					PlayerController->ClientMessage(FString(L" "), FName(), 1.f);

				if (category.empty() || category == "objects")
				{
					std::vector<std::string> names;
					names.reserve(Misc::ObjectNames.size());
					for (auto& pair : Misc::ObjectNames)
						names.push_back(pair.first);
					std::sort(names.begin(), names.end());

					PlayerController->ClientMessage(FString(L"Short commands for 'cheat spawn <name>':"), FName(), 1.f);
					for (auto& name : names)
						PlayerController->ClientMessage(FString(std::wstring(name.begin(), name.end()).c_str()), FName(), 1.f);
				}

				if (!category.empty() && category != "items" && category != "objects")
					PlayerController->ClientMessage(FString(L"Usage: cheat shortcmds <items/objects>"), FName(), 1.f);
			}
			else
				goto _help;
		}
		else
		{
			if (command == "suicide")
			{
				PlayerController->ServerSuicide();
				PlayerController->ClientMessage(FString(L"Killed player!"), FName(), 1.f);
			}
			else
			{
				PlayerController->ClientMessage(FString(L"Commands are currently disabled for this session."), FName(), 1.f);
				return;
			}

			if (GUI::GetSelectedPlaylist() == static_cast<int>(Playlist::OnlyUp))
			{
				if (command == "savewaypoint" || command == "s")
				{
					if (args.size() < 2)
					{
						PlayerController->ClientMessage(FString(L"Please provide a phrase to save the waypoint to."), FName(), 1.f);
						return;
					}

					auto Pawn = PlayerController->Pawn;

					if (!Pawn)
					{
						PlayerController->ClientMessage(FString(L"Couldn't find a pawn!"), FName(), 1.f);
						return;
					}

					FVector PawnLocation(Pawn->K2_GetActorLocation().X, Pawn->K2_GetActorLocation().Y, Pawn->K2_GetActorLocation().Z);

					if (PawnLocation.X == 0.0f && PawnLocation.Y == 0.0f && PawnLocation.Z == 0.0f)
					{
						PlayerController->ClientMessage(FString(L"Failed to save a waypoint."), FName(), 1.f);
						return;
					}

					std::string Phrase = args[1].c_str();

					auto It = Waypoints.find(Phrase);

					if (It != Waypoints.end())
					{
						if (args.size() >= 3 && (args[2] == "override" || args[2] == "o"))
						{
							It->second.clear();
							It->second.push_back(PawnLocation);

							PlayerController->ClientMessage(FString(L"Waypoint overridden successfully!"), FName(), 1.f);
						}
						else
						{
							PlayerController->ClientMessage(FString(L"A waypoint with this phrase already exists! Use 'waypoint {phrase} override' to override it."), FName(), 1);
						}
					}
					else
					{
						std::vector<FVector> Locations;
						Locations.push_back(PawnLocation);
						Waypoints[Phrase] = Locations;

						PlayerController->ClientMessage(FString(L"Waypoint saved! Use \" cheat waypoint (phrase) \" to teleport to that location!"), FName(), 1);
					}
				}
				else if (command == "waypoint" || command == "w")
				{
					if (args.size() < 2)
					{
						PlayerController->ClientMessage(FString(L"Please provide a waypoint phrase to teleport to."), FName(), 1.f);
						return;
					}

					std::string Phrase = args[1].c_str();

					auto It = Waypoints.find(Phrase);

					if (It == Waypoints.end() || It->second.empty())
					{
						PlayerController->ClientMessage(FString(L"A saved waypoint with this phrase was not found!"), FName(), 1.f);
						return;
					}

					const auto& WaypointList = It->second;

					if (args.size() >= 3 && (args[2] == "previous" || args[2] == "p"))
					{
						if (WaypointList.size() < 2)
						{
							PlayerController->ClientMessage(FString(L"No previous waypoint available for this phrase!"), FName(), 1.f);
							return;
						}

						FVector Destination = Waypoints[Phrase][Waypoints[Phrase].size() - 2];

						auto Pawn = PlayerController->Pawn;

						if (Pawn)
						{
							Pawn->K2_TeleportTo(Destination, Pawn->K2_GetActorRotation(), false, true);
							Pawn->CharacterMovement->Velocity = FVector{};
							RegenerateMinimumGodHealthAfterWaypoint(
								PlayerController, Pawn);
							PlayerController->ClientMessage(FString(L"Teleported to previous waypoint!"), FName(), 1.f);
						}
						else
						{
							PlayerController->ClientMessage(FString(L"Couldn't find a pawn to teleport!"), FName(), 1.f);
						}
					}
					else
					{
						FVector Destination = WaypointList.back();

						if (Destination.X == 0.0f && Destination.Y == 0.0f && Destination.Z == 0.0f)
						{
							PlayerController->ClientMessage(FString(L"Waypoint is invalid (0, 0, 0)! Aborting teleport."), FName(), 1.f);
							return;
						}

						auto Pawn = PlayerController->Pawn;

						if (Pawn)
						{
							Pawn->K2_TeleportTo(Destination, Pawn->K2_GetActorRotation(), false, true);
							Pawn->CharacterMovement->Velocity = FVector{};
							RegenerateMinimumGodHealthAfterWaypoint(
								PlayerController, Pawn);
							PlayerController->ClientMessage(FString(L"Teleported to waypoint!"), FName(), 1.f);
						}
						else
						{
							PlayerController->ClientMessage(FString(L"Couldn't find a pawn to teleport!"), FName(), 1.f);
						}
					}
				}
			}
		}
	}
}

static UFortVehicleSeatWeaponComponent*
ResolveVehicleSeatWeaponComponent(
	AActor* Vehicle,
	int32 SeatIndex)
{
	if (!Vehicle || SeatIndex < 0)
		return nullptr;

	// FN30 vehicle mods can add several dynamic weapon-seat components. The
	// generic component lookup returns the first one, which is not necessarily
	// the component assigned to this passenger seat. Ask the vehicle interface
	// for its authoritative seat mapping and validate the exact reflected ABI
	// before invoking it.
	static UFunction* GetSeatWeaponComponentFunction = nullptr;
	static bool bResolvedGetSeatWeaponComponent = false;
	if (!bResolvedGetSeatWeaponComponent)
	{
		bResolvedGetSeatWeaponComponent = true;
		GetSeatWeaponComponentFunction =
			const_cast<UFunction*>(FindObject<UFunction>(
				L"/Script/FortniteGame.FortVehicleInterface."
				L"GetSeatWeaponComponent"));

		bool bSeatIndexValid = false;
		bool bReturnValueValid = false;
		if (GetSeatWeaponComponentFunction)
		{
			auto Params =
				GetSeatWeaponComponentFunction->GetParamsNamed();
			if (Params.Size == 0x10)
			{
				for (const auto& Param : Params.NameOffsetMap)
				{
					if (Param.Name == "SeatIndex")
					{
						bSeatIndexValid =
							Param.Offset == 0x0 &&
							Param.ElementSize == sizeof(int32) &&
							(Param.PropertyFlags & 0x82) == 0x82 &&
							(Param.PropertyFlags & 0x500) == 0;
					}
					else if (Param.Name == "ReturnValue")
					{
						bReturnValueValid =
							Param.Offset == 0x8 &&
							Param.ElementSize == sizeof(void*) &&
							(Param.PropertyFlags & 0x580) == 0x580;
					}
				}
			}
		}

		if (!bSeatIndexValid || !bReturnValueValid)
			GetSeatWeaponComponentFunction = nullptr;
	}

	if (GetSeatWeaponComponentFunction)
	{
		struct alignas(8) FGetSeatWeaponComponentParams
		{
			int32 SeatIndex = -1;
			uint8 Pad[4]{};
			UFortVehicleSeatWeaponComponent* ReturnValue = nullptr;
		} Params;
		static_assert(
			sizeof(FGetSeatWeaponComponentParams) == 0x10,
			"FN30 GetSeatWeaponComponent ABI changed");

		Params.SeatIndex = SeatIndex;
		Vehicle->ProcessEvent(
			GetSeatWeaponComponentFunction, &Params);
		auto SeatWeaponClass =
			UFortVehicleSeatWeaponComponent::StaticClass();
		if (Params.ReturnValue && SeatWeaponClass &&
			Params.ReturnValue->IsA(SeatWeaponClass))
		{
			return Params.ReturnValue;
		}
	}

	// Preserve legacy vehicle behavior when the interface method does not
	// exist. Season 30 modded cars normally return above.
	return static_cast<UFortVehicleSeatWeaponComponent*>(
		Vehicle->GetComponentByClass(
			UFortVehicleSeatWeaponComponent::StaticClass()));
}

static bool ConfigureVehicleSeatWeapon(
	AFortPlayerControllerAthena* PlayerController,
	AFortPlayerPawnAthena* RiderPawn,
	AActor* Vehicle,
	UFortVehicleSeatWeaponComponent* SeatWeaponComponent,
	FWeaponSeatDefinition& WeaponDefinition,
	int32 SeatIndex,
	AActor* WeaponToRestore)
{
	if (!PlayerController || !PlayerController->WorldInventory ||
		!RiderPawn || !Vehicle || !SeatWeaponComponent ||
		SeatIndex < 0 || WeaponDefinition.SeatIndex != SeatIndex ||
		!WeaponDefinition.VehicleWeapon)
	{
		return false;
	}

	// MountedWeaponInfoRepped stores a real interface pointer on FN30. Validate
	// it before granting anything so a non-vehicle actor can never leave an
	// unusable temporary weapon in the player's inventory.
	const IInterface* VehicleInterface = nullptr;
	if (FMountedWeaponInfoRepped::HasHostVehicleCached())
	{
		auto VehicleInterfaceClass =
			IFortVehicleInterface::StaticClass();
		if (!VehicleInterfaceClass)
			return false;

		VehicleInterface = Vehicle->GetInterface(
			VehicleInterfaceClass);
		if (!VehicleInterface)
			return false;
	}
	else if (!FMountedWeaponInfoRepped::HasHostVehicleCachedActor())
	{
		return false;
	}

	auto Tracked = GTrackedVehicleLoadouts.find(PlayerController);
	if (Tracked == GTrackedVehicleLoadouts.end())
	{
		Tracked = GTrackedVehicleLoadouts.emplace(
			PlayerController,
			CaptureVehicleLoadout(PlayerController, RiderPawn)).first;
	}

	// A native seat transition may already have granted the exact weapon.
	// Reuse only an entry that this vehicle lifecycle tracked; never select an
	// arbitrary player-owned duplicate merely because its definition matches.
	auto ItemEntry =
		PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search(
			[&](FFortItemEntry& Candidate)
			{
				return Candidate.ItemDefinition ==
						WeaponDefinition.VehicleWeapon &&
					VehicleLoadoutContainsGuid(
						Tracked->second.TemporaryItemGuids,
						Candidate.ItemGuid);
			},
			FFortItemEntry::Size());
	if (!ItemEntry)
	{
		const auto InventoryBefore =
			SnapshotVehicleInventoryGuids(
				PlayerController->WorldInventory);
		auto Stats = AFortInventory::GetStats(
			WeaponDefinition.VehicleWeapon);
		auto Item = PlayerController->WorldInventory->GiveItem(
			WeaponDefinition.VehicleWeapon,
			1,
			Stats ? Stats->ClipSize : 0);
		if (!Item)
			return false;

		TrackNewVehicleItems(
			Tracked->second,
			PlayerController->WorldInventory,
			InventoryBefore,
			WeaponDefinition.VehicleWeapon);
		ItemEntry = &Item->ItemEntry;
	}

	const auto ItemGuid = ItemEntry->ItemGuid;
	const auto TrackerGuid = FFortItemEntry::HasTrackerGuid()
		? ItemEntry->TrackerGuid : FGuid();
	const int32 ItemLevel = ItemEntry->Level;
	PlayerController->ServerExecuteInventoryItem(ItemGuid);
	PlayerController->ClientEquipItem(ItemGuid, true);

	auto MountedWeapon = RiderPawn->CurrentWeapon
		? RiderPawn->CurrentWeapon->Cast<AFortWeapon>()
		: nullptr;
	if (!MountedWeapon ||
		!VehicleLoadoutGuidsEqual(
			MountedWeapon->ItemEntryGuid, ItemGuid))
	{
		MountedWeapon = (AFortWeapon*)RiderPawn->EquipWeaponDefinition(
			WeaponDefinition.VehicleWeapon,
			ItemGuid,
			TrackerGuid,
			false);
	}
	if (!MountedWeapon)
		return false;

	if (RiderPawn->HasPreviousWeapon() &&
		WeaponToRestore &&
		WeaponToRestore != MountedWeapon)
	{
		RiderPawn->PreviousWeapon = WeaponToRestore;
	}

	// A mod-created seat component can miss the pawn-enter callback when the
	// turret is attached to an occupied car. Re-run its retained lifecycle
	// setup so targeting/input delegates are bound for this exact definition.
	if (SeatWeaponComponent->GetFunction("EquipVehicleWeapon"))
	{
		SeatWeaponComponent->EquipVehicleWeapon(
			RiderPawn,
			WeaponDefinition,
			ItemLevel);
	}

	// EquipVehicleWeapon can replace both the dynamic component and the weapon
	// actor. Re-resolve the seat-specific component and retain the weapon that
	// native actually left equipped before publishing any transient state.
	auto FinalSeatWeaponComponent =
		ResolveVehicleSeatWeaponComponent(Vehicle, SeatIndex);
	if (FinalSeatWeaponComponent)
		SeatWeaponComponent = FinalSeatWeaponComponent;

	auto FinalMountedWeapon = RiderPawn->CurrentWeapon
		? RiderPawn->CurrentWeapon->Cast<AFortWeapon>()
		: nullptr;
	if (FinalMountedWeapon &&
		((FinalMountedWeapon->HasWeaponData() &&
			FinalMountedWeapon->WeaponData ==
				WeaponDefinition.VehicleWeapon) ||
			VehicleLoadoutGuidsEqual(
				FinalMountedWeapon->ItemEntryGuid, ItemGuid)))
	{
		MountedWeapon = FinalMountedWeapon;
	}
	// MountedWeaponInfoRepped belongs to FortWeaponRangedForVehicle, not
	// AFortWeapon. Its generated accessor caches one reflected offset globally;
	// never let a normal/dual ranged weapon seed or reuse that subclass offset.
	static const UClass* VehicleWeaponClass = nullptr;
	if (!VehicleWeaponClass)
		VehicleWeaponClass =
			FindClass("FortWeaponRangedForVehicle");
	if (!MountedWeapon ||
		!VehicleWeaponClass ||
		!MountedWeapon->IsA(VehicleWeaponClass) ||
		!MountedWeapon->HasMountedWeaponInfoRepped())
	{
		return false;
	}

	if (RiderPawn->HasPreviousWeapon() &&
		WeaponToRestore &&
		WeaponToRestore != MountedWeapon)
	{
		RiderPawn->PreviousWeapon = WeaponToRestore;
	}

	// Preserve the camera/aim cache populated by the final native transition.
	// Clearing the full structure detaches a mounted projectile ray from its
	// passenger camera and leaves only the host fields valid.
	FMountedWeaponInfoRepped MountedInfo =
		MountedWeapon->MountedWeaponInfoRepped;
	if (FMountedWeaponInfoRepped::HasHostVehicleCached())
	{
		MountedInfo.HostVehicleCached.ObjectPointer = Vehicle;
		MountedInfo.HostVehicleCached.InterfacePointer =
			VehicleInterface;
	}
	else
	{
		MountedInfo.HostVehicleCachedActor = Vehicle;
	}
	MountedInfo.HostVehicleSeatIndexCached = SeatIndex;
	MountedWeapon->MountedWeaponInfoRepped = MountedInfo;
	MountedWeapon->OnRep_MountedWeaponInfoRepped();

	// The enhanced-input F toggle passes this transient value to SeatIsTurret.
	// Publish it only after the native equip path has settled, on the exact
	// component mapped to the occupied passenger seat.
	if (SeatWeaponComponent->HasActiveSeatIdx())
		SeatWeaponComponent->ActiveSeatIdx = SeatIndex;

	MountedWeapon->ForceNetUpdate();
	RiderPawn->ForceNetUpdate();
	return true;
}

extern bool bDidntFind;

struct FServerAttemptInteractParams27_11
{
	AActor* ReceivingActor;
	UObject* InteractComponent;
	uint8 InteractType;
	uint8 InteractTypePadding[7];
	UObject* OptionalObjectData;
	uint8 InteractionBeingAttempted;
	uint8 InteractionPadding[3];
	int32 RequestId;
};

static_assert(
	sizeof(FServerAttemptInteractParams27_11) == 0x28,
	"27.11 ServerAttemptInteract parameter layout changed");
static_assert(
	offsetof(
		FServerAttemptInteractParams27_11,
		InteractionBeingAttempted) == 0x20,
	"27.11 ServerAttemptInteract action offset changed");

struct FServerNotifyEndLongUseParams27_11
{
	AActor* ReceivingActor;
	bool bUseCompleted;
	uint8 Padding[7];
};

static_assert(
	sizeof(FServerNotifyEndLongUseParams27_11) == 0x10,
	"27.11 ServerNotifyEndLongUse parameter layout changed");

struct FPendingReviveLongUse27_11
{
	TWeakObjectPtr<AActor> Target;
	TWeakObjectPtr<UObject> InteractionContext;
	ULONGLONG StartedAtMs = 0;
	float StartedAtWorldTime = -1.f;
};

struct FLongInteractValidateInfoPatch27_11
{
	uint8 PreviousBytes[0x10]{};
	TWeakObjectPtr<UObject> InteractionContext;
	uint32 ValidateInfoOffset = uint32(-1);
	bool bArmed = false;
};

using FInteractionExec27_11 = void (*)(UObject*, FFrame&);
static FInteractionExec27_11
	GServerNotifyStartLongUse27_11OG = nullptr;
static FInteractionExec27_11
	GServerNotifyEndLongUse27_11OG = nullptr;
static UFunction*
	GServerAttemptInteractFunction27_11 = nullptr;
static bool
	GServerAttemptInteractSchema27_11 = false;
static uint8*
	GInteractValidateLongEnable27_11 = nullptr;
static std::unordered_map<
	AFortPlayerControllerAthena*,
	FPendingReviveLongUse27_11>
	GPendingReviveLongUses27_11;

static AFortPlayerControllerAthena*
	ResolveInteractionPlayerController(UObject* Context)
{
	if (!IsUsableDeathObject(Context))
		return nullptr;

	auto InteractionComponentClass =
		FindClass("FortControllerComponent_Interaction");
	if (InteractionComponentClass &&
		Context->IsA(InteractionComponentClass))
	{
		auto Owner =
			((UActorComponent*)Context)->GetOwner();
		return IsUsableDeathObject(Owner)
			? Owner->Cast<AFortPlayerControllerAthena>()
			: nullptr;
	}

	return Context->Cast<AFortPlayerControllerAthena>();
}

static bool HasExactInputParameter(
	const UFunction::ParamsNamed& Params,
	const char* Name,
	uint32 Offset,
	uint32 Size)
{
	constexpr uint64 CPF_Parm = 0x80;
	constexpr uint64 CPF_ReturnParm = 0x400;
	for (const auto& Param : Params.NameOffsetMap)
	{
		if (Param.Name == Name &&
			Param.Offset == Offset &&
			Param.ElementSize == Size &&
			(Param.PropertyFlags & CPF_Parm) &&
			!(Param.PropertyFlags & CPF_ReturnParm))
		{
			return true;
		}
	}
	return false;
}

static bool HasServerNotifyStartLongUseSchema27_11(
	UFunction* Function)
{
	if (!Function ||
		Function->GetPropertiesSize() != sizeof(AActor*))
	{
		return false;
	}

	const auto Params = Function->GetParamsNamed();
	return Params.Size == sizeof(AActor*) &&
		Params.NameOffsetMap.size() == 1 &&
		HasExactInputParameter(
			Params,
			"ReceivingActor",
			0x00,
			sizeof(AActor*));
}

static bool HasServerNotifyEndLongUseSchema27_11(
	UFunction* Function)
{
	if (!Function ||
		Function->GetPropertiesSize() !=
			sizeof(FServerNotifyEndLongUseParams27_11))
	{
		return false;
	}

	const auto Params = Function->GetParamsNamed();
	return Params.Size ==
			sizeof(FServerNotifyEndLongUseParams27_11) &&
		Params.NameOffsetMap.size() == 2 &&
		HasExactInputParameter(
			Params,
			"ReceivingActor",
			0x00,
			sizeof(AActor*)) &&
		HasExactInputParameter(
			Params,
			"bUseCompleted",
			0x08,
			sizeof(bool));
}

static bool IsWritableInteractionMemory(
	void* Address,
	size_t Size)
{
	if (!Address || !Size)
		return false;

	MEMORY_BASIC_INFORMATION MemoryInfo{};
	if (VirtualQuery(
			Address,
			&MemoryInfo,
			sizeof(MemoryInfo)) != sizeof(MemoryInfo) ||
		MemoryInfo.State != MEM_COMMIT ||
		(MemoryInfo.Protect &
			(PAGE_GUARD | PAGE_NOACCESS)))
	{
		return false;
	}

	const DWORD Protection =
		MemoryInfo.Protect & 0xFF;
	const bool bWritable =
		Protection == PAGE_READWRITE ||
		Protection == PAGE_WRITECOPY ||
		Protection == PAGE_EXECUTE_READWRITE ||
		Protection == PAGE_EXECUTE_WRITECOPY;
	const auto Begin =
		reinterpret_cast<uintptr_t>(Address);
	const auto End = Begin + Size;
	const auto RegionEnd =
		reinterpret_cast<uintptr_t>(
			MemoryInfo.BaseAddress) +
		MemoryInfo.RegionSize;
	return bWritable &&
		End >= Begin &&
		End <= RegionEnd;
}

static uint8* PrepareLongInteractValidateInfo27_11(
	UObject* InteractionContext,
	AActor* ReceivingActor,
	float StartedAtWorldTime,
	FLongInteractValidateInfoPatch27_11& Patch,
	const char* Source)
{
	Patch = {};
	if (VersionInfo.FortniteVersion != 27.11 ||
		!IsUsableDeathObject(InteractionContext) ||
		!IsUsableDeathObject(ReceivingActor) ||
		!FPlatformMath::IsFinite(StartedAtWorldTime) ||
		StartedAtWorldTime < 0.f)
	{
		return nullptr;
	}

	auto InteractionComponentClass =
		FindClass("FortControllerComponent_Interaction");
	auto TargetPawn =
		ReceivingActor->Cast<AFortPlayerPawnAthena>();
	if (!InteractionComponentClass ||
		!InteractionContext->IsA(
			InteractionComponentClass) ||
		!TargetPawn ||
		!IsPawnDBNOForSpectating(TargetPawn))
	{
		return nullptr;
	}

	constexpr uint64 CASTCLASS_FStructProperty =
		0x100000;
	constexpr uint64 CASTCLASS_FObjectProperty =
		0x10000;
	auto ValidateInfoProperty =
		InteractionContext->GetProperty(
			"LongInteractValidateInfo",
			CASTCLASS_FStructProperty);
	auto ValidateInfoStruct =
		FindStruct("ServerLongInteractInfo");
	auto TargetProperty =
		ValidateInfoStruct
			? ValidateInfoStruct->GetProperty(
				"Target",
				CASTCLASS_FObjectProperty)
			: nullptr;
	const size_t PropertyMetadataSize =
		static_cast<size_t>((std::max)(
			Offsets::ElementSize,
			Offsets::Offset_Internal)) +
		sizeof(uint32);
	if (!ValidateInfoProperty ||
		!ValidateInfoStruct ||
		!TargetProperty ||
		!SDK::MemReadable(
			ValidateInfoProperty,
			PropertyMetadataSize) ||
		!SDK::MemReadable(
			TargetProperty,
			PropertyMetadataSize))
	{
		SDK::DbgLog(
			"[Revive] 27.11 validate target schema missing "
			"source=%s context=%p outer=%p struct=%p "
			"targetProperty=%p\n",
			Source ? Source : "unknown",
			(void*)InteractionContext,
			(void*)ValidateInfoProperty,
			(void*)ValidateInfoStruct,
			(void*)TargetProperty);
		return nullptr;
	}

	const uint32 ValidateInfoOffset =
		DecryptPropOffset(GetFromOffset<uint32>(
			ValidateInfoProperty,
			Offsets::Offset_Internal));
	const uint32 ValidateInfoSize =
		GetFromOffset<uint32>(
			ValidateInfoProperty,
			Offsets::ElementSize);
	const uint32 TargetOffset =
		DecryptPropOffset(GetFromOffset<uint32>(
			TargetProperty,
			Offsets::Offset_Internal));
	const uint32 TargetSize =
		GetFromOffset<uint32>(
			TargetProperty,
			Offsets::ElementSize);
	const int32 ComponentSize =
		InteractionContext->Class
			? InteractionContext->Class->
				GetPropertiesSize()
			: 0;
	const bool bSchemaValid =
		ValidateInfoSize == 0x10 &&
		ValidateInfoStruct->GetPropertiesSize() == 0x10 &&
		TargetOffset == 0 &&
		TargetSize == sizeof(AActor*) &&
		ComponentSize > 0 &&
		ValidateInfoOffset <=
			static_cast<uint32>(ComponentSize) &&
		ValidateInfoSize <=
			static_cast<uint32>(ComponentSize) -
				ValidateInfoOffset;
	if (!bSchemaValid)
	{
		SDK::DbgLog(
			"[Revive] 27.11 validate target schema rejected "
			"source=%s outerOffset=0x%X outerSize=0x%X "
			"structSize=0x%X targetOffset=0x%X "
			"targetSize=0x%X componentSize=0x%X\n",
			Source ? Source : "unknown",
			ValidateInfoOffset,
			ValidateInfoSize,
			ValidateInfoStruct->GetPropertiesSize(),
			TargetOffset,
			TargetSize,
			ComponentSize);
		return nullptr;
	}

	auto TargetAddress =
		reinterpret_cast<uint8*>(
			InteractionContext) +
		ValidateInfoOffset +
		TargetOffset;
	constexpr uint32 StartedAtTimeOffset = 0x08;
	if (!IsWritableInteractionMemory(
			TargetAddress,
			ValidateInfoSize))
	{
		SDK::DbgLog(
			"[Revive] 27.11 validate info memory "
			"rejected source=%s context=%p address=%p\n",
			Source ? Source : "unknown",
			(void*)InteractionContext,
			(void*)TargetAddress);
		return nullptr;
	}

	AActor* PreviousTarget = nullptr;
	float PreviousStartedAtWorldTime = -1.f;
	Patch.InteractionContext = InteractionContext;
	Patch.ValidateInfoOffset = ValidateInfoOffset;
	Patch.bArmed = true;
	memcpy(
		Patch.PreviousBytes,
		TargetAddress,
		ValidateInfoSize);
	memcpy(
		&PreviousTarget,
		TargetAddress,
		sizeof(PreviousTarget));
	memcpy(
		&PreviousStartedAtWorldTime,
		TargetAddress + StartedAtTimeOffset,
		sizeof(PreviousStartedAtWorldTime));
	memcpy(
		TargetAddress,
		&ReceivingActor,
		sizeof(ReceivingActor));
	memcpy(
		TargetAddress + StartedAtTimeOffset,
		&StartedAtWorldTime,
		sizeof(StartedAtWorldTime));
	AActor* VerifiedTarget = nullptr;
	float VerifiedStartedAtWorldTime = -1.f;
	memcpy(
		&VerifiedTarget,
		TargetAddress,
		sizeof(VerifiedTarget));
	memcpy(
		&VerifiedStartedAtWorldTime,
		TargetAddress + StartedAtTimeOffset,
		sizeof(VerifiedStartedAtWorldTime));
	const bool bSucceeded =
		VerifiedTarget == ReceivingActor &&
		VerifiedStartedAtWorldTime ==
			StartedAtWorldTime;
	SDK::DbgLog(
		"[Revive] 27.11 validate info prepare "
		"source=%s context=%p target=%p previous=%p "
		"started=%.3f previousStarted=%.3f "
		"offset=0x%X success=%d\n",
		Source ? Source : "unknown",
		(void*)InteractionContext,
		(void*)ReceivingActor,
		(void*)PreviousTarget,
		StartedAtWorldTime,
		PreviousStartedAtWorldTime,
		ValidateInfoOffset,
		(int)bSucceeded);
	if (!bSucceeded)
	{
		memcpy(
			TargetAddress,
			Patch.PreviousBytes,
			sizeof(Patch.PreviousBytes));
		const bool bRolledBack =
			memcmp(
				TargetAddress,
				Patch.PreviousBytes,
				sizeof(Patch.PreviousBytes)) == 0;
		SDK::DbgLog(
			"[Revive] 27.11 validate info prepare rollback "
			"source=%s context=%p success=%d\n",
			Source ? Source : "unknown",
			(void*)InteractionContext,
			(int)bRolledBack);
		Patch = {};
		return nullptr;
	}
	return TargetAddress;
}

static bool RestoreLongInteractValidateInfo27_11(
	const FLongInteractValidateInfoPatch27_11& Patch,
	const char* Source)
{
	if (VersionInfo.FortniteVersion != 27.11 ||
		!Patch.bArmed ||
		Patch.ValidateInfoOffset == uint32(-1))
	{
		return false;
	}

	auto InteractionContext =
		Patch.InteractionContext.Get();
	auto InteractionComponentClass =
		FindClass("FortControllerComponent_Interaction");
	const int32 ComponentSize =
		IsUsableDeathObject(InteractionContext) &&
			InteractionContext->Class
			? InteractionContext->Class->
				GetPropertiesSize()
			: 0;
	if (!InteractionComponentClass ||
		!IsUsableDeathObject(InteractionContext) ||
		!InteractionContext->IsA(
			InteractionComponentClass) ||
		ComponentSize <= 0 ||
		Patch.ValidateInfoOffset >
			static_cast<uint32>(ComponentSize) ||
		sizeof(Patch.PreviousBytes) >
			static_cast<uint32>(ComponentSize) -
				Patch.ValidateInfoOffset)
	{
		SDK::DbgLog(
			"[Revive] 27.11 validate info restore rejected "
			"source=%s context=%p offset=0x%X size=0x%X\n",
			Source ? Source : "unknown",
			(void*)InteractionContext,
			Patch.ValidateInfoOffset,
			ComponentSize);
		return false;
	}

	auto Address =
		reinterpret_cast<uint8*>(InteractionContext) +
		Patch.ValidateInfoOffset;
	if (!IsWritableInteractionMemory(
			Address,
			sizeof(Patch.PreviousBytes)))
	{
		return false;
	}
	memcpy(
		Address,
		Patch.PreviousBytes,
		sizeof(Patch.PreviousBytes));
	const bool bSucceeded =
		memcmp(
			Address,
			Patch.PreviousBytes,
			sizeof(Patch.PreviousBytes)) == 0;
	SDK::DbgLog(
		"[Revive] 27.11 validate info restore "
		"source=%s context=%p address=%p success=%d\n",
		Source ? Source : "unknown",
		(void*)InteractionContext,
		(void*)Address,
		(int)bSucceeded);
	return bSucceeded;
}

static void ServerNotifyStartLongUse27_11(
	UObject* Context,
	FFrame& Stack)
{
	AActor* ReceivingActor =
		Stack.Locals &&
			SDK::MemReadable(
				Stack.Locals,
				sizeof(ReceivingActor))
			? *(AActor**)Stack.Locals
			: nullptr;
	auto PlayerController =
		ResolveInteractionPlayerController(Context);

	if (GServerNotifyStartLongUse27_11OG &&
		GServerNotifyStartLongUse27_11OG !=
			ServerNotifyStartLongUse27_11)
	{
		GServerNotifyStartLongUse27_11OG(
			Context, Stack);
	}

	if (VersionInfo.FortniteVersion != 27.11 ||
		!IsUsableDeathObject(PlayerController) ||
		!IsUsableDeathObject(ReceivingActor))
	{
		return;
	}

	auto ExistingPending =
		GPendingReviveLongUses27_11.find(
			PlayerController);
	if (ExistingPending !=
		GPendingReviveLongUses27_11.end())
	{
		GPendingReviveLongUses27_11.erase(
			ExistingPending);
	}

	auto TargetPawn =
		ReceivingActor->Cast<AFortPlayerPawnAthena>();
	if (!TargetPawn ||
		!IsPawnDBNOForSpectating(TargetPawn))
	{
		return;
	}

	auto World = UWorld::GetWorld();
	const double StartedAtWorldTimeDouble =
		World
			? UGameplayStatics::GetTimeSeconds(
				World)
			: -1.0;
	const float StartedAtWorldTime =
		std::isfinite(StartedAtWorldTimeDouble) &&
			StartedAtWorldTimeDouble >= 0.0 &&
			StartedAtWorldTimeDouble <=
				static_cast<double>(
					FLT_MAX)
			? static_cast<float>(
				StartedAtWorldTimeDouble)
			: -1.f;

	FPendingReviveLongUse27_11 Pending{};
	Pending.Target = ReceivingActor;
	Pending.InteractionContext = Context;
	Pending.StartedAtMs = GetTickCount64();
	Pending.StartedAtWorldTime =
		StartedAtWorldTime;
	GPendingReviveLongUses27_11[
		PlayerController] = Pending;
	SDK::DbgLog(
		"[Revive] 27.11 long-use start reviver=%p "
		"target=%p started=%llu worldTime=%.3f\n",
		(void*)PlayerController,
		(void*)TargetPawn,
		Pending.StartedAtMs,
		Pending.StartedAtWorldTime);
}

static void ServerNotifyEndLongUse27_11(
	UObject* Context,
	FFrame& Stack)
{
	FServerNotifyEndLongUseParams27_11 Params{};
	if (Stack.Locals &&
		SDK::MemReadable(
			Stack.Locals,
			sizeof(Params)))
	{
		Params =
			*reinterpret_cast<
				const FServerNotifyEndLongUseParams27_11*>(
					Stack.Locals);
	}
	auto PlayerController =
		ResolveInteractionPlayerController(Context);

	if (GServerNotifyEndLongUse27_11OG &&
		GServerNotifyEndLongUse27_11OG !=
			ServerNotifyEndLongUse27_11)
	{
		GServerNotifyEndLongUse27_11OG(
			Context, Stack);
	}

	if (VersionInfo.FortniteVersion != 27.11 ||
		!PlayerController)
	{
		return;
	}

	auto Pending =
		GPendingReviveLongUses27_11.find(
			PlayerController);
	if (Pending ==
		GPendingReviveLongUses27_11.end())
	{
		return;
	}

	auto PendingTarget = Pending->second.Target.Get();
	if (!Params.ReceivingActor ||
		PendingTarget == Params.ReceivingActor)
	{
		SDK::DbgLog(
			"[Revive] 27.11 long-use end reviver=%p "
			"target=%p completed=%d retained=%d\n",
			(void*)PlayerController,
			(void*)Params.ReceivingActor,
			(int)Params.bUseCompleted,
			(int)Params.bUseCompleted);
		// The normal 27.11 client emits Attempt before End(true), but retain
		// completed proof briefly in case packet processing is reordered.
		// Attempt consumes it; a cancellation cannot reuse it.
		if (!Params.bUseCompleted)
		{
			GPendingReviveLongUses27_11.erase(Pending);
		}
	}
	else
	{
		GPendingReviveLongUses27_11.erase(Pending);
	}
}

static bool HasServerAttemptInteractSchema27_11(
	UFunction* Function)
{
	if (!Function ||
		Function->GetPropertiesSize() !=
			sizeof(FServerAttemptInteractParams27_11))
	{
		return false;
	}

	const auto Params = Function->GetParamsNamed();
	if (Params.Size !=
			sizeof(FServerAttemptInteractParams27_11) ||
		Params.NameOffsetMap.size() != 6)
	{
		return false;
	}

	constexpr uint64 CPF_Parm = 0x80;
	constexpr uint64 CPF_ReturnParm = 0x400;
	auto HasParam =
		[&](const char* Name, uint32 Offset, uint32 Size)
		{
			for (const auto& Param :
				Params.NameOffsetMap)
			{
				if (Param.Name == Name &&
					Param.Offset == Offset &&
					Param.ElementSize == Size &&
					(Param.PropertyFlags & CPF_Parm) &&
					!(Param.PropertyFlags &
						CPF_ReturnParm))
				{
					return true;
				}
			}
			return false;
		};

	const bool bHasRequestId =
		HasParam("RequestId", 0x24, sizeof(int32)) ||
		HasParam("RequestID", 0x24, sizeof(int32));
	return
		HasParam(
			"ReceivingActor", 0x00, sizeof(void*)) &&
		HasParam(
			"InteractComponent", 0x08, sizeof(void*)) &&
		HasParam("InteractType", 0x10, sizeof(uint8)) &&
		HasParam(
			"OptionalObjectData", 0x18, sizeof(void*)) &&
		HasParam(
			"InteractionBeingAttempted",
			0x20,
			sizeof(uint8)) &&
		bHasRequestId;
}

static bool ConsumeCompletedReviveLongUse27_11(
	AFortPlayerControllerAthena* ReviverController,
	UObject* InteractionContext,
	AActor* ReceivingActor,
	uint8 InteractionBeingAttempted,
	int32 RequestId,
	FPendingReviveLongUse27_11& ConsumedPending,
	ULONGLONG& ElapsedMs)
{
	ConsumedPending = {};
	ElapsedMs = 0;
	auto Pending =
		GPendingReviveLongUses27_11.find(
			ReviverController);
	if (Pending ==
		GPendingReviveLongUses27_11.end())
	{
		if (InteractionBeingAttempted == 0)
		{
			SDK::DbgLog(
				"[Revive] 27.11 interaction fallback "
				"missing long-use start request=%d "
				"reviver=%p target=%p\n",
				RequestId,
				(void*)ReviverController,
				(void*)ReceivingActor);
		}
		return false;
	}

	ConsumedPending = Pending->second;
	const auto PendingTarget =
		ConsumedPending.Target.Get();
	const auto PendingContext =
		ConsumedPending.InteractionContext.Get();
	const ULONGLONG StartedAtMs =
		ConsumedPending.StartedAtMs;
	GPendingReviveLongUses27_11.erase(Pending);

	const ULONGLONG NowMs = GetTickCount64();
	ElapsedMs =
		NowMs >= StartedAtMs
			? NowMs - StartedAtMs
			: 0;
	constexpr ULONGLONG MinReviveLongUseMs = 8000;
	constexpr ULONGLONG MaxReviveLongUseMs = 30000;
	const bool bValid =
		InteractionBeingAttempted == 0 &&
		PendingTarget == ReceivingActor &&
		PendingContext == InteractionContext &&
		FPlatformMath::IsFinite(
			ConsumedPending.StartedAtWorldTime) &&
		ConsumedPending.StartedAtWorldTime >= 0.f &&
		ElapsedMs >= MinReviveLongUseMs &&
		ElapsedMs <= MaxReviveLongUseMs;
	if (!bValid &&
		InteractionBeingAttempted == 0)
	{
		SDK::DbgLog(
			"[Revive] 27.11 interaction fallback "
			"rejected long-use proof request=%d "
			"reviver=%p target=%p recorded=%p "
			"context=%p recordedContext=%p "
			"worldTime=%.3f elapsed=%llu\n",
			RequestId,
			(void*)ReviverController,
			(void*)ReceivingActor,
			(void*)PendingTarget,
			(void*)InteractionContext,
			(void*)PendingContext,
			ConsumedPending.StartedAtWorldTime,
			ElapsedMs);
	}
	return bValid;
}

static bool ValidateReviveInteraction27_11(
	UObject* InteractionContext,
	AFortPlayerControllerAthena* ReviverController,
	AActor* ReceivingActor,
	uint8 InteractType,
	uint8 InteractionBeingAttempted,
	int32 RequestId,
	FPendingReviveLongUse27_11& ConsumedPending)
{
	ConsumedPending = {};
	// This repair is intentionally tied to the exact 27.11 RPC ABI. On that
	// build, the native long-use validator loses ValidateInfo.Target even
	// though ServerAttemptInteract still contains the exact ReceivingActor.
	// Fully authorize that request here; the caller then scopes either the
	// native validation switch or the reflected target repair to the single
	// synchronous native attempt.
	if (VersionInfo.FortniteVersion != 27.11 ||
		InteractType != 2 || // ETInteractionType::LongPress
		!IsUsableDeathObject(ReviverController) ||
		!IsUsableDeathObject(ReceivingActor))
	{
		return false;
	}

	ULONGLONG LongUseElapsedMs = 0;
	if (!ConsumeCompletedReviveLongUse27_11(
			ReviverController,
			InteractionContext,
			ReceivingActor,
			InteractionBeingAttempted,
			RequestId,
			ConsumedPending,
			LongUseElapsedMs))
	{
		return false;
	}

	auto TargetPawn =
		ReceivingActor->Cast<AFortPlayerPawnAthena>();
	if (!TargetPawn ||
		!IsPawnDBNOForSpectating(TargetPawn))
	{
		return false;
	}

	auto ReviverPawn =
		ResolveVehicleRiderPawn(ReviverController);
	auto TargetController =
		TargetPawn->Controller
			? TargetPawn->Controller->Cast<
				AFortPlayerControllerAthena>()
			: nullptr;
	if (!IsUsableDeathObject(ReviverPawn) ||
		!IsUsableDeathObject(TargetController) ||
		ReviverPawn == TargetPawn ||
		!ReviverController->HasAuthority() ||
		!ReviverPawn->HasAuthority() ||
		!TargetPawn->HasAuthority() ||
		(AActor*)ReviverController->Pawn !=
			(AActor*)ReviverPawn ||
		(ReviverController->HasMyFortPawn() &&
			ReviverController->MyFortPawn !=
				ReviverPawn) ||
		ReviverPawn->Controller != ReviverController ||
		IsPawnDBNOForSpectating(ReviverPawn) ||
		(ReviverPawn->HasbIsDying() &&
			ReviverPawn->bIsDying) ||
		(TargetPawn->HasbIsDying() &&
			TargetPawn->bIsDying))
	{
		SDK::DbgLog(
			"[Revive] 27.11 interaction fallback rejected "
			"actor state request=%d reviver=%p pawn=%p "
			"target=%p targetController=%p\n",
			RequestId,
			(void*)ReviverController,
			(void*)ReviverPawn,
			(void*)TargetPawn,
			(void*)TargetController);
		return false;
	}

	const float ReviverHealth = ReviverPawn->GetHealth();
	if (!FPlatformMath::IsFinite(ReviverHealth) ||
		ReviverHealth <= 0.f ||
		!ReviverController->HasPlayerState() ||
		!TargetController->HasPlayerState() ||
		!IsUsableDeathObject(
			ReviverController->PlayerState) ||
		!IsUsableDeathObject(
			TargetController->PlayerState))
	{
		SDK::DbgLog(
			"[Revive] 27.11 interaction fallback rejected "
			"health/ownership request=%d health=%.2f\n",
			RequestId,
			ReviverHealth);
		return false;
	}

	auto ReviverPlayerState =
		ReviverController->PlayerState->Cast<
			AFortPlayerStateAthena>();
	auto TargetPlayerState =
		TargetController->PlayerState->Cast<
			AFortPlayerStateAthena>();
	constexpr uint8 InvalidTeam = uint8(-1);
	constexpr uint8 FirstPlayableTeam = 3;
	constexpr uint8 FirstReservedTeam = 250;
	const bool bSameValidTeam =
		ReviverPlayerState &&
		TargetPlayerState &&
		ReviverPlayerState->HasTeamIndex() &&
		TargetPlayerState->HasTeamIndex() &&
		ReviverPlayerState->TeamIndex >=
			FirstPlayableTeam &&
		ReviverPlayerState->TeamIndex <
			FirstReservedTeam &&
		ReviverPlayerState->TeamIndex ==
			TargetPlayerState->TeamIndex;
	const bool bSameValidSquad =
		!bSameValidTeam
			? false
			: (!ReviverPlayerState->HasSquadId() &&
					!TargetPlayerState->HasSquadId()) ||
				(ReviverPlayerState->HasSquadId() &&
					TargetPlayerState->HasSquadId() &&
					ReviverPlayerState->SquadId != InvalidTeam &&
					ReviverPlayerState->SquadId ==
						TargetPlayerState->SquadId);
	if (!bSameValidTeam || !bSameValidSquad)
	{
		SDK::DbgLog(
			"[Revive] 27.11 interaction fallback rejected "
			"team request=%d reviverState=%p targetState=%p "
			"sameTeam=%d sameSquad=%d\n",
			RequestId,
			(void*)ReviverPlayerState,
			(void*)TargetPlayerState,
			(int)bSameValidTeam,
			(int)bSameValidSquad);
		return false;
	}

	const auto Delta =
		ReviverPawn->K2_GetActorLocation() -
		TargetPawn->K2_GetActorLocation();
	const auto DistanceSquared = Delta.SizeSquared();
	constexpr double MaxReviveDistance = 500.0;
	if (!std::isfinite(
			static_cast<double>(DistanceSquared)) ||
		DistanceSquared >
			MaxReviveDistance * MaxReviveDistance)
	{
		SDK::DbgLog(
			"[Revive] 27.11 interaction fallback rejected "
			"distance request=%d distanceSquared=%.2f\n",
			RequestId,
			(double)DistanceSquared);
		return false;
	}

	SDK::DbgLog(
		"[Revive] 27.11 validated native long interaction "
		"request=%d reviver=%p target=%p elapsed=%llu "
		"distanceSquared=%.2f\n",
		RequestId,
		(void*)ReviverController,
		(void*)TargetPawn,
		LongUseElapsedMs,
		(double)DistanceSquared);
	return true;
}

void AFortPlayerControllerAthena::ServerAttemptInteract_(UObject* Context, FFrame& Stack)
{
	// The weapon-mod runtime package can stream in after the global post-load
	// pass. Interaction is a natural, non-ticking retry point before a player
	// can submit a workbench purchase.
	FFortWeaponMods::EnsureBenchHooks();

	AActor* ReceivingActor =
		Stack.Locals &&
			SDK::MemReadable(
				Stack.Locals,
				sizeof(ReceivingActor))
			? *(AActor**)Stack.Locals
			: nullptr;
	uint8 InteractType27_11 = uint8(-1);
	uint8 InteractionBeingAttempted27_11 = uint8(-1);
	int32 RequestId27_11 = -1;
	if (VersionInfo.FortniteVersion == 27.11 &&
		GServerAttemptInteractSchema27_11 &&
		Stack.Locals &&
		SDK::MemReadable(
			Stack.Locals,
			sizeof(FServerAttemptInteractParams27_11)))
	{
		const auto Params =
			reinterpret_cast<
				const FServerAttemptInteractParams27_11*>(
					Stack.Locals);
		ReceivingActor = Params->ReceivingActor;
		InteractType27_11 = Params->InteractType;
		InteractionBeingAttempted27_11 =
			Params->InteractionBeingAttempted;
		RequestId27_11 = Params->RequestId;
	}
	/*AActor* ReceivingActor;
	UObject* InteractComponent;
	uint8_t InteractType;
	UObject* OptionalObjectData;
	uint8_t InteractionBeingAttempted;
	int32 RequestID;

	Stack.StepCompiledIn(&ReceivingActor);
	Stack.StepCompiledIn(&InteractComponent);
	Stack.StepCompiledIn(&InteractType);
	Stack.StepCompiledIn(&OptionalObjectData);
	Stack.StepCompiledIn(&InteractionBeingAttempted);
	Stack.StepCompiledIn(&RequestID);
	Stack.IncrementCode();*/

	auto PlayerController =
		ResolveInteractionPlayerController(Context);
	if (!PlayerController)
	{
		if (ServerAttemptInteract_OG &&
			ServerAttemptInteract_OG !=
				ServerAttemptInteract_)
		{
			return ServerAttemptInteract_OG(
				Context, Stack);
		}
		return;
	}

	auto Pawn = (AFortPlayerPawnAthena*)PlayerController->Pawn;
	auto CollectingPawn =
		PlayerController->MyFortPawn
			? PlayerController->MyFortPawn
			: Pawn;
	auto InteractingPickup =
		ReceivingActor
			? ReceivingActor->Cast<AFortPickupAthena>()
			: nullptr;
	static const UClass* GameModePickupClass1040 = nullptr;
	if (!GameModePickupClass1040)
		GameModePickupClass1040 =
			FindClass("FortGameModePickup");
	if (std::fabs(
			VersionInfo.FortniteVersion - 10.40) < 0.001 &&
		ReceivingActor &&
		GameModePickupClass1040 &&
		ReceivingActor->IsA(GameModePickupClass1040) &&
		InteractingPickup &&
		FFortAthenaNativeLTMCompatibility::
			ShouldRejectAshtonPickup(
				CollectingPawn,
				InteractingPickup
					->PrimaryPickupItemEntry
					.ItemDefinition))
	{
		// The 10.40 long-use RPC has exactly four parameters. Consume it
		// locally and do not enter the native interaction path: otherwise the
		// Ashton pickup blueprint can mark the stone captured before any of
		// the ordinary inventory/pickup guards get a chance to reject it.
		AActor* ParsedReceivingActor = nullptr;
		UObject* InteractComponent = nullptr;
		uint8 InteractType = 0;
		UObject* OptionalObjectData = nullptr;
		Stack.StepCompiledIn(&ParsedReceivingActor);
		Stack.StepCompiledIn(&InteractComponent);
		Stack.StepCompiledIn(&InteractType);
		Stack.StepCompiledIn(&OptionalObjectData);
		Stack.IncrementCode();

		SDK::DbgLog(
			"[Ashton1040] rejected non-villain stone "
			"interaction type=%u actor=%p parsed=%p item=%p\n",
			static_cast<unsigned>(InteractType),
			static_cast<void*>(ReceivingActor),
			static_cast<void*>(ParsedReceivingActor),
			static_cast<const void*>(
				InteractingPickup
					->PrimaryPickupItemEntry
					.ItemDefinition));
		return;
	}
	auto AshtonStoneDefinition =
		InteractingPickup
			? InteractingPickup
				  ->PrimaryPickupItemEntry
				  .ItemDefinition
			: nullptr;
	if (std::fabs(
			VersionInfo.FortniteVersion - 10.40) < 0.001 &&
		ReceivingActor &&
		GameModePickupClass1040 &&
		ReceivingActor->IsA(GameModePickupClass1040) &&
		InteractingPickup &&
		FFortAthenaNativeLTMCompatibility::
			IsCurrentAshtonStone(
				CollectingPawn,
				AshtonStoneDefinition))
	{
		// The stock game-mode interaction remains the authority for the
		// authored long-use timer and distance checks. Remember the collector
		// on the objective actor so its native destroy callback can select the
		// exact Chitauri that completed the interaction.
		const bool bWasAlreadyCaptured =
			FFortAthenaNativeLTMCompatibility::
				IsAshtonStoneCaptured(
					AshtonStoneDefinition);
		InteractingPickup
			->PickupLocationData.PickupTarget =
			CollectingPawn;
		ServerAttemptInteract_OG(Context, Stack);

		const bool bPickupStillUsable =
			IsUsableDeathObject(InteractingPickup);
		const bool bNativeConsumedPickup =
			!bPickupStillUsable ||
			(bPickupStillUsable &&
			 InteractingPickup
				 ->HasbActorIsBeingDestroyed() &&
			 InteractingPickup
				 ->bActorIsBeingDestroyed) ||
			(bPickupStillUsable &&
			 InteractingPickup->HasbPickedUp() &&
			 InteractingPickup->bPickedUp);
		const bool bNativeCapturedStone =
			FFortAthenaNativeLTMCompatibility::
				IsAshtonStoneCaptured(
					AshtonStoneDefinition);

		// A live, unclaimed actor with an unchanged row means native rejected
		// or has not completed the long use. Do not advance objective state
		// from the RPC alone; the validated world-pickup spline completes it.
		if (bWasAlreadyCaptured ||
			bNativeCapturedStone ||
			bNativeConsumedPickup)
		{
			auto CompletionPawn =
				IsUsableDeathObject(CollectingPawn)
					? CollectingPawn
					: PlayerController->MyFortPawn;
			FFortAthenaNativeLTMCompatibility::
				TryCompleteAshtonStonePickup(
					CompletionPawn,
					bPickupStillUsable
						? InteractingPickup
						: nullptr,
					AshtonStoneDefinition,
					"long-use-native-result");
		}
		return;
	}
	const UClass* WaxPickupClass =
		AFortGameModePickup_Wax::StaticClass();
	if (std::fabs(
			VersionInfo.FortniteVersion - 10.40) < 0.001 &&
		ReceivingActor &&
		WaxPickupClass &&
		ReceivingActor->IsA(WaxPickupClass))
	{
		// The 10.40 coin is a zero-duration simple Use interaction. Consume
		// the four-parameter component RPC before routing it to the private
		// Wax scorer; the stripped interaction callback never reaches that
		// scorer on its own.
		AActor* ParsedReceivingActor = nullptr;
		UObject* InteractComponent = nullptr;
		uint8 InteractType = 0;
		UObject* OptionalObjectData = nullptr;
		Stack.StepCompiledIn(&ParsedReceivingActor);
		Stack.StepCompiledIn(&InteractComponent);
		Stack.StepCompiledIn(&InteractType);
		Stack.StepCompiledIn(&OptionalObjectData);
		Stack.IncrementCode();

		if (InteractType != 1 ||
			ParsedReceivingActor != ReceivingActor)
		{
			SDK::DbgLog(
				"[Wax1040] rejected malformed coin interaction "
				"type=%u actor=%p parsed=%p\n",
				static_cast<unsigned>(InteractType),
				static_cast<void*>(ReceivingActor),
				static_cast<void*>(
					ParsedReceivingActor));
			return;
		}

		auto WaxPickup =
			ParsedReceivingActor &&
					ParsedReceivingActor->IsA(
						WaxPickupClass)
				? ParsedReceivingActor
					  ->Cast<AFortPickupAthena>()
				: nullptr;
		FFortAthenaNativeLTMCompatibility::
			TryCollectWaxPickup(
				CollectingPawn,
				WaxPickup);
		return;
	}
	auto Vehicle = ReceivingActor
		? ReceivingActor->Cast<AFortAthenaVehicle>() : nullptr;
	auto CharacterVehicle = ReceivingActor
		? ReceivingActor->Cast<AFortCharacterVehicle>() : nullptr;

	auto sendStat = [&]()
	{
		if (!ReceivingActor)
			return;

		FGameplayTagContainer TargetTags{};
		
		auto Interface = (IGameplayTagAssetInterface*)ReceivingActor->GetInterface(IGameplayTagAssetInterface::StaticClass());
		if (Interface)
		{
			auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface->Vft[0x2];
			GetOwnedGameplayTags(Interface, &TargetTags);
			//Interface->GetOwnedGameplayTags(&TargetTags);
		}

		PlayerController->GetQuestManager(1)->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetInteract(), 1, false, ReceivingActor, TargetTags);

		//TargetTags.GameplayTags.Free();
		//TargetTags.ParentTags.Free();
	};

	if (auto Container = bDidntFind ? ReceivingActor->Cast<ABuildingContainer>() : nullptr)
		UFortLootPackage::SpawnLootHook(Container);
	else if (Vehicle || CharacterVehicle)
	{
		auto RiderPawn = PlayerController->MyFortPawn
			? PlayerController->MyFortPawn : Pawn;
		auto TrackedLoadout =
			CaptureVehicleLoadout(PlayerController, RiderPawn);
		auto InventoryBeforeEntry = SnapshotVehicleInventoryGuids(
			PlayerController->WorldInventory);

		ServerAttemptInteract_OG(Context, Stack);
		sendStat();

		auto VehicleActor = Vehicle
			? (AActor*)Vehicle : (AActor*)CharacterVehicle;
		auto SeatComponent =
			(UFortVehicleSeatComponent*)
			VehicleActor->GetComponentByClass(
				UFortVehicleSeatComponent::StaticClass());

		int32 SeatIdx = SeatComponent && RiderPawn
			? SeatComponent->FindSeatIndex(RiderPawn) : -1;
		auto SeatWeaponComponent =
			ResolveVehicleSeatWeaponComponent(
				VehicleActor, SeatIdx);
		const bool bPossessedVehicle =
			(AActor*)PlayerController->Pawn == VehicleActor;
		const bool bEnteredVehicle =
			bPossessedVehicle || SeatIdx >= 0;

		if (!bEnteredVehicle)
			return;

		GVehiclePossessionVehicle[PlayerController] =
			VehicleActor;

		// Save the exact live rider while it is still known here. Some legacy
		// CharacterVehicle builds update MyFortPawn before the later possession
		// acknowledgement, so that acknowledgement alone cannot always discover
		// which character native exit must return to.
		if (bPossessedVehicle &&
			IsLiveRemoteControlReturnPawn(RiderPawn))
		{
			GVehiclePossessionReturnPawn[PlayerController] =
				RiderPawn;
		}

		TrackNewVehicleItems(
			TrackedLoadout,
			PlayerController->WorldInventory,
			InventoryBeforeEntry);
		GTrackedVehicleLoadouts[PlayerController] =
			std::move(TrackedLoadout);

		if (SeatIdx < 0 || !SeatWeaponComponent ||
			!PlayerController->WorldInventory || !RiderPawn)
		{
			return;
		}

		// The CharacterVehicle ability set drives the mech itself, but the
		// dedicated server must still grant/configure the Ostrich ranged weapon
		// when interaction places this rider directly in the gunner seat.
		for (int32 Index = 0;
			Index < SeatWeaponComponent->WeaponSeatDefinitions.Num();
			++Index)
		{
			auto& WeaponDefinition =
				SeatWeaponComponent->WeaponSeatDefinitions.Get(
					Index, FWeaponSeatDefinition::Size());
			if (WeaponDefinition.SeatIndex != SeatIdx)
				continue;

			ConfigureVehicleSeatWeapon(
				PlayerController,
				RiderPawn,
				VehicleActor,
				SeatWeaponComponent,
				WeaponDefinition,
				SeatIdx,
				RiderPawn->CurrentWeapon);
			break;
		}
		return;
	}
	else if (auto CollectorActor = ReceivingActor->Cast<ABuildingItemCollectorActor>())
	{
		CollectorActor->ControllingPlayer = PlayerController;


		auto Collection = CollectorActor->ItemCollections.Search([&](FCollectorUnitInfo& Coll)
			{
				return Coll.InputItem == CollectorActor->ActiveInputItem;
			}, FCollectorUnitInfo::Size());

		if (!Collection)
		{
			CollectorActor->bCurrentInteractionSuccess = false;
			CollectorActor->ControllingPlayer = nullptr;
			CollectorActor->Call(CollectorActor->GetFunction("BlueprintOnInteract"), PlayerController->MyFortPawn);
			ServerAttemptInteract_OG(Context, Stack);
			sendStat();
			return;
		}

		float Cost = Collection->InputCount.Evaluate((float)CollectorActor->StartingGoalLevel);
		if (Cost > 0)
		{
			auto ItemP = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
				{
					return entry->ItemEntry.ItemDefinition == Collection->InputItem;
				});

			if (!ItemP || (*ItemP)->ItemEntry.Count < (int)Cost)
			{
				CollectorActor->bCurrentInteractionSuccess = false;
				CollectorActor->ControllingPlayer = nullptr;
				CollectorActor->Call(CollectorActor->GetFunction("BlueprintOnInteract"), Pawn);
				//CollectorActor->PlayVendFailFX();
				ServerAttemptInteract_OG(Context, Stack);
				sendStat();
				return;
			}

			auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
				{
					return entry.ItemDefinition == Collection->InputItem;
				}, FFortItemEntry::Size());
			auto Item = *ItemP;

			/*for (int i = 0; i < itemEntry->StateValues.Num(); i++)
			{
				auto& StateValue = itemEntry->StateValues.Get(i, FFortItemEntryStateValue::Size());

				if (StateValue.StateType != 2)
					continue;

				StateValue.IntValue = 0;
			}*/

			itemEntry->Count -= (int)Cost;
			if (itemEntry->Count <= 0)
				PlayerController->WorldInventory->Remove(itemEntry->ItemGuid);
			else
			{
				/*for (int i = 0; i < itemEntry->StateValues.Num(); i++)
				{
					auto& StateValue = itemEntry->StateValues.Get(i, FFortItemEntryStateValue::Size());

					if (StateValue.StateType != 2)
						continue;

					StateValue.IntValue = 0;
					break;
				}*/

				Item->ItemEntry.Count = itemEntry->Count;
				PlayerController->WorldInventory->UpdateEntry(*itemEntry);
				Item->ItemEntry.bIsDirty = true;
			}
		}

		CollectorActor->ClientPausedActiveInputItem = CollectorActor->ActiveInputItem;
		CollectorActor->bCurrentInteractionSuccess = true;
		CollectorActor->Call(CollectorActor->GetFunction("BlueprintOnInteract"), Pawn);
		//CollectorActor->PlayVendFX();
	}
	else if (auto LockDevice = ReceivingActor->Cast<ABuildingProp_LockDevice>())
	{
		printf("yo %s\n", LockDevice->LockableObject->Name.ToString().c_str());
		LockDevice->CurrentLockState = 1;
		LockDevice->OnRep_CurrentLockState();
	}

	FPendingReviveLongUse27_11
		ConsumedReviveLongUse27_11{};
	const bool bValidatedReviveAttempt27_11 =
		ValidateReviveInteraction27_11(
			Context,
			PlayerController,
			ReceivingActor,
			InteractType27_11,
			InteractionBeingAttempted27_11,
			RequestId27_11,
			ConsumedReviveLongUse27_11);
	FLongInteractValidateInfoPatch27_11
		ValidateInfoPatch27_11{};
	const bool bValidateInfoPrepared27_11 =
		bValidatedReviveAttempt27_11 &&
		PrepareLongInteractValidateInfo27_11(
			Context,
			ReceivingActor,
			ConsumedReviveLongUse27_11.
				StartedAtWorldTime,
			ValidateInfoPatch27_11,
			"attempt") != nullptr;
	uint8 PreviousValidateLongValue27_11 = uint8(-1);
	bool bScopedValidationEnabled27_11 = false;
	bool bScopedValidationWriteAttempted27_11 = false;
	if (bValidateInfoPrepared27_11 &&
		GInteractValidateLongEnable27_11 &&
		IsWritableInteractionMemory(
			GInteractValidateLongEnable27_11,
			sizeof(uint8)))
	{
		memcpy(
			&PreviousValidateLongValue27_11,
			GInteractValidateLongEnable27_11,
			sizeof(uint8));
		if (PreviousValidateLongValue27_11 <= 1)
		{
			const uint8 Enabled = 1;
			bScopedValidationWriteAttempted27_11 = true;
			memcpy(
				GInteractValidateLongEnable27_11,
				&Enabled,
				sizeof(Enabled));
			uint8 VerifiedValue = uint8(-1);
			memcpy(
				&VerifiedValue,
				GInteractValidateLongEnable27_11,
				sizeof(VerifiedValue));
			bScopedValidationEnabled27_11 =
				VerifiedValue == 1;
			SDK::DbgLog(
				"[Revive] 27.11 native validation enabled "
				"request=%d cvar=%p previous=%u active=%d "
				"infoPrepared=%d\n",
				RequestId27_11,
				(void*)GInteractValidateLongEnable27_11,
				PreviousValidateLongValue27_11,
				(int)bScopedValidationEnabled27_11,
				(int)bValidateInfoPrepared27_11);
		}
	}
	ServerAttemptInteract_OG(Context, Stack);
	if (bScopedValidationWriteAttempted27_11)
	{
		memcpy(
			GInteractValidateLongEnable27_11,
			&PreviousValidateLongValue27_11,
			sizeof(PreviousValidateLongValue27_11));
		uint8 RestoredValue = uint8(-1);
		memcpy(
			&RestoredValue,
			GInteractValidateLongEnable27_11,
			sizeof(RestoredValue));
		SDK::DbgLog(
			"[Revive] 27.11 validation value restore "
			"request=%d cvar=%p value=%u success=%d\n",
			RequestId27_11,
			(void*)GInteractValidateLongEnable27_11,
			RestoredValue,
			(int)(RestoredValue ==
				PreviousValidateLongValue27_11));
	}
	bool bValidateInfoRestored27_11 = false;
	if (bValidateInfoPrepared27_11)
	{
		bValidateInfoRestored27_11 =
			RestoreLongInteractValidateInfo27_11(
				ValidateInfoPatch27_11,
				"attempt");
	}
	sendStat();
	if (bValidatedReviveAttempt27_11)
	{
		auto TargetPawn =
			IsUsableDeathObject(ReceivingActor)
				? ReceivingActor->Cast<
					AFortPlayerPawnAthena>()
				: nullptr;
		bool bStillDBNO =
			TargetPawn &&
			IsPawnDBNOForSpectating(TargetPawn);
		float TargetHealth =
			TargetPawn
				? TargetPawn->GetHealth()
				: 0.f;
		const bool bNativeSucceeded =
			TargetPawn && !bStillDBNO &&
			FPlatformMath::IsFinite(TargetHealth) &&
			TargetHealth > 0.f;
		bool bFallbackAttempted = false;
		bool bFallbackSucceeded = false;
		if (TargetPawn && bStillDBNO)
		{
			bFallbackAttempted = true;
			bFallbackSucceeded =
				AFortPlayerPawnAthena::
					ReviveFromDBNOCompat(
						TargetPawn,
						reinterpret_cast<AController*>(
							PlayerController));
			if (IsUsableDeathObject(TargetPawn))
			{
				bStillDBNO =
					IsPawnDBNOForSpectating(
						TargetPawn);
				TargetHealth =
					TargetPawn->GetHealth();
			}
		}
		const bool bFinalSucceeded =
			TargetPawn && !bStillDBNO &&
			FPlatformMath::IsFinite(TargetHealth) &&
			TargetHealth > 0.f;
		SDK::DbgLog(
			"[Revive] 27.11 interaction result "
			"request=%d target=%p stillDBNO=%d "
			"health=%.2f prepared=%d restored=%d "
			"validationEnabled=%d native=%d "
			"fallback=%d/%d success=%d\n",
			RequestId27_11,
			(void*)TargetPawn,
			(int)bStillDBNO,
			TargetHealth,
			(int)bValidateInfoPrepared27_11,
			(int)bValidateInfoRestored27_11,
			(int)bScopedValidationEnabled27_11,
			(int)bNativeSucceeded,
			(int)bFallbackAttempted,
			(int)bFallbackSucceeded,
			(int)bFinalSucceeded);
	}
}

void AFortPlayerControllerAthena::ServerDropAllItems(UObject* Context, FFrame& Stack)
{
	auto PlayerController =
		(AFortPlayerControllerAthena*)Context;
	auto World = UWorld::GetWorld();
	auto GameMode =
		World
			? (AFortGameMode*)World->AuthorityGameMode
			: nullptr;
	auto GameState =
		GameMode
			? (AFortGameStateAthena*)GameMode->GameState
			: nullptr;
	const bool bIsNative1040LTM =
		IsCurrentNative1040LTM(GameState);
	const bool bIsGetawayMatch =
		IsCurrentGetawayMatch(GameState);
	const bool bKeepInventoryApplies =
		FConfiguration::bKeepInventory &&
		(!bIsNative1040LTM || bIsGetawayMatch);

	// Food Fight, Arsenal and Wax/Bounty have native death inventory
	// mutators. Getaway also owns its normal full-drop path. Preserve those
	// implementations unless the explicit Keep Inventory policy needs the
	// Jewel-only exception.
	if (bIsNative1040LTM &&
		(!bIsGetawayMatch || !bKeepInventoryApplies) &&
		ServerDropAllItemsOG)
	{
		ServerDropAllItemsOG(Context, Stack);
		return;
	}

	UFortItemDefinition* IgnoreItemDef;

	Stack.StepCompiledIn(&IgnoreItemDef);
	Stack.IncrementCode();
	printf(
		"ServerDropAllItems[Ignore %s]\n",
		IgnoreItemDef
			? IgnoreItemDef->Name.ToString().c_str()
			: "none");

	if (!IsUsableDeathObject(PlayerController) ||
		!PlayerController->WorldInventory ||
		!IsUsableDeathObject(PlayerController->MyFortPawn))
	{
		return;
	}

	auto Loc = PlayerController->MyFortPawn->K2_GetActorLocation();
	for (int i =
		PlayerController->WorldInventory->Inventory
			.ReplicatedEntries.Num() - 1;
		i >= 0;
		--i)
	{
		auto& Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());
		const bool bIsGetawayJewel =
			bIsGetawayMatch &&
			IsGetawayJewelDefinition(
				Entry.ItemDefinition);

		// Keep Inventory is authoritative for ordinary inventory, but the
		// Getaway objective is always transferable on death. Food Fight,
		// Arsenal and Wax/Bounty retain their native inventory ownership.
		if (bKeepInventoryApplies && !bIsGetawayJewel)
			continue;

		if ((bIsGetawayJewel ||
				Entry.ItemDefinition != IgnoreItemDef) &&
			CanDropInventoryItem(Entry.ItemDefinition))
		{
			auto DropLocation = Loc;
			if (bIsGetawayJewel)
				DropLocation.Z += 50.f;
			auto Pickup = AFortInventory::SpawnPickup(
				DropLocation, Entry,
				EFortPickupSourceTypeFlag::GetPlayer(),
				EFortPickupSpawnSource::GetUnset(),
				PlayerController->MyFortPawn);
			if (!Pickup && bIsGetawayJewel)
			{
				DropLocation.Z += 50.f;
				Pickup = AFortInventory::SpawnPickup(
					DropLocation, Entry,
					EFortPickupSourceTypeFlag::GetPlayer(),
					EFortPickupSpawnSource::GetUnset(),
					PlayerController->MyFortPawn);
			}
			// Never delete an entry unless its world representation exists.
			// This is essential for the Jewel: losing both copies makes
			// Getaway unwinnable.
			if (Pickup)
				PlayerController->WorldInventory->Remove(
					Entry.ItemGuid);
		}
	}
}


void (*OnUnEquipOG)(AFortWeapon*);
void OnUnEquip(AFortWeapon* Weapon)
{
	AFortInventory::RemoveWeaponAbilities(Weapon);

	return OnUnEquipOG(Weapon);
}

class UFortHeldObjectComponent : public UActorComponent
{
public:
	UCLASS_COMMON_MEMBERS(UFortHeldObjectComponent);

	DEFINE_PROP(EquippedWeaponItemDefinition, TSoftObjectPtr<UFortItemDefinition>);
	DEFINE_PROP(HeldObjectState, uint8);
	DEFINE_PROP(OwningPawn, AFortPlayerPawnAthena*);
	DEFINE_PROP(PreviousOwningPawn, TWeakObjectPtr<AFortPlayerPawnAthena>);
	DEFINE_PROP(GrantedWeaponItem, TWeakObjectPtr<UFortWorldItem>);
	DEFINE_PROP(GrantedWeapon, TWeakObjectPtr<AFortWeapon>);
	DEFINE_PROP(OnHeldObjectOwningPawnChanged, TMulticastInlineDelegate<void()>);
	DEFINE_PROP(OnHeldObjectPickedUp, TMulticastInlineDelegate<void()>);
	DEFINE_PROP(OnHeldObjectDropped, TMulticastInlineDelegate<void()>);

	DEFINE_FUNC(OnRep_OwningPawn, void);
};


void SetHeldObject(AFortPlayerPawnAthena* Pawn, AActor* HeldObject)
{
	auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;

	Pawn->HeldObject = HeldObject;
	PlayerController->bHoldingObject = HeldObject != nullptr;
}

void SetupOwningPawn(UFortHeldObjectComponent* HeldObjectComponent, AFortPlayerPawnAthena* Pawn)
{
	auto HeldObject = (AActor*)HeldObjectComponent->GetOwner();

	if (!HeldObject)
		return;

	HeldObject->FlushNetDormancy();

	if (Pawn)
		HeldObjectComponent->HeldObjectState = 1;

	AFortPlayerPawnAthena* PreviousPawn = nullptr;
	if (HeldObjectComponent->HasOwningPawn())
	{
		auto OldPawn = HeldObjectComponent->OwningPawn;
		HeldObjectComponent->PreviousOwningPawn = !Pawn ? HeldObjectComponent->OwningPawn : nullptr;
		HeldObjectComponent->OwningPawn = Pawn;
		//HeldObjectComponent->OnRep_OwningPawn(OldPawn);
		PreviousPawn = OldPawn;
	}
	else
	{
		// its a weakobjectptr on older builds
		static auto OwningPawnOff = HeldObjectComponent->GetOffset("OwningPawn", 0x8000000);
		auto& OwningPawn = *(TWeakObjectPtr<AFortPlayerPawnAthena>*)(__int64(HeldObjectComponent) + OwningPawnOff);

		HeldObjectComponent->PreviousOwningPawn = !Pawn ? OwningPawn.Get() : nullptr;
		auto OldPawn = OwningPawn.Get();
		OwningPawn = Pawn;
		//HeldObjectComponent->OnRep_OwningPawn(OldPawn);
		PreviousPawn = OldPawn;
	}

	if (Pawn)
		SetHeldObject(Pawn, HeldObject);

	HeldObject->ForceNetUpdate();
	if (HeldObjectComponent->HasOnHeldObjectOwningPawnChanged())
		HeldObjectComponent->OnHeldObjectOwningPawnChanged.Process();
	if (Pawn->HasOnHeldObjectPickedUp())
	{
		if (Pawn)
			Pawn->OnHeldObjectPickedUp.Process(HeldObject);
		else
			PreviousPawn->OnHeldObjectDropped.Process(HeldObject);
	}
}

void PickupHeldObject(UObject* Context, FFrame& Stack)
{
	AFortPlayerPawnAthena* Pawn;

	Stack.StepCompiledIn(&Pawn);
	Stack.IncrementCode();
	auto HeldObjectComponent = (UFortHeldObjectComponent*)Context;

	auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;

	SetupOwningPawn(HeldObjectComponent, Pawn);

	if (!HeldObjectComponent->GrantedWeaponItem.Get())
	{
		auto ItemDefinition = HeldObjectComponent->EquippedWeaponItemDefinition.Get();

		auto Item = PlayerController->WorldInventory->GiveItem(ItemDefinition, 1, 99999);

		if (!Item)
			return;

		auto Weapon = (AFortWeapon*)Pawn->EquipWeaponDefinition(ItemDefinition, Item->ItemEntry.ItemGuid, FFortItemEntry::HasTrackerGuid() ? Item->ItemEntry.TrackerGuid : FGuid(), false);

		if (!Weapon)
			return;
		FFortWeaponMods::ApplyEntrySlotsAfterEquip(
			Weapon, Item->ItemEntry);
		PlayerController->ClientEquipItem(Item->ItemEntry.ItemGuid, true);

		HeldObjectComponent->GrantedWeapon = Weapon;
		HeldObjectComponent->GrantedWeaponItem = Item;
	}
	HeldObjectComponent->OnHeldObjectPickedUp.Process();
}


void PlaceHeldObject(UObject* Context, FFrame& Stack)
{
	Stack.IncrementCode();
	printf("PlaceHeldObject\n");
}

void ThrowHeldObject(UObject* Context, FFrame& Stack)
{
	FVector DetachLocation;
	FRotator ThrowDirection;

	Stack.StepCompiledIn(&DetachLocation);
	Stack.StepCompiledIn(&ThrowDirection);
	Stack.IncrementCode();
	printf("ThrowHeldObject\n");
}

void DropHeldObject(UObject* Context, FFrame& Stack)
{
	Stack.IncrementCode();
	printf("DropHeldObject\n");
	auto HeldObjectComponent = (UFortHeldObjectComponent*)Context;

	AFortPlayerPawnAthena* OwningPawn = nullptr;

	if (HeldObjectComponent->HasOwningPawn())
		OwningPawn = HeldObjectComponent->OwningPawn;
	else
	{
		// its a weakobjectptr on older builds
		static auto OwningPawnOff = HeldObjectComponent->GetOffset("OwningPawn", 0x8000000);
		OwningPawn = (*(TWeakObjectPtr<AFortPlayerPawnAthena>*)(__int64(HeldObjectComponent) + OwningPawnOff)).Get();
	}
	if (!OwningPawn)
		return;


	auto PlayerController = (AFortPlayerControllerAthena*)OwningPawn->Controller;

	/*auto Item = HeldObjectComponent->GrantedWeaponItem.Get();

	if (!Item)HeldObjectComponent->GrantedWeaponItem
		return;*/
		//printf("GUID: %d %d %d %d, ID: %s\n", Item->ItemEntry.ItemGuid.A, Item->ItemEntry.ItemGuid.B, Item->ItemEntry.ItemGuid.C, Item->ItemEntry.ItemGuid.D, Item->ItemEntry.ItemDefinition->Name.ToString().c_str());

		/*PlayerController->WorldInventory->Remove(Item->ItemEntry.ItemGuid);

		HeldObjectComponent->GrantedWeapon = nullptr;
		HeldObjectComponent->GrantedWeaponItem = nullptr;*/

	HeldObjectComponent->HeldObjectState = 4;


	if (HeldObjectComponent->HasOwningPawn())
	{
		auto OldPawn = HeldObjectComponent->OwningPawn;
		printf("%p\n", OldPawn->PreviousWeapon);
		auto PreviousIns = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
			{
				return entry.ItemGuid == ((AFortWeapon*)OldPawn->PreviousWeapon)->ItemEntryGuid;
			}, FFortItemEntry::Size());

		if (PreviousIns)
		{
			auto Weapon = (AFortWeapon*)OldPawn->EquipWeaponDefinition(PreviousIns->ItemDefinition, PreviousIns->ItemGuid, FFortItemEntry::HasTrackerGuid() ? PreviousIns->TrackerGuid : FGuid(), false);

			if (Weapon)
			{
				FFortWeaponMods::ApplyEntrySlotsAfterEquip(
					Weapon, *PreviousIns);
				PlayerController->ClientEquipItem(
					Weapon->ItemEntryGuid, true);
			}
		}
	}
	else
	{
		// its a weakobjectptr on older builds
		static auto OwningPawnOff = HeldObjectComponent->GetOffset("OwningPawn", 0x8000000);
		auto& OwningPawn = *(TWeakObjectPtr<AFortPlayerPawnAthena>*)(__int64(Context) + OwningPawnOff);

		auto PreviousIns = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
			{
				return entry.ItemGuid == ((AFortWeapon*)OwningPawn->PreviousWeapon)->ItemEntryGuid;
			}, FFortItemEntry::Size());

		if (PreviousIns)
		{
			auto Weapon = (AFortWeapon*)OwningPawn->EquipWeaponDefinition(PreviousIns->ItemDefinition, PreviousIns->ItemGuid, FFortItemEntry::HasTrackerGuid() ? PreviousIns->TrackerGuid : FGuid(), false);

			if (Weapon)
			{
				FFortWeaponMods::ApplyEntrySlotsAfterEquip(
					Weapon, *PreviousIns);
				PlayerController->ClientEquipItem(
					Weapon->ItemEntryGuid, true);
			}
		}
	}

	SetupOwningPawn(HeldObjectComponent, nullptr);
	HeldObjectComponent->OnHeldObjectDropped.Process();
}


void AFortPlayerControllerAthena::SpawnToyInstance(UObject* Context, FFrame& Stack, AActor** Ret)
{
	TSubclassOf<AActor> ToyClass;
	FTransform SpawnPosition;

	Stack.StepCompiledIn(&ToyClass);
	Stack.StepCompiledIn(&SpawnPosition);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	auto Toy = UWorld::SpawnActor(ToyClass, SpawnPosition, PlayerController);
	PlayerController->ActiveToyInstances.Add(Toy);

	*Ret = Toy;
}

void AFortPlayerControllerAthena::EnterAircraft(UObject* Object, AActor* Aircraft)
{
	auto PlayerController =
		ResolveAircraftPlayerController(Object);

	// PlayerAI entities are virtual passengers: native entry can destroy or
	// replace their warmup pawn/inventory and native jump RPCs reject
	// connectionless controllers on old versions. Their bus ride and skydive
	// are owned by the bounded PlayerAI transport flow.
	if (PlayerController &&
		MagnesiumPlayerAIIntegration::IsPlayerAIController(
			PlayerController))
		return;

	const bool bWasInAircraft =
		PlayerController &&
		PlayerController->IsInAircraft();
	EnterAircraftOG(Object, Aircraft);
	const bool bEnteredAircraft =
		PlayerController &&
		!bWasInAircraft &&
		PlayerController->IsInAircraft();
	if (bEnteredAircraft)
	{
		ClearDroppableInventoryForAircraft(
			PlayerController, "enter", true);
	}
}

class AFortPlayerStartCreative : public AActor
{
public:
	UCLASS_COMMON_MEMBERS(AFortPlayerStartCreative);

	DEFINE_PROP(PlayerStartTags, FGameplayTagContainer);
};
void AFortPlayerControllerAthena::ServerTeleportToPlaygroundLobbyIsland(UObject* Context, FFrame& Stack)
{
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (!PlayerController || !PlayerController->Pawn)
		return;

	auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;

	static auto CreativePhone = FindObject<UFortWeaponItemDefinition>(L"/Game/Athena/Items/Weapons/Prototype/WID_CreativeTool.WID_CreativeTool");

	auto ItemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
		{
			return entry.ItemDefinition == CreativePhone;
		}, FFortItemEntry::Size());
	if (ItemEntry)
		PlayerController->WorldInventory->Remove(ItemEntry->ItemGuid);

	if (PlayerController->HasbIsCreativeQuickbarEnabled())
	{
		auto OldbIsCreativeQuickbarEnabled = PlayerController->bIsCreativeQuickbarEnabled;
		PlayerController->bIsCreativeQuickbarEnabled = false;
		PlayerController->OnRep_IsCreativeQuickbarEnabled(OldbIsCreativeQuickbarEnabled);
	}
	if (PlayerController->HasbIsCreativeQuickmenuEnabled())
		PlayerController->bIsCreativeQuickmenuEnabled = false;
	if (PlayerController->HasbIsCreativeModeEnabled())
	{
		PlayerController->bIsCreativeModeEnabled = false;
		PlayerController->OnRep_IsCreativeModeEnabled();
	}

	AActor* Actor = GameMode->ChoosePlayerStart(PlayerController);
	PlayerController->Pawn->K2_TeleportTo(Actor->K2_GetActorLocation(), Actor->K2_GetActorRotation());

	PlayerController->CreativePlotLinkedVolume = PlayerController->GetCurrentVolume();
	PlayerController->OnRep_CreativePlotLinkedVolume();
}

inline std::string CleanupString(std::string& s)
{
	if (s.rfind("Schematic:", 0) == 0)
	{
		s.erase(0, 10);
	}
	return s;
}

void AFortPlayerControllerAthena::ServerCraftSchematic(UObject* Context, FFrame& Stack)
{
	FString ItemId;
	int32 PostCraftSlot;
	bool bIsQuickCrafted;
	Stack.StepCompiledIn(&ItemId);
	Stack.StepCompiledIn(&PostCraftSlot);
	Stack.StepCompiledIn(&bIsQuickCrafted);
	Stack.IncrementCode();

	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	auto SchematicStr = ItemId.ToString();
	std::string SchematicStdStr = SchematicStr.c_str();

	printf("CraftShit: ItemId='%s'\n", SchematicStdStr.c_str());

	auto CleanedSchematic = CleanupString(SchematicStdStr);

	printf("clean schematic broo: '%s'\n", CleanedSchematic.c_str());

	auto Schematic = TUObjectArray::FindObject<UFortSchematicItemDefinition>(CleanedSchematic.c_str());
	if (Schematic)
	{
		printf("schematic found : %s\n", Schematic->Name.ToString().c_str());

		auto ResultItemDef = Schematic->GetResultWorldItemDefinition();

		printf("item: %s\n", ResultItemDef->Name.ToString().c_str());

		if (ResultItemDef) {

			auto subbed = Schematic->MaxLevel - Schematic->MinLevel;

			if (subbed <= -1)
				subbed = 0;
			else
			{
				auto calc = (int)(((float)rand() / 32767) * (float)(subbed + 1));
				if (calc <= subbed)
					subbed = calc;
			}

			auto NewEntry = AFortInventory::MakeItemEntry(ResultItemDef, Schematic->GetQuantityProduced(), subbed + Schematic->MinLevel);

			PlayerController->InternalPickup(NewEntry);
			free(NewEntry);

			if (Schematic->CraftingRecipe.DataTable)
			{
				auto FoundRecipe = Schematic->CraftingRecipe.DataTable->RowMap.Search([&](FName& Key, uint8_t*& Value)
					{ return Key == Schematic->CraftingRecipe.RowName; });

				if (!FoundRecipe)
				{
					printf("[Crafting] Failed to find recipe!\n");
					return;
				}

				auto Recipe = *(FRecipe**)FoundRecipe;
				auto CostCount = Recipe->RecipeCosts.Num();

				printf("num costs: %d\n", CostCount);

				for (int i = 0; i < Recipe->RecipeCosts.Num(); i++)
				{
					auto& Cost = Recipe->RecipeCosts.Get(i, FFortItemQuantityPair::Size());

					auto ItemDef = Cost.ItemDefinition.Get();

					auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Entry)
						{ return Entry.ItemDefinition == ItemDef; }, FFortItemEntry::Size());
					auto ItemP = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem*& Item)
						{ return Item->ItemEntry.ItemDefinition == ItemDef; });

					if (itemEntry)
					{
						auto Item = *ItemP;

						itemEntry->Count -= Cost.Quantity;

						if (itemEntry->Count <= 0)
							PlayerController->WorldInventory->Remove(itemEntry->ItemGuid);
						else
						{
							Item->ItemEntry.Count = itemEntry->Count;
							PlayerController->WorldInventory->UpdateEntry(*itemEntry);
							Item->ItemEntry.bIsDirty = true;
						}
					}
				}
			}

			if (Schematic->HasCraftingRequirements() && Schematic->CraftingRequirements.DataTable)
			{
				auto FoundRequirements = Schematic->CraftingRequirements.DataTable->RowMap.Search([&](FName& Key, uint8_t*& Value)
					{ return Key == Schematic->CraftingRequirements.RowName; });

				if (!FoundRequirements)
				{
					printf("[Crafting] Failed to find requirements!\n");
					return;
				}

				auto Reqirements = *(FSchematicRequirements**)FoundRequirements;
				auto CostCount = Reqirements->Requirements.Num();

				printf("num requirements: %d\n", CostCount);

				for (int i = 0; i < Reqirements->Requirements.Num(); i++)
				{
					auto& Requirement = Reqirements->Requirements.Get(i, FSchematicRequirement::Size());
					auto ItemDef = Requirement.ItemDefinition;

					auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Entry)
						{ return Entry.ItemDefinition == ItemDef; }, FFortItemEntry::Size());
					auto ItemP = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem*& Item)
						{ return Item->ItemEntry.ItemDefinition == ItemDef; });

					if (itemEntry)
					{
						auto Item = *ItemP;

						itemEntry->Count -= Requirement.Count;

						if (itemEntry->Count <= 0)
							PlayerController->WorldInventory->Remove(itemEntry->ItemGuid);
						else
						{
							Item->ItemEntry.Count = itemEntry->Count;
							PlayerController->WorldInventory->UpdateEntry(*itemEntry);
							Item->ItemEntry.bIsDirty = true;
						}
					}
				}
			}
		}
	}
}

void AFortPlayerControllerAthena::ServerGiveCreativeItem(UObject* Context, FFrame& Stack)
{
	auto CreativeItem = (FFortItemEntry*)malloc(FFortItemEntry::Size());
	memset(CreativeItem, 0, FFortItemEntry::Size());
	FGuid ItemToRemoveGuid{};
	Stack.StepCompiledIn(CreativeItem);
	Stack.StepCompiledIn(&ItemToRemoveGuid);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (auto WeaponDef = CreativeItem->ItemDefinition->Cast<UFortWeaponItemDefinition>())
		CreativeItem->LoadedAmmo = AFortInventory::GetStats(WeaponDef)->ClipSize;

	PlayerController->InternalPickup(CreativeItem);
	free(CreativeItem);
}

void AFortPlayerControllerAthena::ServerRequestSeatChange_(UObject* Context, FFrame& Stack)
{
	int TargetSeatIndex;

	Stack.StepCompiledIn(&TargetSeatIndex);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (!PlayerController || !PlayerController->WorldInventory ||
		!PlayerController->Pawn)
		return;

	auto Pawn = (AActor*)PlayerController->Pawn;
	auto Vehicle = ResolveVehicleForPawn(Pawn);

	if (!Vehicle && IsNativeVehiclePossessionPawn(Pawn))
		Vehicle = Pawn;

	// FN10.40's gunner pawn can report a null local vehicle even though its
	// authoritative mech seat is still occupied. Retain the actor discovered at
	// interaction/driver possession so F can switch back in either direction.
	if (!Vehicle)
	{
		auto TrackedVehicle =
			GVehiclePossessionVehicle.find(PlayerController);
		if (TrackedVehicle != GVehiclePossessionVehicle.end() &&
			IsUsableDeathObject(TrackedVehicle->second))
			Vehicle = TrackedVehicle->second;
		else if (TrackedVehicle != GVehiclePossessionVehicle.end())
			GVehiclePossessionVehicle.erase(TrackedVehicle);
	}

	if (!Vehicle)
		return callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerRequestSeatChange, TargetSeatIndex);

	auto RiderPawn = ResolveVehicleRiderPawn(
		PlayerController, Pawn);
	if (!RiderPawn)
		return callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerRequestSeatChange, TargetSeatIndex);

	UFortVehicleSeatWeaponComponent* SeatWeaponComponent =
		ResolveVehicleSeatWeaponComponent(
			Vehicle, TargetSeatIndex);

	UFortVehicleSeatComponent* SeatComponent = (UFortVehicleSeatComponent*)Vehicle->GetComponentByClass(UFortVehicleSeatComponent::StaticClass());
	if (!SeatComponent)
		return callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerRequestSeatChange, TargetSeatIndex);

	GVehiclePossessionVehicle[PlayerController] = Vehicle;

	auto Tracked = GTrackedVehicleLoadouts.find(PlayerController);
	if (Tracked == GTrackedVehicleLoadouts.end())
	{
		Tracked = GTrackedVehicleLoadouts.emplace(
			PlayerController,
			CaptureVehicleLoadout(
				PlayerController, RiderPawn)).first;
	}

	AActor* WeaponToRestore =
		RiderPawn ? RiderPawn->CurrentWeapon : nullptr;
	auto CurrentRiderWeapon = WeaponToRestore
		? WeaponToRestore->Cast<AFortWeapon>() : nullptr;
	if ((!CurrentRiderWeapon ||
			VehicleLoadoutContainsGuid(
				Tracked->second.TemporaryItemGuids,
				CurrentRiderWeapon->ItemEntryGuid)) &&
		RiderPawn->HasPreviousWeapon() &&
		RiderPawn->PreviousWeapon)
	{
		// Prefer the currently equipped player item. PreviousWeapon is only a
		// fallback while the current actor is a tracked mech weapon; otherwise
		// repeated driver/gunner toggles can carry a removed Ostrich actor
		// forward as the next restore target.
		WeaponToRestore = RiderPawn->PreviousWeapon;
	}
	auto InventoryBeforeSeatChange =
		SnapshotVehicleInventoryGuids(
			PlayerController->WorldInventory);
	callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerRequestSeatChange, TargetSeatIndex);

	// A rejected seat-change RPC must not receive a weapon configured for a
	// seat the pawn never entered. The old path also stored the pre-change
	// SeatIdx in MountedWeaponInfoRepped, which left passenger firing detached
	// from the actual seat.
	const int32 ActualSeatIndex =
		SeatComponent->FindSeatIndex(RiderPawn);
	if (ActualSeatIndex != TargetSeatIndex)
		return;

	// The native seat change may replace a dynamic mod component. Resolve the
	// final authoritative seat mapping after the transition as well.
	SeatWeaponComponent =
		ResolveVehicleSeatWeaponComponent(
			Vehicle, ActualSeatIndex);

	RemoveTrackedVehicleItems(
		PlayerController, Tracked->second);
	TrackNewVehicleItems(
		Tracked->second,
		PlayerController->WorldInventory,
		InventoryBeforeSeatChange);

	if (!SeatWeaponComponent)
	{
		EquipTrackedVehicleOriginalItem(
			PlayerController, Tracked->second);
		return;
	}

	FWeaponSeatDefinition* NewWeaponDefinition = nullptr;
	for (int32 Index = 0;
		Index < SeatWeaponComponent->WeaponSeatDefinitions.Num();
		++Index)
	{
		auto& Candidate =
			SeatWeaponComponent->WeaponSeatDefinitions.Get(
				Index, FWeaponSeatDefinition::Size());
		if (Candidate.SeatIndex != ActualSeatIndex)
			continue;

		NewWeaponDefinition = &Candidate;
		break;
	}

	if (!NewWeaponDefinition ||
		!NewWeaponDefinition->VehicleWeapon)
	{
		EquipTrackedVehicleOriginalItem(
			PlayerController, Tracked->second);
		return;
	}

	TrackNewVehicleItems(
		Tracked->second,
		PlayerController->WorldInventory,
		InventoryBeforeSeatChange,
		NewWeaponDefinition->VehicleWeapon);
	ConfigureVehicleSeatWeapon(
		PlayerController,
		RiderPawn,
		Vehicle,
		SeatWeaponComponent,
		*NewWeaponDefinition,
		ActualSeatIndex,
		WeaponToRestore);
}

static bool IsCompatibleVehicleModSeatRpc(UFunction* Function)
{
	if (!Function)
		return false;

	const auto Params = Function->GetParamsNamed();
	if (Params.Size != 0x18)
		return false;

	bool bRequestTypeValid = false;
	bool bModInstanceValid = false;
	bool bSeatIndexValid = false;
	for (const auto& Param : Params.NameOffsetMap)
	{
		if (Param.Name == "RequestType")
		{
			bRequestTypeValid =
				Param.Offset == 0x0 &&
				Param.ElementSize == 0x4 &&
				(Param.PropertyFlags & 0x80) != 0 &&
				(Param.PropertyFlags & 0x500) == 0;
		}
		else if (Param.Name == "ModInstance")
		{
			bModInstanceValid =
				Param.Offset == 0x8 &&
				Param.ElementSize == sizeof(void*) &&
				(Param.PropertyFlags & 0x80) != 0 &&
				(Param.PropertyFlags & 0x500) == 0;
		}
		else if (Param.Name == "SeatIndex")
		{
			bSeatIndexValid =
				Param.Offset == 0x10 &&
				Param.ElementSize == sizeof(int32) &&
				(Param.PropertyFlags & 0x82) == 0x82 &&
				(Param.PropertyFlags & 0x500) == 0;
		}
	}

	return bRequestTypeValid &&
		bModInstanceValid &&
		bSeatIndexValid;
}

static bool IsVehicleModSeatWeaponEquipped(
	AFortPlayerPawnAthena* RiderPawn)
{
	if (!RiderPawn || !RiderPawn->HasCurrentWeapon() ||
		!RiderPawn->CurrentWeapon)
	{
		return false;
	}

	// This subclass is unique to the FN30 mod-seat lifecycle. Do not use the
	// broader vehicle-weapon class: ordinary fixed turrets must continue using
	// physical seat changes rather than the same-seat inventory toggle.
	static const UClass* VehicleModWeaponClass = nullptr;
	if (!VehicleModWeaponClass)
		VehicleModWeaponClass =
			FindClass("FortWeaponRangedForVehicleMod");

	return VehicleModWeaponClass &&
		RiderPawn->CurrentWeapon->IsA(VehicleModWeaponClass);
}

void AFortPlayerControllerAthena::ServerVehicleModSeatRpc_(
	UObject* Context, FFrame& Stack)
{
	FGameplayTag RequestType{};
	UObject* ModInstanceObject = nullptr;
	int32 SeatIndex = -1;

	Stack.StepCompiledIn(&RequestType);
	Stack.StepCompiledIn(&ModInstanceObject);
	Stack.StepCompiledIn(&SeatIndex);
	Stack.IncrementCode();

	auto* PlayerController =
		(AFortPlayerControllerAthena*)Context;
	auto* NativeFunction = Stack.GetCurrentNativeFunction();
	auto CallOriginal = [&]()
	{
		if (PlayerController && NativeFunction &&
			ServerVehicleModSeatRpc_OG)
		{
			callOG(
				PlayerController,
				NativeFunction,
				ServerVehicleModSeatRpc,
				RequestType,
				ModInstanceObject,
				SeatIndex);
		}
	};

	if (VersionInfo.FortniteVersion < 30.00 ||
		VersionInfo.FortniteVersion >= 31.00)
	{
		CallOriginal();
		return;
	}

	// Network object references are normally validated by the engine. Retain
	// that boundary here as well: the fallback may invoke protected authority
	// methods only on a live FN30 mod component owned by the occupied vehicle.
	static const UClass* VehicleModComponentClass = nullptr;
	if (!VehicleModComponentClass)
		VehicleModComponentClass =
			FindClass("FortVehicleModComponent");

	if (!PlayerController ||
		!PlayerController->WorldInventory ||
		!ModInstanceObject ||
		!SDK::MemReadable(
			ModInstanceObject, sizeof(UObject)) ||
		!ModInstanceObject->Class ||
		!VehicleModComponentClass ||
		!ModInstanceObject->IsA(
			VehicleModComponentClass) ||
		SeatIndex < 0)
	{
		CallOriginal();
		return;
	}

	auto* ModInstance =
		(UFortVehicleModComponent*)ModInstanceObject;
	auto* OwnerObject = ModInstance->GetOwner();
	auto* Vehicle = OwnerObject
		? OwnerObject->Cast<AFortAthenaVehicle>()
		: nullptr;
	auto* RiderPawn = ResolveVehicleRiderPawn(
		PlayerController,
		(AActor*)PlayerController->Pawn);
	auto* SeatComponent = Vehicle
		? (UFortVehicleSeatComponent*)
			Vehicle->GetComponentByClass(
				UFortVehicleSeatComponent::StaticClass())
		: nullptr;

	if (!Vehicle || !RiderPawn || !SeatComponent ||
		!Vehicle->HasRole() || !Vehicle->HasAuthority() ||
		SeatComponent->FindSeatIndex(RiderPawn) != SeatIndex)
	{
		CallOriginal();
		return;
	}

	GVehiclePossessionVehicle[PlayerController] = Vehicle;
	const bool bWasMounted =
		IsVehicleModSeatWeaponEquipped(RiderPawn);

	auto Tracked =
		GTrackedVehicleLoadouts.find(PlayerController);
	if (Tracked == GTrackedVehicleLoadouts.end())
	{
		FTrackedVehicleLoadout InitialState{};
		if (!bWasMounted)
		{
			InitialState = CaptureVehicleLoadout(
				PlayerController, RiderPawn);
		}
		else
		{
			// If this server was attached after vehicle entry, the current
			// mod weapon is temporary; the previous inventory weapon is the
			// best exact restoration target.
			auto* PreviousWeapon =
				RiderPawn->HasPreviousWeapon()
				? RiderPawn->PreviousWeapon
					? RiderPawn->PreviousWeapon->Cast<AFortWeapon>()
					: nullptr
				: nullptr;
			if (PreviousWeapon &&
				VehicleInventoryContainsGuid(
					PlayerController->WorldInventory,
					PreviousWeapon->ItemEntryGuid))
			{
				InitialState.bHasOriginalEquippedItem = true;
				InitialState.OriginalEquippedItem =
					PreviousWeapon->ItemEntryGuid;
			}

			auto* CurrentWeapon =
				RiderPawn->CurrentWeapon
				? RiderPawn->CurrentWeapon->Cast<AFortWeapon>()
				: nullptr;
			if (CurrentWeapon &&
				VehicleInventoryContainsGuid(
					PlayerController->WorldInventory,
					CurrentWeapon->ItemEntryGuid))
			{
				InitialState.TemporaryItemGuids.push_back(
					CurrentWeapon->ItemEntryGuid);
			}
		}

		Tracked = GTrackedVehicleLoadouts.emplace(
			PlayerController, std::move(InitialState)).first;
	}

	AActor* WeaponToRestore = RiderPawn->CurrentWeapon;
	if (bWasMounted && RiderPawn->HasPreviousWeapon() &&
		RiderPawn->PreviousWeapon)
	{
		WeaponToRestore = RiderPawn->PreviousWeapon;
	}
	const auto InventoryBeforeToggle =
		SnapshotVehicleInventoryGuids(
			PlayerController->WorldInventory);
	TWeakObjectPtr<UFortVehicleModComponent> WeakModInstance(
		ModInstance);

	// Preserve Epic's request-tag handling first. Raw-spawned/mod-replayed
	// vehicles can leave its internal seat-input state incomplete, in which
	// case the native RPC returns without changing the equipped mode.
	CallOriginal();

	bool bIsMounted =
		IsVehicleModSeatWeaponEquipped(RiderPawn);
	auto* LiveModInstance = WeakModInstance.Get();
	if (bIsMounted == bWasMounted &&
		LiveModInstance &&
		SDK::MemReadable(
			LiveModInstance, sizeof(UObject)))
	{
		const char* AuthorityAction = bWasMounted
			? "AuthorityTryUnequipModSeatWeapon"
			: "AuthorityTryEquipModSeatWeapon";
		if (auto* Action =
				LiveModInstance->GetFunction(AuthorityAction))
		{
			LiveModInstance->Call<void>(
				Action, SeatIndex);
		}
		bIsMounted =
			IsVehicleModSeatWeaponEquipped(RiderPawn);
	}

	if (bWasMounted)
	{
		// Native mod-seat unequip can destroy its granted inventory entry.
		// Removing only GUIDs recorded by this vehicle lifecycle is idempotent
		// and never touches a player's similarly named normal weapon.
		RemoveTrackedVehicleItems(
			PlayerController, Tracked->second);
		EquipTrackedVehicleOriginalItem(
			PlayerController, Tracked->second);
		bIsMounted =
			IsVehicleModSeatWeaponEquipped(RiderPawn);
	}
	else
	{
		TrackNewVehicleItems(
			Tracked->second,
			PlayerController->WorldInventory,
			InventoryBeforeToggle);

		// If both FN30 authority paths were no-ops, complete the transition
		// through the already validated exact-seat weapon lifecycle.
		if (!bIsMounted)
		{
			auto* SeatWeaponComponent =
				ResolveVehicleSeatWeaponComponent(
					Vehicle, SeatIndex);
			if (SeatWeaponComponent)
			{
				for (int32 Index = 0;
					Index <
						SeatWeaponComponent->
							WeaponSeatDefinitions.Num();
					++Index)
				{
					auto& WeaponDefinition =
						SeatWeaponComponent->
							WeaponSeatDefinitions.Get(
								Index,
								FWeaponSeatDefinition::Size());
					if (WeaponDefinition.SeatIndex !=
						SeatIndex ||
						!WeaponDefinition.VehicleWeapon)
					{
						continue;
					}

					ConfigureVehicleSeatWeapon(
						PlayerController,
						RiderPawn,
						Vehicle,
						SeatWeaponComponent,
						WeaponDefinition,
						SeatIndex,
						WeaponToRestore);
					break;
				}
			}
			bIsMounted =
				IsVehicleModSeatWeaponEquipped(RiderPawn);
		}
	}

	static uint32 ToggleLogCount = 0;
	if (ToggleLogCount++ < 32)
	{
		SDK::DbgLog(
			"[VehicleMods] mod-seat toggle request-name-index=%u "
			"controller=%p vehicle=%p mod=%p seat=%d "
			"mounted=%d->%d\n",
			(unsigned)RequestType.TagName.ComparisonIndex,
			PlayerController,
			Vehicle,
			LiveModInstance,
			SeatIndex,
			bWasMounted,
			bIsMounted);
	}

	Vehicle->FlushNetDormancy();
	Vehicle->ForceNetUpdate();
	RiderPawn->ForceNetUpdate();
}

void AFortPlayerControllerAthena::ServerLoadingScreenDropped_(UObject* Context, FFrame& Stack)
{
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (!PlayerController)
		return callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerLoadingScreenDropped);

	return callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerLoadingScreenDropped);
}

void AFortPlayerControllerAthena::ServerCreativeSetFlightSpeedIndex(UObject* Context, FFrame& Stack)
{
	int32 Index_0;

	Stack.StepCompiledIn(&Index_0);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	auto bIsFlyingPossible = PlayerController->Validation_IsFlyingPossible();

	if (PlayerController->Validation_IsFlyingPossible__Ptr && !bIsFlyingPossible)
		return;

	PlayerController->FlyingModifierIndex = Index_0;
	PlayerController->OnRep_FlyingModifierIndex();
}

void AFortPlayerControllerAthena::ServerCreativeSetFlightSprint(UObject* Context, FFrame& Stack)
{
	bool bSprint;

	Stack.StepCompiledIn(&bSprint);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	PlayerController->bIsFlightSprinting = bSprint;
	PlayerController->OnRep_IsFlightSprinting();
}

static size_t GetNativeFunctionParamCount(FFrame& Stack)
{
	auto Function = Stack.GetCurrentNativeFunction();
	if (!Function)
		Function = Stack.Node;

	return Function ? Function->GetParamsNamed().NameOffsetMap.size() : 0;
}

static int32 GetStructPropertyOffset(const SDK::UStruct* Struct, const char* PropertyName)
{
	if (!Struct)
		return -1;

	auto Prop = Struct->GetProperty(PropertyName);
	return Prop ? SDK::DecryptPropOffset(GetFromOffset<uint32>(Prop, Offsets::Offset_Internal)) : -1;
}

static int32 GetObjectPropertyElementSize(const UObject* Object, const char* PropertyName)
{
	if (!Object)
		return 0;

	auto Prop = Object->GetProperty(PropertyName);
	return Prop ? GetFromOffset<uint32>(Prop, Offsets::ElementSize) : 0;
}

static int32 FindEntriesOffsetFromListStructs(int32 ExpectedListSize, const char* PreferredStructName, const char* FallbackStructName)
{
	const SDK::UStruct* Candidates[2] =
	{
		SDK::FindStruct(PreferredStructName),
		SDK::FindStruct(FallbackStructName)
	};

	int32 FallbackOffset = -1;

	for (auto Struct : Candidates)
	{
		const int32 EntriesOffset = GetStructPropertyOffset(Struct, "Entries");
		if (EntriesOffset == -1)
			continue;

		if (FallbackOffset == -1)
			FallbackOffset = EntriesOffset;

		if (ExpectedListSize <= 0 || Struct->GetPropertiesSize() == ExpectedListSize)
			return EntriesOffset;
	}

	return FallbackOffset;
}

static int32 GetIndicatedEntriesOffset(UFortIndicatedActorManagementComponent* Component)
{
	static int32 EntriesOffset = -2;

	if (EntriesOffset == -2)
		EntriesOffset = FindEntriesOffsetFromListStructs(GetObjectPropertyElementSize(Component, "IndicatedActorList"), "IndicatedActorInfoArray", "IndicatedActorList");

	return EntriesOffset;
}

static int32 GetStenciledEntriesOffset(UFortIndicatedActorManagementComponent* Component)
{
	static int32 EntriesOffset = -2;

	if (EntriesOffset == -2)
		EntriesOffset = FindEntriesOffsetFromListStructs(GetObjectPropertyElementSize(Component, "StenciledActorList"), "StenciledActorInfoArray", "StenciledActorList");

	return EntriesOffset;
}

static TArray<FIndicatedActorInfoEntry>* GetIndicatedEntries(UFortIndicatedActorManagementComponent* Component, FIndicatedActorList& List)
{
	const int32 EntriesOffset = GetIndicatedEntriesOffset(Component);
	return EntriesOffset != -1 ? &GetFromOffset<TArray<FIndicatedActorInfoEntry>>(&List, EntriesOffset) : nullptr;
}

static TArray<FStenciledActorInfoEntry>* GetStenciledEntries(UFortIndicatedActorManagementComponent* Component, FStenciledActorList& List)
{
	const int32 EntriesOffset = GetStenciledEntriesOffset(Component);
	return EntriesOffset != -1 ? &GetFromOffset<TArray<FStenciledActorInfoEntry>>(&List, EntriesOffset) : nullptr;
}

static bool CanUseIndicatedActorInfoEntry()
{
	return FIndicatedActorData::StaticStruct()
		&& FIndicatedActorInfoEntry::StaticStruct()
		&& FIndicatedActorInfoEntry::HasActor()
		&& FIndicatedActorInfoEntry::HasStartTime()
		&& FIndicatedActorInfoEntry::HasEndTime()
		&& FIndicatedActorInfoEntry::HasData();
}

static bool CanUseStenciledActorInfoEntry()
{
	return FStenciledActorData::StaticStruct()
		&& FStenciledActorInfoEntry::StaticStruct()
		&& FStenciledActorInfoEntry::HasActor()
		&& FStenciledActorInfoEntry::HasStartTime()
		&& FStenciledActorInfoEntry::HasEndTime()
		&& FStenciledActorInfoEntry::HasData();
}

static UFortIndicatedActorManagementComponent* GetIndicatedActorManagementComponent(AFortPlayerControllerAthena* PC)
{
	if (!PC || !PC->HasIndicatedActorManagementComponent())
		return nullptr;

	auto Component = PC->IndicatedActorManagementComponent;
	return Component && Component->HasIndicatedActorList() ? Component : nullptr;
}

static UFortIndicatedActorManagementComponent* GetStenciledActorManagementComponent(AFortPlayerControllerAthena* PC)
{
	if (!PC || !PC->HasIndicatedActorManagementComponent())
		return nullptr;

	auto Component = PC->IndicatedActorManagementComponent;
	return Component && Component->HasStenciledActorList() ? Component : nullptr;
}

static void ClearReflectedFString(void* StructData, int32 StructStorageSize, int32 StringOffset)
{
	if (StringOffset < 0 || StringOffset + (int32)sizeof(FString) > StructStorageSize)
		return;

	memset((uint8*)StructData + StringOffset, 0, sizeof(FString));
}

static void CopyReflectedFStringIfReadable(void* DestStructData, const void* SourceStructData, int32 StructStorageSize, int32 StringOffset)
{
	if (StringOffset < 0 || StringOffset + (int32)sizeof(FString) > StructStorageSize)
		return;

	auto& Dest = *(FString*)((uint8*)DestStructData + StringOffset);
	const auto& Source = *(const FString*)((const uint8*)SourceStructData + StringOffset);
	const int32 CharCount = Source.Num();

	if (CharCount <= 0 || CharCount > 256 || !Source.CStr() || !SDK::MemReadable(Source.CStr(), CharCount * sizeof(wchar_t)))
	{
		ClearReflectedFString(DestStructData, StructStorageSize, StringOffset);
		return;
	}

	FString Copy;
	for (int32 i = 0; i < CharCount; i++)
		Copy.Add(Source.CStr()[i]);

	Dest = Copy;
}

static void CopyIndicatedData(FIndicatedActorData& Dest, const FIndicatedActorData& Source)
{
	if (!FIndicatedActorData::StaticStruct())
		return;

	const int32 CopySize = std::min<int32>(FIndicatedActorData::Size(), sizeof(FIndicatedActorData));
	if (CopySize <= 0)
		return;

	memcpy(&Dest, &Source, CopySize);

	if (FIndicatedActorData::HasGroupIdentifier())
		CopyReflectedFStringIfReadable(&Dest, &Source, sizeof(FIndicatedActorData), FIndicatedActorData::GroupIdentifier__Offset);
}

static void CopyStenciledData(FStenciledActorData& Dest, const FStenciledActorData& Source)
{
	if (!FStenciledActorData::StaticStruct())
		return;

	const int32 CopySize = std::min<int32>(FStenciledActorData::Size(), sizeof(FStenciledActorData));
	if (CopySize <= 0)
		return;

	memcpy(&Dest, &Source, CopySize);

	if (FStenciledActorData::HasGroupIdentifier())
		CopyReflectedFStringIfReadable(&Dest, &Source, sizeof(FStenciledActorData), FStenciledActorData::GroupIdentifier__Offset);
}

static float GetIndicatedDuration(const FIndicatedActorData& Data)
{
	return FIndicatedActorData::HasDuration() ? Data.Duration : 10.f;
}

static float GetStenciledDuration(const FStenciledActorData& Data)
{
	return FStenciledActorData::HasDuration() ? Data.Duration : 10.f;
}

static bool ShouldShareIndicatedWithSquad(const FIndicatedActorData& Data)
{
	return FIndicatedActorData::HasShareActorWith() && Data.ShareActorWith != 0;
}

static bool ShouldShareStenciledWithSquad(const FStenciledActorData& Data)
{
	return FStenciledActorData::HasShareActorWith() && Data.ShareActorWith != 0;
}

static AFortPlayerControllerAthena* GetActorFortController(AActor* Actor)
{
	if (!Actor || !SDK::MemReadable(Actor, sizeof(void*)))
		return nullptr;

	if (auto PC = Actor->Cast<AFortPlayerControllerAthena>())
		return PC;

	if (auto Pawn = Actor->Cast<AFortPlayerPawnAthena>())
		return Pawn->Controller ? Pawn->Controller->Cast<AFortPlayerControllerAthena>() : nullptr;

	if (auto PlayerState = Actor->Cast<AFortPlayerStateAthena>())
		return PlayerState->GetOwner() ? PlayerState->GetOwner()->Cast<AFortPlayerControllerAthena>() : nullptr;

	// Owner can be a stale/poisoned pointer (e.g. 0xffffffffffffffff) that is non-null yet
	// unreadable, so validate it before dereferencing rather than trusting the null check.
	auto Owner = Actor->Owner;
	if (Owner && SDK::MemReadable(Owner, sizeof(void*)))
	{
		if (auto PC = Owner->Cast<AFortPlayerControllerAthena>())
			return PC;

		if (auto PlayerState = Owner->Cast<AFortPlayerStateAthena>())
			return PlayerState->GetOwner() ? PlayerState->GetOwner()->Cast<AFortPlayerControllerAthena>() : nullptr;
	}

	return nullptr;
}

// AFortPlayerState::PlayerTeam points at a UObject (AFortTeamInfo), not a fixed-layout struct.
// Its TeamMembers array sits at a build-specific reflected offset (0x228 on 13.40), so resolve
// it dynamically instead of assuming offset 0 - reading offset 0 lands on the UObject header and
// yields a garbage count/pointers, which is the source of the ForEachSquadController crash.
static int32 GetTeamMembersOffset(UObject* TeamInfo)
{
	static int32 Offset = -2;

	if (Offset < 0 && TeamInfo)
	{
		const int32 Found = (int32)TeamInfo->GetOffset("TeamMembers");
		if (Found >= 0)
			Offset = Found;
	}

	return Offset;
}

static void ForEachSquadController(AFortPlayerControllerAthena* InstigatingController, const std::function<void(AFortPlayerControllerAthena*)>& Visitor)
{
	if (!InstigatingController || !InstigatingController->PlayerState)
		return;

	auto PlayerState = InstigatingController->PlayerState->Cast<AFortPlayerStateAthena>();
	if (!PlayerState || !PlayerState->HasPlayerTeam())
		return;

	auto TeamInfo = (UObject*)PlayerState->PlayerTeam;
	if (!TeamInfo || !SDK::MemReadable(TeamInfo, sizeof(void*)))
		return;

	const int32 TeamMembersOffset = GetTeamMembersOffset(TeamInfo);
	if (TeamMembersOffset < 0)
		return;

	auto& TeamMembers = GetFromOffset<TArray<AActor*>>(TeamInfo, TeamMembersOffset);
	const int32 MemberCount = TeamMembers.Num();
	if (MemberCount <= 0 || MemberCount > 1024)
		return;

	for (int32 i = 0; i < MemberCount; i++)
	{
		auto TeamMember = TeamMembers[i];
		if (!TeamMember || !SDK::MemReadable(TeamMember, sizeof(void*)))
			continue;

		// TeamMembers may hold AController* (13.40+) or AFortPlayerStateAthena* depending on the
		// build; GetActorFortController resolves either kind to the owning controller.
		auto TeamMemberController = GetActorFortController(TeamMember);

		if (!TeamMemberController || TeamMemberController == InstigatingController)
			continue;

		Visitor(TeamMemberController);
	}
}

static void AddIndicatedEntry(AFortPlayerControllerAthena* PC, const FIndicatedActorInfoEntry& NewEntry, bool bAddAsUnique)
{
	if (!CanUseIndicatedActorInfoEntry())
		return;

	auto Component = GetIndicatedActorManagementComponent(PC);
	if (!Component)
		return;

	auto& List = Component->IndicatedActorList;
	auto Entries = GetIndicatedEntries(Component, List);
	if (!Entries)
		return;

	const int32 EntrySize = FIndicatedActorInfoEntry::Size();
	if (EntrySize <= 0)
		return;

	if (bAddAsUnique)
	{
		for (int32 i = 0; i < Entries->Num(); i++)
		{
			auto& Existing = Entries->Get(i, EntrySize);

			if (Existing.Actor != NewEntry.Actor)
				continue;

			Existing.StartTime = NewEntry.StartTime;
			Existing.EndTime = NewEntry.EndTime;
			CopyIndicatedData(Existing.Data, NewEntry.Data);
			List.MarkItemDirty(Existing);
			return;
		}
	}

	auto& Added = Entries->Add(NewEntry, EntrySize);
	Added.ReplicationID = -1;
	Added.ReplicationKey = -1;
	Added.MostRecentArrayReplicationKey = -1;
	CopyIndicatedData(Added.Data, NewEntry.Data);
	List.MarkItemDirty(Added);
}

static void AddStenciledEntry(AFortPlayerControllerAthena* PC, const FStenciledActorInfoEntry& NewEntry, bool bAddAsUnique)
{
	if (!CanUseStenciledActorInfoEntry())
		return;

	auto Component = GetStenciledActorManagementComponent(PC);
	if (!Component)
		return;

	auto& List = Component->StenciledActorList;
	auto Entries = GetStenciledEntries(Component, List);
	if (!Entries)
		return;

	const int32 EntrySize = FStenciledActorInfoEntry::Size();
	if (EntrySize <= 0)
		return;

	if (bAddAsUnique)
	{
		for (int32 i = 0; i < Entries->Num(); i++)
		{
			auto& Existing = Entries->Get(i, EntrySize);

			if (Existing.Actor != NewEntry.Actor)
				continue;

			Existing.StartTime = NewEntry.StartTime;
			Existing.EndTime = NewEntry.EndTime;
			CopyStenciledData(Existing.Data, NewEntry.Data);
			List.MarkItemDirty(Existing);
			return;
		}
	}

	auto& Added = Entries->Add(NewEntry, EntrySize);
	Added.ReplicationID = -1;
	Added.ReplicationKey = -1;
	Added.MostRecentArrayReplicationKey = -1;
	CopyStenciledData(Added.Data, NewEntry.Data);
	List.MarkItemDirty(Added);
}

static void MarkActorsIndicated(AFortPlayerControllerAthena* InstigatingController, const TArray<AActor*>& IndicatedActors, const FIndicatedActorData& Data, bool bAddAsUnique, bool bAllowOwningPlayer)
{
	if (!InstigatingController || !CanUseIndicatedActorInfoEntry())
		return;

	float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
	const bool bShareWithSquad = ShouldShareIndicatedWithSquad(Data);
	const int32 EntrySize = FIndicatedActorInfoEntry::Size();

	for (int32 i = 0; i < IndicatedActors.Num(); i++)
	{
		auto IndicatedActor = IndicatedActors[i];

		if (!IndicatedActor)
			continue;

		if (IndicatedActor == InstigatingController->MyFortPawn && !bAllowOwningPlayer)
			continue;

		auto EntryMemory = malloc(EntrySize);
		if (!EntryMemory)
			continue;

		memset(EntryMemory, 0, EntrySize);
		auto Entry = (FIndicatedActorInfoEntry*)EntryMemory;
		Entry->Actor = IndicatedActor;
		Entry->StartTime = TimeSeconds;
		Entry->EndTime = TimeSeconds + GetIndicatedDuration(Data);
		CopyIndicatedData(Entry->Data, Data);

		AddIndicatedEntry(InstigatingController, *Entry, bAddAsUnique);

		if (bAllowOwningPlayer)
		{
			auto OwningController = GetActorFortController(IndicatedActor);
			if (OwningController && OwningController != InstigatingController)
				AddIndicatedEntry(OwningController, *Entry, bAddAsUnique);
		}

		if (bShareWithSquad)
		{
			ForEachSquadController(InstigatingController, [&](AFortPlayerControllerAthena* TeamMemberController)
			{
				if (TeamMemberController->MyFortPawn != IndicatedActor)
					AddIndicatedEntry(TeamMemberController, *Entry, bAddAsUnique);
			});
		}

		free(EntryMemory);
	}
}

static void MarkActorsStenciled(AFortPlayerControllerAthena* InstigatingController, const TArray<AActor*>& StenciledActors, const FStenciledActorData& Data, bool bAddAsUnique)
{
	if (!InstigatingController || !CanUseStenciledActorInfoEntry())
		return;

	float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
	const bool bShareWithSquad = ShouldShareStenciledWithSquad(Data);
	const int32 EntrySize = FStenciledActorInfoEntry::Size();

	for (int32 i = 0; i < StenciledActors.Num(); i++)
	{
		auto StenciledActor = StenciledActors[i];

		if (!StenciledActor || StenciledActor == InstigatingController->MyFortPawn)
			continue;

		auto EntryMemory = malloc(EntrySize);
		if (!EntryMemory)
			continue;

		memset(EntryMemory, 0, EntrySize);
		auto Entry = (FStenciledActorInfoEntry*)EntryMemory;
		Entry->Actor = StenciledActor;
		Entry->StartTime = TimeSeconds;
		Entry->EndTime = TimeSeconds + GetStenciledDuration(Data);
		CopyStenciledData(Entry->Data, Data);

		AddStenciledEntry(InstigatingController, *Entry, bAddAsUnique);

		auto OwningController = GetActorFortController(StenciledActor);
		if (OwningController && OwningController != InstigatingController)
			AddStenciledEntry(OwningController, *Entry, bAddAsUnique);

		if (bShareWithSquad)
		{
			ForEachSquadController(InstigatingController, [&](AFortPlayerControllerAthena* TeamMemberController)
			{
				if (TeamMemberController->MyFortPawn != StenciledActor)
					AddStenciledEntry(TeamMemberController, *Entry, bAddAsUnique);
			});
		}

		free(EntryMemory);
	}
}

// Shakedown reveal: when a player interrogates a downed enemy, expose that enemy's surviving
// squadmates (their pawns) as indicated actors to the interrogator and, via the share flag, the
// interrogator's whole squad. Reuses the same version-independent marking as the ping system.
void AFortPlayerControllerAthena::RevealInterrogatedTeam(AFortPlayerControllerAthena* Interrogator, AActor* DBNOPlayer)
{
	if (!Interrogator || !DBNOPlayer || !CanUseIndicatedActorInfoEntry())
		return;

	auto DBNOController = GetActorFortController(DBNOPlayer);
	if (!DBNOController)
		return;

	// Collect the downed player's living squadmates' pawns (ForEachSquadController already skips
	// the downed player itself and resolves each member to its controller across builds).
	TArray<AActor*> Revealed{};
	ForEachSquadController(DBNOController, [&](AFortPlayerControllerAthena* TeamMemberController)
	{
		auto Pawn = TeamMemberController->MyFortPawn;
		if (Pawn && SDK::MemReadable(Pawn, sizeof(void*)))
			Revealed.Add((AActor*)Pawn);
	});

	if (Revealed.Num() <= 0)
		return;

	FIndicatedActorData Data{};
	if (FIndicatedActorData::HasDuration())
		Data.Duration = 12.f;
	if (FIndicatedActorData::HasShareActorWith())
		Data.ShareActorWith = 1; // non-None -> share the reveal with the interrogator's squad

	MarkActorsIndicated(Interrogator, Revealed, Data, true /*bAddAsUnique*/, false /*bAllowOwningPlayer*/);

	Revealed.Free();
}

static bool ActorMatchesIndicatedFilter(AActor* Actor, const FIndicatedActorDataWithFilter& FilterData)
{
	if (!Actor || !FIndicatedActorDataWithFilter::HasIndicatedActorTags())
		return Actor != nullptr;

	auto& RequiredTags = FilterData.IndicatedActorTags;
	if (RequiredTags.GameplayTags.Num() == 0 && RequiredTags.ParentTags.Num() == 0)
		return true;

	FGameplayTagContainer ActorTags{};
	bool bHasActorTags = false;

	if (auto Pawn = Actor->Cast<AFortPlayerPawnAthena>())
	{
		ActorTags = Pawn->GameplayTags;
		bHasActorTags = true;
	}
	else if (auto Interface = (IGameplayTagAssetInterface*)Actor->GetInterface(IGameplayTagAssetInterface::StaticClass()))
	{
		auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface->Vft[0x2];
		GetOwnedGameplayTags(Interface, &ActorTags);
		bHasActorTags = true;
	}

	bool bMatches = bHasActorTags && ActorTags.HasAll(RequiredTags);

	if (bHasActorTags && !Actor->IsA<AFortPlayerPawnAthena>())
	{
		ActorTags.GameplayTags.Free();
		ActorTags.ParentTags.Free();
	}

	return bMatches;
}

static bool SphereOverlapActorsForIndicatedFilter(AFortPlayerControllerAthena* InstigatingController, const FIndicatedActorDataWithFilter& FilterData, TArray<AActor*>& Actors)
{
	if (!InstigatingController || !FIndicatedActorDataWithFilter::HasOverlapRadius() || FilterData.OverlapRadius <= 0.f)
		return false;

	static UFunction* SphereOverlapActorsFn = nullptr;
	static bool bCheckedSphereOverlapActorsFn = false;

	if (!bCheckedSphereOverlapActorsFn)
	{
		bCheckedSphereOverlapActorsFn = true;
		SphereOverlapActorsFn = UKismetSystemLibrary::GetDefaultObj()->GetFunction("SphereOverlapActors");
	}

	if (!SphereOverlapActorsFn)
		return false;

	auto Origin = InstigatingController->MyFortPawn ? InstigatingController->MyFortPawn->K2_GetActorLocation() : InstigatingController->K2_GetActorLocation();
	TArray<AActor*> IgnoredActors{};
	auto ObjectTypes = FIndicatedActorDataWithFilter::HasObjectTypes() ? FilterData.ObjectTypes : TArray<uint8>{};
	auto ActorClassFilter = FIndicatedActorDataWithFilter::HasActorClassFilter() ? FilterData.ActorClassFilter : nullptr;

	return UKismetSystemLibrary::GetDefaultObj()->Call<bool>(SphereOverlapActorsFn, (UObject*)InstigatingController, Origin, FilterData.OverlapRadius, ObjectTypes, ActorClassFilter, IgnoredActors, &Actors);
}

static TArray<AActor*> FindActorsInFilterRadius(AFortPlayerControllerAthena* InstigatingController, const FIndicatedActorDataWithFilter& FilterData)
{
	TArray<AActor*> FilteredActors;
	TArray<AActor*> OverlappedActors;

	if (!SphereOverlapActorsForIndicatedFilter(InstigatingController, FilterData, OverlappedActors))
		return FilteredActors;

	for (int32 i = 0; i < OverlappedActors.Num(); i++)
	{
		auto Actor = OverlappedActors[i];
		if (ActorMatchesIndicatedFilter(Actor, FilterData))
			FilteredActors.Add(Actor);
	}

	OverlappedActors.Free();
	return FilteredActors;
}

static void RemoveIndicatedEntries(AFortPlayerControllerAthena* PC, AActor* Actor, const FString& GroupIdentifier, bool bMatchByGroup)
{
	if (bMatchByGroup || !Actor || !CanUseIndicatedActorInfoEntry())
		return;

	auto Component = GetIndicatedActorManagementComponent(PC);
	if (!Component)
		return;

	auto& List = Component->IndicatedActorList;
	auto Entries = GetIndicatedEntries(Component, List);
	if (!Entries)
		return;

	const int32 EntrySize = FIndicatedActorInfoEntry::Size();
	if (EntrySize <= 0)
		return;

	bool bRemovedAny = false;

	for (int32 i = Entries->Num() - 1; i >= 0; i--)
	{
		auto& Entry = Entries->Get(i, EntrySize);
		if (Entry.Actor != Actor)
			continue;

		Entries->Remove(i, EntrySize);
		bRemovedAny = true;
	}

	if (bRemovedAny)
		List.MarkArrayDirty();
}

static void RemoveStenciledEntries(AFortPlayerControllerAthena* PC, AActor* Actor, const FString& GroupIdentifier, bool bMatchByGroup)
{
	if (bMatchByGroup || !Actor || !CanUseStenciledActorInfoEntry())
		return;

	auto Component = GetStenciledActorManagementComponent(PC);
	if (!Component)
		return;

	auto& List = Component->StenciledActorList;
	auto Entries = GetStenciledEntries(Component, List);
	if (!Entries)
		return;

	const int32 EntrySize = FStenciledActorInfoEntry::Size();
	if (EntrySize <= 0)
		return;

	bool bRemovedAny = false;

	for (int32 i = Entries->Num() - 1; i >= 0; i--)
	{
		auto& Entry = Entries->Get(i, EntrySize);
		if (Entry.Actor != Actor)
			continue;

		Entries->Remove(i, EntrySize);
		bRemovedAny = true;
	}

	if (bRemovedAny)
		List.MarkArrayDirty();
}

static void RemoveIndicatedEntriesWithSquad(AFortPlayerControllerAthena* InstigatingController, AActor* Actor, const FString& GroupIdentifier, bool bMatchByGroup, bool bIncludeSquad)
{
	if (!InstigatingController)
		return;

	RemoveIndicatedEntries(InstigatingController, Actor, GroupIdentifier, bMatchByGroup);

	if (!bIncludeSquad)
		return;

	ForEachSquadController(InstigatingController, [&](AFortPlayerControllerAthena* TeamMemberController)
	{
		RemoveIndicatedEntries(TeamMemberController, Actor, GroupIdentifier, bMatchByGroup);
	});
}

static void RemoveStenciledEntriesWithSquad(AFortPlayerControllerAthena* InstigatingController, AActor* Actor, const FString& GroupIdentifier, bool bMatchByGroup, bool bIncludeSquad)
{
	if (!InstigatingController)
		return;

	RemoveStenciledEntries(InstigatingController, Actor, GroupIdentifier, bMatchByGroup);

	if (!bIncludeSquad)
		return;

	ForEachSquadController(InstigatingController, [&](AFortPlayerControllerAthena* TeamMemberController)
	{
		RemoveStenciledEntries(TeamMemberController, Actor, GroupIdentifier, bMatchByGroup);
	});
}

static AFortPlayerControllerAthena* GetIndicatedComponentController(UObject* Component)
{
	auto ActorComponent = Component ? Component->Cast<UActorComponent>() : nullptr;
	auto Owner = ActorComponent ? ActorComponent->GetOwner() : nullptr;
	return Owner ? Owner->Cast<AFortPlayerControllerAthena>() : nullptr;
}

void AFortPlayerControllerAthena::AddActorsToIndicatedList(UObject* Context, FFrame& Stack)
{
	AFortPlayerControllerAthena* InstigatingController = nullptr;
	TArray<AActor*> IndicatedActors;
	FIndicatedActorData IndicatedActorData{};
	bool bAddAsUnique = false;
	bool bAllowOwningPlayer = false;
	bool bIgnored = false;
	auto ParamCount = GetNativeFunctionParamCount(Stack);

	Stack.StepCompiledIn(&InstigatingController);
	Stack.StepCompiledIn(&IndicatedActors);
	Stack.StepCompiledIn(&IndicatedActorData);
	Stack.StepCompiledIn(&bAddAsUnique);
	Stack.StepCompiledIn(&bAllowOwningPlayer);
	if (ParamCount > 5)
		Stack.StepCompiledIn(&bIgnored);
	if (ParamCount > 6)
		Stack.StepCompiledIn(&bIgnored);

	MarkActorsIndicated(InstigatingController, IndicatedActors, IndicatedActorData, bAddAsUnique, bAllowOwningPlayer);

	if (AddActorsToIndicatedListOG)
		return AddActorsToIndicatedListOG(Context, Stack);
}

void AFortPlayerControllerAthena::AddActorsToStenciledList(UObject* Context, FFrame& Stack)
{
	AFortPlayerControllerAthena* InstigatingController = nullptr;
	TArray<AActor*> StenciledActors;
	FStenciledActorData StenciledActorData{};
	bool bAddAsUnique = false;
	bool bIgnored = false;
	auto ParamCount = GetNativeFunctionParamCount(Stack);

	Stack.StepCompiledIn(&InstigatingController);
	Stack.StepCompiledIn(&StenciledActors);
	Stack.StepCompiledIn(&StenciledActorData);
	Stack.StepCompiledIn(&bAddAsUnique);
	if (ParamCount > 4)
		Stack.StepCompiledIn(&bIgnored);
	if (ParamCount > 5)
		Stack.StepCompiledIn(&bIgnored);

	MarkActorsStenciled(InstigatingController, StenciledActors, StenciledActorData, bAddAsUnique);

	if (AddActorsToStenciledListOG)
		return AddActorsToStenciledListOG(Context, Stack);
}

void AFortPlayerControllerAthena::AddActorsInRadiusToIndicatedList(UObject* Context, FFrame& Stack)
{
	AFortPlayerControllerAthena* InstigatingController = nullptr;
	TArray<FIndicatedActorDataWithFilter> FilterDatas;
	bool bAddAsUnique = false;

	Stack.StepCompiledIn(&InstigatingController);
	Stack.StepCompiledIn(&FilterDatas);
	Stack.StepCompiledIn(&bAddAsUnique);

	if (InstigatingController)
	{
		for (int32 i = 0; i < FilterDatas.Num(); i++)
		{
			auto& FilterData = FilterDatas.Get(i, FIndicatedActorDataWithFilter::Size());
			auto Actors = FindActorsInFilterRadius(InstigatingController, FilterData);

			if (Actors.Num() > 0)
				MarkActorsIndicated(InstigatingController, Actors, FilterData.IndicatedData, bAddAsUnique, true);

			Actors.Free();
		}
	}

	if (AddActorsInRadiusToIndicatedListOG)
		return AddActorsInRadiusToIndicatedListOG(Context, Stack);
}

void AFortPlayerControllerAthena::AddActorsInRadiusToStenciledList(UObject* Context, FFrame& Stack)
{
	AFortPlayerControllerAthena* InstigatingController = nullptr;
	TArray<FIndicatedActorDataWithFilter> FilterDatas;
	bool bAddAsUnique = false;

	Stack.StepCompiledIn(&InstigatingController);
	Stack.StepCompiledIn(&FilterDatas);
	Stack.StepCompiledIn(&bAddAsUnique);

	if (InstigatingController)
	{
		for (int32 i = 0; i < FilterDatas.Num(); i++)
		{
			auto& FilterData = FilterDatas.Get(i, FIndicatedActorDataWithFilter::Size());
			auto Actors = FindActorsInFilterRadius(InstigatingController, FilterData);

			if (Actors.Num() > 0)
				MarkActorsStenciled(InstigatingController, Actors, FilterData.StenciledData, bAddAsUnique);

			Actors.Free();
		}
	}

	if (AddActorsInRadiusToStenciledListOG)
		return AddActorsInRadiusToStenciledListOG(Context, Stack);
}

void AFortPlayerControllerAthena::RemoveActorFromIndicatedList(UObject* Context, FFrame& Stack)
{
	AFortPlayerControllerAthena* InstigatingController = nullptr;
	AActor* IndicatedActor = nullptr;
	bool bIncludeSquad = false;

	Stack.StepCompiledIn(&InstigatingController);
	Stack.StepCompiledIn(&IndicatedActor);
	Stack.StepCompiledIn(&bIncludeSquad);

	if (IndicatedActor)
		RemoveIndicatedEntriesWithSquad(InstigatingController, IndicatedActor, FString(), false, bIncludeSquad);

	if (RemoveActorFromIndicatedListOG)
		return RemoveActorFromIndicatedListOG(Context, Stack);
}

void AFortPlayerControllerAthena::RemoveActorFromStenciledList(UObject* Context, FFrame& Stack)
{
	AFortPlayerControllerAthena* InstigatingController = nullptr;
	AActor* StenciledActor = nullptr;
	bool bIncludeSquad = false;

	Stack.StepCompiledIn(&InstigatingController);
	Stack.StepCompiledIn(&StenciledActor);
	Stack.StepCompiledIn(&bIncludeSquad);

	if (StenciledActor)
		RemoveStenciledEntriesWithSquad(InstigatingController, StenciledActor, FString(), false, bIncludeSquad);

	if (RemoveActorFromStenciledListOG)
		return RemoveActorFromStenciledListOG(Context, Stack);
}

void AFortPlayerControllerAthena::RemoveGroupFromIndicatedList(UObject* Context, FFrame& Stack)
{
	AFortPlayerControllerAthena* InstigatingController = nullptr;
	FString GroupIdentifier;
	bool bIncludeSquad = false;

	Stack.StepCompiledIn(&InstigatingController);
	Stack.StepCompiledIn(&GroupIdentifier);
	Stack.StepCompiledIn(&bIncludeSquad);

	RemoveIndicatedEntriesWithSquad(InstigatingController, nullptr, GroupIdentifier, true, bIncludeSquad);

	if (RemoveGroupFromIndicatedListOG)
		return RemoveGroupFromIndicatedListOG(Context, Stack);
}

void AFortPlayerControllerAthena::RemoveGroupFromStenciledList(UObject* Context, FFrame& Stack)
{
	AFortPlayerControllerAthena* InstigatingController = nullptr;
	FString GroupIdentifier;
	bool bIncludeSquad = false;

	Stack.StepCompiledIn(&InstigatingController);
	Stack.StepCompiledIn(&GroupIdentifier);
	Stack.StepCompiledIn(&bIncludeSquad);

	RemoveStenciledEntriesWithSquad(InstigatingController, nullptr, GroupIdentifier, true, bIncludeSquad);

	if (RemoveGroupFromStenciledListOG)
		return RemoveGroupFromStenciledListOG(Context, Stack);
}

void AFortPlayerControllerAthena::ComponentAddActorsToIndicatedList(UObject* Context, FFrame& Stack)
{
	auto& IndicatedActors = Stack.StepCompiledInRef<TArray<AActor*>>();
	FIndicatedActorData Data{};
	bool bAddAsUnique = false;
	bool bAllowOwningPlayer = false;
	bool bIgnored = false;
	auto ParamCount = GetNativeFunctionParamCount(Stack);

	Stack.StepCompiledIn(&Data);
	Stack.StepCompiledIn(&bAddAsUnique);
	Stack.StepCompiledIn(&bAllowOwningPlayer);
	if (ParamCount > 4)
		Stack.StepCompiledIn(&bIgnored);
	if (ParamCount > 5)
		Stack.StepCompiledIn(&bIgnored);

	MarkActorsIndicated(GetIndicatedComponentController(Context), IndicatedActors, Data, bAddAsUnique, bAllowOwningPlayer);

	if (ComponentAddActorsToIndicatedListOG)
		return ComponentAddActorsToIndicatedListOG(Context, Stack);
}

void AFortPlayerControllerAthena::ComponentAddActorsToStenciledList(UObject* Context, FFrame& Stack)
{
	auto& StenciledActors = Stack.StepCompiledInRef<TArray<AActor*>>();
	FStenciledActorData Data{};
	bool bAddAsUnique = false;
	bool bIgnored = false;
	auto ParamCount = GetNativeFunctionParamCount(Stack);

	Stack.StepCompiledIn(&Data);
	Stack.StepCompiledIn(&bAddAsUnique);
	if (ParamCount > 3)
		Stack.StepCompiledIn(&bIgnored);
	if (ParamCount > 4)
		Stack.StepCompiledIn(&bIgnored);

	MarkActorsStenciled(GetIndicatedComponentController(Context), StenciledActors, Data, bAddAsUnique);

	if (ComponentAddActorsToStenciledListOG)
		return ComponentAddActorsToStenciledListOG(Context, Stack);
}

// Chapter 3+ (above v21) routes flare-gun style radius reveals through the component rather than
// the FortIndicatedActorManagementLibrary, so the component radius entrypoints must be hooked too
// or nothing gets marked. Controller comes from the component owner (no InstigatingController param).
void AFortPlayerControllerAthena::ComponentAddActorsInRadiusToIndicatedList(UObject* Context, FFrame& Stack)
{
	auto& FilterDatas = Stack.StepCompiledInRef<TArray<FIndicatedActorDataWithFilter>>();
	bool bAddAsUnique = false;
	bool bIgnored = false;
	auto ParamCount = GetNativeFunctionParamCount(Stack);

	Stack.StepCompiledIn(&bAddAsUnique);
	if (ParamCount > 2)
		Stack.StepCompiledIn(&bIgnored);
	if (ParamCount > 3)
		Stack.StepCompiledIn(&bIgnored);

	auto InstigatingController = GetIndicatedComponentController(Context);
	if (InstigatingController)
	{
		for (int32 i = 0; i < FilterDatas.Num(); i++)
		{
			auto& FilterData = FilterDatas.Get(i, FIndicatedActorDataWithFilter::Size());
			auto Actors = FindActorsInFilterRadius(InstigatingController, FilterData);

			if (Actors.Num() > 0)
				MarkActorsIndicated(InstigatingController, Actors, FilterData.IndicatedData, bAddAsUnique, true);

			Actors.Free();
		}
	}

	if (ComponentAddActorsInRadiusToIndicatedListOG)
		return ComponentAddActorsInRadiusToIndicatedListOG(Context, Stack);
}

void AFortPlayerControllerAthena::ComponentAddActorsInRadiusToStenciledList(UObject* Context, FFrame& Stack)
{
	auto& FilterDatas = Stack.StepCompiledInRef<TArray<FIndicatedActorDataWithFilter>>();
	bool bAddAsUnique = false;
	bool bIgnored = false;
	auto ParamCount = GetNativeFunctionParamCount(Stack);

	Stack.StepCompiledIn(&bAddAsUnique);
	if (ParamCount > 2)
		Stack.StepCompiledIn(&bIgnored);
	if (ParamCount > 3)
		Stack.StepCompiledIn(&bIgnored);

	auto InstigatingController = GetIndicatedComponentController(Context);
	if (InstigatingController)
	{
		for (int32 i = 0; i < FilterDatas.Num(); i++)
		{
			auto& FilterData = FilterDatas.Get(i, FIndicatedActorDataWithFilter::Size());
			auto Actors = FindActorsInFilterRadius(InstigatingController, FilterData);

			if (Actors.Num() > 0)
				MarkActorsStenciled(InstigatingController, Actors, FilterData.StenciledData, bAddAsUnique);

			Actors.Free();
		}
	}

	if (ComponentAddActorsInRadiusToStenciledListOG)
		return ComponentAddActorsInRadiusToStenciledListOG(Context, Stack);
}

void AFortPlayerControllerAthena::ComponentRemoveActorFromIndicatedList(UObject* Context, FFrame& Stack)
{
	AActor* IndicatedActor = nullptr;
	bool bIncludeSquad = false;

	Stack.StepCompiledIn(&IndicatedActor);
	Stack.StepCompiledIn(&bIncludeSquad);

	if (IndicatedActor)
		RemoveIndicatedEntriesWithSquad(GetIndicatedComponentController(Context), IndicatedActor, FString(), false, bIncludeSquad);

	if (ComponentRemoveActorFromIndicatedListOG)
		return ComponentRemoveActorFromIndicatedListOG(Context, Stack);
}

void AFortPlayerControllerAthena::ComponentRemoveActorFromStenciledList(UObject* Context, FFrame& Stack)
{
	AActor* StenciledActor = nullptr;
	bool bIncludeSquad = false;

	Stack.StepCompiledIn(&StenciledActor);
	Stack.StepCompiledIn(&bIncludeSquad);

	if (StenciledActor)
		RemoveStenciledEntriesWithSquad(GetIndicatedComponentController(Context), StenciledActor, FString(), false, bIncludeSquad);

	if (ComponentRemoveActorFromStenciledListOG)
		return ComponentRemoveActorFromStenciledListOG(Context, Stack);
}

void AFortPlayerControllerAthena::ComponentRemoveGroupFromIndicatedList(UObject* Context, FFrame& Stack)
{
	FString GroupIdentifier;
	bool bIncludeSquad = false;

	Stack.StepCompiledIn(&GroupIdentifier);
	Stack.StepCompiledIn(&bIncludeSquad);

	RemoveIndicatedEntriesWithSquad(GetIndicatedComponentController(Context), nullptr, GroupIdentifier, true, bIncludeSquad);

	if (ComponentRemoveGroupFromIndicatedListOG)
		return ComponentRemoveGroupFromIndicatedListOG(Context, Stack);
}

void AFortPlayerControllerAthena::ComponentRemoveGroupFromStenciledList(UObject* Context, FFrame& Stack)
{
	FString GroupIdentifier;
	bool bIncludeSquad = false;

	Stack.StepCompiledIn(&GroupIdentifier);
	Stack.StepCompiledIn(&bIncludeSquad);

	RemoveStenciledEntriesWithSquad(GetIndicatedComponentController(Context), nullptr, GroupIdentifier, true, bIncludeSquad);

	if (ComponentRemoveGroupFromStenciledListOG)
		return ComponentRemoveGroupFromStenciledListOG(Context, Stack);
}

void AFortPlayerControllerAthena::ServerAwardVehicleTrickPoints_(UObject* Context, FFrame& Stack)
{
	int32 InPoints;
	int32 InAirTimeX1000;
	int32 NumberOfTricks = 0;
	float AirDistance = 0.f;
	float AirHeight = 0.f;

	Stack.StepCompiledIn(&InPoints);
	Stack.StepCompiledIn(&InAirTimeX1000);
	Stack.StepCompiledIn(&NumberOfTricks);
	Stack.StepCompiledIn(&AirDistance);
	Stack.StepCompiledIn(&AirHeight);
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerAwardVehicleTrickPoints, InPoints, InAirTimeX1000, NumberOfTricks, AirDistance, AirHeight);

	FGameplayTagContainer TargetTags{};

	auto Interface = (IGameplayTagAssetInterface*)PlayerController->Pawn->GetInterface(IGameplayTagAssetInterface::StaticClass());
	if (Interface)
	{
		auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))Interface->Vft[0x2];
		GetOwnedGameplayTags(Interface, &TargetTags);
		//Interface->GetOwnedGameplayTags(&TargetTags);
	}

	static auto GetVehicleFunc = PlayerController->Pawn->GetFunction("GetVehicleActor");
	if (!GetVehicleFunc)
		GetVehicleFunc = PlayerController->Pawn->GetFunction("GetVehicle");
	auto Vehicle = PlayerController->Pawn->Call<AActor*>(GetVehicleFunc);

	auto VehicleInterface = (IGameplayTagAssetInterface*)Vehicle->GetInterface(IGameplayTagAssetInterface::StaticClass());
	if (VehicleInterface)
	{
		auto GetOwnedGameplayTags = (void(*)(IGameplayTagAssetInterface*, FGameplayTagContainer*))VehicleInterface->Vft[0x2];
		GetOwnedGameplayTags(VehicleInterface, &TargetTags);
		//Interface->GetOwnedGameplayTags(&TargetTags);
	}

	auto RealAirTime = (float)InAirTimeX1000 * 0.001f;
	PlayerController->GetQuestManager(1)->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetEarnVehicleTrickPoints(), InPoints, false, PlayerController, TargetTags);
	PlayerController->GetQuestManager(1)->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetVehicleAirTime(), (int)RealAirTime, false, PlayerController, TargetTags);

	TargetTags.GameplayTags.Free();
	TargetTags.ParentTags.Free();
}

void ServerRestartPlayer_(AFortPlayerControllerAthena* _this)
{
	if (!_this)
		return;

	if (IsTerminalManagedBot(_this))
	{
		auto PlayerState =
			IsUsableDeathObject(_this->PlayerState)
				? (AFortPlayerStateAthena*)_this->PlayerState
				: nullptr;
		if (PlayerState)
			InvalidateRespawnHandshake(PlayerState);

		SDK::DbgLog(
			"[Respawn] blocked terminal managed-bot restart "
			"controller=%p playerState=%p FN=%.2f\n",
			(void*)_this, (void*)PlayerState,
			VersionInfo.FortniteVersion);
		return;
	}

	PurgeExclusiveGadgets(_this);

	if (_this->Pawn)
		_this->UnPossess(_this->Pawn);

	auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;

	GameMode->RestartPlayer(_this);
}

void AFortPlayerControllerAthena::ServerOnMaterialSelection(UObject* Context, FFrame& Stack)
{
	EFortResourceType NewResourceType;
	uint8 NewResourceLevel;

	Stack.StepCompiledIn(&NewResourceType);
	Stack.StepCompiledIn(&NewResourceLevel);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	PlayerController->CurrentResourceType = NewResourceType;
	PlayerController->CurrentResourceLevel = NewResourceLevel;
}

struct FAthenaQuickChatLeafEntry
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FAthenaQuickChatLeafEntry);

	DEFINE_STRUCT_PROP(TeamCommType, uint8_t);
	DEFINE_STRUCT_PROP(EmojiItemDefinition, UFortItemDefinition*);
};

class UAthenaQuickChatBank : public UObject
{
public:
	UCLASS_COMMON_MEMBERS(UAthenaQuickChatBank);

	DEFINE_PROP(ChatOptions, TArray<FAthenaQuickChatLeafEntry>);
};

struct FAthenaQuickChatActiveEntry
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FAthenaQuickChatActiveEntry);
	uint8_t Pad[0x20];

	DEFINE_STRUCT_PROP(Index, int8);
	DEFINE_STRUCT_PROP(Bank, TWeakObjectPtr<UAthenaQuickChatBank>);
};

void AFortPlayerControllerAthena::ServerPlaySquadQuickChatMessage(UObject* Context, FFrame& Stack)
{
	auto& ChatEntry = Stack.StepCompiledInRef<FAthenaQuickChatActiveEntry>();
	auto& SenderID = Stack.StepCompiledInRef<FUniqueNetIdRepl>();
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	auto Bank = ChatEntry.Bank.Get();

	if (!Bank)
		return;

	if (!Bank->ChatOptions.IsValidIndex(ChatEntry.Index))
		return;

	auto& ChatOption = Bank->ChatOptions.Get(ChatEntry.Index, FAthenaQuickChatLeafEntry::Size());

	auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

	PlayerState->TeamMemberState = ChatOption.TeamCommType;
	PlayerState->ReplicatedTeamMemberState = ChatOption.TeamCommType;

	PlayerController->ServerPlayEmoteItem(ChatOption.EmojiItemDefinition, 0);

	PlayerState->OnRep_ReplicatedTeamMemberState();
}

void AFortPlayerControllerAthena::PostLoadHook()
{
	if (VersionInfo.FortniteVersion >= 27)
	{
		CanPlaceBuildableClassInStructuralGrid_ = FindCanPlaceBuildableClassInStructuralGrid();
		//InitializeBuildingActor_ = FindInitializeBuildingActor();
		//PostInitializeSpawnedBuildingActor_ = FindPostInitializeSpawnedBuildingActor();
	}
	CantBuild_ = FindCantBuild();
	ReplaceBuildingActor_ = FindReplaceBuildingActor(); // pre-cache building offsets
	RemoveFromAlivePlayers_ = FindRemoveFromAlivePlayers();
	GiveAbilityAndActivateOnce = FindGiveAbilityAndActivateOnce();
	CanAffordToPlaceBuildableClass_ = FindCanAffordToPlaceBuildableClass();
	PayBuildableClassPlacementCost_ = FindPayBuildableClassPlacementCost();
	InitializePlayerGameplayAbilities_ = FindInitializePlayerGameplayAbilities();
	SDK::DbgLog("  [FPC] 1 finds done\n");

	auto DefaultFortPC = DefaultObjImpl("FortPlayerController");
	SDK::DbgLog("  [FPC] 2 DefaultFortPC=%p\n", (void*)DefaultFortPC);

	Utils::Hook(FindGetPlayerViewPoint(), GetPlayerViewPoint, GetPlayerViewPointOG);
	SDK::DbgLog("  [FPC] 3 GetPlayerViewPoint hooked\n");
	// they only stripped it on athena for some reason
	//auto ServerAcknowledgePossessionIdx = GetDefaultObj()->GetFunction("ServerAcknowledgePossession")->GetVTableIndex();
	//Utils::Hook<AFortPlayerControllerAthena>(ServerAcknowledgePossessionIdx, DefaultFortPC->Vft[ServerAcknowledgePossessionIdx]);

	if (VersionInfo.FortniteVersion >= 11)
	{
        auto ServerRestartPlayerIdx = GetDefaultObj()->GetFunction("ServerRestartPlayer")->GetVTableIndex();
        auto DefaultFortPCZone = DefaultObjImpl("FortPlayerControllerZone");
        SDK::DbgLog("  [FPC] 4 ServerRestartPlayerIdx=0x%X DefaultFortPCZone=%p\n", ServerRestartPlayerIdx, (void*)DefaultFortPCZone);
        if (DefaultFortPCZone && ServerRestartPlayerIdx < 0x1000)
            Utils::Hook<AFortPlayerControllerAthena>(ServerRestartPlayerIdx, DefaultFortPCZone->Vft[ServerRestartPlayerIdx]);

		if (VersionInfo.FortniteVersion >= 15 && VersionInfo.FortniteVersion < 16)
			Utils::Hook(uint64_t(DefaultObjImpl("PlayerController")->Vft[ServerRestartPlayerIdx]), ServerRestartPlayer_);
	}
	SDK::DbgLog("  [FPC] 5 ServerRestartPlayer done\n");

	auto ServerSuicideIdx = GetDefaultObj()->GetFunction("ServerSuicide")->GetVTableIndex();
	auto DefaultFortPCZone = DefaultObjImpl("FortPlayerControllerZone");
	SDK::DbgLog("  [FPC] 6 ServerSuicideIdx=0x%X\n", ServerSuicideIdx);
	if (DefaultFortPCZone && ServerSuicideIdx < 0x1000)
		Utils::Hook<AFortPlayerControllerAthena>(ServerSuicideIdx, DefaultFortPCZone->Vft[ServerSuicideIdx]);

	//if (VersionInfo.FortniteVersion >= 11)
	//{
	if (VersionInfo.FortniteVersion < 11)
		ServerAttemptAircraftJumpVft = GetDefaultObj()->GetFunction("ServerAttemptAircraftJump")->GetVTableIndex();
	
	SDK::DbgLog("  [FPC] 7 ServerSuicide done, pre-aircraft\n");
	auto ServerAttemptAircraftJumpPC = GetDefaultObj()->GetFunction("ServerAttemptAircraftJump");
	if (!ServerAttemptAircraftJumpPC)
	{
		if (auto acc = DefaultObjImpl("FortControllerComponent_Aircraft"))
			Utils::ExecHook(acc->GetFunction("ServerAttemptAircraftJump"), ServerAttemptAircraftJump_, ServerAttemptAircraftJump_OG);
	}
	else
		Utils::ExecHook(ServerAttemptAircraftJumpPC, ServerAttemptAircraftJump_);
	//}
	SDK::DbgLog("  [FPC] 8 aircraft done\n");

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerAcknowledgePossession"), ServerAcknowledgePossession);
	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerExecuteInventoryItem"), ServerExecuteInventoryItem_);
	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerExecuteInventoryWeapon"), ServerExecuteInventoryWeapon); // S9 shenanigans

	// same as serveracknowledgepossession
	auto ServerReturnToMainMenuIdx = GetDefaultObj()->GetFunction("ServerReturnToMainMenu")->GetVTableIndex();
	if (DefaultFortPC && ServerReturnToMainMenuIdx < 0x1000)
		Utils::Hook<AFortPlayerControllerAthena>(ServerReturnToMainMenuIdx, DefaultFortPC->Vft[ServerReturnToMainMenuIdx]);
	SDK::DbgLog("  [FPC] 9 ServerReturnToMainMenu done\n");

	//if (VersionInfo.FortniteVersion != 1.72 && VersionInfo.FortniteVersion != 1.8)
	{
		Utils::ExecHook(GetDefaultObj()->GetFunction("ServerCreateBuildingActor"), ServerCreateBuildingActor);
		Utils::ExecHook(GetDefaultObj()->GetFunction("ServerBeginEditingBuildingActor"), ServerBeginEditingBuildingActor);
		Utils::ExecHook(GetDefaultObj()->GetFunction("ServerEditBuildingActor"), ServerEditBuildingActor);
		Utils::ExecHook(GetDefaultObj()->GetFunction("ServerEndEditingBuildingActor"), ServerEndEditingBuildingActor);
		Utils::ExecHook(GetDefaultObj()->GetFunction("ServerRepairBuildingActor"), ServerRepairBuildingActor);

	}
	auto ServerAttemptInventoryDropFn = GetDefaultObj()->GetFunction("ServerAttemptInventoryDrop");
	if (ServerAttemptInventoryDropFn)
		Utils::ExecHook(ServerAttemptInventoryDropFn, ServerAttemptInventoryDrop);
	else
		Utils::ExecHook(GetDefaultObj()->GetFunction("ServerSpawnInventoryDrop"), ServerAttemptInventoryDrop);

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerPlayEmoteItem"), ServerPlayEmoteItem_);
	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerPlaySprayItem"), ServerPlayEmoteItem_);

	auto ClientOnPawnDiedAddr = FindFunctionCall(L"ClientOnPawnDied", VersionInfo.EngineVersion == 4.16 ? std::vector<uint8_t>{ 0x48, 0x89, 0x54 } : (VersionInfo.FortniteVersion >= 24 && VersionInfo.FortniteVersion < 25 ? std::vector<uint8_t>{ 0x48, 0x8B, 0xC4 } : std::vector<uint8_t>{ 0x48, 0x89, 0x5C }));
	Utils::Hook(ClientOnPawnDiedAddr, ClientOnPawnDied, ClientOnPawnDiedOG);

	if (VersionInfo.FortniteVersion >= 16)
		Utils::ExecHook(GetDefaultObj()->GetFunction("ServerClientIsReadyToRespawn"), ServerClientIsReadyToRespawn);

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerCheat"), ServerCheat);

	auto InteractionComponentDefault =
		DefaultObjImpl("FortControllerComponent_Interaction");
	auto ServerAttemptInteractPC =
		GetDefaultObj()->GetFunction(
			"ServerAttemptInteract");
	auto ServerAttemptInteractComponent =
		InteractionComponentDefault
			? InteractionComponentDefault->GetFunction(
				"ServerAttemptInteract")
			: nullptr;
	auto ServerAttemptInteractFunction =
		VersionInfo.FortniteVersion == 27.11 &&
			ServerAttemptInteractComponent
			? ServerAttemptInteractComponent
			: (ServerAttemptInteractPC
				? ServerAttemptInteractPC
				: ServerAttemptInteractComponent);
	if (VersionInfo.FortniteVersion == 27.11)
	{
		GServerAttemptInteractFunction27_11 =
			ServerAttemptInteractFunction;
		GServerAttemptInteractSchema27_11 =
			HasServerAttemptInteractSchema27_11(
				ServerAttemptInteractFunction);
	}
	if (ServerAttemptInteractFunction &&
		ServerAttemptInteractFunction->ExecFunction !=
			reinterpret_cast<void*>(
				ServerAttemptInteract_))
	{
		Utils::ExecHook(
			ServerAttemptInteractFunction,
			ServerAttemptInteract_,
			ServerAttemptInteract_OG);
	}

	if (VersionInfo.FortniteVersion == 27.11 &&
		InteractionComponentDefault)
	{
		constexpr uintptr_t
			InteractValidateLongEnableRva27_11 =
				0x108D6700;
		auto ModuleBase = reinterpret_cast<uintptr_t>(
			GetModuleHandleW(nullptr));
		auto ExpectedValidateLongCVar =
			reinterpret_cast<uint8*>(
				ModuleBase +
				InteractValidateLongEnableRva27_11);
		auto FoundValidateLongCVar =
			FindCVar<uint8>(
				L"Interact.ValidateLong.Enable");
		uint8 InitialValidateLongValue = uint8(-1);
		if (FoundValidateLongCVar ==
				ExpectedValidateLongCVar &&
			IsWritableInteractionMemory(
				FoundValidateLongCVar,
				sizeof(uint8)))
		{
			memcpy(
				&InitialValidateLongValue,
				FoundValidateLongCVar,
				sizeof(InitialValidateLongValue));
			if (InitialValidateLongValue <= 1)
			{
				GInteractValidateLongEnable27_11 =
					FoundValidateLongCVar;
			}
		}

		auto StartLongUseFunction =
			InteractionComponentDefault->GetFunction(
				"ServerNotifyStartLongUse");
		auto EndLongUseFunction =
			InteractionComponentDefault->GetFunction(
				"ServerNotifyEndLongUse");
		const bool bStartSchemaValid =
			HasServerNotifyStartLongUseSchema27_11(
				StartLongUseFunction);
		const bool bEndSchemaValid =
			HasServerNotifyEndLongUseSchema27_11(
				EndLongUseFunction);

		bool bStartHooked = false;
		if (bStartSchemaValid &&
			StartLongUseFunction->ExecFunction !=
				reinterpret_cast<void*>(
					ServerNotifyStartLongUse27_11))
		{
			Utils::ExecHook(
				StartLongUseFunction,
				ServerNotifyStartLongUse27_11,
				GServerNotifyStartLongUse27_11OG);
		}
		bStartHooked =
			StartLongUseFunction &&
			StartLongUseFunction->ExecFunction ==
				reinterpret_cast<void*>(
					ServerNotifyStartLongUse27_11);

		bool bEndHooked = false;
		if (bEndSchemaValid &&
			EndLongUseFunction->ExecFunction !=
				reinterpret_cast<void*>(
					ServerNotifyEndLongUse27_11))
		{
			Utils::ExecHook(
				EndLongUseFunction,
				ServerNotifyEndLongUse27_11,
				GServerNotifyEndLongUse27_11OG);
		}
		bEndHooked =
			EndLongUseFunction &&
			EndLongUseFunction->ExecFunction ==
				reinterpret_cast<void*>(
					ServerNotifyEndLongUse27_11);

		SDK::DbgLog(
			"  [FPC] 27.11 revive long-use hooks "
			"startSchema=%d startHooked=%d "
			"endSchema=%d endHooked=%d "
			"attemptSchema=%d attempt=%p "
			"validateCVar=%p initial=%u\n",
			(int)bStartSchemaValid,
			(int)bStartHooked,
			(int)bEndSchemaValid,
			(int)bEndHooked,
			(int)GServerAttemptInteractSchema27_11,
			(void*)GServerAttemptInteractFunction27_11,
			(void*)GInteractValidateLongEnable27_11,
			InitialValidateLongValue);
	}

	Utils::ExecHook(
		GetDefaultObj()->GetFunction("ServerDropAllItems"),
		ServerDropAllItems, ServerDropAllItemsOG);

	auto DefaultWeaponComp = DefaultObjImpl("FortWeaponComponent");

	if (VersionInfo.FortniteVersion >= 14.00)
	{
		auto OnUnEquipAddr = FindFunctionCall(L"K2_OnUnEquip", std::vector<uint8_t>{ 0x48, 0x89, 0x5C });

		Utils::Hook(OnUnEquipAddr, OnUnEquip, OnUnEquipOG);

		//Utils::ExecHook(DefaultWeaponComp->GetFunction("OnUnEquip"), OnUnEquip);
	}

	auto DefaultHeldObjComp = DefaultObjImpl("FortHeldObjectComponent");

	if (DefaultHeldObjComp)
	{
		Utils::ExecHook(DefaultHeldObjComp->GetFunction("PickupHeldObject"), PickupHeldObject);
		Utils::ExecHook(DefaultHeldObjComp->GetFunction("DropHeldObject"), DropHeldObject);
		Utils::ExecHook(DefaultHeldObjComp->GetFunction("PlaceHeldObject"), PlaceHeldObject);
		Utils::ExecHook(DefaultHeldObjComp->GetFunction("ThrowHeldObject"), ThrowHeldObject);
	}

	Utils::ExecHook(GetDefaultObj()->GetFunction("SpawnToyInstance"), SpawnToyInstance);
	Utils::Hook(FindEnterAircraft(), EnterAircraft, EnterAircraftOG);

	if (wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2")) // on 24.20+ u get killed for going into water without this
		Utils::ExecHook(GetDefaultObj()->GetFunction("ServerTeleportToPlaygroundLobbyIsland"), ServerTeleportToPlaygroundLobbyIsland);

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerCraftSchematic"), ServerCraftSchematic);
	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerGiveCreativeItem"), ServerGiveCreativeItem);

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerRequestSeatChange"), ServerRequestSeatChange_, ServerRequestSeatChange_OG);
	if (VersionInfo.FortniteVersion >= 30.00 &&
		VersionInfo.FortniteVersion < 31.00)
	{
		auto* VehicleModSeatRpc =
			GetDefaultObj()->GetFunction(
				"ServerVehicleModSeatRpc");
		if (IsCompatibleVehicleModSeatRpc(
			VehicleModSeatRpc))
		{
			Utils::ExecHook(
				VehicleModSeatRpc,
				ServerVehicleModSeatRpc_,
				ServerVehicleModSeatRpc_OG);
			SDK::DbgLog(
				"[VehicleMods] installed FN30 "
				"ServerVehicleModSeatRpc hook function=%p\n",
				VehicleModSeatRpc);
		}
		else
		{
			SDK::DbgLog(
				"[VehicleMods] skipped incompatible FN30 "
				"ServerVehicleModSeatRpc function=%p\n",
				VehicleModSeatRpc);
		}
	}

	//Utils::ExecHook(GetDefaultObj()->GetFunction("ServerLoadingScreenDropped"), ServerLoadingScreenDropped_, ServerLoadingScreenDropped_OG);

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerCreativeSetFlightSpeedIndex"), ServerCreativeSetFlightSpeedIndex);
	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerCreativeSetFlightSprint"), ServerCreativeSetFlightSprint);
	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerAwardVehicleTrickPoints"), ServerAwardVehicleTrickPoints_, ServerAwardVehicleTrickPoints_OG);

	auto DefaultIndicatedActorLibrary = DefaultObjImpl("FortIndicatedActorManagementLibrary");

	if (DefaultIndicatedActorLibrary)
	{
		Utils::ExecHook(DefaultIndicatedActorLibrary->GetFunction("AddActorsToIndicatedList"), AddActorsToIndicatedList, AddActorsToIndicatedListOG);
		Utils::ExecHook(DefaultIndicatedActorLibrary->GetFunction("AddActorsToStenciledList"), AddActorsToStenciledList, AddActorsToStenciledListOG);
		Utils::ExecHook(DefaultIndicatedActorLibrary->GetFunction("AddActorsInRadiusToIndicatedList"), AddActorsInRadiusToIndicatedList, AddActorsInRadiusToIndicatedListOG);
		Utils::ExecHook(DefaultIndicatedActorLibrary->GetFunction("AddActorsInRadiusToStenciledList"), AddActorsInRadiusToStenciledList, AddActorsInRadiusToStenciledListOG);
		Utils::ExecHook(DefaultIndicatedActorLibrary->GetFunction("RemoveActorFromIndicatedList"), RemoveActorFromIndicatedList, RemoveActorFromIndicatedListOG);
		Utils::ExecHook(DefaultIndicatedActorLibrary->GetFunction("RemoveActorFromStenciledList"), RemoveActorFromStenciledList, RemoveActorFromStenciledListOG);
		Utils::ExecHook(DefaultIndicatedActorLibrary->GetFunction("RemoveGroupFromIndicatedList"), RemoveGroupFromIndicatedList, RemoveGroupFromIndicatedListOG);
		Utils::ExecHook(DefaultIndicatedActorLibrary->GetFunction("RemoveGroupFromStenciledList"), RemoveGroupFromStenciledList, RemoveGroupFromStenciledListOG);
	}

	auto DefaultIndicatedActorComponent = DefaultObjImpl("FortControllerComponent_IndicatedActorManagement");

	if (DefaultIndicatedActorComponent)
	{
		Utils::ExecHook(DefaultIndicatedActorComponent->GetFunction("AddActorsToIndicatedList"), ComponentAddActorsToIndicatedList, ComponentAddActorsToIndicatedListOG);
		Utils::ExecHook(DefaultIndicatedActorComponent->GetFunction("AddActorsToStenciledList"), ComponentAddActorsToStenciledList, ComponentAddActorsToStenciledListOG);
		// Radius variants only exist on newer builds (Ch3+); ExecHook no-ops when the function is absent.
		Utils::ExecHook(DefaultIndicatedActorComponent->GetFunction("AddActorsInRadiusToIndicatedList"), ComponentAddActorsInRadiusToIndicatedList, ComponentAddActorsInRadiusToIndicatedListOG);
		Utils::ExecHook(DefaultIndicatedActorComponent->GetFunction("AddActorsInRadiusToStenciledList"), ComponentAddActorsInRadiusToStenciledList, ComponentAddActorsInRadiusToStenciledListOG);
		Utils::ExecHook(DefaultIndicatedActorComponent->GetFunction("RemoveActorFromIndicatedList"), ComponentRemoveActorFromIndicatedList, ComponentRemoveActorFromIndicatedListOG);
		Utils::ExecHook(DefaultIndicatedActorComponent->GetFunction("RemoveActorFromStenciledList"), ComponentRemoveActorFromStenciledList, ComponentRemoveActorFromStenciledListOG);
		Utils::ExecHook(DefaultIndicatedActorComponent->GetFunction("RemoveGroupFromIndicatedList"), ComponentRemoveGroupFromIndicatedList, ComponentRemoveGroupFromIndicatedListOG);
		Utils::ExecHook(DefaultIndicatedActorComponent->GetFunction("RemoveGroupFromStenciledList"), ComponentRemoveGroupFromStenciledList, ComponentRemoveGroupFromStenciledListOG);
	}

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerOnMaterialSelection"), ServerOnMaterialSelection);
	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerPlaySquadQuickChatMessage"), ServerPlaySquadQuickChatMessage);

	auto DefaultMarkerComp = DefaultObjImpl("AthenaMarkerComponent");
	if (DefaultMarkerComp)
		Utils::ExecHook(DefaultMarkerComp->GetFunction("ServerAddMapMarker"), ServerAddMapMarker, ServerAddMapMarkerOG);
}
