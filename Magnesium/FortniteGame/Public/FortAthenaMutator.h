#pragma once
#include "../../pch.h"
#include "../../Engine/Public/CurveTable.h"
#include "FortInventory.h"
#include "FortGameMode.h"

class UNetDriver;
class AFortAthenaExitCraft;
class AFortAthenaExitCraftSpawner;
class UFortAthenaExitCraftInfo;

struct FItemsToGive final
{
public:
    UFortWorldItemDefinition* ItemToDrop;
    FScalableFloat NumberToGive;
};

class AFortAthenaMutator : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator);

    DEFINE_PROP(CachedGameMode, AFortGameMode*);
    DEFINE_PROP(CachedGameState, AFortGameStateAthena*);
};

struct FExitCraftInfo
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FExitCraftInfo);

    DEFINE_STRUCT_PROP(ExitCraftSpawnDelay, FScalableFloat);
    DEFINE_STRUCT_PROP(ExitCraftZOffset, FScalableFloat);
};

class UFortAthenaExitCraftInfo : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortAthenaExitCraftInfo);

    DEFINE_PROP(ExitCaftClass, TSubclassOf<AFortAthenaExitCraft>);
    DEFINE_PROP(ExitCraftInfo, FExitCraftInfo);
};

class AFortAthenaExitCraft : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaExitCraft);

    DEFINE_PROP(ExitCraftInfo, UFortAthenaExitCraftInfo*);
    DEFINE_PROP(CurrentState, uint8);
};

class AFortAthenaExitCraftSpawner : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaExitCraftSpawner);

    DEFINE_PROP(ExitCraftInfo, UFortAthenaExitCraftInfo*);
};

struct FHeistExitCraftData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FHeistExitCraftData);

    DEFINE_STRUCT_PROP(ExitCraftSpawner, AFortAthenaExitCraftSpawner*);
    DEFINE_STRUCT_PROP(SpawnedExitCraft, AFortAthenaExitCraft*);
};

class AFortAthenaMutator_Heist : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_Heist);

    DEFINE_PROP(ExitCraftInfo, UFortAthenaExitCraftInfo*);
    DEFINE_PROP(SpawnedExitCraftList, TArray<FHeistExitCraftData>);
    DEFINE_PROP(SpawnExitCraftTime, float);
};

class FFortAthenaHeistCompatibility final
{
public:
    static bool IsSupportedBuild();
    static bool IsHeistPlaylist(const UFortPlaylistAthena* Playlist);
    static void PreparePlaylist(
        AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    static bool LoadAdditionalPlaylistLevels(
        AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    static void Tick(UNetDriver* Driver, float DeltaSeconds);
};

class AFortAthenaMutator_GiveItemsAtGamePhaseStep : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_GiveItemsAtGamePhaseStep);

    DEFINE_PROP(PhaseToGiveItems, uint8);
    DEFINE_PROP(ItemsToGive, TArray<FItemsToGive>);

    DefUHookOg(OnGamePhaseStepChanged);

    InitPostLoadHooks;
};

class AFortAthenaMutator_GiveItemsAtGamePhase : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_GiveItemsAtGamePhase);

    DEFINE_PROP(PhaseToGiveItems, uint8);
    DEFINE_PROP(ItemsToGive, TArray<FItemsToGive>);

    DefUHookOg(OnGamePhaseChanged);

    InitPostLoadHooks;
};
