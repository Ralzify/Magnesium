#pragma once
#include "../../pch.h"
#include "FortGameStateAthena.h"
#include "FortGameSessionAthena.h"
#include "FortSafeZoneIndicator.h"
#include "FortInventory.h"
#include "FortPlayerPawnAthena.h"
#include "FortPlayerControllerAthena.h"
#include "FortAthenaSpawningPolicyManager.h"

enum class EEvaluateCurveTableResult : uint8
{
    RowFound = 0, RowNotFound = 1, EEvaluateCurveTableResult_MAX = 2,
};

class UNetDriver;

class UFortSpawnActorInfo : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortSpawnActorInfo);

    DEFINE_PROP(SpawnActorID, FName);
    DEFINE_PROP(SpawnActorClass, TSubclassOf<AActor>);
    DEFINE_PROP(SpawnTiming, uint8);
    DEFINE_PROP(SafeZoneIndex, FScalableFloat);
    DEFINE_PROP(SpawnAtSafeZoneIndex, FScalableFloat);
};

struct FFortSpawnActorData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FFortSpawnActorData);

    DEFINE_STRUCT_PROP(SpawnActorInfo, UFortSpawnActorInfo*);
    DEFINE_STRUCT_PROP(NumSpawnsRemaining, int32);
    DEFINE_STRUCT_PROP(TimeUntilNextSpawn, float);
    DEFINE_STRUCT_PROP(SpawnedFortSpawnActors, TArray<AActor*>);
};

// Element layouts change between releases, but the reflected arrays keep Unreal's 0x10 script-array header.
struct FSupplyDropSpawnDataArrayHeader
{
    void* Data = nullptr;
    int32 NumElements = 0;
    int32 MaxElements = 0;
};

static_assert(sizeof(FSupplyDropSpawnDataArrayHeader) == 0x10,
    "Supply-drop array header must match Unreal's TArray header");

class AFortGameMode : public AActor
{
public:
    static inline uint8_t CurrentTeam = 3;
    static inline uint8_t PlayersOnCurTeam = 0;
    static inline TArray<const UFortAbilitySet*> AbilitySets;
    static inline const UFortAbilitySet* TacticalSprintAbilitySet = nullptr;
    static inline FVector SafeZoneLoc{};

    UCLASS_COMMON_MEMBERS(AFortGameMode);

    DEFINE_PROP(CurrentPlaylistId, int32);
    DEFINE_PROP(WarmupRequiredPlayerCount, int32);
    DEFINE_BITFIELD_PROP(bWorldIsReady);
    DEFINE_PROP(GameSession, AFortGameSession*);
    DEFINE_PROP(CurrentPlaylistName, FName);
    DEFINE_PROP(GameState, AFortGameStateAthena*);
    DEFINE_PROP(AlivePlayers, TArray<AActor*>);
    DEFINE_PROP(AliveBots, TArray<AActor*>); // native player bots (playlist/NPC AI)
    DEFINE_PROP(SafeZoneIndicator, AFortSafeZoneIndicator*);
    DEFINE_PROP(SafeZonePhase, int32);
    DEFINE_PROP(StartingItems, TArray<FItemAndCount>);
    DEFINE_PROP(bDisableGCOnServerDuringMatch, bool);
    DEFINE_PROP(bPlaylistHotfixChangedGCDisabling, bool);
    DEFINE_PROP(AthenaGameDataTable, UCurveTable*);
    DEFINE_PROP(RedirectAthenaLootTierGroups, TMap<FName, FName>);
    DEFINE_PROP(WarmupCountdownDuration, float);
    DEFINE_PROP(WarmupEarlyCountdownDuration, float);
    DEFINE_PROP(SafeZoneLocations, TArray<FVector>);
    DEFINE_PROP(bSafeZoneLocationsInitialized, bool);
    DEFINE_PROP(GE_OutsideSafeZone, UClass*);
    DEFINE_PROP(DefaultPawnClass, const UClass*);
    DEFINE_PROP(PlayerControllerClass, const UClass*);
    DEFINE_PROP(PlaylistHotfixOriginalGCFrequency, float);
    DEFINE_PROP(SafeZoneIndicatorClass, TSubclassOf<AFortSafeZoneIndicator>);
    DEFINE_PROP(TimeBetweenStormCapDamage, FScalableFloat);
    DEFINE_PROP(StormCapDamagePerTick, FScalableFloat);
    DEFINE_PROP(StormCampingIncrementTimeAfterDelay, FScalableFloat);
    DEFINE_PROP(StormCampingInitialDelayTime, FScalableFloat);
    DEFINE_PROP(bSafeZoneActive, bool);
    DEFINE_PROP(bSafeZonePaused, bool);
    DEFINE_PROP(OnSafeZoneIndicatorSpawned, TMulticastInlineDelegate<void(AFortSafeZoneIndicator*)>);
    DEFINE_PROP(MatchState, FName);
    DEFINE_PROP(bAlwaysDBNO, bool);
    DEFINE_PROP(bDBNOEnabled, bool);
    DEFINE_PROP(bEnableDBNO, bool);
    DEFINE_PROP(AIDirector, AActor*);
    DEFINE_PROP(AIGoalManager, AActor*);
    DEFINE_PROP(bEnableReplicationGraph, bool);
    DEFINE_PROP(bAllowSpectateAfterDeath, bool);
    DEFINE_PROP(ServerBotManager, UObject*);
    DEFINE_PROP(SpawningPolicyManager, AFortAthenaSpawningPolicyManager*);
    DEFINE_PROP(OnPlaylistLootTablesAppliedDelegate, TMulticastInlineDelegate<void()>);
    DEFINE_PROP(SupplyDropSpawnDataList, FSupplyDropSpawnDataArrayHeader);

    DEFINE_FUNC(SpawnDefaultPawnAtTransform, AFortPlayerPawnAthena*);
    DEFINE_FUNC(RestartPlayer, void);
    DEFINE_FUNC(ReadyToStartMatch, bool);
    DEFINE_FUNC(HandleStartingNewPlayer, void);
    DEFINE_FUNC(OnAircraftExitedDropZone, void);
    DEFINE_FUNC(ChangeName, void);
    DEFINE_FUNC(ChoosePlayerStart, AActor*);
    DEFINE_FUNC(GetDefaultPawnClassForController, const UClass*);
    DEFINE_FUNC(IsInCurrentSafeZone, bool);

    DefUHookOgRet(bool, ReadyToStartMatch_);
    static void SpawnDefaultPawnFor(UObject*, FFrame&, AActor**);
    DefHookOg(void, HandlePostSafeZonePhaseChanged, AFortGameMode*, int);
    DefHookOg(uint8_t, PickTeam, AFortGameMode*, uint8_t, AFortPlayerControllerAthena*);
    DefUHookOg(HandleStartingNewPlayer_);
    DefHookOg(bool, StartAircraftPhase, AFortGameMode*, char);
    DefUHookOg(OnAircraftExitedDropZone_);
    DefHookOg(void, FinishWorldInitialization, AFortGameMode*, AActor*);
    static int GetLateSafeZoneIndex();
    static bool ProbeMovingSafeZonePhasePublisher();
    static int32 ResolveMovingSafeZonePreflightCapacity(AFortGameMode* GameMode,
        AFortAthenaMapInfo* MapInfo);
    static void TickLateGameSafeZonePhaseFallback(UNetDriver* Driver);
    static void TickPendingVehicleSpawns();
    static void TickSupplyDropSuppression(bool bForceDiscovery = false);
    static void TickGameplayConfigurationPolicy(float DeltaSeconds);
    static const UFortPlaylistAthena* GetActivePlaylist(AFortGameStateAthena* GameState);
    static bool AssignCheatBotIsolatedTeam(AFortGameMode* GameMode,
        AFortPlayerControllerAthena* BotController, AActor* Instigator, uint8& OutTeamIndex);

    InitHooks;
    InitPostLoadHooks;
};

class AFortGameModeAthena : public AFortGameMode
{
public:
    UCLASS_COMMON_MEMBERS(AFortGameModeAthena);

    DEFINE_PROP(SpawnActorDataList, TArray<FFortSpawnActorData>);
};
