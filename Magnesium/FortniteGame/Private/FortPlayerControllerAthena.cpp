#include "pch.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortGameMode.h"
#include "../../Erbium/PlayerAI/Public/MagnesiumPlayerAIIntegration.h"
#include "../../Erbium/PlayerAI/Public/AIDebugLogger.h"
#include "../../Erbium/PlayerAI/Public/PlayerAIManager.h"
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
#include "../../Erbium/Public/GUI.h"
#include "../Public/FortAthenaCreativePortal.h"
#include "../Public/FortInventory.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../Erbium/Public/Misc.h"
#include "../Public/FortControllerComponent_VictoryCrowns.h"

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

// A guided missile's controller Pawn fields are not reliable on legacy builds:
// Season 4 can leave both of them pointing at the character for the entire
// remote-control session. This lifecycle map is populated by the confirmed
// remote-pawn acknowledgement and cleared by the native return acknowledgement.
static std::unordered_map<AFortPlayerControllerAthena*, AActor*> GRemoteControlReturnPawn;

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
	if (bRemoteControlActive || IsRemoteControlledPawn(ControlledPawn) ||
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
// Keep them distinct from both real players and the managed PlayerAI system.
static std::unordered_set<AFortPlayerControllerAthena*> GSpawnedBotControllers;
// If 1.7.2 native pawn-death handling leaves a synthetic controller in
// AlivePlayers, its native removal fallback must be attempted at most once.
static std::unordered_set<AFortPlayerControllerAthena*> G172SpawnedBotRemovalAttempts;
struct FPendingSpawnedBotCleanup
{
	TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
	TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
	TWeakObjectPtr<AFortPlayerStateAthena> PlayerState;
	TWeakObjectPtr<AFortInventory> Inventory;
	float RemainingSeconds = 3.f;
};
static std::vector<FPendingSpawnedBotCleanup> GPendingSpawnedBotCleanup;
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
	static auto RemoteControlledPawnClass = FindClass("FortRemoteControlledPawnAthena");
	return RemoteControlledPawnClass;
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

extern uint64_t ApplyCharacterCustomization;
uint64_t InitializePlayerGameplayAbilities_;

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

	const bool bPendingLateGameAircraftLoadout =
		GPendingLateGameAircraftLoadout.erase(PlayerController) > 0;
	const auto LastAcknowledgedPawn = GLastAcknowledgedPawn.find(PlayerController);
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
		return;

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

		if (FConfiguration::bDisableJumpFatigue && FortPawn->HasCharacterMovement())
		{
			auto MovementCompAthena = (UFortMovementComp_CharacterAthena*)(FortPawn->GetCharacterMovement());
			if (MovementCompAthena->HasJumpPenaltyResetTime())
				MovementCompAthena->JumpPenaltyResetTime = 0.0f;
		}
	}

	static auto IsRespawningAllowedFunc = GameState->GetFunction("IsRespawningAllowed");

	bool bRespawnAllowed = false;

	if (!IsRespawningAllowedFunc)
	{
		auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;

		bRespawnAllowed = Playlist
			? (Playlist->HasRespawnType() ? Playlist->RespawnType > 0 : FConfiguration::bForceRespawns)
			: FConfiguration::bForceRespawns;
	}
	else
		bRespawnAllowed = GameState->Call<bool>(IsRespawningAllowedFunc, PlayerController->PlayerState);

	bRespawnAllowed |= FConfiguration::bForceRespawns;

	// Inventory count is not a lifecycle signal: death may legitimately leave a
	// respawning player with zero entries. Track whether this controller has
	// already acknowledged a pawn instead, so an empty-inventory respawn still
	// exits the death/spectating state and restores usable equipment.
	const bool bRestoringRespawnPawn = !bPendingLateGameAircraftLoadout &&
		bNewAcknowledgedPawn && bRespawnAllowed && PlayersInitialized.contains(PlayerController);
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
	if (bRespawnAllowed && !bSkipRespawn && TryGetConfiguredRespawnLocation(GameMode, ConfiguredRespawnLocation))
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

		auto Wood = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData");
		auto Stone = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData");
		auto Metal = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData");

		PlayerController->WorldInventory->GiveItem(RifleAmmo, Medium(rng));
		PlayerController->WorldInventory->GiveItem(Shells, ShellAmmo(rng));
		PlayerController->WorldInventory->GiveItem(LightAmmo, Light(rng));
		PlayerController->WorldInventory->GiveItem(HeavyAmmo, Heavy(rng));
		PlayerController->WorldInventory->GiveItem(RocketAmmo, 12);

		PlayerController->WorldInventory->GiveItem(Wood, Mats(rng));
		PlayerController->WorldInventory->GiveItem(Stone, Mats(rng));
		PlayerController->WorldInventory->GiveItem(Metal, Mats(rng));
	}

	if (wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Respawn/Playlist_Respawn_Solo.Playlist_Respawn_Solo") && VersionInfo.FortniteVersion == 30.00 && GUI::SelectedPlaylist == static_cast<int>(Playlist::Boxfight))
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

		auto Wood = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData");
		auto Stone = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData");
		auto Metal = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData");

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

		PlayerController->WorldInventory->GiveItem(Wood, 500);
		PlayerController->WorldInventory->GiveItem(Stone, 500);
		PlayerController->WorldInventory->GiveItem(Metal, 500);
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

	auto Interface = PlayerController->PlayerState->GetInterface(IFortAbilitySystemInterface::StaticClass());
	if (InitializePlayerGameplayAbilities_ && Interface)
	{
		auto InitializePlayerGameplayAbilities = (void (*&)(const IInterface*))InitializePlayerGameplayAbilities_;

		InitializePlayerGameplayAbilities(Interface);

		if (FConfiguration::bDisableJumpFatigue && FortPawn->HasCharacterMovement())
		{
			auto MovementCompAthena = (UFortMovementComp_CharacterAthena*)(FortPawn->GetCharacterMovement());
			if (MovementCompAthena->HasJumpPenaltyResetTime())
				MovementCompAthena->JumpPenaltyResetTime = 0.0f;
		}
	}
	else if (!bRestoringRespawnPawn)
		for (auto& AbilitySet : AFortGameMode::AbilitySets)
			PlayerController->PlayerState->AbilitySystemComponent->GiveAbilitySet(AbilitySet);

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
			FortPawn->SetShield(100.f);
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

		auto Interface = PlayerController->PlayerState->GetInterface(IFortAbilitySystemInterface::StaticClass());
		if (InitializePlayerGameplayAbilities_ && Interface)
		{
			auto InitializePlayerGameplayAbilities = (void(*&)(const IInterface*))InitializePlayerGameplayAbilities_;

			InitializePlayerGameplayAbilities(Interface);
		}
		else
			for (auto& AbilitySet : AFortGameMode::AbilitySets)
				PlayerController->PlayerState->AbilitySystemComponent->GiveAbilitySet(AbilitySet);

	}
	if (FConfiguration::bLateGame && (bPendingLateGameAircraftLoadout || Num != 0) &&
		(!FConfiguration::bKeepInventory || FConfiguration::bLateGame))
	{
		if (bPendingLateGameAircraftLoadout || !PlayersInitialized.contains(PlayerController))
		{
			PlayersInitialized.insert(PlayerController);

			FortPawn->SetShield(100.f);

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

		if (FConfiguration::bDisableJumpFatigue && FortPawn->HasCharacterMovement())
		{
			auto MovementCompAthena = (UFortMovementComp_CharacterAthena*)(FortPawn->GetCharacterMovement());
			if (MovementCompAthena->HasJumpPenaltyResetTime())
				MovementCompAthena->JumpPenaltyResetTime = 0.0f;
		}
	}

	// Mark every successful first possession, including Num == 0. Previously
	// this happened only in the late-game/non-empty-inventory branch, causing a
	// later empty respawn to be mistaken for another initial possession.
	PlayersInitialized.insert(PlayerController);

	// Very last thing: if this ack is a "possess" command takeover, put the pawn
	// on the ground and give it health. Nothing above overrode it because this
	// runs after the native possession setup and the ability re-init that reset
	// them.
	if (GFinalizePossessTakeover.erase(PlayerController) > 0)
		FinalizePossessedPawnForCommand(PlayerController, FortPawn);
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

	if (VersionInfo.FortniteVersion >= 11.00 || FConfiguration::bLateGame)
	{
		static auto bIsComp = Context->IsA(FindClass("FortControllerComponent_Aircraft"));
		if (bIsComp)
			PlayerController = (AFortPlayerControllerAthena*)((UActorComponent*)Context)->GetOwner();
		else
			PlayerController = (AFortPlayerControllerAthena*)Context;

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

		ServerAttemptAircraftJumpOG((AFortPlayerControllerAthena*)Context, Rotation);
	}
	
	if (FConfiguration::bLateGame)
	{
		PlayerController->MyFortPawn->SetShield(100.f);
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

		if (FConfiguration::bDisableJumpFatigue && PlayerController->MyFortPawn->HasCharacterMovement())
		{
			auto MovementCompAthena = (UFortMovementComp_CharacterAthena*)(PlayerController->MyFortPawn->GetCharacterMovement());
			if (MovementCompAthena->HasJumpPenaltyResetTime())
				MovementCompAthena->JumpPenaltyResetTime = 0.0f;
		}
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

	// Season 4 special gadgets carry their own server execution path. The
	// Infinity Gauntlet uses it to perform the original Thanos transformation;
	// treating it as an ordinary weapon only equips the gauntlet mesh/abilities.
	if (VersionInfo.FortniteVersion >= 4.0 && VersionInfo.FortniteVersion <= 4.5)
	{
		if (auto Gadget = RealDef->Cast<UFortGadgetItemDefinition>())
		{
			auto ItemInstance = PlayerController->WorldInventory->Inventory.ItemInstances.Search(
				[&](UFortWorldItem* Item)
				{
					return Item && Item->ItemEntry.ItemGuid == ItemGuid;
				});

			if (ItemInstance && *ItemInstance && Gadget->ServerExecute((UFortItem*)*ItemInstance, PlayerController))
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
	if (!PlayerController)
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

	PlayerController->MyFortPawn->EquipWeaponDefinition(ItemDefinition, entry->ItemGuid, entry->HasTrackerGuid() ? entry->TrackerGuid : FGuid(), false);

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
	if (_this->Role == 3 && (!_this->EditingPlayer || !NewEditingPlayer))
	{
		_this->SetNetDormancy(2 - (NewEditingPlayer != 0));
		_this->ForceNetUpdate();

		auto EditingPlayer = _this->EditingPlayer;
		if (EditingPlayer)
		{
			auto Handle = EditingPlayer->Owner;

			if (Handle)
				if (auto PlayerController = Handle->Cast<AFortPlayerControllerAthena>())
				{
					_this->EditingPlayer = NewEditingPlayer;
					_this->OnRep_EditingPlayer();
					return;
				}
		}
		else
		{
			if (!NewEditingPlayer)
			{
				_this->EditingPlayer = NewEditingPlayer;
				_this->OnRep_EditingPlayer();
				return;
			}

			auto Handle = NewEditingPlayer->Owner;

			if (auto PlayerController = Handle->Cast<AFortPlayerControllerAthena>())
			{
				_this->EditingPlayer = NewEditingPlayer;
				_this->OnRep_EditingPlayer();
			}
		}
	}
}

void AFortPlayerControllerAthena::ServerBeginEditingBuildingActor(UObject* Context, FFrame& Stack)
{
	ABuildingSMActor* Building;
	Stack.StepCompiledIn(&Building);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	if (!PlayerController || !PlayerController->MyFortPawn || !Building->IsA<ABuildingSMActor>() /* || Building->Team != static_cast<AFortPlayerStateAthena*>(PlayerController->PlayerState)->TeamIndex*/)
		return;

	AFortPlayerStateAthena* PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;
	if (!PlayerState)
		return;
	
	SetEditingPlayer(Building, PlayerState);
	
	if (!PlayerController->MyFortPawn->CurrentWeapon->IsA<AFortWeap_EditingTool>())
	{
		auto EditToolEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
			{
				return entry.ItemDefinition->Class == UFortEditToolItemDefinition::StaticClass();
			}, FFortItemEntry::Size());

		PlayerController->MyFortPawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)EditToolEntry->ItemDefinition, EditToolEntry->ItemGuid, EditToolEntry->HasTrackerGuid() ? EditToolEntry->TrackerGuid : FGuid(), false);
	}

	if (auto EditTool = PlayerController->MyFortPawn->CurrentWeapon->Cast<AFortWeap_EditingTool>())
	{
		EditTool->EditActor = Building;
		EditTool->OnRep_EditActor();
	}
}

uint64_t ReplaceBuildingActor_ = 0;
uint64_t InitializeBuildingActor_ = 0;
uint64_t PostInitializeSpawnedBuildingActor_ = 0;
void AFortPlayerControllerAthena::ServerEditBuildingActor(UObject* Context, FFrame& Stack)
{
	ABuildingSMActor* Building;
	TSubclassOf<AActor> NewClass;
	uint8 RotationIterations;
	bool bMirrored;
	Stack.StepCompiledIn(&Building);
	Stack.StepCompiledIn(&NewClass);
	Stack.StepCompiledIn(&RotationIterations);
	Stack.StepCompiledIn(&bMirrored);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (!PlayerController || !Building || !NewClass || !Building->IsA<ABuildingSMActor>() || !CanBePlacedByPlayer(NewClass) || Building->EditingPlayer != PlayerController->PlayerState || Building->bDestroyed)
	{
		return;
	}

	SetEditingPlayer(Building, nullptr);

	auto ReplaceBuildingActor = (ABuildingSMActor * (*&)(ABuildingSMActor*, unsigned int, TSubclassOf<AActor>, unsigned int, int, bool, AFortPlayerControllerAthena*)) ReplaceBuildingActor_;
	auto ReplaceBuildingActor__New = (ABuildingSMActor * (*&)(ABuildingSMActor*, unsigned int, TSubclassOf<AActor>&, unsigned int, int, bool, AFortPlayerControllerAthena*)) ReplaceBuildingActor_;

	ABuildingSMActor* NewBuild;
	
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
	ABuildingSMActor* Building;
	Stack.StepCompiledIn(&Building);
	Stack.IncrementCode();

	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	if (!PlayerController || !PlayerController->MyFortPawn || !Building || !Building->IsA<ABuildingSMActor>() || Building->EditingPlayer != PlayerController->PlayerState/* || Building->Team != static_cast<AFortPlayerStateAthena*>(PlayerController->PlayerState)->TeamIndex*/ || Building->bDestroyed)
		return;

	SetEditingPlayer(Building, nullptr);

	/*if (VersionInfo.EngineVersion >= 4.24)
	{
		auto EditToolEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
			{
				return entry.ItemDefinition->Class == UFortEditToolItemDefinition::StaticClass();
			}, FFortItemEntry::Size());

		PlayerController->MyFortPawn->EquipWeaponDefinition((UFortWeaponItemDefinition*)EditToolEntry->ItemDefinition, EditToolEntry->ItemGuid, EditToolEntry->HasTrackerGuid() ? EditToolEntry->TrackerGuid : FGuid(), false);
	}*/

	auto EditToolEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
		{
			return entry.ItemDefinition->Class == UFortEditToolItemDefinition::StaticClass();
		}, FFortItemEntry::Size());

	if (!EditToolEntry)
		return;

	auto EditToolPtr = PlayerController->Pawn->CurrentWeaponList.Search([&](AActor* Weapon__Uncasted)
		{ return ((AFortWeapon*)Weapon__Uncasted)->ItemEntryGuid == EditToolEntry->ItemGuid; });

	if (!EditToolPtr)
		return;

	if (auto EditTool = *(AFortWeap_EditingTool**)EditToolPtr)
	{
		EditTool->EditActor = nullptr;
		EditTool->OnRep_EditActor();
	}
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
	FGuid Guid;
	int32 Count;
	bool bTrash = false; // this only exists on some newer builds
	Stack.StepCompiledIn(&Guid);
	Stack.StepCompiledIn(&Count);
	Stack.StepCompiledIn(&bTrash);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	if (!PlayerController || !PlayerController->Pawn)
		return;

	auto ItemP = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
		{ return entry->ItemEntry.ItemGuid == Guid; });
	auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
		{ return entry.ItemGuid == Guid; }, FFortItemEntry::Size());
	if (!ItemP)
		return;
	auto Item = *ItemP;

	itemEntry->Count -= Count;

	FVector FinalLoc = PlayerController->Pawn->K2_GetActorLocation();

	static auto WID_Launcher_Petrol = FindObject<UFortWorldItemDefinition>(L"/Game/Athena/Items/Weapons/Prototype/WID_Launcher_Petrol.WID_Launcher_Petrol");

	if (itemEntry->ItemDefinition == WID_Launcher_Petrol)
	{
		static auto BGA_Petrol_PickupClass = FindObject<UClass>(L"/Game/Athena/Items/Weapons/Prototype/PetrolPump/BGA_Petrol_Pickup.BGA_Petrol_Pickup_C");

		AActor* PetrolPickup = UWorld::SpawnActor<AActor>(BGA_Petrol_PickupClass, FinalLoc);

		PlayerController->WorldInventory->Remove(Guid);
		PlayerController->WorldInventory->Update(itemEntry);

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

	AFortInventory::SpawnPickup(PlayerController->Pawn->K2_GetActorLocation() + PlayerController->Pawn->GetActorForwardVector() * 70.f + FVector(0, 0, 50), *itemEntry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetUnset(), PlayerController->MyFortPawn, Count, true, true, true, nullptr, FinalLoc);
	if (itemEntry->Count <= 0 || Count < 0)
		PlayerController->WorldInventory->Remove(Guid);
	else
	{
		Item->ItemEntry.Count = itemEntry->Count;
		PlayerController->WorldInventory->UpdateEntry(*itemEntry);
		Item->ItemEntry.bIsDirty = true;
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
	if (!Item || Item->Object != Object || (Item->Flags & 0x20))
		return false;

	return Object->Class && !IsBadReadPtr(Object->Class);
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
		GSpawnedBotControllers.contains(Controller) ||
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
	if (GSpawnedBotControllers.contains(PlayerController))
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
	if (VersionInfo.FortniteVersion != 2.50 || !PlayerController)
		return;

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

	GPendingSpawnedBotCleanup.push_back({
		TWeakObjectPtr<AFortPlayerControllerAthena>(PlayerController),
		TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn),
		TWeakObjectPtr<AFortPlayerStateAthena>(PlayerState),
		TWeakObjectPtr<AFortInventory>(Inventory),
		3.f
	});

	SDK::DbgLog("[Elimination] scheduled 2.50 spawnbot cleanup controller=%p pawn=%p playerState=%p inventory=%p\n",
		(void*)PlayerController, (void*)Pawn, (void*)PlayerState,
		(void*)Inventory);
}

static bool IsControllerInAlivePlayers(
	AFortGameMode* GameMode,
	AFortPlayerControllerAthena* PlayerController)
{
	if (!GameMode || !PlayerController || !IsUsableDeathObject(GameMode))
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
	if (FConfiguration::bForceRespawns ||
		!bWaitingPostMatch)
	{
		return;
	}

	auto WinnerController =
		IsUsableDeathObject(ReportedKillerController) &&
		!GSpawnedBotControllers.contains(ReportedKillerController)
		? ReportedKillerController : nullptr;
	if (!WinnerController && ReportedKillerPawn &&
		IsUsableDeathObject(ReportedKillerPawn) &&
		IsUsableDeathObject(ReportedKillerPawn->Controller))
	{
		auto PawnController =
			ReportedKillerPawn->Controller->Cast<AFortPlayerControllerAthena>();
		if (PawnController &&
			!GSpawnedBotControllers.contains(PawnController))
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
			if (!Candidate || GSpawnedBotControllers.contains(Candidate))
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
		GSpawnedBotControllers.contains(Controller) ||
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
		FConfiguration::bForceRespawns ||
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

void AFortPlayerControllerAthena::ClientOnPawnDied(AFortPlayerControllerAthena* PlayerController, FFortPlayerDeathReport& DeathReport)
{
	if (!PlayerController)
		return ClientOnPawnDiedOG(PlayerController, DeathReport);

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

	RestoreRespawnHiddenWeapon(PlayerController);
	GPendingRespawnLandingFinalization.erase(
		PlayerController);
	GRespawnSkydivingObserved.erase(
		PlayerController);
	GPendingLegacyAircraftLandingEquipment.erase(
		PlayerController);
	GLegacyAircraftSkydivingObserved.erase(
		PlayerController);

	// Allow the next replacement pawn to receive exactly one respawn setup
	// even if the allocator happens to reuse the dead pawn's address.
	GLastAcknowledgedPawn.erase(PlayerController);

	auto World = UWorld::GetWorld();
	auto GameMode = World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
	auto GameState = GameMode ? (AFortGameStateAthena*)GameMode->GameState : nullptr;
	auto PlayerState = PlayerController->HasPlayerState() ? (AFortPlayerStateAthena*)PlayerController->PlayerState : nullptr;

	if (!GameMode || !GameState || !PlayerState)
		return ClientOnPawnDiedOG(PlayerController, DeathReport);

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
		GSpawnedBotControllers.contains(PlayerController);
	const bool bIsFinal172SpawnedBotDeath =
		bIs172SpawnedBotVictim &&
		(!PlayerController->Pawn || !PlayerController->Pawn->IsDBNO()) &&
		!FConfiguration::bForceRespawns;
	const bool bCanAttempt172SpawnedBotRemoval =
		bIsFinal172SpawnedBotDeath &&
		G172SpawnedBotRemovalAttempts.insert(PlayerController).second;

	if (PlayerController->WorldInventory && PlayerController->Pawn && ((PlayerController->Pawn->HasbShouldDropItemsOnDeath() ? PlayerController->Pawn->bShouldDropItemsOnDeath : true) && !FConfiguration::bKeepInventory))
	{
		std::vector<FGuid> DroppedItemGuids;
		for (int i = 0; i < PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num(); i++)
		{
			auto& entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

			if (entry.ItemDefinition->HasbCanBeDropped() ? entry.ItemDefinition->bCanBeDropped : (entry.ItemDefinition->GetPickupComponent() ? entry.ItemDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false))
			{
				AFortInventory::SpawnPickup(PlayerController->Pawn->K2_GetActorLocation(), entry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetPlayerElimination(), PlayerController->MyFortPawn);
				DroppedItemGuids.push_back(entry.ItemGuid);
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
	PurgeExclusiveGadgets(PlayerController);

	auto KillerPlayerState = DeathReport.HasKillerPlayerState() ? (AFortPlayerStateAthena*)DeathReport.KillerPlayerState : nullptr;
	auto KillerPawn = DeathReport.HasKillerPawn() ? (AFortPlayerPawnAthena*)DeathReport.KillerPawn : nullptr;
	if (!IsUsableDeathObject(KillerPlayerState))
		KillerPlayerState = nullptr;
	if (!IsUsableDeathObject(KillerPawn))
		KillerPawn = nullptr;

	auto KillerPlayerController = (KillerPlayerState && KillerPlayerState->HasOwner() && IsUsableDeathObject(KillerPlayerState->Owner)) ? KillerPlayerState->Owner->Cast<AFortPlayerControllerAthena>() : nullptr;

	// PlayerAI victim diagnostic: kill credit for the killer flows from this
	// report - a missing killer here explains "no credit for killing AI".
	if (MagnesiumPlayerAIIntegration::IsPlayerAIController(PlayerController))
		AIDebugLogger::Log("Elimination", "AI death report: killerPS %d, killerPawn %d, killerCtrl %d",
			KillerPlayerState ? 1 : 0, KillerPawn ? 1 : 0,
			(KillerPawn && KillerPawn->Controller) ? 1 : 0);
	FGameplayTagContainer EmptyDeathTags{};
	auto& DeathTags = DeathReport.HasTags() ? DeathReport.Tags : EmptyDeathTags;

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
				PlayerState->DeathInfo.DeathTags = /*DeathReport.Tags*/ PlayerController->Pawn ? *(FGameplayTagContainer*)(__int64(&PlayerController->Pawn->MoveSoundStimulusBroadcastInterval) + (VersionInfo.FortniteVersion >= 11 && VersionInfo.FortniteVersion < 18 ? 0x18 : 0x10)) : FGameplayTagContainer();
			if (FDeathInfo::HasDeathClassSlot())
				PlayerState->DeathInfo.DeathClassSlot = -1;
			PlayerState->DeathInfo.DeathCause = ToDeathCause(PlayerController->Pawn, DeathTags, PlayerState->DeathInfo.bDBNO);
			//PlayerState->DeathInfo.Downer = KillerPlayerState;
			if (FDeathInfo::HasFinisherOrDowner())
				PlayerState->DeathInfo.FinisherOrDowner = KillerPlayerState ? KillerPlayerState : PlayerState;
			if (FDeathInfo::HasFinisherOrDownerTags())
				PlayerState->DeathInfo.FinisherOrDownerTags =
					KillerPawn
						? KillerPawn->GameplayTags
						: (PlayerController->Pawn
							? PlayerController->Pawn
								->GameplayTags
							: FGameplayTagContainer{});
			if (FDeathInfo::HasVictimTags())
				PlayerState->DeathInfo.VictimTags =
					PlayerController->Pawn
						? PlayerController->Pawn
							->GameplayTags
						: FGameplayTagContainer{};
			if (FDeathInfo::HasDistance())
				PlayerState->DeathInfo.Distance = PlayerController->Pawn ? (PlayerState->DeathInfo.DeathCause != /*EDeathCause::FallDamage*/ 1 ? (KillerPawn ? KillerPawn->GetDistanceTo(PlayerController->Pawn) : 0) : (PlayerController->MyFortPawn->HasLastFallDistance() ? PlayerController->MyFortPawn->LastFallDistance : 0)) : 0;
			if (FDeathInfo::HasbInitialized())
				PlayerState->DeathInfo.bInitialized = true;
			PlayerState->OnRep_DeathInfo();
		}

		if (PlayerController->Pawn &&
			KillerPlayerState && KillerPawn &&
			KillerPawn->Controller &&
			KillerPawn->Controller != PlayerController)
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
		}

		if ((GUI::IsArenaPlaylist() || GUI::IsTournamentPlaylist()) && VersionInfo.FortniteVersion < 20.40) // crashes on 20.40, test other versions
		{
			KillerPlayerState->ClientReportTournamentStatUpdate();
		}

		static auto IsRespawningAllowedFunc = GameState->GetFunction("IsRespawningAllowed");

		bool bRespawnAllowed = false;

		if (!IsRespawningAllowedFunc)
		{
			auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;

			// respawn except storm needs to be fixed
			bRespawnAllowed = Playlist ? (Playlist->HasRespawnType() ? Playlist->RespawnType > 0 : FConfiguration::bForceRespawns) : FConfiguration::bForceRespawns;
		}
		else
			bRespawnAllowed = GameState->Call<bool>(IsRespawningAllowedFunc, PlayerState);

		bRespawnAllowed |= FConfiguration::bForceRespawns;

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
					ClientOnPawnDiedOG(PlayerController, DeathReport);
					bCalledNativeDeathEarly = true;
				}

				((void (*)(AFortGameMode*, AFortPlayerControllerAthena*, AFortPlayerStateAthena*, AFortPlayerPawnAthena*, UFortItemDefinition*, uint8, char))RemoveFromAlivePlayers_)(GameMode, PlayerController, KillerPlayerState == PlayerState ? nullptr : KillerPlayerState, KillerPawn, DamageCauserWeaponData, PlayerState->HasDeathInfo() ? PlayerState->DeathInfo.DeathCause : 0, 0);

				if (VersionInfo.FortniteVersion == 2.50 &&
					GSpawnedBotControllers.erase(PlayerController) > 0)
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

		if ((FConfiguration::bSiphon && FConfiguration::SiphonAmount > 0) && PlayerController->Pawn && KillerPlayerState && KillerPlayerState->AbilitySystemComponent && KillerPawn && KillerPawn->Controller != PlayerController)
		{
			auto Health = KillerPawn->GetHealth();
			auto Shield = KillerPawn->GetShield();

			if (Health == 100)
			{
				Shield += FConfiguration::SiphonAmount;
			}
			else if (Health + FConfiguration::SiphonAmount > 100)
			{
				float Overflow = (Health + FConfiguration::SiphonAmount) - 100;
				Health = 100;
				Shield += Overflow;
			}
			else if (Health + FConfiguration::SiphonAmount <= 100)
			{
				Health += FConfiguration::SiphonAmount;
			}

			KillerPawn->SetHealth(Health);
			KillerPawn->SetShield(Shield);

			static auto WoodItemData = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/WoodItemData.WoodItemData");
			static auto StoneItemData = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/StoneItemData.StoneItemData");
			static auto MetalItemData = FindObject<UFortItemDefinition>(L"/Game/Items/ResourcePickups/MetalItemData.MetalItemData");

			KillerPlayerController->WorldInventory->GiveItem(WoodItemData, FConfiguration::SiphonAmount);
			KillerPlayerController->WorldInventory->GiveItem(StoneItemData, FConfiguration::SiphonAmount);
			KillerPlayerController->WorldInventory->GiveItem(MetalItemData, FConfiguration::SiphonAmount);

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

	if (VersionInfo.FortniteVersion < 6)
	{
		if (GameState->GamePhase > 2)
		{
			if (GameMode->bAllowSpectateAfterDeath)
			{
				PlayerController->PlayerToSpectateOnDeath = KillerPawn ? KillerPawn : (GameMode->AlivePlayers.Num() > 0 ? ((AFortPlayerControllerAthena*)GameMode->AlivePlayers[rand() % GameMode->AlivePlayers.Num()])->Pawn : nullptr);

				UKismetSystemLibrary::K2_SetTimer(PlayerController, FString(L"SpectateOnDeath"), 5.f, false);
			}
		}
	}

	if (bIsLateSeasonBotVictim)
	{
		LateSeasonFinishingWeapon =
			GetWeaponDataSafe(ResolveDeathReportWeapon(DeathReport));
		if (PlayerState->HasDeathInfo())
			LateSeasonDeathCause = PlayerState->DeathInfo.DeathCause;
	}

	if (!bCalledNativeDeathEarly)
		ClientOnPawnDiedOG(PlayerController, DeathReport);

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
			GSpawnedBotControllers.erase(PlayerController);
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

	// Run once more after native death handling, which may restore persistent
	// gadget items as part of its pawn-replacement teardown.
	PurgeExclusiveGadgets(PlayerController);
}

void AFortPlayerControllerAthena::ServerClientIsReadyToRespawn(UObject* Context, FFrame& Stack)
{
	Stack.IncrementCode();

	auto PlayerController = (AFortPlayerControllerAthena*)Context;

	auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
	auto GameState = (AFortGameStateAthena*)GameMode->GameState;
	auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;

	if (PlayerState->RespawnData.bRespawnDataAvailable && PlayerState->RespawnData.bServerIsReady)
	{
		PurgeExclusiveGadgets(PlayerController);
		auto OldPawn = PlayerController->MyFortPawn;
		ResetLowerSeasonStormStateForRespawn(PlayerController, OldPawn, nullptr);

		auto RespawnData = PlayerState->RespawnData;
		FTransform SpawnTransform{};

		FQuat Rotation = PlayerState->RespawnData.RespawnRotation;
		SpawnTransform.Translation = PlayerState->RespawnData.RespawnLocation;
		SpawnTransform.Rotation = Rotation;

		FVector ConfiguredRespawnLocation{};
		if (TryGetConfiguredRespawnLocation(GameMode, ConfiguredRespawnLocation))
			SpawnTransform.Translation = ConfiguredRespawnLocation;

		auto Scale = FVector(1, 1, 1);
		SpawnTransform.Scale3D = Scale;

		auto NewPawn = GameMode->SpawnDefaultPawnAtTransform(PlayerController, SpawnTransform);
		PlayerController->Possess(NewPawn);
		PlayerController->RespawnPlayerAfterDeath(true);

		NewPawn->SetHealth(100.f);
		NewPawn->SetShield(FConfiguration::bLateGame ? 100.f : 0.f);

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

			if (FConfiguration::bDisableJumpFatigue && NewPawn->HasCharacterMovement())
			{
				auto MovementCompAthena = (UFortMovementComp_CharacterAthena*)(NewPawn->GetCharacterMovement());
				if (MovementCompAthena->HasJumpPenaltyResetTime())
					MovementCompAthena->JumpPenaltyResetTime = 0.0f;
			}
		}

		if (FConfiguration::bDisableJumpFatigue && NewPawn->HasCharacterMovement())
		{
			auto MovementCompAthena = (UFortMovementComp_CharacterAthena*)(NewPawn->GetCharacterMovement());
			if (MovementCompAthena->HasJumpPenaltyResetTime())
				MovementCompAthena->JumpPenaltyResetTime = 0.0f;
		}

		auto Interface = PlayerController->PlayerState->GetInterface(IFortAbilitySystemInterface::StaticClass());

		if (InitializePlayerGameplayAbilities_ && Interface)
		{
			auto InitializePlayerGameplayAbilities = (void(*&)(const IInterface*))InitializePlayerGameplayAbilities_;

			InitializePlayerGameplayAbilities(Interface);
		}

		ResetLowerSeasonStormStateForRespawn(PlayerController, OldPawn, NewPawn);
		// On the earliest clients, wait for the observed landing transition
		// before equipping. Performing the server equip here and again through
		// the legacy quickbar produces competing CurrentWeapon actors.
		if (!UsesEarlyAthenaLandingClientRefresh())
			RestoreEquipmentAfterRespawn(PlayerController);
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


					AFortInventory::SpawnPickup(Pawn->K2_GetActorLocation() + Pawn->GetActorForwardVector() * 70.f + FVector(0, 0, 50), *itemEntry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetUnset(), MyFortPawn, -1, true, true, true, nullptr, FinalLoc);
					WorldInventory->Remove(((AFortWeapon*)MyFortPawn->CurrentWeapon)->ItemEntryGuid);
					auto Item = WorldInventory->GiveItem(*PickupEntry, PickupEntry->Count, true);
					ServerExecuteInventoryItem(Item->ItemEntry.ItemGuid);
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
		auto ShortNames = Misc::ItemNames.find(ItemArg.c_str());

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
static void ClientForceViewTargetForCommand(AFortPlayerControllerAthena* PC, AActor* Target)
{
	if (!PC || !Target)
		return;

	static auto Fn = PC->GetFunction("ClientSetViewTarget");

	if (!Fn)
		return;

	uint8_t Params[0x40];
	memset(Params, 0, sizeof(Params));
	*(AActor**)(Params + 0) = Target;

	PC->ProcessEvent(Fn, Params);
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
	ClientForceViewTargetForCommand(PC, (AActor*)Pawn);

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
	ClientForceViewTargetForCommand(PC, (AActor*)NewPawn);

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

		if (Pawn->HasbCanBeDamaged() && !Pawn->bCanBeDamaged)
			continue;

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
				TryForceKillPawnForNuke(Pawn, InstigatorController, DamageCauser);
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
	for (int CleanupIndex =
		static_cast<int>(GPendingSpawnedBotCleanup.size()) - 1;
		CleanupIndex >= 0; --CleanupIndex)
	{
		auto& Cleanup = GPendingSpawnedBotCleanup[CleanupIndex];
		Cleanup.RemainingSeconds -= DeltaSeconds;
		if (Cleanup.RemainingSeconds > 0.f)
			continue;

		auto PlayerController = Cleanup.Controller.Get();
		auto Pawn = Cleanup.Pawn.Get();
		auto PlayerState = Cleanup.PlayerState.Get();
		auto Inventory = Cleanup.Inventory.Get();

		if (PlayerController)
		{
			GSpawnedBotControllers.erase(PlayerController);
			GPendingRespawnLandingFinalization.erase(PlayerController);
			GRespawnSkydivingObserved.erase(PlayerController);
			GPendingLegacyAircraftLandingEquipment.erase(PlayerController);
			GLegacyAircraftSkydivingObserved.erase(PlayerController);
			GRespawnHiddenWeapons.erase(PlayerController);
			GLastAcknowledgedPawn.erase(PlayerController);
			GRemoteControlReturnPawn.erase(PlayerController);
			GPendingLateGameAircraftLoadout.erase(PlayerController);
			PlayersInitialized.erase(PlayerController);
		}

		// Destroy the synthetic actor graph together after one complete legacy
		// elimination effect. Keeping the controller/PlayerState alive is what
		// leaves its health timers and drone beam repeating indefinitely.
		if (IsUsableDeathObject(Pawn))
			Pawn->K2_DestroyActor();
		if (IsUsableDeathObject(Inventory))
			Inventory->K2_DestroyActor();
		if (IsUsableDeathObject(PlayerState))
			PlayerState->K2_DestroyActor();
		if (IsUsableDeathObject(PlayerController))
			PlayerController->K2_DestroyActor();

		SDK::DbgLog("[Elimination] completed 2.50 spawnbot cleanup controller=%p pawn=%p playerState=%p inventory=%p\n",
			(void*)PlayerController, (void*)Pawn, (void*)PlayerState,
			(void*)Inventory);
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

void AFortPlayerControllerAthena::ServerCheat(UObject* Context, FFrame& Stack)
{
	FString Msg;
	Stack.StepCompiledIn(&Msg);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
	auto GameState = (AFortGameStateAthena*)GameMode->GameState;

	auto originalCommand = Msg.ToString();
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
cheat god - Toggles god mode
cheat godall - Toggles god mode for all players
cheat speed <scale> - Sets the player's movement speed
cheat size <scale> | size <X> <Y> <Z> - Resizes your pawn (uniform or per-axis)
cheat timeofday <hour> - Sets the time of day (0-23)
cheat pausetimeofday - Pauses/Unpauses the time of day
cheat spawnbot <count> <weapon> <s[size] | s[X,Y,Z]> [X Y Z] - Spawns a player bot at your or a specified location (WIP)
cheat tpbot - Teleports the player bot to your location
cheat delbot - Removes every spawned player bot (PlayerAI is left alone)
cheat dumppawns - Lists every player pawn with its index and owner
cheat dumpge - Lists every loaded gameplay effect with its index and class
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
cheat giveall - Gives you all ammo, mats, and traps
cheat givetraps - Gives you all available traps
cheat giveammo - Gives you 999 of every ammo type
cheat givemats - Gives you 500 of each material
cheat spawnpickup <WID/path> <Count = 1> [X Y Z] - Spawns a pickup at your player's or specified location
cheat lootrain <Count = 20> <Radius = 600> <TierGroup = Loot_AthenaTreasure> - Rains loot drops around you
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

		auto& IP = PlayerController->PlayerState->GetSavedNetworkAddress();
		auto IPStr = IP.ToString();

		if (FConfiguration::bEnableCheats || (!FConfiguration::bEnableCheats && IPStr == "127.0.0.1" && GUI::SelectedPlaylist != static_cast<int>(Playlist::OnlyUp)) || IPStr == "26.235.221.122")
		{
			if (command == "startaircraft")
			{
				if (UFortGameStateComponent_BattleRoyaleGamePhaseLogic::GetDefaultObj())
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
				FConfiguration::bInfiniteAmmo ^= 1;
			else if (command == "infinitemats")
				FConfiguration::bInfiniteMats ^= 1;
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
				auto Pawn = PlayerController->Pawn;
				auto PlayerState = PlayerController->PlayerState;

				bool bUseMin = false;

				float MinValue = bUseMin ? 1.f : Pawn->GetMaxHealth();
				auto& Health = PlayerController->MyFortPawn->HealthSet->Health;

				float MaxHealth = Pawn->GetMaxHealth();
				float MaxShield = Pawn->GetMaxShield();

				if (args.size() > 1)
				{
					std::string FullCommand = args[1].c_str();
					std::transform(FullCommand.begin(), FullCommand.end(), FullCommand.begin(), tolower);

					if (FullCommand == "min" || FullCommand == "minimum")
					{
						bUseMin = true;
					}
					else if (FullCommand == "check" || FullCommand == "c")
					{
						if (Health.Minimum == MinValue || PlayerController->Pawn->bCanBeDamaged == true)
						{
							PlayerController->ClientMessage(FString(L"You currently have god mode **ENABLED**."), FName(), 1);
							return;
						}
						else
						{
							PlayerController->ClientMessage(FString(L"You currently have god mode **DISABLED**."), FName(), 1);
							return;
						}
					}
				}

				if (VersionInfo.FortniteVersion >= 21)
				{
					PlayerController->Pawn->bCanBeDamaged ^= 1;
					PlayerController->ClientMessage(FString(L"Toggled god mode!"), FName(), 1.f);

					if (PlayerController->Pawn->bCanBeDamaged == 0)
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
					}

					return;
				}
				else if (Health.Minimum != MinValue)
				{
					Health.Minimum = MinValue;
					PlayerController->MyFortPawn->HealthSet->OnRep_Health(Health);

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

					if (bUseMin)
						PlayerController->ClientMessage(FString(L"Minimum Health God mode enabled!"), FName(), 1);
					else
						PlayerController->ClientMessage(FString(L"God mode enabled!"), FName(), 1);
				}
				else
				{
					Health.Minimum = 0.f;
					PlayerController->MyFortPawn->HealthSet->OnRep_Health(Health);

					PlayerController->ClientMessage(FString(L"God mode disabled!"), FName(), 1);
				}
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
						auto& Health = PC->MyFortPawn->HealthSet->Health;
						float MinValue = Pawn->GetMaxHealth();

						if (Health.Minimum != MinValue)
						{
							Health.Minimum = MinValue;
							PC->MyFortPawn->HealthSet->OnRep_Health(Health);

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
							PC->MyFortPawn->HealthSet->OnRep_Health(Health);
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
						FConfiguration::bUseVersionizedLoadout = true;
						LateGame::EquipLoadout(PlayerController);
						FConfiguration::bUseVersionizedLoadout = false;
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
								FConfiguration::bUseCustomLoadout = true;
								LateGame::EquipLoadout(PlayerController);
								FConfiguration::bUseCustomLoadout = false;
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

				auto DeadPlayerState = PlayerController->PlayerState;

				if (!DeadPlayerState)
				{
					PlayerController->ClientMessage(FString(L"Could not find a player state!"), FName(), 1.f);
					return;
				}

				if (Pawn->IsDBNO())
				{
					auto ASC = DeadPlayerState->AbilitySystemComponent;

					auto& Items = ASC->ActivatableAbilities.Items;

					for (int i = Items.Num() - 1; i >= 0; i--)
					{
						auto& Spec = Items.Get(i, FGameplayAbilitySpec::Size());

						if (!Spec.Ability)
							continue;

						if (Spec.Ability->IsA(UGAB_AthenaDBNO_C::StaticClass()))
						{
							ASC->ServerCancelAbility(Spec.Handle, Spec.ActivationInfo);
							ASC->ClientCancelAbility(Spec.Handle, Spec.ActivationInfo);
						}
					}

					auto& Effects = ASC->ActiveGameplayEffects.GameplayEffects_Internal;

					for (int i = Effects.Num() - 1; i >= 0; i--)
					{
						auto& Effect = Effects.Get(i, FActiveGameplayEffect::Size());

						if (!Effect.Spec.Def)
							continue;

						auto EffectName = Effect.Spec.Def->Name.ToString();

						if (EffectName.find("DBNO") != std::string::npos || EffectName.find("Downed") != std::string::npos)
						{
							Effects.Remove(i, FActiveGameplayEffect::Size());
						}
					}

					Pawn->bIsDBNO = false;
					if (Pawn->HasbPlayedDying())
						Pawn->bPlayedDying = false;
					if (Pawn->HasbIsDying())
						Pawn->bIsDying = false;

					auto MaxHealth = Pawn->GetMaxHealth();
					Pawn->SetHealth(MaxHealth);
					Pawn->OnRep_IsDBNO();

					PlayerController->ClientOnPawnRevived(PlayerController->PlayerState);
					PlayerController->RespawnPlayerAfterDeath(false);

					PlayerController->ClientMessage(FString(L"Player revived!"), FName(), 1.f);
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
				if (args.size() < 2)
				{
					PlayerController->ClientMessage(FString(L"Please include a phrase/name you'd want to change to!"), FName(), 1);
					return;
				}

				std::string nameStr;

				for (size_t i = 1; i < args.size(); i++)
				{
					nameStr += std::string(args[i].begin(), args[i].end());
					if (i + 1 < args.size())
						nameStr += " ";
				}

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
				static bool bIsPaused = false;

				float Speed = bIsPaused ? 1.f : 0.f;

				UFortKismetLibrary::SetTimeOfDaySpeed(UWorld::GetWorld(), Speed);

				if (bIsPaused)
					PlayerController->ClientMessage(FString(L"Unpaused time of day!"), FName(), 1.f);
				else
					PlayerController->ClientMessage(FString(L"Paused time of day!"), FName(), 1.f);

				bIsPaused ^= 1;
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
					PlayerController->ClientMessage(FString(L"Please provide a custom Gameplay Effect."), FName(), 1.f);
					return;
				}

				auto PlayerState = PlayerController->PlayerState;
				auto Pawn = PlayerController->Pawn;

				if (UAbilitySystemComponent* AbilitySystemComponent = PlayerState->AbilitySystemComponent)
				{
					auto GEClass = FindObject<UClass>(UEAllocatedWString(args[1].begin(), args[1].end()).c_str());

					if (!GEClass)
						GEClass = FindClass(args[1].c_str());

					if (!GEClass)
					{
						PlayerController->ClientMessage(FString(L"Could not find a Gameplay Effect."), FName(), 1.f);
						return;
					}

					if (GEClass)
					{
						FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();

						Context.Instigator = PlayerController;
						Context.Causer = Pawn;
						Context.AddSourceObject(Pawn);

						AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(GEClass, 1.0f, Context);
						PlayerController->ClientMessage(FString(L"Applied Gameplay Effect!"), FName(), 1.f);
					}
				}
			}
			else if (command == "removege")
			{
				auto PlayerState = PlayerController->PlayerState;

				auto ASC = PlayerState->AbilitySystemComponent;

				auto& Effects = ASC->ActiveGameplayEffects.GameplayEffects_Internal;

				for (int i = Effects.Num() - 1; i >= 0; i--)
				{
					auto& Effect = Effects.Get(i, FActiveGameplayEffect::Size());

					if (!Effect.Spec.Def)
						continue;

					auto EffectName = Effect.Spec.Def->Name.ToString();

					Effects.Remove(i, FActiveGameplayEffect::Size());
					PlayerController->ClientMessage(FString(L"Removed all Gameplay Effects!"), FName(), 1.f);
				}
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
				FConfiguration::bKeepInventory ^= 1;
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
				try { NewTOD = std::stof(std::string(args[1])); }
				catch (...) {}

				UFortKismetLibrary::SetTimeOfDay(UWorld::GetWorld(), NewTOD);
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
						continue;

					PC->Possess(Pawn);
					PC->MyFortPawn = Pawn; // dont't ask, crashes on 27+

					auto PlayerState = (AFortPlayerStateAthena*)UWorld::SpawnActor(AFortPlayerStateAthena::StaticClass(), Transform);

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
						memset((PBYTE)Member, 0, FGameMemberInfo::Size());

						Member->MostRecentArrayReplicationKey = -1;
						Member->ReplicationID = -1;
						Member->ReplicationKey = -1;
						Member->TeamIndex = PlayerState->TeamIndex;
						Member->SquadId = PlayerState->SquadId;
						Member->MemberUniqueId = PlayerState->UniqueId;

						GameState->GameMemberInfoArray.Members.Add(*Member, FGameMemberInfo::Size());
						GameState->GameMemberInfoArray.MarkItemDirty(*Member);

						auto NotifyGameMemberAdded = (void(*)(AFortGameStateAthena*, uint8_t, uint8_t, FUniqueNetIdRepl*)) NotifyGameMemberAdded_;
						if (NotifyGameMemberAdded)
							NotifyGameMemberAdded(GameState, Member->SquadId, Member->TeamIndex, &Member->MemberUniqueId);

						free(Member);
					}

					for (auto& AbilitySet : AFortGameMode::AbilitySets)
						PlayerState->AbilitySystemComponent->GiveAbilitySet(AbilitySet);

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
					if (UsesTrackedLegacySpawnedBotLifecycle())
						GSpawnedBotControllers.insert(PC);

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
						Car->SetFuel(100.f);

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

					if (FConfiguration::bEnableCheats)
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

				FName TierGroup = Loot_AthenaTreasure;

				if (!TierGroupArg.empty())
				{
					auto TierGroupWide = UEAllocatedWString(TierGroupArg.begin(), TierGroupArg.end());
					TierGroup = FName(TierGroupWide.c_str());
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
					PlayerController->ClientMessage(FString(L"Found no loot for that tier group. Check the name (e.g. Loot_AthenaTreasure, Loot_AthenaFloorLoot)."), FName(), 1.f);
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
				auto GameplayEffectClass = UGameplayEffect::StaticClass();

				if (!GameplayEffectClass)
				{
					PlayerController->ClientMessage(FString(L"GameplayEffect class was not found on this version."), FName(), 1.f);
					return;
				}

				std::vector<const UObject*> GameplayEffects;
				const int ObjectCount = SDK::TUObjectArray::Num();

				for (int i = 0; i < ObjectCount; i++)
				{
					auto Object = SDK::TUObjectArray::GetObjectByIndex(i);

					if (!Object || !SDK::MemReadable((void*)Object, 0x40) ||
						!Object->Class || !SDK::MemReadable(Object->Class, 0x40))
						continue;

					if (Object->IsA(GameplayEffectClass))
						GameplayEffects.push_back(Object);
				}

				wchar_t wcount[80];
				swprintf_s(wcount, 80, L"Found %zu loaded gameplay effects:", GameplayEffects.size());
				PlayerController->ClientMessage(FString(wcount), FName(), 1.f);

				for (size_t i = 0; i < GameplayEffects.size(); i++)
				{
					auto Effect = GameplayEffects[i];
					std::string Line = "[" + std::to_string(i) + "] " +
						std::string(Effect->Name.ToString().c_str());

					if (Effect->Class)
						Line += " (" + std::string(Effect->Class->GetName().ToString().c_str()) + ")";

					auto Message = std::wstring(Line.begin(), Line.end());
					PlayerController->ClientMessage(FString(Message.c_str()), FName(), 1.f);
				}
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
				bool bTargetIsFortPawn = IsFortPlayerPawnForCommand(TargetPawn);

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
					GSpawnedBotControllers.erase(BotPC);
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

			if (GUI::SelectedPlaylist == static_cast<int>(Playlist::OnlyUp))
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

extern bool bDidntFind;
void AFortPlayerControllerAthena::ServerAttemptInteract_(UObject* Context, FFrame& Stack)
{
	AActor* ReceivingActor = *(AActor**)Stack.Locals;
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

	AFortPlayerControllerAthena* PlayerController = nullptr;

	static auto bIsComp = Context->IsA(FindClass("FortControllerComponent_Interaction"));
	if (bIsComp)
		PlayerController = (AFortPlayerControllerAthena*)((UActorComponent*)Context)->GetOwner();
	else
		PlayerController = (AFortPlayerControllerAthena*)Context;

	auto Pawn = (AFortPlayerPawnAthena*)PlayerController->Pawn;

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
	else if (auto Vehicle = ReceivingActor->Cast<AFortAthenaVehicle>())
	{
		ServerAttemptInteract_OG(Context, Stack);
		sendStat();

		UFortVehicleSeatWeaponComponent* SeatWeaponComponent = nullptr;

		if (Vehicle)
			SeatWeaponComponent = (UFortVehicleSeatWeaponComponent*)Vehicle->GetComponentByClass(UFortVehicleSeatWeaponComponent::StaticClass());
		else if (auto CharacterVehicle = Pawn->Cast<AFortCharacterVehicle>())
			SeatWeaponComponent = (UFortVehicleSeatWeaponComponent*)CharacterVehicle->GetComponentByClass(UFortVehicleSeatWeaponComponent::StaticClass());

		if (SeatWeaponComponent)
		{
			UFortVehicleSeatComponent* SeatComponent = nullptr;

			if (Vehicle)
				SeatComponent = (UFortVehicleSeatComponent*)Vehicle->GetComponentByClass(UFortVehicleSeatComponent::StaticClass());
			else if (auto CharacterVehicle = Pawn->Cast<AFortCharacterVehicle>())
				SeatComponent = (UFortVehicleSeatComponent*)CharacterVehicle->GetComponentByClass(UFortVehicleSeatComponent::StaticClass());

			auto SeatIdx = SeatComponent->FindSeatIndex(Pawn);
			UFortWeaponItemDefinition* Weapon = nullptr;

			for (int i = 0; i < SeatWeaponComponent->WeaponSeatDefinitions.Num(); i++)
			{
				auto& WeaponDefinition = SeatWeaponComponent->WeaponSeatDefinitions.Get(i, FWeaponSeatDefinition::Size());

				if (WeaponDefinition.SeatIndex != SeatIdx)
					continue;

				Weapon = WeaponDefinition.VehicleWeapon;
				break;
			}

			if (Weapon)
			{
				printf("Weapon: %s\n", Weapon->Name.ToString().c_str());
				auto Item = PlayerController->WorldInventory->GiveItem(Weapon, 1, AFortInventory::GetStats(Weapon)->ClipSize);
				auto ItemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
					{ return entry.ItemDefinition == Weapon; }, FFortItemEntry::Size());

				auto OldWeapon = Pawn->CurrentWeapon;

				PlayerController->ServerExecuteInventoryItem(ItemEntry->ItemGuid);
				PlayerController->ClientEquipItem(ItemEntry->ItemGuid, true);
				if (Pawn->HasPreviousWeapon())
					Pawn->PreviousWeapon = OldWeapon;

				auto Weapon = (AFortWeapon*)Pawn->CurrentWeapon;

				if (Weapon)
				{
					printf("[Vehicles] Setting up MountedWeaponInfoRepped\n");
					auto RepWeaponInfo = (FMountedWeaponInfoRepped*)malloc(FMountedWeaponInfoRepped::Size());

					if (RepWeaponInfo->HasHostVehicleCached())
					{
						RepWeaponInfo->HostVehicleCached.ObjectPointer = Vehicle;
						RepWeaponInfo->HostVehicleCached.InterfacePointer = Vehicle->GetInterface(IFortVehicleInterface::StaticClass());
					}
					else
						RepWeaponInfo->HostVehicleCachedActor = Vehicle;
					RepWeaponInfo->HostVehicleSeatIndexCached = SeatIdx;

					static auto DualClass = FindClass("FortWeaponRangedDualForVehicle");

					if (Weapon->IsA(DualClass))
					{
						static auto MountedWeaponInfoReppedOff = Weapon->GetOffset("MountedWeaponInfoRepped");
						static auto OnRep_MountedWeaponInfoRepped = Weapon->GetFunction("OnRep_MountedWeaponInfoRepped");
						*(FMountedWeaponInfoRepped*)(__int64(Weapon) + MountedWeaponInfoReppedOff) = *RepWeaponInfo;
						Weapon->Call(OnRep_MountedWeaponInfoRepped);
					}
					else
					{
						static auto MountedWeaponInfoReppedOff = Weapon->GetOffset("MountedWeaponInfoRepped");
						static auto OnRep_MountedWeaponInfoRepped = Weapon->GetFunction("OnRep_MountedWeaponInfoRepped");
						*(FMountedWeaponInfoRepped*)(__int64(Weapon) + MountedWeaponInfoReppedOff) = *RepWeaponInfo;
						Weapon->Call(OnRep_MountedWeaponInfoRepped);
					}

					free(RepWeaponInfo);
				}
			}
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

	ServerAttemptInteract_OG(Context, Stack);
	sendStat();
	//return bIsComp ? (void)callOG(((UFortControllerComponent_Interaction*)Context), Stack.GetCurrentNativeFunction(), ServerAttemptInteract, ReceivingActor, InteractComponent, InteractType, OptionalObjectData, InteractionBeingAttempted, RequestID) : callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerAttemptInteract, ReceivingActor, InteractComponent, InteractType, OptionalObjectData, InteractionBeingAttempted, RequestID);
}

void AFortPlayerControllerAthena::ServerDropAllItems(UObject* Context, FFrame& Stack)
{
	UFortItemDefinition* IgnoreItemDef;

	Stack.StepCompiledIn(&IgnoreItemDef);
	Stack.IncrementCode();
	auto PlayerController = (AFortPlayerControllerAthena*)Context;
	printf("ServerDropAllItems[Ignore %s]\n", IgnoreItemDef ? IgnoreItemDef->Name.ToString().c_str() : nullptr);

	auto Loc = PlayerController->MyFortPawn->K2_GetActorLocation();
	for (int i = 0; i < PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num(); i++)
	{
		auto& Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

		if (Entry.ItemDefinition != IgnoreItemDef && (Entry.ItemDefinition->HasbCanBeDropped() ? Entry.ItemDefinition->bCanBeDropped : (Entry.ItemDefinition->GetPickupComponent() ? Entry.ItemDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false)))
		{
			AFortInventory::SpawnPickup(Loc, Entry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetUnset(), PlayerController->MyFortPawn);
			PlayerController->WorldInventory->Remove(Entry.ItemGuid);
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

			PlayerController->ClientEquipItem(Weapon->ItemEntryGuid, true);
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

			PlayerController->ClientEquipItem(Weapon->ItemEntryGuid, true);
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
	AFortPlayerControllerAthena* PlayerController = nullptr;

	static auto bIsComp = Object->IsA(FindClass("FortControllerComponent_Aircraft"));
	if (bIsComp)
		PlayerController = (AFortPlayerControllerAthena*)((UActorComponent*)Object)->GetOwner();
	else
		PlayerController = (AFortPlayerControllerAthena*)Object;

	// PlayerAI entities are never real aircraft passengers: the native
	// aircraft kills anyone still flagged aboard when the drop zone ends,
	// and the jump RPC rejects connectionless controllers on old versions.
	// Their bus ride + skydive is handled by the PlayerAI transport flow.
	if (MagnesiumPlayerAIIntegration::IsPlayerAIController(PlayerController))
		return;

	if (!FConfiguration::bKeepInventory && PlayerController->WorldInventory)
	{
		UEAllocatedVector<FGuid> GuidsToRemove;
		for (int i = 0; i < PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num(); i++)
			{
			auto& Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

			if (Entry.ItemDefinition->HasbCanBeDropped() ? Entry.ItemDefinition->bCanBeDropped : (Entry.ItemDefinition->GetPickupComponent() ? Entry.ItemDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false))
			{
				//NewPlayer->WorldInventory->Inventory.ReplicatedEntries.Remove(i, FFortItemEntry::Size());
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

	return EnterAircraftOG(Object, Aircraft);
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

	auto Pawn = PlayerController->Pawn;

	static auto GetVehicleFunc = Pawn->GetFunction("GetVehicleActor");
	if (!GetVehicleFunc)
		GetVehicleFunc = Pawn->GetFunction("GetVehicle");
	auto Vehicle = Pawn->Call<AActor*>(GetVehicleFunc);

	if (!Vehicle && Pawn->IsA<AFortCharacterVehicle>())
		Vehicle = Pawn;

	if (!Vehicle)
		return callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerRequestSeatChange, TargetSeatIndex);

	UFortVehicleSeatWeaponComponent* SeatWeaponComponent = (UFortVehicleSeatWeaponComponent*)Vehicle->GetComponentByClass(UFortVehicleSeatWeaponComponent::StaticClass());
	if (!SeatWeaponComponent)
		return callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerRequestSeatChange, TargetSeatIndex);

	UFortVehicleSeatComponent* SeatComponent = (UFortVehicleSeatComponent*)Vehicle->GetComponentByClass(UFortVehicleSeatComponent::StaticClass());

	auto SeatIdx = SeatComponent->FindSeatIndex(PlayerController->MyFortPawn);

	UFortWeaponItemDefinition* OldWeapon = nullptr;
	UFortWeaponItemDefinition* NewWeapon = nullptr;
	if (SeatWeaponComponent)
	{
		for (int i = 0; i < SeatWeaponComponent->WeaponSeatDefinitions.Num(); i++)
		{
			auto& WeaponDefinition = SeatWeaponComponent->WeaponSeatDefinitions.Get(i, FWeaponSeatDefinition::Size());

			if (WeaponDefinition.SeatIndex != SeatIdx)
				continue;

			OldWeapon = WeaponDefinition.VehicleWeapon;
			break;
		}

		if (OldWeapon)
		{
			auto ItemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
				{ return entry.ItemDefinition == OldWeapon; }, FFortItemEntry::Size());

			if (ItemEntry)
				PlayerController->WorldInventory->Remove(ItemEntry->ItemGuid);
		}

		for (int i = 0; i < SeatWeaponComponent->WeaponSeatDefinitions.Num(); i++)
		{
			auto& WeaponDefinition = SeatWeaponComponent->WeaponSeatDefinitions.Get(i, FWeaponSeatDefinition::Size());

			if (WeaponDefinition.SeatIndex != TargetSeatIndex)
				continue;

			NewWeapon = WeaponDefinition.VehicleWeapon;
			break;
		}
	}

	callOG(PlayerController, Stack.GetCurrentNativeFunction(), ServerRequestSeatChange, TargetSeatIndex);

	if (OldWeapon && !NewWeapon)
	{
		auto LastItem = Pawn->HasPreviousWeapon() ? (AFortWeapon*)Pawn->PreviousWeapon : nullptr;

		if (LastItem)
		{
			PlayerController->ServerExecuteInventoryItem(LastItem->ItemEntryGuid);
			PlayerController->ClientEquipItem(LastItem->ItemEntryGuid, true);
		}
		else
		{
			printf("yo\n");
			auto pickaxeEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([](FFortItemEntry& entry)
				{ return entry.ItemDefinition->IsA<UFortWeaponMeleeItemDefinition>(); }, FFortItemEntry::Size());

			if (pickaxeEntry)
			{
				printf("yo2\n");
				PlayerController->ServerExecuteInventoryItem(pickaxeEntry->ItemGuid);
				PlayerController->ClientEquipItem(pickaxeEntry->ItemGuid, true);
			}
		}
	}

	if (NewWeapon)
	{
		auto NewItem = PlayerController->WorldInventory->GiveItem(NewWeapon, 1, AFortInventory::GetStats(NewWeapon)->ClipSize);
		auto ItemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
			{ return entry.ItemDefinition == NewWeapon; }, FFortItemEntry::Size());
		auto CurrentWeapon = Pawn->CurrentWeapon;

		PlayerController->ServerExecuteInventoryItem(ItemEntry->ItemGuid);
		PlayerController->ClientEquipItem(ItemEntry->ItemGuid, true);
		if (Pawn->HasPreviousWeapon())
			Pawn->PreviousWeapon = CurrentWeapon;

		auto Weapon = (AFortWeapon*)Pawn->CurrentWeapon;

		if (Weapon)
		{
			auto RepWeaponInfo = (FMountedWeaponInfoRepped*)malloc(FMountedWeaponInfoRepped::Size());

			if (RepWeaponInfo->HasHostVehicleCached())
			{
				RepWeaponInfo->HostVehicleCached.ObjectPointer = Vehicle;
				RepWeaponInfo->HostVehicleCached.InterfacePointer = Vehicle->GetInterface(IFortVehicleInterface::StaticClass());
			}
			else
				RepWeaponInfo->HostVehicleCachedActor = Vehicle;
			RepWeaponInfo->HostVehicleSeatIndexCached = SeatIdx;

			static auto DualClass = FindClass("FortWeaponRangedDualForVehicle");

			if (Weapon->IsA(DualClass))
			{
				static auto MountedWeaponInfoReppedOff = Weapon->GetOffset("MountedWeaponInfoRepped");
				static auto OnRep_MountedWeaponInfoRepped = Weapon->GetFunction("OnRep_MountedWeaponInfoRepped");
				*(FMountedWeaponInfoRepped*)(__int64(Weapon) + MountedWeaponInfoReppedOff) = *RepWeaponInfo;
				Weapon->Call(OnRep_MountedWeaponInfoRepped);
			}
			else
			{
				static auto MountedWeaponInfoReppedOff = Weapon->GetOffset("MountedWeaponInfoRepped");
				static auto OnRep_MountedWeaponInfoRepped = Weapon->GetFunction("OnRep_MountedWeaponInfoRepped");
				*(FMountedWeaponInfoRepped*)(__int64(Weapon) + MountedWeaponInfoReppedOff) = *RepWeaponInfo;
				Weapon->Call(OnRep_MountedWeaponInfoRepped);
			}

			free(RepWeaponInfo);
		}
	}
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

	auto ServerAttemptInteractPC = GetDefaultObj()->GetFunction("ServerAttemptInteract");
	if (!ServerAttemptInteractPC)
		Utils::ExecHook(DefaultObjImpl("FortControllerComponent_Interaction")->GetFunction("ServerAttemptInteract"), ServerAttemptInteract_, ServerAttemptInteract_OG);
	else
		Utils::ExecHook(ServerAttemptInteractPC, ServerAttemptInteract_, ServerAttemptInteract_OG);

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerDropAllItems"), ServerDropAllItems);

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
