#pragma once
#include "../../pch.h"
#include "../../Engine/Public/CurveTable.h"
#include "FortInventory.h"
#include "FortGameMode.h"

class UNetDriver;
class AFortAthenaExitCraft;
class AFortAthenaExitCraftSpawner;
class UFortAthenaExitCraftInfo;
class AAthenaBarrierFlag;
class AAthenaBarrierObjective;
class AAthenaBigBaseWall;
class AAthenaFillFloor;
class AFortPlayerStateAthena;
class AFortPlayerControllerAthena;
class AFortPlayerPawnAthena;
class UFortHealthSet;
class AFortAthenaMutator_Wax;
class AFortAthena_WaxToken;
class ABuildingContainer;

struct FBuildingSupportCellIndex
{
public:
    int32 X = 0;
    int32 Y = 0;
    int32 Z = 0;
};

struct FAthenaGameMessageData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FAthenaGameMessageData);

    DEFINE_STRUCT_PROP(MsgType, uint8);
    DEFINE_STRUCT_PROP(MsgText, FText);
    DEFINE_STRUCT_PROP(MsgSound, UObject*);
    DEFINE_STRUCT_PROP(MsgDelay, float);
    DEFINE_STRUCT_PROP(bIsTeamBased, bool);
    DEFINE_STRUCT_PROP(TeamIndex, int32);
    DEFINE_STRUCT_PROP(DisplayTime, float);
};

struct FControlPointInstanceData
{
public:
    AActor* ControlPoint = nullptr;
    uint8 ControlPointState = 0;
    uint8 ControlPointStatePadding[3]{};
    int32 SpawnDataIdx = -1;
    float SpawnTime = 0.0f;
    float EnableTime = 0.0f;
    float DisableTime = 0.0f;
    uint8 PrevOwningTeam = 0;
    uint8 PrevOwningTeamPadding[3]{};
    AActor* CachedOwningTeamInfo = nullptr;
    float PointAccrualTime = 0.0f;
    float PointsRemainder = 0.0f;
    float BonusPointAccrualTime = 0.0f;
    float BonusPointsRemainder = 0.0f;
    float CachedPointAccrualValue = 0.0f;
    float CachedBonusPointAccrualValue = 0.0f;
    uint8 bPointFinished = 0;
    uint8 PointFinishedPadding[3]{};
    int32 CachedSafeZonePhaseWhenToSpawn = 0;
    uint8 bIgnoreForOrderMessaging = 0;
    uint8 bAlwaysInPlay = 0;
    uint8 AlwaysInPlayPadding[2]{};
    float TimeOfShutdown = 0.0f;
};

static_assert(sizeof(FControlPointInstanceData) == 0x50,
    "10.40 Disco control-point instance layout changed");

struct FAshtonStoneState
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FAshtonStoneState);

    DEFINE_STRUCT_PROP(StoneType, uint8);
    DEFINE_STRUCT_PROP(StoneState, uint8);
    DEFINE_STRUCT_PROP(GameplayTag, FGameplayTag);
    DEFINE_STRUCT_PROP(SpawnTime, float);
    DEFINE_STRUCT_PROP(bHasEverSpawned, bool);
    DEFINE_STRUCT_PROP(SpawnDataIdx, int32);
};

struct FItemLoadoutContainer
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FItemLoadoutContainer);

    DEFINE_STRUCT_PROP(Loadout, TArray<FItemAndCount>);
};

struct FItemLoadoutTeamMap
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FItemLoadoutTeamMap);

    DEFINE_STRUCT_PROP(TeamIndex, uint8);
    DEFINE_STRUCT_PROP(LoadoutIndex, uint8);
    DEFINE_STRUCT_PROP(UpdateOverrideType, uint8);
    DEFINE_STRUCT_PROP(DropAllItemsOverride, uint8);
};

class UBuildingStructuralSupportSystem : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UBuildingStructuralSupportSystem);

    DEFINE_FUNC(K2_GetGridIndicesFromWorldLoc, bool);
    DEFINE_FUNC(K2_GetWorldLocFromGridIndices, bool);
};

struct FItemsToGive final
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FItemsToGive);

    DEFINE_STRUCT_PROP(ItemToDrop, UFortWorldItemDefinition*);
    DEFINE_STRUCT_PROP(NumberToGive, FScalableFloat);
};

struct FItemsToGiveAtPhase final
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FItemsToGiveAtPhase);

    DEFINE_STRUCT_PROP(ItemToDrop, UFortWorldItemDefinition*);
    DEFINE_STRUCT_PROP(NumberToGive, FScalableFloat);
};

class AFortGameplayMutator : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortGameplayMutator);

    DEFINE_BITFIELD_PROP(bMutatorActive);

    DEFINE_FUNC(SetMutatorActive, void);
    DEFINE_FUNC(IsMutatorActive, bool);
};

class UFortMutatorListComponent : public UActorComponent
{
public:
    UCLASS_COMMON_MEMBERS(UFortMutatorListComponent);

    DEFINE_PROP(MutatorDefs, TArray<TSoftClassPtr<UClass>>);
    DEFINE_PROP(Mutators, TArray<AFortGameplayMutator*>);
    // 10.40 exposes this EnumProperty with an IntProperty underlying storage type.
    DEFINE_PROP(InitState, uint32);

    DEFINE_FUNC(GetMutatorByClass, AFortGameplayMutator*);
    DEFINE_FUNC(SetMutatorsActive, void);
};

class AFortAthenaMutator : public AFortGameplayMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator);

    DEFINE_PROP(bMutatesGameMode, bool);
    DEFINE_PROP(bMutatesGameState, bool);
    DEFINE_PROP(CachedGameMode, AFortGameMode*);
    DEFINE_PROP(CachedGameState, AFortGameStateAthena*);
};

enum class EAthenaScoringEventCompat : uint8
{
    None = 0, Elimination = 1, ChestOpened = 2, AmmoCanOpened = 3, SupplyDropOpened = 4,
    SupplyLlamaOpened = 5, ForagedItemConsumed = 6, SurvivalInMinutes = 7, CollectedCoinBronze = 8,
    CollectedCoinSilver = 9, CollectedCoinGold = 10, AIKilled = 11
};

class AFortAthenaMutator_Score : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_Score);

    DEFINE_PROP(NumCoinWaves, int32);
    DEFINE_PROP(bSupportsRespawnConfig, bool);
    DEFINE_PROP(bRespawnsAllowed, bool);
    DEFINE_PROP(StopRespawnPhase, FScalableFloat);
    DEFINE_PROP(GameMsgText_Intro, FText);
    DEFINE_PROP(GameMsgText_FirstCoinsSpawned, FText);
    DEFINE_PROP(GameMsgText_CoinsSpawned, FText);
    DEFINE_PROP(GameMsg_NoMoreRespawnsWarning, FAthenaGameMessageData);
    DEFINE_PROP(GameMsg_NoMoreRespawns, FAthenaGameMessageData);
};

class AFortSpawnedScoreActor : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortSpawnedScoreActor);

    DEFINE_PROP(GameplayTags, FGameplayTagContainer);

    DefUHookOg(OnScoreActorCollected_);
    InitPostLoadHooks;
};

class FFortAthenaScoreRoyaleCompatibility final
{
public:
    static bool IsSupportedBuild();
    static bool IsScoreRoyalePlaylist(const UFortPlaylistAthena* Playlist);
    static bool IsActive();
    static void PreparePlaylist(AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    static void Tick(UNetDriver* Driver, float DeltaSeconds);
    static bool AwardEvent(AFortPlayerControllerAthena* PlayerController,
        EAthenaScoringEventCompat Event, const FGameplayTagContainer* ContextTags = nullptr);
    static void HandleContainerSearched(ABuildingContainer* Container,
        AFortPlayerPawnAthena* SearchingPawn, const FName& OriginalTierGroup);
    static void HandleForagedItemConsumed(AFortPlayerControllerAthena* PlayerController,
        AActor* SourceActor, int32 ScoreBefore);
    static void HandleElimination(AFortPlayerStateAthena* KillerPlayerState,
        AFortPlayerStateAthena* VictimPlayerState, int32 ScoreBefore);
    static bool TryGetRespawnAllowed(AFortPlayerStateAthena* PlayerState, bool& OutAllowed);
};

struct FActiveGameplayModifier
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FActiveGameplayModifier);

    DEFINE_STRUCT_PROP(ModifierDef, UFortGameplayModifierItemDefinition*);
    DEFINE_STRUCT_PROP(Mutators, TArray<AFortGameplayMutator*>);
};

struct FActiveGameplayModifierArray
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FActiveGameplayModifierArray);

    DEFINE_STRUCT_PROP(Items, TArray<FActiveGameplayModifier>);
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
    DEFINE_STRUCT_PROP(ExitCraftState, uint8);
    DEFINE_STRUCT_PROP(SpawnTime, float);
};

class AFortAthenaMutator_Heist : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_Heist);

    DEFINE_PROP(ExitCraftInfo, UFortAthenaExitCraftInfo*);
    DEFINE_PROP(SpawnedExitCraftList, TArray<FHeistExitCraftData>);
    DEFINE_PROP(SpawnExitCraftTime, float);
    DEFINE_PROP(RemainingExitCraftSpawnIndexes, TArray<int32>);
    DEFINE_PROP(NumUnspawnedExitCrafts, int32);
    DEFINE_PROP(NumSpawnedExitCrafts, int32);
    DEFINE_PROP(NumDepartedExitCrafts, int32);

    DefUHookOg(OnGamePhaseStepChanged);

    InitPostLoadHooks;
};

struct FGunGameGunEntry
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FGunGameGunEntry);

    DEFINE_STRUCT_PROP(Weapon, UFortWeaponItemDefinition*);
    DEFINE_STRUCT_PROP(Enabled, FScalableFloat);
    DEFINE_STRUCT_PROP(AwardAtElim, FScalableFloat);
};

class AFortAthenaMutator_GG : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_GG);

    DEFINE_PROP(UseInfiniteAmmo, FScalableFloat);
    DEFINE_PROP(GameIsReverse, FScalableFloat);
    DEFINE_PROP(ElimsWithFinalTierToWin, FScalableFloat);
    DEFINE_PROP(WeaponEntries, TArray<FGunGameGunEntry>);
    DEFINE_PROP(ScoreToWin, int32);
};

struct FWaxPlayerDataEntry : public FFastArraySerializerItem
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FWaxPlayerDataEntry);

    DEFINE_STRUCT_PROP(PlayerState, AFortPlayerStateAthena*);
    DEFINE_STRUCT_PROP(bPermanentlyWaxed, bool);
    DEFINE_STRUCT_PROP(bPlayerWasLeader, bool);
    DEFINE_STRUCT_PROP(TokenBasedPlacement, int32);
    DEFINE_STRUCT_PROP(CurrentTokens, int32);
    DEFINE_STRUCT_PROP(PreviousTokens, int32);
    DEFINE_STRUCT_PROP(CurrentTeamTokens, int32);
    DEFINE_STRUCT_PROP(PreviousTeamTokens, int32);
    DEFINE_STRUCT_PROP(CurrentKills, int32);
    DEFINE_STRUCT_PROP(PreviousKills, int32);
    DEFINE_STRUCT_PROP(CurrentLives, int32);
    DEFINE_STRUCT_PROP(PreviousVictimLocation, FVector);
};

struct FWaxPlayerDataArray : public FFastArraySerializer
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FWaxPlayerDataArray);

    DEFINE_STRUCT_PROP(OwningMutator, UObject*);
    DEFINE_STRUCT_PROP(Entries, TArray<FWaxPlayerDataEntry>);
};

class AFortGameModePickup_Wax : public AFortPickupAthena
{
public:
    UCLASS_COMMON_MEMBERS(AFortGameModePickup_Wax);

    DEFINE_PROP(bIsFirstGeneration, bool);
    DEFINE_PROP(AmountOfTokens, int32);
};

class AFortAthenaMutator_Wax : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_Wax);

    DEFINE_PROP(TokenClass, TSubclassOf<AFortAthena_WaxToken>);
    DEFINE_PROP(TokenPickupClass, TSubclassOf<AFortGameModePickup_Wax>);
    DEFINE_PROP(TeamLeadersInOrder, TArray<AFortPlayerStateAthena*>);
    DEFINE_PROP(PlayerLeadersInOrder, TArray<AFortPlayerStateAthena*>);
    DEFINE_PROP(TokensToStartWith, FScalableFloat);
    DEFINE_PROP(LivesToStartPlayerWith, FScalableFloat);
    DEFINE_PROP(PlayerData, FWaxPlayerDataArray);

    DEFINE_FUNC(TokensCollected, void);
    DEFINE_FUNC(OnRep_Leaders, void);
    DEFINE_FUNC(GetTokensToWinBP, int32);
    DEFINE_FUNC(GetPlayerLives, int32);
    DEFINE_FUNC(CommonDeadPawn, void);

    DefUHookOg(CommonDeadPawnHook);

    InitPostLoadHooks;
};

class FFortAthenaHeistCompatibility final
{
public:
    static bool IsSupportedBuild();
    static bool IsHeistPlaylist(const UFortPlaylistAthena* Playlist);
    static void PreparePlaylist(AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    static bool LoadAdditionalPlaylistLevels(AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    static void Tick(UNetDriver* Driver, float DeltaSeconds);
};

class FFortAthenaNativeLTMCompatibility final
{
public:
    static bool IsSupportedBuild();
    static bool IsOriginalFoodFightSupportedBuild();
    static bool IsOriginalFoodFightPlaylist(const UFortPlaylistAthena* Playlist);
    static bool IsTargetPlaylist(const UFortPlaylistAthena* Playlist);
    static bool IsReadyForMatch(AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    static void BeginPlaylistPublication(AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    static void EndPlaylistPublication(AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    static void PreparePlaylist(AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    static void Tick(UNetDriver* Driver, float DeltaSeconds);
    static void RequestFoodFightWallDrop();
    static bool TryGetFoodFightRespawnAllowed(const AFortPlayerStateAthena* PlayerState,
        bool& OutAllowed);
    static bool TryGetWaxRespawnAllowed(const AFortPlayerStateAthena* PlayerState,
        bool& OutAllowed);
    static bool TryGetDiscoRespawnAllowed(const AFortPlayerStateAthena* PlayerState,
        bool& OutAllowed);
    static void HandleDiscoPlayerReady(AFortPlayerControllerAthena* PlayerController);
    static void HandleAshtonPlayerReady(AFortPlayerControllerAthena* PlayerController);
    static bool IsCurrentAshtonLeader(AFortPlayerControllerAthena* PlayerController);
    static void HandleAshtonLeaderEliminated(AFortPlayerControllerAthena* PlayerController,
        bool bAfterNativeDeath);
    static bool ShouldSuppressAshtonLeaderWorldPickup(const UFortItemDefinition* ItemDefinition);
    static bool ShouldRejectAshtonLeaderGrant(AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition);
    static bool ShouldPreserveAshtonInventoryItem(AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition);
    static bool ShouldBlockAshtonInventoryDrop(AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition);
    static bool ShouldRejectAshtonPickup(AFortPlayerPawnAthena* Pawn,
        const UFortItemDefinition* ItemDefinition);
    static bool ShouldBlockAshtonGenericPickup(AFortPlayerPawnAthena* Pawn,
        const UFortItemDefinition* ItemDefinition);
    static bool IsCurrentAshtonStone(AFortPlayerPawnAthena* Pawn,
        const UFortItemDefinition* ItemDefinition);
    static bool IsAshtonStoneCaptured(const UFortItemDefinition* ItemDefinition);
    static bool TryCompleteAshtonStonePickup(AFortPlayerPawnAthena* Pawn, AFortPickupAthena* Pickup,
        const UFortItemDefinition* ItemDefinition, const char* Reason);
    static void HandleWaxPlayerReady(AFortPlayerControllerAthena* PlayerController);
    static void HandleWaxElimination(AFortPlayerStateAthena* VictimPlayerState,
        AFortPlayerPawnAthena* VictimPawn);
    static bool TryCollectWaxPickup(AFortPlayerPawnAthena* Pawn, AFortPickupAthena* Pickup);
    static bool ShouldSuppressArsenalWorldLoot();
    static bool TryClaimArsenalElimination(AFortPlayerStateAthena* VictimPlayerState,
        AFortPlayerPawnAthena* VictimPawn);
    static void HandleArsenalPlayerReady(AFortPlayerControllerAthena* PlayerController);
    static void HandleArsenalElimination(AFortPlayerControllerAthena* KillerController,
        AFortPlayerStateAthena* KillerPlayerState, AFortPlayerControllerAthena* VictimController,
        AFortPlayerStateAthena* VictimPlayerState, AFortPlayerPawnAthena* VictimPawn);
};

struct FFillFloorPositionData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FFillFloorPositionData);

    DEFINE_STRUCT_PROP(MoveTime, FScalableFloat);
    DEFINE_STRUCT_PROP(Height, FScalableFloat);
    DEFINE_STRUCT_PROP(WaitTime, FScalableFloat);
};

class AAthenaFillFloor : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AAthenaFillFloor);

    DEFINE_PROP(StepIndex, int32);
    DEFINE_PROP(FloorZ, float);
    DEFINE_PROP(FloorPositionData, TArray<FFillFloorPositionData>);
    DEFINE_PROP(EventHeights, TArray<float>);
};

class AFortAthenaMutator_Fill : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_Fill);

    DEFINE_PROP(SpawnHeight, FScalableFloat);
    DEFINE_PROP(BuildableOffset, FScalableFloat);
    DEFINE_PROP(CanBuildOnLava, FScalableFloat);
    DEFINE_PROP(LavaFloor, AAthenaFillFloor*);
};

class AAthenaBarrierObjective : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AAthenaBarrierObjective);

    DEFINE_PROP(FoodTeam, uint8);
    DEFINE_PROP(ObjectiveDamageState, uint8);
    DEFINE_PROP(HeadRotationYaw, float);
    DEFINE_PROP(bAllowDamage, bool);
    DEFINE_PROP(BuildingAttributeSet, UFortHealthSet*);
    DEFINE_PROP(ReplicatedBuildingAttributeSet, UFortHealthSet*);
    DEFINE_PROP(MaxHealthInitializationValue, float);

    DEFINE_FUNC(OnRep_FoodTeam, void);
    DEFINE_FUNC(OnRep_ObjectiveDamageState, void);
    DEFINE_FUNC(OnRep_HeadRotationYaw, void);
    DEFINE_FUNC(GetHealth, float);
    DEFINE_FUNC(GetMaxHealth, float);
    DEFINE_FUNC(GetHealthPercent, float);
    DEFINE_FUNC(SetHealth, void);
    DEFINE_FUNC(SetTeam, void);
    DEFINE_FUNC(UpdateInGameHealth, void);
    DEFINE_FUNC(OnGeneratorDestroyed, void);
};

// Restores the authored tournament-stat gameplay modifier when stripped
// dedicated-server asset discovery leaves an Arena playlist without it.
// Registration is deliberately limited to canonical Arena playlists and is
// idempotent against GameState's active-modifier list.
class FFortAthenaTournamentStatsCompatibility final
{
public:
    static bool IsSupportedBuild();
    static bool IsArenaPlaylist(
        const UFortPlaylistAthena* Playlist);
    static void PreparePlaylist(
        AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist);
    // True only when every bounded registration attempt failed before
    // ProcessEvent began. A successful/ambiguous native dispatch never permits
    // reflected fallback presentation for the same match.
    static bool ShouldUseFallbackPresentation(
        AFortGameStateAthena* GameState);
    static void Tick(UNetDriver* Driver, float DeltaSeconds);
};

class AAthenaBigBaseWall : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AAthenaBigBaseWall);

    DEFINE_PROP(WallGravity, float);
    DEFINE_PROP(TimeUntilWallComesDown, float);
    DEFINE_PROP(BarrierState, uint8);

    DEFINE_FUNC(OnRep_WallGravity, void);
    DEFINE_FUNC(OnRep_TimeUntilWallComesDown, void);
    DEFINE_FUNC(OnRep_BarrierState, void);
};

class AAthenaBarrierFlag : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AAthenaBarrierFlag);

    DEFINE_PROP(CurrentState, uint8);
    DEFINE_PROP(FoodTeam, uint8);
    DEFINE_PROP(SnapGridSize, float);
    DEFINE_PROP(VertSnapGridSize, float);
    DEFINE_PROP(SnapOffset, FVector);
    DEFINE_PROP(CentroidOffset, FVector);
    DEFINE_PROP(BaseLocToPivotOffset, FVector);

    DEFINE_FUNC(OnNewFoodTeam, void);
    DEFINE_FUNC(OnRep_CurrentState, void);
    DEFINE_FUNC(OnRep_FoodTeam, void);
    DEFINE_FUNC(GetObjectiveActor, AAthenaBarrierObjective*);
    DEFINE_FUNC(SetTeam, void);

    static inline AAthenaBarrierObjective* (*GetObjectiveActorOG)(AAthenaBarrierFlag*) = nullptr;
    static AAthenaBarrierObjective* GetObjectiveActorHook(AAthenaBarrierFlag* Flag);
};

struct FBarrierTeamState
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FBarrierTeamState);

    DEFINE_STRUCT_PROP(TeamNum, uint8);
    DEFINE_STRUCT_PROP(FoodTeam, uint8);
    DEFINE_STRUCT_PROP(ObjectiveFlag, AAthenaBarrierFlag*);
    DEFINE_STRUCT_PROP(ObjectiveObject, AAthenaBarrierObjective*);
    DEFINE_STRUCT_PROP(bRespawnEnabled, bool);
};

class AFortAthenaMutator_Barrier : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_Barrier);

    DEFINE_PROP(BigBaseWallClass, TSubclassOf<AAthenaBigBaseWall>);
    DEFINE_PROP(ObjectiveFlag, TSubclassOf<AAthenaBarrierFlag>);
    DEFINE_PROP(bGameEndsWhenObjectiveIsDestroyed, bool);
    DEFINE_PROP(BigBaseWall, AAthenaBigBaseWall*);
    DEFINE_PROP(Team_0_State, FBarrierTeamState);
    DEFINE_PROP(Team_1_State, FBarrierTeamState);
    DEFINE_PROP(ObjectiveDistanceFromWall, FScalableFloat);
    DEFINE_PROP(ObjectiveZOffset, FScalableFloat);
    DEFINE_PROP(ObjectiveMaxZDiff, FScalableFloat);
    DEFINE_PROP(ObjectiveMaxSpawnHeight, FScalableFloat);
    DEFINE_PROP(ObjectivesSpawnSameHeight, FScalableFloat);
    DEFINE_PROP(LavaLevelRelativeToMascot, FScalableFloat);
    DEFINE_PROP(WallGravity, FScalableFloat);
    DEFINE_PROP(SafeZonePhaseWhenToBringDownWall, FScalableFloat);
    DEFINE_PROP(TimeToBringDownWall, FScalableFloat);
    DEFINE_PROP(GameMsg_WallComingDown, FAthenaGameMessageData);
    DEFINE_PROP(GameMsg_WallDown, FAthenaGameMessageData);

    DEFINE_FUNC(CheckHealthThreshold, void);

    DefUHookOg(OnGamePhaseStepChanged);

    InitPostLoadHooks;
};

class AFortAthenaMutator_Ashton : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_Ashton);

    DEFINE_PROP(StoneList, TArray<FAshtonStoneState>);
    DEFINE_PROP(VillainLeaderItemDef, UFortGadgetItemDefinition*);
    DEFINE_PROP(VillainItemDefs, TArray<UFortWorldItemDefinition*>);
    DEFINE_PROP(VillainLeaderPC, AFortPlayerControllerAthena*);

    DEFINE_FUNC(OnRep_StoneList, void);

    DefUHookOg(OnGamePhaseStepChanged);
    DefUHookOg(OnPickupDestroying);
    DefUHookOg(SelectNextVillainLeader);

    InitPostLoadHooks;
};

class AFortAthenaMutator_InventoryOverride : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_InventoryOverride);

    DEFINE_PROP(InventoryLoadouts, TArray<FItemLoadoutContainer>);
    DEFINE_PROP(TeamLoadouts, TArray<FItemLoadoutTeamMap>);
};

class AFortAthenaMutator_Disco : public AFortAthenaMutator
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_Disco);

    DEFINE_PROP(SpawnedControlPoints, TArray<FControlPointInstanceData>);
    DEFINE_FUNC(IsRespawningAllowed, bool);

    DefUHookOg(OnGamePhaseChanged);
    DefUHookOg(OnGamePhaseStepChanged);

    InitPostLoadHooks;
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
    DEFINE_PROP(ItemsToGive, TArray<FItemsToGiveAtPhase>);

    DefUHookOg(OnGamePhaseChanged);

    InitPostLoadHooks;
};
